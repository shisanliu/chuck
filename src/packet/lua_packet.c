/*
*  数据包表示       |2字节数据长度|数据...|
*  二进制数据表示法	|2字节数据长度|数据...|
*/


#include "lua/chk_lua.h"
#include "util/chk_error.h"
#include "util/chk_bytechunk.h"
#include "util/chk_order.h"
#include "socket/chk_decoder.h"

#ifndef  cast
# define  cast(T,P) ((T)(P))
#endif

#define BYTEBUFFER_METATABLE "lua_bytebuffer"

#define WPACKET_METATABLE "lua_wpacket"

#define RPACKET_METATABLE "lua_rpacket"


typedef struct {
    chk_bytebuffer *buff;               
}lua_wpacket;


typedef struct {
	chk_bytebuffer *buff;
    chk_bytechunk  *cur;       
    uint32_t        readpos;           
    uint32_t        data_remain;        
}lua_rpacket;


#define lua_checkbytebuffer(L,I)	\
	(chk_bytebuffer*)luaL_checkudata(L,I,BYTEBUFFER_METATABLE)

#define lua_checkwpacket(L,I)	\
	(lua_wpacket*)luaL_checkudata(L,I,WPACKET_METATABLE)

#define lua_checkrpacket(L,I)	\
	(lua_rpacket*)luaL_checkudata(L,I,RPACKET_METATABLE)	


//一个解包器,包头2字节,表示后面数据大小.
typedef struct _decoder {
	void (*update)(chk_decoder*,chk_bytechunk *b,uint32_t spos,uint32_t size);
	chk_bytebuffer *(*unpack)(chk_decoder*,int32_t *err);
	void (*dctor)(chk_decoder*);
	uint32_t       spos;
	uint32_t       size;
	uint32_t       max;
	chk_bytechunk *b;
}_decoder;

static inline void _update(chk_decoder *_,chk_bytechunk *b,uint32_t spos,uint32_t size) {
	_decoder *d = ((_decoder*)_);
    if(!d->b) {
	    d->b = chk_bytechunk_retain(b);
	    d->spos  = spos;
	    d->size = 0;
    }
    d->size += size;
}

static inline chk_bytebuffer *_unpack(chk_decoder *_,int32_t *err) {
	_decoder *d = ((_decoder*)_);
	chk_bytebuffer *ret  = NULL;
	chk_bytechunk  *head = d->b;
	uint16_t        pk_len;
	uint32_t        pk_total,size,pos;
	do {
		if(d->size <= sizeof(pk_len)) break;
		size = sizeof(pk_len);
		pos  = d->spos;
		chk_bytechunk_read(head,(char*)&pk_len,&pos,&size);//读取payload大小
		pk_len = chk_ntoh16(pk_len);
		if(pk_len == 0) {
			if(err) *err = -1;
			break;
		}
		pk_total = size + pk_len;
		if(pk_total > d->max) {
			if(err) *err = CHK_EPKTOOLARGE;//数据包操作限制大小
			break;
		}
		if(pk_total > d->size) break;//没有足够的数据
		ret = chk_bytebuffer_new(head,d->spos,pk_total);
		//调整pos及其b
		do {
			head = d->b;
			size = head->cap - d->spos;
			size = pk_total > size ? size:pk_total;
			d->spos  += size;
			pk_total-= size;
			d->size -= size;
			if(d->spos >= head->cap) { //当前b数据已经用完
				d->b = chk_bytechunk_retain(head->next);
				chk_bytechunk_release(head);
				d->spos = 0;
				if(!d->b) break;
			}
		}while(pk_total);			
	}while(0);
	return ret;
}

static inline void _dctor(chk_decoder *_) {
	_decoder *d = ((_decoder*)_);
	if(d->b) chk_bytechunk_release(d->b);
}

static inline _decoder *_decoder_new(uint32_t max) {
	_decoder *d = calloc(1,sizeof(*d));
	d->update = _update;
	d->unpack = _unpack;
	d->max    = max;
	d->dctor  = _dctor;
	return d;
}


/*
* lua_rpacket用于从一个完整的协议包中读取数据
*/


static inline int32_t lua_rpacket_read(lua_rpacket *r,char *out,uint32_t size) {
    if(size > r->data_remain) return -1;//请求数据大于剩余数据
    r->cur      = chk_bytechunk_read(r->cur,out,&r->readpos,&size);
    r->data_remain -= size;
    return 0;
}

#define LUA_RPACKET_READ(R,TYPE)                                   ({\
    TYPE __result=0;                                                 \
    lua_rpacket_read(R,(char*)&__result,sizeof(TYPE));               \
    __result;                                                       })

static inline int32_t lua_rpacket_readI8(lua_State *L) {
	lua_rpacket *r = lua_checkrpacket(L,1);
    lua_pushinteger(L,LUA_RPACKET_READ(r,int8_t));
    return 1;
}

static inline int32_t lua_rpacket_readI16(lua_State *L) {
	lua_rpacket *r = lua_checkrpacket(L,1);
    lua_pushinteger(L,chk_ntoh16(LUA_RPACKET_READ(r,int16_t)));
    return 1;
}

static inline int32_t lua_rpacket_readI32(lua_State *L) {
	lua_rpacket *r = lua_checkrpacket(L,1);
    lua_pushinteger(L,chk_ntoh32(LUA_RPACKET_READ(r,int32_t)));
    return 1;
}

static inline int32_t lua_rpacket_readI64(lua_State *L) {
	lua_rpacket *r = lua_checkrpacket(L,1);
    lua_pushinteger(L,chk_ntoh64(LUA_RPACKET_READ(r,int64_t)));
    return 1;
}

static inline int32_t lua_rpacket_readDub(lua_State *L) {
	lua_rpacket *r = lua_checkrpacket(L,1);
    lua_pushnumber(L,LUA_RPACKET_READ(r,double));
    return 1;
}

static inline int32_t lua_rpacket_readStr(lua_State *L) {
	luaL_Buffer     lb;
	char           *in;
	lua_rpacket    *r = lua_checkrpacket(L,1);
	uint16_t        size = chk_ntoh16(LUA_RPACKET_READ(r,uint16_t));
	if(size == 0) return 0;
	luaL_buffinit(L, &lb);
	in = luaL_buffinitsize(L,&lb,(size_t)size);
	if(0 != (uint16_t)lua_rpacket_read(r,in,size))
		return luaL_error(L,"lua_rpacket_readstr invaild packet");
	luaL_pushresultsize(&lb,size);
	return 1;
}


/*
* lua_wpacket用于向buffer中写入符合协议的数据
*/

static inline int32_t lua_wpacket_write(lua_wpacket *w,char *in,uint32_t size) {
    if(w->buff->datasize + size < w->buff->datasize) {
    	//log
    	return -1;
    }
    chk_bytebuffer_append(w->buff,(uint8_t*)in,size);
    *((uint16_t*)(w->buff->head->data)) = chk_hton16(w->buff->datasize - sizeof(uint16_t));
    return 0;
}


static inline int32_t lua_wpacket_writeI8(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1);
	int8_t value = (int8_t)luaL_checkinteger(L,2);  
    if(0 != lua_wpacket_write(w,cast(char*,&value),sizeof(value)))
    	return luaL_error(L,"write beyond limited");
    return 0;
}

static inline int32_t lua_wpacket_writeI16(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1); 
    int16_t value = chk_hton16((int16_t)luaL_checkinteger(L,2));
    if(0 != lua_wpacket_write(w,cast(char*,&value),sizeof(value)))
    	return luaL_error(L,"write beyond limited");
    return 0;        
}

static inline int32_t lua_wpacket_writeI32(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1);    
    int32_t value = chk_hton32((int32_t)luaL_checkinteger(L,2));
    if(0 != lua_wpacket_write(w,cast(char*,&value),sizeof(value)))
    	return luaL_error(L,"write beyond limited");
    return 0;
}

static inline int32_t lua_wpacket_writeI64(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1);    
    int64_t value = chk_hton64((int64_t)luaL_checkinteger(L,2));
    if(0 != lua_wpacket_write(w,cast(char*,&value),sizeof(value)))
    	return luaL_error(L,"write beyond limited");
    return 0;
}

static inline int32_t lua_wpacket_writeDub(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1);
	double value = luaL_checknumber(L,2);    
    if(0 != lua_wpacket_write(w,cast(char*,&value),sizeof(value)))
    	return luaL_error(L,"write beyond limited");
    return 0;
}

static inline int32_t lua_wpacket_writeStr(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1);
	const char  *str;
	size_t       len;
	uint16_t     size;
	if(!lua_isstring(L,2)) luaL_error(L,"argument 2 or lua_rpacket_readstr must be string");
	str = lua_tolstring(L,2,&len);
	size = chk_hton16((uint16_t)len);
	if(0 != lua_wpacket_write(w,cast(char*,&size),sizeof(size)))
		return luaL_error(L,"write beyond limited");
	if(0 != lua_wpacket_write(w,cast(char*,str),(uint32_t)len))
		return luaL_error(L,"write beyond limited");
	return 0;
}

static inline int32_t lua_new_wpacket(lua_State *L) {
	chk_bytebuffer *buff = lua_checkbytebuffer(L,1);
	lua_wpacket    *w = (lua_wpacket*)lua_newuserdata(L, sizeof(*w));
	w->buff = buff;
	chk_bytebuffer_append_word(buff,0);
	luaL_getmetatable(L, WPACKET_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}

static inline int32_t lua_new_rpacket(lua_State *L) {
	chk_bytebuffer *buff = lua_checkbytebuffer(L,1);
	lua_rpacket    *r = (lua_rpacket*)lua_newuserdata(L, sizeof(*r));
	r->cur = buff->head;
	r->buff = buff;
	r->data_remain = buff->datasize - 2;
	if(r->cur->cap - buff->spos > 2)
		r->readpos = buff->spos + 2;
	else {
		r->cur = r->cur->next;
		r->readpos = 1;
	}
	luaL_getmetatable(L, RPACKET_METATABLE);
	lua_setmetatable(L, -2);	
	return 1;
}

static inline int32_t lua_rpacket_buff(lua_State *L) {
	lua_rpacket *r = lua_checkrpacket(L,1);
	lua_pushlightuserdata(L,r->buff);
	luaL_getmetatable(L, BYTEBUFFER_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}

static inline int32_t lua_wpacket_buff(lua_State *L) {
	lua_wpacket *w = lua_checkwpacket(L,1);
	lua_pushlightuserdata(L,w->buff);
	luaL_getmetatable(L, BYTEBUFFER_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}

static inline int32_t lua_new_decoder(lua_State *L) {
	uint32_t max = (uint32_t)luaL_optinteger(L,1,1024);
	lua_pushlightuserdata(L,_decoder_new(max));
	return 1;
}

#define SET_FUNCTION(L,NAME,FUNC) do{\
	lua_pushstring(L,NAME);\
	lua_pushcfunction(L,FUNC);\
	lua_settable(L, -3);\
}while(0)

int32_t luaopen_packet(lua_State *L)
{

	luaL_Reg wpacket_methods[] = {
		{"WriteI8",   lua_wpacket_writeI8},
		{"WriteI16",  lua_wpacket_writeI16},
		{"WriteI32",  lua_wpacket_writeI32},
		{"WriteI64",  lua_wpacket_writeI64},
		{"WriteNum",  lua_wpacket_writeDub},
		{"WriteStr",  lua_wpacket_writeStr},
		{"Buff", 	  lua_wpacket_buff},
		{NULL,     NULL}
	};

	luaL_Reg rpacket_methods[] = {
		{"ReadI8",   lua_rpacket_readI8},
		{"ReadI16",  lua_rpacket_readI16},
		{"ReadI32",  lua_rpacket_readI32},
		{"ReadI64",  lua_rpacket_readI64},
		{"ReadNum",  lua_rpacket_readDub},
		{"ReadStr",  lua_rpacket_readStr},
		{"Buff", 	 lua_rpacket_buff},
		{NULL,     NULL}
	};	

	luaL_newmetatable(L, WPACKET_METATABLE);
	luaL_newlib(L, wpacket_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);


	luaL_newmetatable(L, RPACKET_METATABLE);
	luaL_newlib(L, rpacket_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);	


	lua_newtable(L);
	SET_FUNCTION(L,"Writer",lua_new_wpacket);
	SET_FUNCTION(L,"Reader",lua_new_rpacket);
	SET_FUNCTION(L,"Decoder",lua_new_decoder);
	return 1;
}
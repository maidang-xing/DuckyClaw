#ifndef __LUA_MODULE_SYS_H__
#define __LUA_MODULE_SYS_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_sys(lua_State *L);
void lua_module_sys_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_SYS_H__ */

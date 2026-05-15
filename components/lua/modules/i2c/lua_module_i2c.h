#ifndef __LUA_MODULE_I2C_H__
#define __LUA_MODULE_I2C_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_i2c(lua_State *L);
void lua_module_i2c_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_I2C_H__ */

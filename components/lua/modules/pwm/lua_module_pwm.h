#ifndef __LUA_MODULE_PWM_H__
#define __LUA_MODULE_PWM_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_pwm(lua_State *L);
void lua_module_pwm_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_PWM_H__ */

#ifndef __LUA_MODULE_ADC_H__
#define __LUA_MODULE_ADC_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_adc(lua_State *L);
void lua_module_adc_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_ADC_H__ */

/**
 * @file lua_module_sys.c
 * @brief Lua C module exposing TuyaOpen system info to sandboxed scripts.
 *
 * Lua API (available after lua_module_sys_register()):
 *   local ms = sys.uptime_ms()      -- ms since boot
 *   local n  = sys.random(range)    -- integer in [0, range)
 *   local b  = sys.free_heap()      -- free internal heap bytes
 *   local r  = sys.reset_reason()   -- string describing last reset
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_sys.h"

#include "tal_api.h"
#include "lauxlib.h"

static int lua_sys_uptime_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)tal_system_get_millisecond());
    return 1;
}

static int lua_sys_random(lua_State *L)
{
    lua_Integer range = luaL_checkinteger(L, 1);
    if (range <= 0) {
        return luaL_error(L, "sys.random: range must be > 0");
    }
    lua_pushinteger(L, (lua_Integer)tal_system_get_random((uint32_t)range));
    return 1;
}

static int lua_sys_free_heap(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)tal_system_get_free_heap_size());
    return 1;
}

static int lua_sys_reset_reason(lua_State *L)
{
    char *desc = NULL;
    tal_system_get_reset_reason(&desc);
    lua_pushstring(L, desc ? desc : "unknown");
    return 1;
}

int luaopen_sys(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_sys_uptime_ms);
    lua_setfield(L, -2, "uptime_ms");
    lua_pushcfunction(L, lua_sys_random);
    lua_setfield(L, -2, "random");
    lua_pushcfunction(L, lua_sys_free_heap);
    lua_setfield(L, -2, "free_heap");
    lua_pushcfunction(L, lua_sys_reset_reason);
    lua_setfield(L, -2, "reset_reason");
    return 1;
}

void lua_module_sys_register(void)
{
    lua_module_register("sys", luaopen_sys);
}

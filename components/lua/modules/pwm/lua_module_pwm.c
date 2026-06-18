/**
 * @file lua_module_pwm.c
 * @brief Lua C module exposing TuyaOpen PWM control to sandboxed scripts.
 *
 * Lua API (available after lua_module_pwm_register()):
 *   pwm.init(ch, freq, duty)   -- init + start; duty 0..10000 (10000=100%)
 *   pwm.deinit(ch)             -- stop + deinit
 *   pwm.set_duty(ch, duty)     -- update duty while running
 *   pwm.set_freq(ch, freq)     -- update frequency (Hz) while running
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_pwm.h"

#include "tkl_pwm.h"
#include "lauxlib.h"

#define PWM_CH_MAX   TUYA_PWM_NUM_MAX
#define PWM_CYCLE    10000u

static bool __ch_valid(int ch)
{
    return ch >= 0 && ch < (int)PWM_CH_MAX;
}

static int lua_pwm_init(lua_State *L)
{
    int ch   = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    int duty = (int)luaL_checkinteger(L, 3);

    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range (0-%d)", ch, PWM_CH_MAX - 1);
    }
    if (freq <= 0) {
        return luaL_error(L, "pwm: freq must be > 0");
    }
    if (duty < 0 || duty > (int)PWM_CYCLE) {
        return luaL_error(L, "pwm: duty must be 0..%u", PWM_CYCLE);
    }

    TUYA_PWM_BASE_CFG_T cfg = {
        .polarity   = TUYA_PWM_POSITIVE,
        .count_mode = TUYA_PWM_CNT_UP,
        .duty       = (UINT_T)duty,
        .cycle      = PWM_CYCLE,
        .frequency  = (UINT_T)freq,
    };

    if (tkl_pwm_init((TUYA_PWM_NUM_E)ch, &cfg) != OPRT_OK) {
        return luaL_error(L, "pwm: init failed for ch%d", ch);
    }
    if (tkl_pwm_start((TUYA_PWM_NUM_E)ch) != OPRT_OK) {
        return luaL_error(L, "pwm: start failed for ch%d", ch);
    }
    return 0;
}

static int lua_pwm_deinit(lua_State *L)
{
    int ch = (int)luaL_checkinteger(L, 1);
    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range (0-%d)", ch, PWM_CH_MAX - 1);
    }
    /* best-effort: ignore errors so deinit always completes cleanup */
    tkl_pwm_stop((TUYA_PWM_NUM_E)ch);
    tkl_pwm_deinit((TUYA_PWM_NUM_E)ch);
    return 0;
}

static int lua_pwm_set_duty(lua_State *L)
{
    int ch   = (int)luaL_checkinteger(L, 1);
    int duty = (int)luaL_checkinteger(L, 2);
    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range (0-%d)", ch, PWM_CH_MAX - 1);
    }
    if (duty < 0 || duty > (int)PWM_CYCLE) {
        return luaL_error(L, "pwm: duty must be 0..%u", PWM_CYCLE);
    }
    if (tkl_pwm_duty_set((TUYA_PWM_NUM_E)ch, (UINT32_T)duty) != OPRT_OK) {
        return luaL_error(L, "pwm: set_duty failed for ch%d", ch);
    }
    return 0;
}

static int lua_pwm_set_freq(lua_State *L)
{
    int ch   = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range (0-%d)", ch, PWM_CH_MAX - 1);
    }
    if (freq <= 0) {
        return luaL_error(L, "pwm: freq must be > 0");
    }
    if (tkl_pwm_frequency_set((TUYA_PWM_NUM_E)ch, (UINT32_T)freq) != OPRT_OK) {
        return luaL_error(L, "pwm: set_freq failed for ch%d", ch);
    }
    return 0;
}

int luaopen_pwm(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_pwm_init);     lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_pwm_deinit);   lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_pwm_set_duty); lua_setfield(L, -2, "set_duty");
    lua_pushcfunction(L, lua_pwm_set_freq); lua_setfield(L, -2, "set_freq");
    return 1;
}

void lua_module_pwm_register(void)
{
    lua_module_register("pwm", luaopen_pwm);
}

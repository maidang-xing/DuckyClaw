/**
 * @file lua_module_adc.c
 * @brief Lua C module exposing TuyaOpen ADC reads to sandboxed scripts.
 *
 * Lua API (available after lua_module_adc_register()):
 *   local mv  = adc.read_voltage(port, ch)  -- voltage in millivolts
 *   local raw = adc.read_raw(port, ch)      -- raw ADC count
 *
 * Each call inits ADC in single-shot mode, reads one sample, then deinits.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_adc.h"

#include "tkl_adc.h"
#include "lauxlib.h"

#define ADC_PORT_MAX  TUYA_ADC_NUM_MAX
#define ADC_CH_MAX    16  /* T5AI supports up to 16 ADC channels (ch0..ch15) */

static bool __port_valid(int port)
{
    return port >= 0 && port < (int)ADC_PORT_MAX;
}

static bool __ch_valid(int ch)
{
    return ch >= 0 && ch < ADC_CH_MAX;
}

static OPERATE_RET __adc_init_single(int port, int ch)
{
    TUYA_ADC_BASE_CFG_T cfg = {0};
    cfg.ch_list.data = (1u << ch);
    cfg.ch_nums      = 1;
    cfg.width        = 12;
    cfg.freq         = 1000;
    cfg.type         = TUYA_ADC_EXTERNAL_SAMPLE_VOL;
    cfg.mode         = TUYA_ADC_SINGLE;
    cfg.conv_cnt     = 1;
    cfg.ref_vol      = 3300;
    return tkl_adc_init((TUYA_ADC_NUM_E)port, &cfg);
}

static int lua_adc_read_voltage(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    int ch   = (int)luaL_checkinteger(L, 2);

    if (!__port_valid(port)) {
        return luaL_error(L, "adc: port %d out of range (0-%d)", port, ADC_PORT_MAX - 1);
    }
    if (!__ch_valid(ch)) {
        return luaL_error(L, "adc: ch %d out of range (0-%d)", ch, ADC_CH_MAX - 1);
    }

    if (__adc_init_single(port, ch) != OPRT_OK) {
        tkl_adc_deinit((TUYA_ADC_NUM_E)port);
        return luaL_error(L, "adc: init failed (port%d ch%d)", port, ch);
    }

    INT32_T mv = 0;
    if (tkl_adc_read_voltage((TUYA_ADC_NUM_E)port, &mv, 1) != OPRT_OK) {
        tkl_adc_deinit((TUYA_ADC_NUM_E)port);
        return luaL_error(L, "adc: read_voltage failed (port%d ch%d)", port, ch);
    }
    tkl_adc_deinit((TUYA_ADC_NUM_E)port);

    lua_pushinteger(L, (lua_Integer)mv);
    return 1;
}

static int lua_adc_read_raw(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    int ch   = (int)luaL_checkinteger(L, 2);

    if (!__port_valid(port)) {
        return luaL_error(L, "adc: port %d out of range (0-%d)", port, ADC_PORT_MAX - 1);
    }
    if (!__ch_valid(ch)) {
        return luaL_error(L, "adc: ch %d out of range (0-%d)", ch, ADC_CH_MAX - 1);
    }

    if (__adc_init_single(port, ch) != OPRT_OK) {
        tkl_adc_deinit((TUYA_ADC_NUM_E)port);
        return luaL_error(L, "adc: init failed (port%d ch%d)", port, ch);
    }

    INT32_T raw = 0;
    if (tkl_adc_read_data((TUYA_ADC_NUM_E)port, &raw, 1) != OPRT_OK) {
        tkl_adc_deinit((TUYA_ADC_NUM_E)port);
        return luaL_error(L, "adc: read_raw failed (port%d ch%d)", port, ch);
    }
    tkl_adc_deinit((TUYA_ADC_NUM_E)port);

    lua_pushinteger(L, (lua_Integer)raw);
    return 1;
}

int luaopen_adc(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_adc_read_voltage); lua_setfield(L, -2, "read_voltage");
    lua_pushcfunction(L, lua_adc_read_raw);     lua_setfield(L, -2, "read_raw");
    return 1;
}

void lua_module_adc_register(void)
{
    lua_module_register("adc", luaopen_adc);
}

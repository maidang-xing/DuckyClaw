# Lua Module Extension Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the embedded Lua runtime with four new C modules (sys, pwm, i2c, adc) covering system info and common peripherals, plus two new serial CLI commands: `lua_run` to execute Lua inline and `ai_say` to forward text to the AI cloud agent.

**Architecture:** Each new peripheral module follows the exact pattern established by `lua_module_gpio.c` and `lua_module_delay.c`: a single `.c`/`.h` pair under `components/lua/modules/<name>/`, a `luaopen_<name>` entry point, and a `lua_module_<name>_register()` helper that calls `lua_module_register()`. CLI commands are added to `src/app_cli_cmd.c` following the existing `cli_cmd_t` dispatch table pattern. All new modules are guarded by Kconfig flags and CMakeLists conditionals that mirror the existing gpio/delay blocks.

**Tech Stack:** C, Lua 5.5 C API (`lauxlib.h`), TuyaOpen TAL (`tal_api.h`), TKL peripheral drivers (`tkl_pwm.h`, `tkl_i2c.h`, `tkl_adc.h`), AI agent API (`ai_agent.h`), TuyaOpen CLI (`tal_cli.h`).

---

## File Map

| Action | Path | Purpose |
|--------|------|---------|
| Create | `components/lua/modules/sys/lua_module_sys.c` | sys.uptime_ms / random / free_heap / reset_reason |
| Create | `components/lua/modules/sys/lua_module_sys.h` | sys module header |
| Create | `components/lua/modules/pwm/lua_module_pwm.c` | pwm.init / deinit / set_duty / set_freq |
| Create | `components/lua/modules/pwm/lua_module_pwm.h` | pwm module header |
| Create | `components/lua/modules/i2c/lua_module_i2c.c` | i2c.init / deinit / write / read |
| Create | `components/lua/modules/i2c/lua_module_i2c.h` | i2c module header |
| Create | `components/lua/modules/adc/lua_module_adc.c` | adc.read_voltage / read_raw |
| Create | `components/lua/modules/adc/lua_module_adc.h` | adc module header |
| Modify | `components/lua/Kconfig` | Add 4 new CONFIG_ENABLE_LUA_MODULE_* symbols |
| Modify | `components/lua/CMakeLists.txt` | Add 4 conditional source/include blocks |
| Modify | `tools/tools_register.c` | Register 4 new modules under their guards |
| Modify | `src/app_cli_cmd.c` | Add `lua_run` and `ai_say` CLI commands |

---

## Task 1: lua_module_sys — System Info Module

**Files:**
- Create: `components/lua/modules/sys/lua_module_sys.h`
- Create: `components/lua/modules/sys/lua_module_sys.c`

**Lua API exposed:**
- `sys.uptime_ms()` → integer milliseconds since boot
- `sys.random(range)` → integer in [0, range)
- `sys.free_heap()` → integer bytes of free internal heap
- `sys.reset_reason()` → string describing the last reset cause

- [ ] **Step 1: Create the header**

Create `components/lua/modules/sys/lua_module_sys.h`:

```c
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
```

- [ ] **Step 2: Create the implementation**

Create `components/lua/modules/sys/lua_module_sys.c`:

```c
#include "lua_module_sys.h"

#include "tal_api.h"
#include "tal_memory.h"
#include "lauxlib.h"

/* sys.uptime_ms() -> integer */
static int lua_sys_uptime_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)tal_system_get_millisecond());
    return 1;
}

/* sys.random(range) -> integer in [0, range) */
static int lua_sys_random(lua_State *L)
{
    lua_Integer range = luaL_checkinteger(L, 1);
    if (range <= 0) {
        return luaL_error(L, "sys.random: range must be > 0");
    }
    lua_pushinteger(L, (lua_Integer)tal_system_get_random((uint32_t)range));
    return 1;
}

/* sys.free_heap() -> integer (bytes) */
static int lua_sys_free_heap(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)tal_system_get_free_heap_size());
    return 1;
}

/* sys.reset_reason() -> string */
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
```

- [ ] **Step 3: Add Kconfig symbol**

In `components/lua/Kconfig`, add inside `if ENABLE_LUA` (after the DELAY block):

```kconfig
config ENABLE_LUA_MODULE_SYS
    bool "Enable lua_module_sys (system info from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose sys.uptime_ms(), sys.random(range), sys.free_heap(),
        and sys.reset_reason() to Lua scripts via tal_system APIs.
```

- [ ] **Step 4: Add CMakeLists entry**

In `components/lua/CMakeLists.txt`, add after the DELAY block (before `target_sources`):

```cmake
if (CONFIG_ENABLE_LUA_MODULE_SYS STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/sys/lua_module_sys.c
    )
endif()
```

Also append to `target_include_directories`:
```cmake
        ${MODULE_PATH}/modules/sys
```

- [ ] **Step 5: Register in tools_register.c**

In `tools/tools_register.c`, add include at top with the other module includes:
```c
#if defined(ENABLE_LUA_MODULE_SYS) && (ENABLE_LUA_MODULE_SYS == 1)
#include "lua_module_sys.h"
#endif
```

In `__ai_mcp_init()`, after the DELAY registration block:
```c
#if defined(ENABLE_LUA_MODULE_SYS) && (ENABLE_LUA_MODULE_SYS == 1)
lua_module_sys_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/sys/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_sys (uptime_ms/random/free_heap/reset_reason)"
```

---

## Task 2: lua_module_pwm — PWM Control Module

**Files:**
- Create: `components/lua/modules/pwm/lua_module_pwm.h`
- Create: `components/lua/modules/pwm/lua_module_pwm.c`

**Lua API exposed:**
- `pwm.init(ch, freq, duty)` — init + start channel. `duty` is 0–10000 (50% = 5000).
- `pwm.deinit(ch)` — stop + deinit channel.
- `pwm.set_duty(ch, duty)` — update duty while running (0–10000).
- `pwm.set_freq(ch, freq)` — update frequency while running (Hz).

Internals: `TUYA_PWM_BASE_CFG_T` has `frequency` (Hz), `duty` and `cycle` (pulse/cycle, 50% = duty=5000 cycle=10000), `polarity` (TUYA_PWM_POSITIVE), `count_mode` (TUYA_PWM_CNT_UP).

- [ ] **Step 1: Create header**

Create `components/lua/modules/pwm/lua_module_pwm.h`:

```c
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
```

- [ ] **Step 2: Create implementation**

Create `components/lua/modules/pwm/lua_module_pwm.c`:

```c
#include "lua_module_pwm.h"

#include "tkl_pwm.h"
#include "lauxlib.h"

#define PWM_CH_MAX   TUYA_PWM_NUM_MAX
#define PWM_CYCLE    10000u  /* fixed cycle; duty is [0, 10000] */

static bool __ch_valid(int ch)
{
    return ch >= 0 && ch < (int)PWM_CH_MAX;
}

/* pwm.init(ch, freq, duty)  duty in [0, 10000] */
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

/* pwm.deinit(ch) */
static int lua_pwm_deinit(lua_State *L)
{
    int ch = (int)luaL_checkinteger(L, 1);
    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range", ch);
    }
    tkl_pwm_stop((TUYA_PWM_NUM_E)ch);
    tkl_pwm_deinit((TUYA_PWM_NUM_E)ch);
    return 0;
}

/* pwm.set_duty(ch, duty)  duty in [0, 10000] */
static int lua_pwm_set_duty(lua_State *L)
{
    int ch   = (int)luaL_checkinteger(L, 1);
    int duty = (int)luaL_checkinteger(L, 2);
    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range", ch);
    }
    if (duty < 0 || duty > (int)PWM_CYCLE) {
        return luaL_error(L, "pwm: duty must be 0..%u", PWM_CYCLE);
    }
    if (tkl_pwm_duty_set((TUYA_PWM_NUM_E)ch, (UINT32_T)duty) != OPRT_OK) {
        return luaL_error(L, "pwm: set_duty failed for ch%d", ch);
    }
    return 0;
}

/* pwm.set_freq(ch, freq) */
static int lua_pwm_set_freq(lua_State *L)
{
    int ch   = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    if (!__ch_valid(ch)) {
        return luaL_error(L, "pwm: ch %d out of range", ch);
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
```

- [ ] **Step 3: Add Kconfig symbol**

In `components/lua/Kconfig`, inside `if ENABLE_LUA`, after the SYS block:

```kconfig
config ENABLE_LUA_MODULE_PWM
    bool "Enable lua_module_pwm (PWM control from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose pwm.init(ch, freq, duty), pwm.deinit(ch),
        pwm.set_duty(ch, duty), pwm.set_freq(ch, freq) to Lua scripts.
        duty range: 0-10000 (10000 = 100%). Requires tkl_pwm support.
```

- [ ] **Step 4: Add CMakeLists entry**

In `components/lua/CMakeLists.txt`, after the SYS block:

```cmake
if (CONFIG_ENABLE_LUA_MODULE_PWM STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/pwm/lua_module_pwm.c
    )
endif()
```

Add to `target_include_directories`:
```cmake
        ${MODULE_PATH}/modules/pwm
```

- [ ] **Step 5: Register in tools_register.c**

Add include:
```c
#if defined(ENABLE_LUA_MODULE_PWM) && (ENABLE_LUA_MODULE_PWM == 1)
#include "lua_module_pwm.h"
#endif
```

Add registration call in `__ai_mcp_init()`:
```c
#if defined(ENABLE_LUA_MODULE_PWM) && (ENABLE_LUA_MODULE_PWM == 1)
lua_module_pwm_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/pwm/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_pwm (init/deinit/set_duty/set_freq)"
```

---

## Task 3: lua_module_i2c — I2C Master Module

**Files:**
- Create: `components/lua/modules/i2c/lua_module_i2c.h`
- Create: `components/lua/modules/i2c/lua_module_i2c.c`

**Lua API exposed:**
- `i2c.init(port, speed)` — init master. `speed`: 100 (100kHz), 400 (400kHz), 1000 (1MHz).
- `i2c.deinit(port)` — deinit.
- `i2c.write(port, addr, data_str)` — send bytes to device. `data_str` is a Lua string where each byte is a raw character.
- `i2c.read(port, addr, len)` — receive `len` bytes, returns Lua string.

- [ ] **Step 1: Create header**

Create `components/lua/modules/i2c/lua_module_i2c.h`:

```c
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
```

- [ ] **Step 2: Create implementation**

Create `components/lua/modules/i2c/lua_module_i2c.c`:

```c
#include "lua_module_i2c.h"

#include "tkl_i2c.h"
#include "tal_api.h"
#include "lauxlib.h"

#define I2C_PORT_MAX  TUYA_I2C_NUM_MAX
#define I2C_READ_MAX  256u

static bool __port_valid(int port)
{
    return port >= 0 && port < (int)I2C_PORT_MAX;
}

static TUYA_IIC_SPEED_E __parse_speed(int khz)
{
    if (khz >= 1000) return TUYA_IIC_BUS_SPEED_1M;
    if (khz >= 400)  return TUYA_IIC_BUS_SPEED_400K;
    return TUYA_IIC_BUS_SPEED_100K;
}

/* i2c.init(port, speed_khz) */
static int lua_i2c_init(lua_State *L)
{
    int port  = (int)luaL_checkinteger(L, 1);
    int speed = (int)luaL_checkinteger(L, 2);

    if (!__port_valid(port)) {
        return luaL_error(L, "i2c: port %d out of range (0-%d)", port, I2C_PORT_MAX - 1);
    }

    TUYA_IIC_BASE_CFG_T cfg = {
        .role       = TUYA_IIC_MODE_MASTER,
        .speed      = __parse_speed(speed),
        .addr_width = TUYA_IIC_ADDRESS_7BIT,
    };

    if (tkl_i2c_init((TUYA_I2C_NUM_E)port, &cfg) != OPRT_OK) {
        return luaL_error(L, "i2c: init failed for port%d", port);
    }
    return 0;
}

/* i2c.deinit(port) */
static int lua_i2c_deinit(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    if (!__port_valid(port)) {
        return luaL_error(L, "i2c: port %d out of range", port);
    }
    tkl_i2c_deinit((TUYA_I2C_NUM_E)port);
    return 0;
}

/* i2c.write(port, addr, data_str) */
static int lua_i2c_write(lua_State *L)
{
    int         port = (int)luaL_checkinteger(L, 1);
    int         addr = (int)luaL_checkinteger(L, 2);
    size_t      len  = 0;
    const char *data = luaL_checklstring(L, 3, &len);

    if (!__port_valid(port)) {
        return luaL_error(L, "i2c: port %d out of range", port);
    }
    if (len == 0) {
        return luaL_error(L, "i2c: write data is empty");
    }

    if (tkl_i2c_master_send((TUYA_I2C_NUM_E)port, (UINT16_T)addr,
                             (const void *)data, (UINT32_T)len, FALSE) != OPRT_OK) {
        return luaL_error(L, "i2c: write failed (port%d addr=0x%02x)", port, addr);
    }
    return 0;
}

/* i2c.read(port, addr, len) -> string */
static int lua_i2c_read(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    int addr = (int)luaL_checkinteger(L, 2);
    int rlen = (int)luaL_checkinteger(L, 3);

    if (!__port_valid(port)) {
        return luaL_error(L, "i2c: port %d out of range", port);
    }
    if (rlen <= 0 || rlen > (int)I2C_READ_MAX) {
        return luaL_error(L, "i2c: read len must be 1..%u", I2C_READ_MAX);
    }

    char buf[I2C_READ_MAX];
    if (tkl_i2c_master_receive((TUYA_I2C_NUM_E)port, (UINT16_T)addr,
                                buf, (UINT32_T)rlen, FALSE) != OPRT_OK) {
        return luaL_error(L, "i2c: read failed (port%d addr=0x%02x)", port, addr);
    }
    lua_pushlstring(L, buf, (size_t)rlen);
    return 1;
}

int luaopen_i2c(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_i2c_init);   lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_i2c_deinit); lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_i2c_write);  lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_i2c_read);   lua_setfield(L, -2, "read");
    return 1;
}

void lua_module_i2c_register(void)
{
    lua_module_register("i2c", luaopen_i2c);
}
```

- [ ] **Step 3: Add Kconfig symbol**

```kconfig
config ENABLE_LUA_MODULE_I2C
    bool "Enable lua_module_i2c (I2C master from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose i2c.init(port, speed_khz), i2c.deinit(port),
        i2c.write(port, addr, data_str), i2c.read(port, addr, len)
        to Lua scripts. Requires tkl_i2c support (master mode only).
```

- [ ] **Step 4: Add CMakeLists entry**

```cmake
if (CONFIG_ENABLE_LUA_MODULE_I2C STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/i2c/lua_module_i2c.c
    )
endif()
```

Add to `target_include_directories`:
```cmake
        ${MODULE_PATH}/modules/i2c
```

- [ ] **Step 5: Register in tools_register.c**

```c
#if defined(ENABLE_LUA_MODULE_I2C) && (ENABLE_LUA_MODULE_I2C == 1)
#include "lua_module_i2c.h"
#endif
```

```c
#if defined(ENABLE_LUA_MODULE_I2C) && (ENABLE_LUA_MODULE_I2C == 1)
lua_module_i2c_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/i2c/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_i2c (init/deinit/write/read master)"
```

---

## Task 4: lua_module_adc — ADC Read Module

**Files:**
- Create: `components/lua/modules/adc/lua_module_adc.h`
- Create: `components/lua/modules/adc/lua_module_adc.c`

**Lua API exposed:**
- `adc.read_voltage(port, ch)` → integer millivolts. Internally inits ADC in single-shot mode and calls `tkl_adc_read_voltage()`.
- `adc.read_raw(port, ch)` → integer raw ADC value. Calls `tkl_adc_read_data()`.

ADC init pattern: set `ch_list.data = (1u << ch)`, `ch_nums = 1`, `width = 12`, `freq = 1000`, `type = TUYA_ADC_EXTERNAL_SAMPLE_VOL`, `mode = TUYA_ADC_SINGLE`, `conv_cnt = 1`, `ref_vol = 3300`.

- [ ] **Step 1: Create header**

Create `components/lua/modules/adc/lua_module_adc.h`:

```c
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
```

- [ ] **Step 2: Create implementation**

Create `components/lua/modules/adc/lua_module_adc.c`:

```c
#include "lua_module_adc.h"

#include "tkl_adc.h"
#include "lauxlib.h"

#define ADC_PORT_MAX  TUYA_ADC_NUM_MAX
#define ADC_CH_MAX    16

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

/* adc.read_voltage(port, ch) -> millivolts */
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
        return luaL_error(L, "adc: init failed (port%d ch%d)", port, ch);
    }

    INT32_T mv = 0;
    if (tkl_adc_read_voltage((TUYA_ADC_NUM_E)port, &mv, 1) != OPRT_OK) {
        return luaL_error(L, "adc: read_voltage failed (port%d ch%d)", port, ch);
    }
    tkl_adc_deinit((TUYA_ADC_NUM_E)port);

    lua_pushinteger(L, (lua_Integer)mv);
    return 1;
}

/* adc.read_raw(port, ch) -> raw integer */
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
        return luaL_error(L, "adc: init failed (port%d ch%d)", port, ch);
    }

    INT32_T raw = 0;
    if (tkl_adc_read_data((TUYA_ADC_NUM_E)port, &raw, 1) != OPRT_OK) {
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
```

- [ ] **Step 3: Add Kconfig symbol**

```kconfig
config ENABLE_LUA_MODULE_ADC
    bool "Enable lua_module_adc (ADC read from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose adc.read_voltage(port, ch) and adc.read_raw(port, ch)
        to Lua scripts. Each call inits the ADC in single-shot mode,
        reads one sample, then deinits. Requires tkl_adc support.
```

- [ ] **Step 4: Add CMakeLists entry**

```cmake
if (CONFIG_ENABLE_LUA_MODULE_ADC STREQUAL "y")
    list(APPEND LUA_PORT_SRCS
        ${MODULE_PATH}/modules/adc/lua_module_adc.c
    )
endif()
```

Add to `target_include_directories`:
```cmake
        ${MODULE_PATH}/modules/adc
```

- [ ] **Step 5: Register in tools_register.c**

```c
#if defined(ENABLE_LUA_MODULE_ADC) && (ENABLE_LUA_MODULE_ADC == 1)
#include "lua_module_adc.h"
#endif
```

```c
#if defined(ENABLE_LUA_MODULE_ADC) && (ENABLE_LUA_MODULE_ADC == 1)
lua_module_adc_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/adc/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_adc (read_voltage/read_raw)"
```

---

## Task 5: CLI `lua_run` Command

**Files:**
- Modify: `src/app_cli_cmd.c`

Add a `lua_run <script>` command that executes inline Lua. The entire `argv[1]` is passed as the script source. Output is printed via `tal_cli_echo`. Because CLI argv[1] is a single token (no spaces), the pattern matches one-liner scripts or scripts stored on the filesystem and loaded with `dofile`-style patterns. This is adequate for embedded serial testing.

- [ ] **Step 1: Add include guard and header in app_cli_cmd.c**

At the top of `src/app_cli_cmd.c`, after existing includes, add:

```c
#if defined(ENABLE_LUA) && (ENABLE_LUA == 1)
#include "lua_runtime.h"
#endif
```

- [ ] **Step 2: Add forward declaration**

After existing forward declarations (the `static void cmd_cfg_clear_proxy` line), add:

```c
#if defined(ENABLE_LUA) && (ENABLE_LUA == 1)
static void cmd_lua_run(int argc, char *argv[]);
#endif
#if defined(ENABLE_AI_AGENT) && (ENABLE_AI_AGENT == 1)
static void cmd_ai_say(int argc, char *argv[]);
#endif
```

Note: `ENABLE_AI_AGENT` check is added here so Task 6 can reference it in the same location.

- [ ] **Step 3: Add cmd_lua_run implementation**

Add before the command table `s_cli_cmd[]`, inside a feature guard:

```c
#if defined(ENABLE_LUA) && (ENABLE_LUA == 1)
static void cmd_lua_run(int argc, char *argv[])
{
    if (argc < 2) {
        tal_cli_echo("Usage: lua_run <lua_script>");
        tal_cli_echo("  Example: lua_run \"print(sys.uptime_ms())\"");
        return;
    }

    char out[512] = {0};
    OPERATE_RET rt = lua_runtime_run_string(argv[1], 5000, out, sizeof(out));
    tal_cli_echo(out);
    if (rt != OPRT_OK) {
        cli_echof_("lua_run: returned rt=%d", rt);
    }
}
#endif
```

- [ ] **Step 4: Add lua_run to cmd table and help**

In `s_cli_cmd[]`, add at the end before the closing `}`:

```c
#if defined(ENABLE_LUA) && (ENABLE_LUA == 1)
    {.name = "lua_run", .help = "Execute inline Lua script", .func = cmd_lua_run},
#endif
```

In `cmd_help()`, add after the proxy entries:

```c
#if defined(ENABLE_LUA) && (ENABLE_LUA == 1)
    tal_cli_echo("");
    tal_cli_echo("[Lua]");
    cli_echof_("  %-28s %s", "lua_run <script>", "Execute inline Lua 5.5 script");
#endif
```

- [ ] **Step 5: Commit**

```bash
git add src/app_cli_cmd.c
git commit -m "feat(cli): add lua_run command to execute Lua inline from serial CLI"
```

---

## Task 6: CLI `ai_say` Command

**Files:**
- Modify: `src/app_cli_cmd.c`

Add `ai_say <text>` that calls `ai_agent_send_text()` directly. This lets you type a message on the serial terminal and inject it into the AI agent loop as if it came from an IM channel.

- [ ] **Step 1: Add ai_agent.h include**

In `src/app_cli_cmd.c`, after the lua include guard, add:

```c
#if defined(ENABLE_AI_AGENT) && (ENABLE_AI_AGENT == 1)
#include "ai_agent.h"
#endif
```

- [ ] **Step 2: Add cmd_ai_say implementation**

Add after `cmd_lua_run` (still before the command table):

```c
#if defined(ENABLE_AI_AGENT) && (ENABLE_AI_AGENT == 1)
static void cmd_ai_say(int argc, char *argv[])
{
    if (argc < 2) {
        tal_cli_echo("Usage: ai_say <text>");
        tal_cli_echo("  Sends text to the AI agent cloud, same as an IM message.");
        return;
    }

    OPERATE_RET rt = ai_agent_send_text(argv[1]);
    if (rt != OPRT_OK) {
        cli_echof_("ai_say: send failed rt=%d", rt);
    } else {
        cli_echof_("ai_say: sent -> %s", argv[1]);
    }
}
#endif
```

- [ ] **Step 3: Add ai_say to cmd table and help**

In `s_cli_cmd[]`:

```c
#if defined(ENABLE_AI_AGENT) && (ENABLE_AI_AGENT == 1)
    {.name = "ai_say", .help = "Send text to AI agent cloud", .func = cmd_ai_say},
#endif
```

In `cmd_help()`, after the Lua section:

```c
#if defined(ENABLE_AI_AGENT) && (ENABLE_AI_AGENT == 1)
    tal_cli_echo("");
    tal_cli_echo("[AI]");
    cli_echof_("  %-28s %s", "ai_say <text>", "Send text directly to AI agent");
#endif
```

- [ ] **Step 4: Commit**

```bash
git add src/app_cli_cmd.c
git commit -m "feat(cli): add ai_say command to inject text into AI agent from serial CLI"
```

---

## Task 7: Build Verification

**Goal:** Confirm all new code compiles for the Linux target (fastest iteration) and the T5AI board config.

- [ ] **Step 1: Activate TuyaOpen environment**

```bash
cd /home/share/samba/tyopen/DuckyClaw
. ./TuyaOpen/export.sh
```

Expected output ends with: `TuyaOpen environment is ready.`

- [ ] **Step 2: Configure for Linux target**

```bash
cp config/RaspberryPi.config app_default.config
mkdir -p TuyaOpen/.cache && touch TuyaOpen/.cache/.dont_prompt_update_platform
```

- [ ] **Step 3: Build Linux target**

```bash
cd TuyaOpen && python3 tos.py build 2>&1 | tail -30
```

Expected: `[100%] Built target ...` with no errors. Warnings are acceptable.

- [ ] **Step 4: Check for format issues**

```bash
cd /home/share/samba/tyopen/DuckyClaw
python3 TuyaOpen/tools/check_format.py components/lua/modules/sys/ components/lua/modules/pwm/ components/lua/modules/i2c/ components/lua/modules/adc/ 2>&1 | tail -20
```

Expected: no format errors. If there are format issues, run:
```bash
clang-format -i components/lua/modules/sys/lua_module_sys.c components/lua/modules/sys/lua_module_sys.h
clang-format -i components/lua/modules/pwm/lua_module_pwm.c components/lua/modules/pwm/lua_module_pwm.h
clang-format -i components/lua/modules/i2c/lua_module_i2c.c components/lua/modules/i2c/lua_module_i2c.h
clang-format -i components/lua/modules/adc/lua_module_adc.c components/lua/modules/adc/lua_module_adc.h
```

Then commit the reformatted files.

- [ ] **Step 5: Final commit and doc**

```bash
git add -p   # review and stage any remaining changes
git commit -m "build: verify lua module extension compiles on Linux target"
```

Generate documentation:
```bash
# Create doc under doc/2026-05-15/ as per project convention
```

---

## Self-Review

**Spec coverage:**
- [x] System interfaces (`tal_system`) → `lua_module_sys` (uptime_ms, random, free_heap, reset_reason)
- [x] Peripheral interfaces (T5AI tkl_*) → `lua_module_pwm`, `lua_module_i2c`, `lua_module_adc`
- [x] MCP tool via `tools/tool_lua.c` → existing, untouched (modules auto-registered into same runtime)
- [x] CLI `lua_run` via `src/app_cli_cmd.c` → Task 5
- [x] CLI `ai_say` via `ai_agent_send_text` → Task 6
- [x] Kconfig flags for each module → Tasks 1–4 Step 3
- [x] CMakeLists for each module → Tasks 1–4 Step 4
- [x] `tools_register.c` wiring → Tasks 1–4 Step 5
- [ ] IoT interfaces (`tuya_cloud_service`) — spec says "包括但不限于", so this is optional scope. Not included here to keep scope tight and each task independently buildable.

**Placeholder scan:** No TBDs or missing code blocks. All implementations are complete.

**Type consistency:**
- `luaopen_sys/pwm/i2c/adc` match the `lua_CFunction` signature `int fn(lua_State*)` ✓
- `lua_module_*_register()` calls `lua_module_register(name, open_fn)` matching the registry API ✓
- `TUYA_PWM_BASE_CFG_T.duty` and `.cycle` are `UINT_T`; cast from `int` is safe for [0,10000] range ✓
- `tkl_i2c_master_send` last arg `BOOL_T xfer_pending = FALSE` (no repeated-start) ✓
- `TUYA_ADC_BASE_CFG_T.ch_list.data = (1u << ch)` sets the correct bit for single-channel ✓

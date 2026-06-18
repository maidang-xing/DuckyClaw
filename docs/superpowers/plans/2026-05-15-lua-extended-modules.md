# Lua Extended Modules Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add five new Lua C modules (dp, uart, fs, camera, ai) to the DuckyClaw embedded Lua 5.5 runtime, covering cloud DP reporting, UART serial, filesystem I/O, camera snapshot, and AI agent text/image injection.

**Architecture:** Each module follows the existing pattern in `components/lua/modules/` — a `.h`/`.c` pair implementing `luaopen_<name>()` and `lua_module_<name>_register()`, gated by a Kconfig symbol, wired into `CMakeLists.txt` and `tools_register.c`. No existing module structure changes.

**Tech Stack:** TuyaOpen TAL/TKL C APIs, Lua 5.5 C API (lauxlib), claw_malloc/free for PSRAM-aware allocation, existing `lua_module_register()` registry.

---

## File Structure

### New files
| File | Responsibility |
|------|----------------|
| `components/lua/modules/dp/lua_module_dp.h` | Header: `luaopen_dp`, `lua_module_dp_register` |
| `components/lua/modules/dp/lua_module_dp.c` | DP reporting: bool/int/str/enum/raw |
| `components/lua/modules/uart/lua_module_uart.h` | Header: `luaopen_uart`, `lua_module_uart_register` |
| `components/lua/modules/uart/lua_module_uart.c` | UART: init/write/read(timeout)/deinit |
| `components/lua/modules/fs/lua_module_fs.h` | Header: `luaopen_fs`, `lua_module_fs_register` |
| `components/lua/modules/fs/lua_module_fs.c` | FS: read/write/append/exists/remove/mkdir/list/size |
| `components/lua/modules/camera/lua_module_camera.h` | Header: `luaopen_camera`, `lua_module_camera_register` |
| `components/lua/modules/camera/lua_module_camera.c` | Camera: snapshot (→ Lua str), send_to_ai |
| `components/lua/modules/ai/lua_module_ai.h` | Header: `luaopen_ai`, `lua_module_ai_register` |
| `components/lua/modules/ai/lua_module_ai.c` | AI: say(text), send_image(jpeg_bytes) |

### Modified files
| File | Change |
|------|--------|
| `components/lua/Kconfig` | +5 config symbols (one per module) |
| `components/lua/CMakeLists.txt` | +5 conditional source + include-dir blocks |
| `tools/tools_register.c` | +5 conditional includes + registration calls |

---

## Reference: Key Patterns

### Lua module skeleton
```c
/* foo.c */
#include "lua_module_foo.h"
#include "lua.h"
#include "lauxlib.h"
#include "tal_api.h"

static int lua_foo_bar(lua_State *L)
{
    int x = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)x * 2);
    return 1;
}

int luaopen_foo(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_foo_bar); lua_setfield(L, -2, "bar");
    return 1;
}

void lua_module_foo_register(void)
{
    lua_module_register("foo", luaopen_foo);
}
```

### Kconfig entry (append inside `if ENABLE_LUA`)
```kconfig
config ENABLE_LUA_MODULE_FOO
    bool "Enable lua_module_foo"
    depends on ENABLE_LUA
    default n
```

### CMakeLists entry (append before `target_sources`)
```cmake
if (CONFIG_ENABLE_LUA_MODULE_FOO STREQUAL "y")
    list(APPEND LUA_PORT_SRCS ${MODULE_PATH}/modules/foo/lua_module_foo.c)
endif()
```
And add `${MODULE_PATH}/modules/foo` to `target_include_directories`.

### tools_register.c entry
```c
/* top of file */
#if defined(ENABLE_LUA_MODULE_FOO) && (ENABLE_LUA_MODULE_FOO == 1)
#include "lua_module_foo.h"
#endif

/* inside __ai_mcp_init() */
#if defined(ENABLE_LUA_MODULE_FOO) && (ENABLE_LUA_MODULE_FOO == 1)
lua_module_foo_register();
#endif
```

### Key API references
```c
/* Lua arg helpers */
luaL_checkinteger(L, n)          /* integer arg n (1-based), raises error if missing */
luaL_checkstring(L, n)           /* C string arg n */
luaL_checklstring(L, n, &len)    /* binary string arg n + length */
luaL_optinteger(L, n, def)       /* optional integer, default def */
lua_toboolean(L, n)              /* bool arg n (no type check) */

/* Lua return helpers */
lua_pushinteger(L, val)          /* push integer */
lua_pushboolean(L, val)          /* push bool */
lua_pushlstring(L, ptr, len)     /* push binary string */
luaL_error(L, fmt, ...)          /* raise error (longjmp, never returns) */

/* Memory */
claw_malloc(n)                   /* PSRAM-aware alloc */
claw_free(p)                     /* corresponding free */

/* DP reporting */
#include "tuya_iot.h"            /* tuya_iot_client_get() */
#include "tuya_iot_dp.h"         /* tuya_iot_dp_obj_report(), tuya_iot_dp_raw_report() */
#include "dp_schema.h"           /* dp_obj_t, dp_raw_t, PROP_BOOL/VALUE/STR/ENUM/BITMAP */
tuya_iot_client_t *client = tuya_iot_client_get();
/* client->activate.devid is the device ID string */
dp_obj_t dp = {0};
dp.id = dpid; dp.type = PROP_BOOL; dp.value.dp_bool = true;
tuya_iot_dp_obj_report(client, client->activate.devid, &dp, 1, 0);

/* UART (TAL layer) */
#include "tal_uart.h"
TAL_UART_CFG_T cfg = {
    .rx_buffer_size = 1024,
    .open_mode      = O_BLOCK,
    .base_cfg = { .baudrate=115200, .parity=TUYA_UART_PARITY_TYPE_NONE,
                  .databits=TUYA_UART_DATA_LEN_8BIT, .stopbits=TUYA_UART_STOP_LEN_1BIT,
                  .flowctrl=TUYA_UART_FLOWCTRL_NONE },
};
tal_uart_init((TUYA_UART_NUM_E)port, &cfg);
tal_uart_write((TUYA_UART_NUM_E)port, data, len);
tal_uart_read((TUYA_UART_NUM_E)port, buf, len);   /* returns bytes read, 0 if none */
tal_uart_get_rx_data_size((TUYA_UART_NUM_E)port); /* bytes available */
tal_uart_deinit((TUYA_UART_NUM_E)port);

/* Filesystem */
#include "tal_fs.h"
TUYA_FILE f = tal_fopen(path, "r");    /* NULL on failure */
int n = tal_fread(buf, bytes, f);
tal_fwrite(buf, bytes, f);
tal_fclose(f);
int sz = tal_fgetsize(path);           /* -1 on error */
BOOL_T ex = FALSE; tal_fs_is_exist(path, &ex);
tal_fs_remove(path);
tal_fs_mkdir(path);
/* dir listing */
TUYA_DIR dir; tal_dir_open(path, &dir);
TUYA_FILEINFO info;
while (tal_dir_read(dir, &info) == 0) {
    const char *name; tal_dir_name(info, &name);
    BOOL_T is_dir; tal_dir_is_directory(info, &is_dir);
}
tal_dir_close(dir);

/* Camera */
#include "ai_video_input.h"
uint8_t *data = NULL; uint32_t len = 0;
ai_video_get_jpeg_frame(&data, &len);  /* allocates buffer */
/* use data[0..len-1] */
ai_video_jpeg_image_free(&data);       /* frees buffer */

/* AI agent */
#include "ai_agent.h"
ai_agent_send_text(text);              /* text = char* */
ai_agent_send_image(data, len);        /* JPEG bytes */
```

---

## Task 1: lua_module_dp — Cloud DP Reporting

**Files:**
- Create: `components/lua/modules/dp/lua_module_dp.h`
- Create: `components/lua/modules/dp/lua_module_dp.c`
- Modify: `components/lua/Kconfig`
- Modify: `components/lua/CMakeLists.txt`
- Modify: `tools/tools_register.c`

**Lua API exposed:**
```lua
dp.report_bool(dpid, value)      -- value: true/false or 1/0
dp.report_int(dpid, value)       -- integer value
dp.report_str(dpid, value)       -- string value
dp.report_enum(dpid, value)      -- integer enum index
dp.report_raw(dpid, data)        -- binary string (raw bytes)
-- all return true on success, raise error on failure
```

- [ ] **Step 1: Create header file**

Write `components/lua/modules/dp/lua_module_dp.h`:
```c
/**
 * @file lua_module_dp.h
 * @brief Lua module for Tuya cloud DP (data point) reporting.
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef __LUA_MODULE_DP_H__
#define __LUA_MODULE_DP_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_dp(lua_State *L);
void lua_module_dp_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_DP_H__ */
```

- [ ] **Step 2: Create implementation file**

Write `components/lua/modules/dp/lua_module_dp.c`:
```c
/**
 * @file lua_module_dp.c
 * @brief Lua module for Tuya cloud DP (data point) reporting.
 *
 * Exposes dp.report_bool/int/str/enum/raw to Lua scripts.
 * Each function gets a fresh client reference via tuya_iot_client_get()
 * so it is safe to call at any point after cloud activation.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_dp.h"
#include "lua.h"
#include "lauxlib.h"
#include "tal_api.h"

#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "dp_schema.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */
static tuya_iot_client_t *__get_client(lua_State *L)
{
    tuya_iot_client_t *client = tuya_iot_client_get();
    if (!client) {
        luaL_error(L, "dp: cloud client not available (not connected?)");
    }
    return client;
}

static int __do_report(lua_State *L, tuya_iot_client_t *client, dp_obj_t *dp)
{
    int rc = tuya_iot_dp_obj_report(client, client->activate.devid, dp, 1, 0);
    if (rc != OPRT_OK) {
        return luaL_error(L, "dp: report failed (rc=%d)", rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Lua-callable functions
 * --------------------------------------------------------------------------- */
static int lua_dp_report_bool(lua_State *L)
{
    int              dpid   = (int)luaL_checkinteger(L, 1);
    int              val    = lua_toboolean(L, 2);
    tuya_iot_client_t *cli  = __get_client(L);
    dp_obj_t dp = {0};

    dp.id             = (uint8_t)dpid;
    dp.type           = PROP_BOOL;
    dp.value.dp_bool  = (bool)val;
    return __do_report(L, cli, &dp);
}

static int lua_dp_report_int(lua_State *L)
{
    int              dpid   = (int)luaL_checkinteger(L, 1);
    int              val    = (int)luaL_checkinteger(L, 2);
    tuya_iot_client_t *cli  = __get_client(L);
    dp_obj_t dp = {0};

    dp.id              = (uint8_t)dpid;
    dp.type            = PROP_VALUE;
    dp.value.dp_value  = val;
    return __do_report(L, cli, &dp);
}

static int lua_dp_report_str(lua_State *L)
{
    int              dpid   = (int)luaL_checkinteger(L, 1);
    const char      *val    = luaL_checkstring(L, 2);
    tuya_iot_client_t *cli  = __get_client(L);
    dp_obj_t dp = {0};

    dp.id            = (uint8_t)dpid;
    dp.type          = PROP_STR;
    dp.value.dp_str  = (char *)val;  /* synchronous call; pointer valid for duration */
    return __do_report(L, cli, &dp);
}

static int lua_dp_report_enum(lua_State *L)
{
    int              dpid   = (int)luaL_checkinteger(L, 1);
    int              val    = (int)luaL_checkinteger(L, 2);
    tuya_iot_client_t *cli  = __get_client(L);
    dp_obj_t dp = {0};

    dp.id             = (uint8_t)dpid;
    dp.type           = PROP_ENUM;
    dp.value.dp_enum  = (uint32_t)val;
    return __do_report(L, cli, &dp);
}

static int lua_dp_report_raw(lua_State *L)
{
    size_t           data_len = 0;
    int              dpid     = (int)luaL_checkinteger(L, 1);
    const char      *data     = luaL_checklstring(L, 2, &data_len);
    tuya_iot_client_t *cli    = __get_client(L);

    if (data_len == 0) {
        return luaL_error(L, "dp: raw data must be non-empty");
    }
    if (data_len > 0xFFFF) {
        return luaL_error(L, "dp: raw data too long (max 65535 bytes)");
    }

    dp_raw_t *raw = (dp_raw_t *)claw_malloc(sizeof(dp_raw_t) + data_len);
    if (!raw) {
        return luaL_error(L, "dp: out of memory");
    }
    raw->id  = (uint8_t)dpid;
    raw->len = (uint16_t)data_len;
    memcpy(raw->data, data, data_len);

    int rc = tuya_iot_dp_raw_report(cli, cli->activate.devid, raw, 5000);
    claw_free(raw);

    if (rc != OPRT_OK) {
        return luaL_error(L, "dp: raw report failed (rc=%d)", rc);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Module opener
 * --------------------------------------------------------------------------- */
int luaopen_dp(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_dp_report_bool); lua_setfield(L, -2, "report_bool");
    lua_pushcfunction(L, lua_dp_report_int);  lua_setfield(L, -2, "report_int");
    lua_pushcfunction(L, lua_dp_report_str);  lua_setfield(L, -2, "report_str");
    lua_pushcfunction(L, lua_dp_report_enum); lua_setfield(L, -2, "report_enum");
    lua_pushcfunction(L, lua_dp_report_raw);  lua_setfield(L, -2, "report_raw");
    return 1;
}

void lua_module_dp_register(void)
{
    lua_module_registry_include(); /* include guard — registry.h provides this pattern */
    lua_module_register("dp", luaopen_dp);
}
```

> **Note:** Remove the `lua_module_registry_include()` call — it was a slip. The correct body is just `lua_module_register("dp", luaopen_dp);`. See sys/pwm/i2c modules for the exact one-liner pattern.

Corrected `lua_module_dp_register`:
```c
void lua_module_dp_register(void)
{
    lua_module_register("dp", luaopen_dp);
}
```

- [ ] **Step 3: Update Kconfig**

Re-read `components/lua/Kconfig`, then append inside `if ENABLE_LUA` block (after the last `endif # ENABLE_LUA` line — insert before it):
```kconfig
config ENABLE_LUA_MODULE_DP
    bool "Enable lua_module_dp (Tuya cloud DP reporting from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose dp.report_bool(id,v), dp.report_int(id,v),
        dp.report_str(id,v), dp.report_enum(id,v), dp.report_raw(id,data)
        to Lua scripts. Requires an active cloud connection at runtime.
```

- [ ] **Step 4: Update CMakeLists.txt**

Re-read `components/lua/CMakeLists.txt`, then:

Add after the last `endif()` before `target_sources`:
```cmake
if (CONFIG_ENABLE_LUA_MODULE_DP STREQUAL "y")
    list(APPEND LUA_PORT_SRCS ${MODULE_PATH}/modules/dp/lua_module_dp.c)
endif()
```

Add `${MODULE_PATH}/modules/dp` to `target_include_directories`.

- [ ] **Step 5: Update tools_register.c**

Re-read `tools/tools_register.c`, then add:

Near top (after existing `#if defined(ENABLE_LUA_MODULE_ADC)` block):
```c
#if defined(ENABLE_LUA_MODULE_DP) && (ENABLE_LUA_MODULE_DP == 1)
#include "lua_module_dp.h"
#endif
```

Inside `__ai_mcp_init()` (after existing `lua_module_adc_register()` block):
```c
#if defined(ENABLE_LUA_MODULE_DP) && (ENABLE_LUA_MODULE_DP == 1)
lua_module_dp_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/dp/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_dp for cloud DP reporting"
```

---

## Task 2: lua_module_uart — UART Serial Communication

**Files:**
- Create: `components/lua/modules/uart/lua_module_uart.h`
- Create: `components/lua/modules/uart/lua_module_uart.c`
- Modify: `components/lua/Kconfig`
- Modify: `components/lua/CMakeLists.txt`
- Modify: `tools/tools_register.c`

**Lua API exposed:**
```lua
uart.init(port, baud)                     -- 8N1, no flow control, 1024 rx buffer
uart.write(port, data)                    -- data: binary string; returns bytes written
uart.read(port, max_len, timeout_ms)      -- returns string (may be empty on timeout)
uart.deinit(port)
```

- [ ] **Step 1: Create header file**

Write `components/lua/modules/uart/lua_module_uart.h`:
```c
/**
 * @file lua_module_uart.h
 * @brief Lua module for UART serial communication.
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef __LUA_MODULE_UART_H__
#define __LUA_MODULE_UART_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_uart(lua_State *L);
void lua_module_uart_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_UART_H__ */
```

- [ ] **Step 2: Create implementation file**

Write `components/lua/modules/uart/lua_module_uart.c`:
```c
/**
 * @file lua_module_uart.c
 * @brief Lua module for UART serial communication.
 *
 * Exposes uart.init(port, baud), uart.write(port, data),
 * uart.read(port, max_len, timeout_ms), uart.deinit(port) to Lua scripts.
 * Uses the TAL UART layer (buffered, non-DMA, 8N1 defaults).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_uart.h"
#include "lua.h"
#include "lauxlib.h"
#include "tal_api.h"
#include "tal_uart.h"

#define UART_READ_MAX   4096u
#define UART_RX_BUF     1024u
#define UART_READ_POLL_MS 5

static int lua_uart_init(lua_State *L)
{
    int      port = (int)luaL_checkinteger(L, 1);
    int      baud = (int)luaL_checkinteger(L, 2);
    TAL_UART_CFG_T cfg;

    if (baud <= 0) {
        return luaL_error(L, "uart: invalid baudrate %d", baud);
    }

    cfg.rx_buffer_size         = UART_RX_BUF;
    cfg.open_mode              = O_BLOCK;
    cfg.base_cfg.baudrate      = (uint32_t)baud;
    cfg.base_cfg.parity        = TUYA_UART_PARITY_TYPE_NONE;
    cfg.base_cfg.databits      = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.stopbits      = TUYA_UART_STOP_LEN_1BIT;
    cfg.base_cfg.flowctrl      = TUYA_UART_FLOWCTRL_NONE;

    OPERATE_RET rt = tal_uart_init((TUYA_UART_NUM_E)port, &cfg);
    if (rt != OPRT_OK) {
        return luaL_error(L, "uart: init failed port=%d baud=%d (rt=%d)", port, baud, rt);
    }
    return 0;
}

static int lua_uart_write(lua_State *L)
{
    size_t      data_len = 0;
    int         port     = (int)luaL_checkinteger(L, 1);
    const char *data     = luaL_checklstring(L, 2, &data_len);
    int         written;

    if (data_len == 0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    written = tal_uart_write((TUYA_UART_NUM_E)port,
                             (const uint8_t *)data, (uint32_t)data_len);
    lua_pushinteger(L, (lua_Integer)written);
    return 1;
}

static int lua_uart_read(lua_State *L)
{
    int     port       = (int)luaL_checkinteger(L, 1);
    int     max_len    = (int)luaL_checkinteger(L, 2);
    int     timeout_ms = (int)luaL_optinteger(L, 3, 1000);
    char   *buf;
    int     got = 0;
    uint32_t deadline;

    if (max_len <= 0 || max_len > (int)UART_READ_MAX) {
        return luaL_error(L, "uart: max_len must be 1..%u", UART_READ_MAX);
    }

    buf = (char *)claw_malloc((size_t)max_len);
    if (!buf) {
        return luaL_error(L, "uart: out of memory");
    }

    deadline = (uint32_t)tal_system_get_millisecond() + (uint32_t)timeout_ms;
    while (got < max_len) {
        if ((uint32_t)tal_system_get_millisecond() >= deadline) {
            break;
        }
        if (tal_uart_get_rx_data_size((TUYA_UART_NUM_E)port) > 0) {
            int n = tal_uart_read((TUYA_UART_NUM_E)port,
                                  (uint8_t *)(buf + got),
                                  (uint32_t)(max_len - got));
            if (n > 0) {
                got += n;
            }
        } else {
            tal_system_sleep(UART_READ_POLL_MS);
        }
    }

    lua_pushlstring(L, buf, (size_t)got);
    claw_free(buf);
    return 1;
}

static int lua_uart_deinit(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    tal_uart_deinit((TUYA_UART_NUM_E)port);
    return 0;
}

int luaopen_uart(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_uart_init);   lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_uart_write);  lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_uart_read);   lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_uart_deinit); lua_setfield(L, -2, "deinit");
    return 1;
}

void lua_module_uart_register(void)
{
    lua_module_register("uart", luaopen_uart);
}
```

- [ ] **Step 3: Update Kconfig**

Re-read `components/lua/Kconfig`. Append before `endif # ENABLE_LUA`:
```kconfig
config ENABLE_LUA_MODULE_UART
    bool "Enable lua_module_uart (UART serial from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose uart.init(port, baud), uart.write(port, data),
        uart.read(port, max_len, timeout_ms), uart.deinit(port)
        to Lua scripts. 8N1, no flow control, 1024-byte RX buffer.
```

- [ ] **Step 4: Update CMakeLists.txt**

Re-read `components/lua/CMakeLists.txt`. Add after the `ENABLE_LUA_MODULE_DP` block:
```cmake
if (CONFIG_ENABLE_LUA_MODULE_UART STREQUAL "y")
    list(APPEND LUA_PORT_SRCS ${MODULE_PATH}/modules/uart/lua_module_uart.c)
endif()
```
Add `${MODULE_PATH}/modules/uart` to `target_include_directories`.

- [ ] **Step 5: Update tools_register.c**

Re-read `tools/tools_register.c`. Add include guard and registration call following the dp module pattern.

Header include section:
```c
#if defined(ENABLE_LUA_MODULE_UART) && (ENABLE_LUA_MODULE_UART == 1)
#include "lua_module_uart.h"
#endif
```

Registration call in `__ai_mcp_init()`:
```c
#if defined(ENABLE_LUA_MODULE_UART) && (ENABLE_LUA_MODULE_UART == 1)
lua_module_uart_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/uart/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_uart for UART serial communication"
```

---

## Task 3: lua_module_fs — Filesystem Operations

**Files:**
- Create: `components/lua/modules/fs/lua_module_fs.h`
- Create: `components/lua/modules/fs/lua_module_fs.c`
- Modify: `components/lua/Kconfig`
- Modify: `components/lua/CMakeLists.txt`
- Modify: `tools/tools_register.c`

**Lua API exposed:**
```lua
fs.read(path)              -- returns full file content as string
fs.write(path, data)       -- write string to file (overwrites)
fs.append(path, data)      -- append string to file
fs.exists(path)            -- returns true/false
fs.size(path)              -- returns file size in bytes (-1 on error)
fs.remove(path)            -- delete file; raises error on failure
fs.mkdir(path)             -- create directory; raises error on failure
fs.list(path)              -- returns array: {{name="...", is_dir=true/false}, ...}
```

- [ ] **Step 1: Create header file**

Write `components/lua/modules/fs/lua_module_fs.h`:
```c
/**
 * @file lua_module_fs.h
 * @brief Lua module for TAL filesystem operations.
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef __LUA_MODULE_FS_H__
#define __LUA_MODULE_FS_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_fs(lua_State *L);
void lua_module_fs_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_FS_H__ */
```

- [ ] **Step 2: Create implementation file**

Write `components/lua/modules/fs/lua_module_fs.c`:
```c
/**
 * @file lua_module_fs.c
 * @brief Lua module for TAL filesystem operations.
 *
 * Exposes fs.read/write/append/exists/size/remove/mkdir/list to Lua.
 * All operations are path-based (no persistent file handles exposed to Lua).
 * Maximum single-file read size is FS_READ_MAX bytes to bound heap usage.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_fs.h"
#include "lua.h"
#include "lauxlib.h"
#include "tal_api.h"
#include "tal_fs.h"

#include <string.h>

#define FS_READ_MAX (64u * 1024u)  /* 64 KB single-read cap */

static int lua_fs_read(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int         sz   = tal_fgetsize(path);
    char       *buf;
    TUYA_FILE   f;
    int         n;

    if (sz < 0) {
        return luaL_error(L, "fs.read: cannot stat '%s'", path);
    }
    if ((size_t)sz > FS_READ_MAX) {
        return luaL_error(L, "fs.read: file too large (%d bytes, max %u)", sz, FS_READ_MAX);
    }
    if (sz == 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }

    buf = (char *)claw_malloc((size_t)sz + 1);
    if (!buf) {
        return luaL_error(L, "fs.read: out of memory");
    }

    f = tal_fopen(path, "r");
    if (!f) {
        claw_free(buf);
        return luaL_error(L, "fs.read: cannot open '%s'", path);
    }
    n = tal_fread(buf, sz, f);
    tal_fclose(f);

    lua_pushlstring(L, buf, (size_t)(n > 0 ? n : 0));
    claw_free(buf);
    return 1;
}

static int __write_mode(lua_State *L, const char *mode)
{
    size_t      data_len = 0;
    const char *path     = luaL_checkstring(L, 1);
    const char *data     = luaL_checklstring(L, 2, &data_len);
    TUYA_FILE   f        = tal_fopen(path, mode);

    if (!f) {
        return luaL_error(L, "fs.write/append: cannot open '%s'", path);
    }
    if (data_len > 0) {
        tal_fwrite((void *)data, (int)data_len, f);
    }
    tal_fclose(f);
    return 0;
}

static int lua_fs_write(lua_State *L)  { return __write_mode(L, "w"); }
static int lua_fs_append(lua_State *L) { return __write_mode(L, "a"); }

static int lua_fs_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    BOOL_T      ex   = FALSE;
    tal_fs_is_exist(path, &ex);
    lua_pushboolean(L, (int)ex);
    return 1;
}

static int lua_fs_size(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)tal_fgetsize(path));
    return 1;
}

static int lua_fs_remove(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (tal_fs_remove(path) != 0) {
        return luaL_error(L, "fs.remove: failed to remove '%s'", path);
    }
    return 0;
}

static int lua_fs_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    if (tal_fs_mkdir(path) != 0) {
        return luaL_error(L, "fs.mkdir: failed to create '%s'", path);
    }
    return 0;
}

static int lua_fs_list(lua_State *L)
{
    const char   *path  = luaL_checkstring(L, 1);
    TUYA_DIR      dir;
    TUYA_FILEINFO info;
    int           idx   = 1;

    if (tal_dir_open(path, &dir) != 0) {
        return luaL_error(L, "fs.list: cannot open dir '%s'", path);
    }

    lua_newtable(L);  /* result array */
    while (tal_dir_read(dir, &info) == 0) {
        const char *name   = NULL;
        BOOL_T      is_dir = FALSE;

        tal_dir_name(info, &name);
        tal_dir_is_directory(info, &is_dir);

        if (!name || name[0] == '\0') {
            continue;
        }

        lua_newtable(L);                              /* entry table */
        lua_pushstring(L, name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, (int)is_dir);
        lua_setfield(L, -2, "is_dir");
        lua_rawseti(L, -2, idx++);
    }
    tal_dir_close(dir);
    return 1;
}

int luaopen_fs(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_fs_read);   lua_setfield(L, -2, "read");
    lua_pushcfunction(L, lua_fs_write);  lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lua_fs_append); lua_setfield(L, -2, "append");
    lua_pushcfunction(L, lua_fs_exists); lua_setfield(L, -2, "exists");
    lua_pushcfunction(L, lua_fs_size);   lua_setfield(L, -2, "size");
    lua_pushcfunction(L, lua_fs_remove); lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, lua_fs_mkdir);  lua_setfield(L, -2, "mkdir");
    lua_pushcfunction(L, lua_fs_list);   lua_setfield(L, -2, "list");
    return 1;
}

void lua_module_fs_register(void)
{
    lua_module_register("fs", luaopen_fs);
}
```

- [ ] **Step 3: Update Kconfig**

Append before `endif # ENABLE_LUA`:
```kconfig
config ENABLE_LUA_MODULE_FS
    bool "Enable lua_module_fs (filesystem from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose fs.read(path), fs.write(path, data), fs.append(path, data),
        fs.exists(path), fs.size(path), fs.remove(path), fs.mkdir(path),
        fs.list(path) to Lua scripts via the TAL filesystem API.
        Single read is capped at 64 KB.
```

- [ ] **Step 4: Update CMakeLists.txt**

Add after `ENABLE_LUA_MODULE_UART` block:
```cmake
if (CONFIG_ENABLE_LUA_MODULE_FS STREQUAL "y")
    list(APPEND LUA_PORT_SRCS ${MODULE_PATH}/modules/fs/lua_module_fs.c)
endif()
```
Add `${MODULE_PATH}/modules/fs` to `target_include_directories`.

- [ ] **Step 5: Update tools_register.c**

```c
/* include */
#if defined(ENABLE_LUA_MODULE_FS) && (ENABLE_LUA_MODULE_FS == 1)
#include "lua_module_fs.h"
#endif

/* registration */
#if defined(ENABLE_LUA_MODULE_FS) && (ENABLE_LUA_MODULE_FS == 1)
lua_module_fs_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/fs/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_fs for TAL filesystem operations"
```

---

## Task 4: lua_module_camera — Camera Snapshot

**Files:**
- Create: `components/lua/modules/camera/lua_module_camera.h`
- Create: `components/lua/modules/camera/lua_module_camera.c`
- Modify: `components/lua/Kconfig`
- Modify: `components/lua/CMakeLists.txt`
- Modify: `tools/tools_register.c`

**Lua API exposed:**
```lua
camera.snapshot()         -- capture JPEG; returns binary string (JPEG bytes)
camera.send_to_ai()       -- capture JPEG and send via ai_agent_send_image; returns true
```

**Preconditions:** `ENABLE_COMP_AI_VIDEO=y` must be set. The video pipeline (`ai_video_init`) is already started by the application before this module is called.

- [ ] **Step 1: Create header file**

Write `components/lua/modules/camera/lua_module_camera.h`:
```c
/**
 * @file lua_module_camera.h
 * @brief Lua module for camera JPEG snapshot.
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef __LUA_MODULE_CAMERA_H__
#define __LUA_MODULE_CAMERA_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_camera(lua_State *L);
void lua_module_camera_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_CAMERA_H__ */
```

- [ ] **Step 2: Create implementation file**

Write `components/lua/modules/camera/lua_module_camera.c`:
```c
/**
 * @file lua_module_camera.c
 * @brief Lua module for camera JPEG snapshot.
 *
 * Exposes camera.snapshot() -> JPEG string and camera.send_to_ai() -> bool.
 * Delegates to ai_video_get_jpeg_frame() / ai_video_jpeg_image_free().
 * The application is expected to have called ai_video_init() before any
 * Lua script reaches these functions.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_camera.h"
#include "lua.h"
#include "lauxlib.h"
#include "tal_api.h"
#include "ai_video_input.h"
#include "ai_agent.h"

static int lua_camera_snapshot(lua_State *L)
{
    uint8_t  *data = NULL;
    uint32_t  len  = 0;

    OPERATE_RET rt = ai_video_get_jpeg_frame(&data, &len);
    if (rt != OPRT_OK || !data || len == 0) {
        return luaL_error(L, "camera.snapshot: capture failed (rt=%d)", rt);
    }

    lua_pushlstring(L, (const char *)data, (size_t)len);
    ai_video_jpeg_image_free(&data);
    return 1;
}

static int lua_camera_send_to_ai(lua_State *L)
{
    uint8_t  *data = NULL;
    uint32_t  len  = 0;

    OPERATE_RET rt = ai_video_get_jpeg_frame(&data, &len);
    if (rt != OPRT_OK || !data || len == 0) {
        return luaL_error(L, "camera.send_to_ai: capture failed (rt=%d)", rt);
    }

    rt = ai_agent_send_image(data, len);
    ai_video_jpeg_image_free(&data);

    if (rt != OPRT_OK) {
        return luaL_error(L, "camera.send_to_ai: send failed (rt=%d)", rt);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_camera(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_camera_snapshot);   lua_setfield(L, -2, "snapshot");
    lua_pushcfunction(L, lua_camera_send_to_ai); lua_setfield(L, -2, "send_to_ai");
    return 1;
}

void lua_module_camera_register(void)
{
    lua_module_register("camera", luaopen_camera);
}
```

- [ ] **Step 3: Update Kconfig**

Append before `endif # ENABLE_LUA`:
```kconfig
config ENABLE_LUA_MODULE_CAMERA
    bool "Enable lua_module_camera (camera snapshot from Lua)"
    depends on ENABLE_LUA && ENABLE_COMP_AI_VIDEO
    default n
    help
        Expose camera.snapshot() (returns JPEG binary string) and
        camera.send_to_ai() (capture + send via ai_agent_send_image)
        to Lua scripts. Requires ENABLE_COMP_AI_VIDEO and an active
        video pipeline (ai_video_init called by application).
```

- [ ] **Step 4: Update CMakeLists.txt**

Add after `ENABLE_LUA_MODULE_FS` block:
```cmake
if (CONFIG_ENABLE_LUA_MODULE_CAMERA STREQUAL "y")
    list(APPEND LUA_PORT_SRCS ${MODULE_PATH}/modules/camera/lua_module_camera.c)
endif()
```
Add `${MODULE_PATH}/modules/camera` to `target_include_directories`.

- [ ] **Step 5: Update tools_register.c**

```c
/* include */
#if defined(ENABLE_LUA_MODULE_CAMERA) && (ENABLE_LUA_MODULE_CAMERA == 1)
#include "lua_module_camera.h"
#endif

/* registration */
#if defined(ENABLE_LUA_MODULE_CAMERA) && (ENABLE_LUA_MODULE_CAMERA == 1)
lua_module_camera_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/camera/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_camera for JPEG snapshot"
```

---

## Task 5: lua_module_ai — AI Agent Interface

**Files:**
- Create: `components/lua/modules/ai/lua_module_ai.h`
- Create: `components/lua/modules/ai/lua_module_ai.c`
- Modify: `components/lua/Kconfig`
- Modify: `components/lua/CMakeLists.txt`
- Modify: `tools/tools_register.c`

**Lua API exposed:**
```lua
ai.say(text)             -- inject text into AI agent (same as IM message); returns true
ai.send_image(jpeg_data) -- send JPEG binary string to AI agent; returns true
```

- [ ] **Step 1: Create header file**

Write `components/lua/modules/ai/lua_module_ai.h`:
```c
/**
 * @file lua_module_ai.h
 * @brief Lua module for AI agent text and image injection.
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef __LUA_MODULE_AI_H__
#define __LUA_MODULE_AI_H__

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int  luaopen_ai(lua_State *L);
void lua_module_ai_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __LUA_MODULE_AI_H__ */
```

- [ ] **Step 2: Create implementation file**

Write `components/lua/modules/ai/lua_module_ai.c`:
```c
/**
 * @file lua_module_ai.c
 * @brief Lua module for AI agent text and image injection.
 *
 * Exposes ai.say(text) and ai.send_image(jpeg_bytes) to Lua scripts.
 * ai.say() is equivalent to sending an IM message to the agent.
 * ai.send_image() sends raw JPEG bytes — combine with camera.snapshot()
 * for a full capture-and-ask workflow.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "lua_module_ai.h"
#include "lua.h"
#include "lauxlib.h"
#include "tal_api.h"
#include "ai_agent.h"

static int lua_ai_say(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    OPERATE_RET  rt  = ai_agent_send_text((char *)text);
    if (rt != OPRT_OK) {
        return luaL_error(L, "ai.say: send failed (rt=%d)", rt);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ai_send_image(lua_State *L)
{
    size_t       len  = 0;
    const char  *data = luaL_checklstring(L, 1, &len);
    OPERATE_RET  rt;

    if (len == 0) {
        return luaL_error(L, "ai.send_image: empty data");
    }

    rt = ai_agent_send_image((uint8_t *)data, (uint32_t)len);
    if (rt != OPRT_OK) {
        return luaL_error(L, "ai.send_image: send failed (rt=%d)", rt);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int luaopen_ai(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_ai_say);        lua_setfield(L, -2, "say");
    lua_pushcfunction(L, lua_ai_send_image); lua_setfield(L, -2, "send_image");
    return 1;
}

void lua_module_ai_register(void)
{
    lua_module_register("ai", luaopen_ai);
}
```

- [ ] **Step 3: Update Kconfig**

Append before `endif # ENABLE_LUA`:
```kconfig
config ENABLE_LUA_MODULE_AI
    bool "Enable lua_module_ai (AI agent interface from Lua)"
    depends on ENABLE_LUA
    default n
    help
        Expose ai.say(text) and ai.send_image(jpeg_bytes) to Lua scripts.
        ai.say() injects text into the AI agent message bus.
        ai.send_image() sends a JPEG binary payload.
        Requires an active AI agent (ai_agent_init called by application).
```

- [ ] **Step 4: Update CMakeLists.txt**

Add after `ENABLE_LUA_MODULE_CAMERA` block:
```cmake
if (CONFIG_ENABLE_LUA_MODULE_AI STREQUAL "y")
    list(APPEND LUA_PORT_SRCS ${MODULE_PATH}/modules/ai/lua_module_ai.c)
endif()
```
Add `${MODULE_PATH}/modules/ai` to `target_include_directories`.

- [ ] **Step 5: Update tools_register.c**

```c
/* include */
#if defined(ENABLE_LUA_MODULE_AI) && (ENABLE_LUA_MODULE_AI == 1)
#include "lua_module_ai.h"
#endif

/* registration */
#if defined(ENABLE_LUA_MODULE_AI) && (ENABLE_LUA_MODULE_AI == 1)
lua_module_ai_register();
#endif
```

- [ ] **Step 6: Commit**

```bash
git add components/lua/modules/ai/ components/lua/Kconfig components/lua/CMakeLists.txt tools/tools_register.c
git commit -m "feat(lua): add lua_module_ai for AI agent text/image injection"
```

---

## Task 6: Build Verification + Documentation

**Files:**
- Modify: `app_default.config` (enable new modules for verification)
- Create: `doc/2026-05-15/lua_extended_modules.md`

- [ ] **Step 1: Enable new modules in config**

Enable all new modules in `app_default.config` by appending:
```ini
CONFIG_ENABLE_LUA_MODULE_DP=y
CONFIG_ENABLE_LUA_MODULE_UART=y
CONFIG_ENABLE_LUA_MODULE_FS=y
CONFIG_ENABLE_LUA_MODULE_CAMERA=y
CONFIG_ENABLE_LUA_MODULE_AI=y
```

- [ ] **Step 2: Set up build environment**

```bash
. ./TuyaOpen/export.sh
```
Expected output ends with: `tos.py Tool and TuyaOpen SDK is now ready.`

- [ ] **Step 3: Build**

```bash
python3 TuyaOpen/tos.py build 2>&1 | tail -30
```
Expected: `BUILD SUCCESS` with target `DuckyClaw_QIO_1.0.0.bin`.

If build fails with missing symbol errors:
- `tuya_iot_client_get` / `tuya_iot_dp_obj_report`: check `#include "tuya_iot.h"` and `#include "tuya_iot_dp.h"` are present and the include path includes TuyaOpen's cloud service headers.
- `PROP_BOOL` / `dp_obj_t`: check `#include "dp_schema.h"` is present.
- `tal_uart_*`: check `#include "tal_uart.h"` is present.
- `tal_fopen` / `tal_fs_*`: check `#include "tal_fs.h"` is present.
- `ai_video_get_jpeg_frame`: check `#include "ai_video_input.h"` is present and `ENABLE_COMP_AI_VIDEO=y`.
- `ai_agent_send_text`: check `#include "ai_agent.h"` is present.

Fix any error, re-run build until `BUILD SUCCESS`.

- [ ] **Step 4: Flash**

```bash
python3 TuyaOpen/tos.py flash -p /dev/ttyACM0 2>&1 | tail -10
```
Expected: `Flash write success.`

- [ ] **Step 5: Create documentation**

Write `doc/2026-05-15/lua_extended_modules.md` covering all 5 new modules with:
- API table (function, params, return, description)
- Usage examples for each module
- Combined examples (e.g., `camera.snapshot()` → `fs.write()` to save file, or `camera.snapshot()` → `ai.send_image()` for vision query)

- [ ] **Step 6: Commit documentation**

```bash
git add doc/2026-05-15/lua_extended_modules.md app_default.config
git commit -m "docs: add lua extended modules documentation and enable in config"
```

---

## Self-Review

**Spec coverage:**
- DP reporting (bool/int/str/enum/raw) → Task 1 ✓
- UART serial → Task 2 ✓
- Filesystem → Task 3 ✓
- Camera snapshot → Task 4 ✓
- AI text/image injection → Task 5 ✓
- Build + docs → Task 6 ✓

**Type consistency:**
- `lua_module_register("dp", luaopen_dp)` — matches registry API `void lua_module_register(const char *name, lua_CFunction open_fn)` ✓
- `ai_video_jpeg_image_free(&data)` — takes `uint8_t **`, used correctly in both snapshot and send_to_ai ✓
- `tuya_iot_dp_raw_report(cli, cli->activate.devid, raw, 5000)` — timeout param is uint32_t, matches header ✓
- `dp_raw_t->data` is a flexible array `uint8_t data[0]`, allocated with `sizeof(dp_raw_t) + data_len` ✓
- `tal_uart_read` returns `int` (bytes read); stored into `int n` ✓

**Placeholder scan:** No TBD, no "add validation later", all code blocks are complete. ✓

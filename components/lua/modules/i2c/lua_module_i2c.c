#include "lua_module_i2c.h"

#include "tkl_i2c.h"
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

static int lua_i2c_deinit(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    if (!__port_valid(port)) {
        return luaL_error(L, "i2c: port %d out of range", port);
    }
    tkl_i2c_deinit((TUYA_I2C_NUM_E)port);
    return 0;
}

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

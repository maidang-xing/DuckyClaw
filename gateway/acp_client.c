/**
 * @file acp_client.c
 * @brief ACP (Agent Client Protocol) WebSocket client implementation.
 *
 * Implements a WebSocket client that speaks the OpenClaw ACP protocol
 * (@agentclientprotocol/sdk) to connect directly to an openclaw gateway
 * over a local-area-network WebSocket connection:
 *
 *   Phase 1 – TCP connect to ACP_GATEWAY_HOST:ACP_GATEWAY_PORT
 *   Phase 2 – RFC 6455 HTTP upgrade handshake
 *   Phase 3 – ACP "connect" request with client descriptor and auth token;
 *              wait for "hello-ok" event to extract sessionKey
 *   Phase 4 – Steady-state: send chat.inject requests, receive chat events
 *              (delta / final), respond to tick heartbeats
 *
 * ACP frame types (JSON-over-WebSocket RPC):
 *   Request  : {"type":"req",   "id":"N",  "method":"...", "params":{...}}
 *   Response : {"type":"res",   "id":"N",  "ok":true/false, "payload":{...}}
 *   Event    : {"type":"event", "event":"...", "payload":{...}, "seq":N}
 *
 * Per RFC 6455 §5.1, frames sent from a client to a server MUST be masked.
 * This file uses a lightweight 4-byte masking key derived from the system
 * tick counter and a monotonic counter; cryptographic strength is not
 * required for the WS masking key.
 *
 * @version 2.0
 * @date 2026-03-19
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "acp_client.h"
#include "tuya_app_config.h"

#include "cJSON.h"
#include "mix_method.h"
#include "tal_hash.h"
#include "tal_log.h"
#include "tal_api.h"
#include "tal_semaphore.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

#define ACP_WS_GUID           "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define ACP_WS_OPCODE_TEXT    0x01
#define ACP_WS_OPCODE_CLOSE   0x08
#define ACP_WS_OPCODE_PING    0x09
#define ACP_WS_OPCODE_PONG    0x0A

/* ACP protocol version negotiation range – gateway requires exactly v3 */
#define ACP_PROTO_MIN         3
#define ACP_PROTO_MAX         3

/* Request IDs for synchronous requests */
#define ACP_CONNECT_REQ_ID    "1"

/* Maximum ms to wait for a single recv before looping */
#define ACP_SELECT_TIMEOUT_MS 200
/* Maximum ms to wait for a synchronous connect/upgrade response */
#define ACP_SYNC_RECV_TIMEOUT_MS  10000

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

typedef enum {
    ACP_STATE_DISCONNECTED = 0,
    ACP_STATE_CONNECTED,
} acp_state_e;

typedef struct {
    THREAD_HANDLE  thread;
    MUTEX_HANDLE   tx_mutex;

    int            fd;
    acp_state_e    state;

    /* sessionKey obtained from hello-ok event; used in chat.inject params */
    char           session_key[128];
    UINT32_T       req_id_counter;

    uint8_t        rx_buf[ACP_CLIENT_RX_BUF_SIZE];
    size_t         rx_len;

    /* Accumulates streaming chat content until done=true / state=final */
    char           reply_buf[ACP_CLIENT_REPLY_BUF_SIZE];
    size_t         reply_len;

    acp_reply_cb_t reply_cb;
    VOID_T        *reply_cb_data;

    /* Synchronous send-and-wait support (for MCP tool callers) */
    MUTEX_HANDLE   sync_mutex;
    SEM_HANDLE     sync_sem;
    BOOL_T         sync_waiting;
    char           sync_reply_buf[ACP_CLIENT_REPLY_BUF_SIZE];
    size_t         sync_reply_len;

    BOOL_T         stop_requested;
} acp_ctx_t;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */

STATIC acp_ctx_t s_ctx;

/* ---------------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------------- */

STATIC VOID_T acp_client_task(VOID_T *arg);

STATIC OPERATE_RET __ws_decode_frame(CONST uint8_t *rx_buf, size_t rx_len,
                                     uint8_t *opcode,
                                     CONST uint8_t **payload,
                                     size_t *payload_len,
                                     size_t *consumed);

/* ---------------------------------------------------------------------------
 * Internal helpers – raw TCP I/O
 * --------------------------------------------------------------------------- */

/**
 * @brief Send all bytes in buf to fd, retrying on EAGAIN.
 * @param[in] fd   Socket file descriptor.
 * @param[in] buf  Data buffer.
 * @param[in] len  Number of bytes to send.
 * @return OPRT_OK on success, OPRT_SEND_ERR on failure.
 */
STATIC OPERATE_RET __send_all(INT_T fd, CONST uint8_t *buf, size_t len)
{
    if (fd < 0 || (!buf && len > 0)) {
        return OPRT_INVALID_PARM;
    }
    size_t sent = 0;
    while (sent < len) {
        INT_T n = tal_net_send(fd, buf + sent, (UINT32_T)(len - sent));
        if (n == OPRT_RESOURCE_NOT_READY) {
            tal_system_sleep(5);
            continue;
        }
        if (n <= 0) {
            return OPRT_SEND_ERR;
        }
        sent += (size_t)n;
    }
    return OPRT_OK;
}

/**
 * @brief Recv bytes from fd into buf, retrying until len bytes or timeout.
 * @param[in]  fd          Socket file descriptor.
 * @param[out] buf         Output buffer.
 * @param[in]  len         Maximum bytes to receive.
 * @param[in]  timeout_ms  Maximum wait in milliseconds.
 * @return Number of bytes received, or 0 on timeout, negative on error.
 */
STATIC INT_T __recv_timeout(INT_T fd, uint8_t *buf, size_t len, UINT32_T timeout_ms)
{
    if (fd < 0 || !buf || len == 0) {
        return -1;
    }
    UINT32_T elapsed = 0;
    while (elapsed < timeout_ms) {
        TUYA_FD_SET_T rfds;
        TAL_FD_ZERO(&rfds);
        TAL_FD_SET(fd, &rfds);
        INT_T ready = tal_net_select(fd + 1, &rfds, NULL, NULL, 50);
        if (ready > 0 && TAL_FD_ISSET(fd, &rfds)) {
            INT_T n = tal_net_recv(fd, buf, (UINT32_T)len);
            if (n > 0) {
                return n;
            }
            if (n == OPRT_RESOURCE_NOT_READY) {
                /* no data this poll cycle */
            } else {
                return -1;
            }
        }
        elapsed += 50;
    }
    return 0; /* timeout */
}

/* ---------------------------------------------------------------------------
 * Internal helpers – WebSocket masked frame send
 * --------------------------------------------------------------------------- */

/**
 * @brief Generate a 4-byte WS masking key.
 *
 * The masking key is used to comply with RFC 6455 §5.3.  It does not need
 * to be cryptographically strong; we use the system tick XOR'd with a
 * monotonic counter to avoid repeating a fixed key.
 *
 * @param[out] mask_key  4-byte output buffer.
 * @return none
 */
STATIC VOID_T __gen_mask_key(uint8_t mask_key[4])
{
    STATIC UINT32_T s_counter = 0;
    UINT32_T tick = tal_system_get_millisecond();
    UINT32_T val  = tick ^ (++s_counter * 0x9e3779b9u);
    mask_key[0] = (uint8_t)((val >> 24) & 0xFF);
    mask_key[1] = (uint8_t)((val >> 16) & 0xFF);
    mask_key[2] = (uint8_t)((val >>  8) & 0xFF);
    mask_key[3] = (uint8_t)( val        & 0xFF);
}

/**
 * @brief Build and send a masked RFC 6455 WebSocket text frame.
 *
 * Per RFC 6455 §5.1, all frames sent from a client to a server MUST be masked.
 * The masked payload is applied in-place on a heap-allocated copy so the
 * caller's buffer is not modified.
 *
 * @param[in] fd          Socket file descriptor.
 * @param[in] opcode      WS opcode (use ACP_WS_OPCODE_TEXT for JSON messages).
 * @param[in] payload     Payload bytes.
 * @param[in] payload_len Payload length in bytes.
 * @return OPRT_OK on success.
 */
STATIC OPERATE_RET __ws_send_masked(INT_T fd, uint8_t opcode,
                                    CONST uint8_t *payload, size_t payload_len)
{
    /* Header: up to 2 + 8 length bytes + 4 mask bytes = 14 bytes */
    uint8_t header[14] = {0};
    size_t  hdr_len    = 0;
    uint8_t mask_key[4];

    __gen_mask_key(mask_key);

    header[0] = (uint8_t)(0x80 | (opcode & 0x0F)); /* FIN=1 */

    /* Byte 1: MASK bit set + payload length */
    if (payload_len <= 125) {
        header[1] = (uint8_t)(0x80 | payload_len);
        hdr_len   = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1] = (uint8_t)(0x80 | 126);
        header[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3] = (uint8_t)( payload_len       & 0xFF);
        hdr_len   = 4;
    } else {
        header[1] = (uint8_t)(0x80 | 127);
        UINT64_T plen64 = (UINT64_T)payload_len;
        for (INT_T i = 0; i < 8; i++) {
            header[2 + i] = (uint8_t)((plen64 >> (56 - i * 8)) & 0xFF);
        }
        hdr_len = 10;
    }

    /* Append 4-byte masking key */
    memcpy(header + hdr_len, mask_key, 4);
    hdr_len += 4;

    OPERATE_RET rt = __send_all(fd, header, hdr_len);
    if (rt != OPRT_OK) {
        return rt;
    }

    if (payload_len == 0) {
        return OPRT_OK;
    }

    /* Allocate a copy to apply the mask without modifying caller's buffer */
    uint8_t *masked = (uint8_t *)tal_malloc(payload_len);
    if (!masked) {
        return OPRT_MALLOC_FAILED;
    }
    for (size_t i = 0; i < payload_len; i++) {
        masked[i] = (uint8_t)(payload[i] ^ mask_key[i % 4]);
    }

    rt = __send_all(fd, masked, payload_len);
    tal_free(masked);
    return rt;
}

/**
 * @brief Send a masked WebSocket text frame containing a JSON string.
 * @param[in] fd   Socket file descriptor.
 * @param[in] json NUL-terminated JSON string.
 * @return OPRT_OK on success.
 */
STATIC OPERATE_RET __ws_send_json(INT_T fd, CONST CHAR_T *json)
{
    if (!json || json[0] == '\0') {
        return OPRT_INVALID_PARM;
    }
    return __ws_send_masked(fd, ACP_WS_OPCODE_TEXT,
                            (CONST uint8_t *)json, strlen(json));
}

/* ---------------------------------------------------------------------------
 * Internal helpers – WebSocket client handshake
 * --------------------------------------------------------------------------- */

/**
 * @brief Build the Base64-encoded Sec-WebSocket-Key from 16 random bytes.
 * @param[out] key_out  Output buffer (at least 32 bytes).
 * @param[in]  out_size Size of key_out.
 * @return OPRT_OK on success.
 */
STATIC OPERATE_RET __ws_build_client_key(CHAR_T *key_out, size_t out_size)
{
    STATIC UINT32_T s_seed = 0;
    uint8_t raw[16] = {0};
    UINT32_T tick   = tal_system_get_tick_count();
    for (INT_T i = 0; i < 4; i++) {
        UINT32_T v = tick ^ (++s_seed * 0x6c62272eu);
        raw[i * 4 + 0] = (uint8_t)((v >> 24) & 0xFF);
        raw[i * 4 + 1] = (uint8_t)((v >> 16) & 0xFF);
        raw[i * 4 + 2] = (uint8_t)((v >>  8) & 0xFF);
        raw[i * 4 + 3] = (uint8_t)( v        & 0xFF);
        tick ^= v;
    }
    if (!tuya_base64_encode(raw, key_out, sizeof(raw))) {
        return OPRT_COM_ERROR;
    }
    (void)out_size;
    return OPRT_OK;
}

/**
 * @brief Perform the RFC 6455 HTTP upgrade handshake.
 *
 * Sends the upgrade request and validates the "101 Switching Protocols"
 * response from the server.
 *
 * @param[in] fd    Connected TCP socket.
 * @param[in] host  Host header value.
 * @param[in] port  Port (for Host header).
 * @return OPRT_OK if the server accepted the upgrade.
 */
STATIC OPERATE_RET __ws_upgrade(INT_T fd, CONST CHAR_T *host, UINT16_T port)
{
    CHAR_T client_key[32] = {0};
    OPERATE_RET rt = __ws_build_client_key(client_key, sizeof(client_key));
    if (rt != OPRT_OK) {
        return rt;
    }

    CHAR_T req[512] = {0};
    INT_T  n = snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Origin: http://%s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        host, (unsigned)port, host, (unsigned)port, client_key);
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        return OPRT_BUFFER_NOT_ENOUGH;
    }

    rt = __send_all(fd, (CONST uint8_t *)req, (size_t)n);
    if (rt != OPRT_OK) {
        PR_ERR("acp upgrade send failed rt=%d", rt);
        return rt;
    }

    /* Read the server HTTP response (look for "101") */
    uint8_t resp[512] = {0};
    INT_T   got = __recv_timeout(fd, resp, sizeof(resp) - 1, ACP_SYNC_RECV_TIMEOUT_MS);
    if (got <= 0) {
        PR_ERR("acp upgrade recv timeout/err got=%d", got);
        return OPRT_RECV_ERR;
    }
    resp[got] = '\0';

    if (!strstr((CONST CHAR_T *)resp, "101")) {
        PR_ERR("acp upgrade rejected: %.80s", (CONST CHAR_T *)resp);
        return OPRT_COM_ERROR;
    }

    PR_INFO("acp ws upgrade ok host=%s:%u", host, (unsigned)port);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Internal helpers – ACP protocol (connect + hello-ok)
 * --------------------------------------------------------------------------- */

/**
 * @brief Extract the mainSessionKey from a parsed ACP hello-ok payload.
 *
 * The hello-ok data lives inside res.payload (v3 protocol).
 * Tries res.payload.snapshot.sessionDefaults.mainSessionKey first,
 * falls back to "agent:main" if not found.
 *
 * @param[in]  hello_payload  cJSON object for the hello-ok payload.
 * @param[out] session_key    Output buffer.
 * @param[in]  sk_size        Buffer size.
 * @return none
 */
STATIC VOID_T __extract_session_key(cJSON *hello_payload, CHAR_T *session_key, size_t sk_size)
{
    cJSON *snapshot = cJSON_GetObjectItem(hello_payload, "snapshot");
    cJSON *sess_def = cJSON_GetObjectItem(snapshot, "sessionDefaults");
    cJSON *main_key = cJSON_GetObjectItem(sess_def, "mainSessionKey");

    if (cJSON_IsString(main_key) && main_key->valuestring &&
        main_key->valuestring[0] != '\0') {
        snprintf(session_key, sk_size, "%s", main_key->valuestring);
        PR_INFO("acp sessionKey=%s", session_key);
    } else {
        PR_WARN("acp hello-ok missing mainSessionKey, using default");
        snprintf(session_key, sk_size, "agent:main");
    }
}

/**
 * @brief Send the ACP "connect" request and wait for the hello-ok response.
 *
 * ACP v3 protocol flow:
 *   1. Gateway immediately sends a "connect.challenge" event (token auth:
 *      no client response needed – token is in connect req params.auth).
 *   2. Client sends the connect request.
 *   3. Gateway replies with a single "res" frame:
 *        {"type":"res","id":"1","ok":true,"payload":{"type":"hello-ok",
 *         "snapshot":{"sessionDefaults":{"mainSessionKey":"agent:main",...}}}}
 *      The hello-ok data is embedded in res.payload, NOT a separate event.
 *
 * Because the hello-ok payload is large (sessions list, capabilities, etc.)
 * it typically spans multiple TCP segments.  This function accumulates raw
 * bytes and uses __ws_decode_frame to wait for a complete WS frame before
 * parsing.
 *
 * @param[in]  fd          Connected, upgraded WebSocket socket.
 * @param[out] session_key Output buffer for the session key.
 * @param[in]  sk_size     Size of session_key buffer.
 * @return OPRT_OK on success.
 */
STATIC OPERATE_RET __acp_connect(INT_T fd, CHAR_T *session_key, size_t sk_size)
{
    /* Build connect request */
    cJSON *root   = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *client = cJSON_CreateObject();
    cJSON *auth   = cJSON_CreateObject();
    if (!root || !params || !client || !auth) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        cJSON_Delete(client);
        cJSON_Delete(auth);
        return OPRT_MALLOC_FAILED;
    }

    cJSON_AddStringToObject(root,   "type",   "req");
    cJSON_AddStringToObject(root,   "id",     ACP_CONNECT_REQ_ID);
    cJSON_AddStringToObject(root,   "method", "connect");

    cJSON_AddNumberToObject(params, "minProtocol", ACP_PROTO_MIN);
    cJSON_AddNumberToObject(params, "maxProtocol", ACP_PROTO_MAX);

    cJSON_AddStringToObject(client, "id",           "openclaw-control-ui");
    cJSON_AddStringToObject(client, "mode",         "ui");
    cJSON_AddStringToObject(client, "version",      "1.0.0");
    cJSON_AddStringToObject(client, "platform",     "embedded");
    cJSON_AddStringToObject(client, "deviceFamily", "DuckyClaw");
    cJSON_AddStringToObject(client, "displayName",  ACP_DEVICE_ID);
    cJSON_AddItemToObject(params, "client", client);

    cJSON_AddStringToObject(auth, "token", ACP_GATEWAY_TOKEN);
    cJSON_AddItemToObject(params, "auth", auth);

    cJSON_AddStringToObject(params, "role", "operator");
    cJSON *scopes = cJSON_CreateArray();
    if (scopes) {
        cJSON_AddItemToArray(scopes, cJSON_CreateString("operator.admin"));
        cJSON_AddItemToObject(params, "scopes", scopes);
    }

    cJSON_AddItemToObject(root, "params", params);

    CHAR_T *req_payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req_payload) {
        return OPRT_MALLOC_FAILED;
    }

    PR_DEBUG("acp connect req: %.128s", req_payload);
    OPERATE_RET rt = __ws_send_json(fd, req_payload);
    cJSON_free(req_payload);
    if (rt != OPRT_OK) {
        PR_ERR("acp connect send failed rt=%d", rt);
        return rt;
    }

    /*
     * Accumulate incoming bytes until __ws_decode_frame signals a complete
     * WS frame is available, then parse the JSON payload.
     * Use a heap buffer (16 KB) to handle the large hello-ok response.
     */
    const size_t RX_CAP = 16 * 1024;
    uint8_t *rx_buf = (uint8_t *)tal_malloc(RX_CAP);
    if (!rx_buf) {
        PR_ERR("acp connect: alloc rx_buf failed");
        return OPRT_MALLOC_FAILED;
    }
    size_t   rx_len = 0;
    UINT32_T elapsed = 0;
    rt = OPRT_RECV_ERR; /* assume timeout unless we succeed */

    while (elapsed < ACP_SYNC_RECV_TIMEOUT_MS) {
        /* Receive available bytes into the accumulation buffer */
        if (rx_len < RX_CAP) {
            INT_T got = __recv_timeout(fd, rx_buf + rx_len, RX_CAP - rx_len, 200);
            if (got < 0) {
                PR_ERR("acp connect recv error");
                rt = OPRT_RECV_ERR;
                break;
            }
            if (got > 0) {
                rx_len += (size_t)got;
            }
        }
        elapsed += 200;

        /* Try to decode complete WS frames from the accumulated data */
        while (rx_len > 0) {
            uint8_t        opcode      = 0;
            CONST uint8_t *ws_payload  = NULL;
            size_t         ws_plen     = 0;
            size_t         consumed    = 0;

            OPERATE_RET dec = __ws_decode_frame(rx_buf, rx_len,
                                                &opcode, &ws_payload,
                                                &ws_plen, &consumed);
            if (dec == OPRT_RESOURCE_NOT_READY) {
                break; /* need more data */
            }
            if (dec != OPRT_OK || consumed == 0) {
                PR_ERR("acp connect frame decode err rt=%d", dec);
                rt = OPRT_COM_ERROR;
                goto done;
            }

            /*
             * Copy payload BEFORE consuming (memmove invalidates ws_payload).
             * Only allocate for text frames that need JSON parsing.
             */
            CHAR_T *tmp = NULL;
            if (opcode == ACP_WS_OPCODE_TEXT && ws_payload && ws_plen > 0) {
                tmp = (CHAR_T *)tal_malloc(ws_plen + 1);
                if (!tmp) {
                    rt = OPRT_MALLOC_FAILED;
                    goto done;
                }
                memcpy(tmp, ws_payload, ws_plen);
                tmp[ws_plen] = '\0';
            }

            /* Consume the frame bytes (invalidates ws_payload) */
            if (consumed < rx_len) {
                memmove(rx_buf, rx_buf + consumed, rx_len - consumed);
            }
            rx_len -= consumed;

            /* Skip non-text frames */
            if (!tmp) {
                continue;
            }

            PR_DEBUG("acp connect frame: %.128s", tmp);

            cJSON *resp = cJSON_Parse(tmp);
            tal_free(tmp);

            if (!resp) {
                continue; /* malformed frame, keep waiting */
            }

            cJSON *type_field = cJSON_GetObjectItem(resp, "type");
            if (!cJSON_IsString(type_field)) {
                cJSON_Delete(resp);
                continue;
            }

            /* event frame (connect.challenge before req, or other events) */
            if (strcmp(type_field->valuestring, "event") == 0) {
                cJSON *event_name = cJSON_GetObjectItem(resp, "event");
                if (cJSON_IsString(event_name) &&
                    strcmp(event_name->valuestring, "connect.challenge") == 0) {
                    cJSON *nonce = cJSON_GetObjectItem(
                        cJSON_GetObjectItem(resp, "payload"), "nonce");
                    PR_DEBUG("acp connect.challenge nonce=%s (token auth: no response needed)",
                             cJSON_IsString(nonce) ? nonce->valuestring : "?");
                }
                cJSON_Delete(resp);
                continue;
            }

            /* res frame – the only expected response to our connect req */
            if (strcmp(type_field->valuestring, "res") == 0) {
                cJSON *ok_field = cJSON_GetObjectItem(resp, "ok");
                if (!cJSON_IsBool(ok_field) || !cJSON_IsTrue(ok_field)) {
                    cJSON *err_obj = cJSON_GetObjectItem(resp, "error");
                    cJSON *err_code = err_obj ? cJSON_GetObjectItem(err_obj, "code") : NULL;
                    cJSON *err_msg  = err_obj ? cJSON_GetObjectItem(err_obj, "message") : NULL;
                    PR_ERR("acp connect res ok=false code=%s msg=%.200s",
                           cJSON_IsString(err_code) ? err_code->valuestring : "?",
                           cJSON_IsString(err_msg)  ? err_msg->valuestring  : "?");
                    cJSON_Delete(resp);
                    rt = OPRT_COM_ERROR;
                    goto done;
                }

                /*
                 * ACP v3: hello-ok data is in res.payload (not a separate
                 * event).  Extract sessionKey from
                 * res.payload.snapshot.sessionDefaults.mainSessionKey.
                 */
                cJSON *res_payload = cJSON_GetObjectItem(resp, "payload");
                __extract_session_key(res_payload, session_key, sk_size);
                cJSON_Delete(resp);
                rt = OPRT_OK;
                goto done;
            }

            cJSON_Delete(resp);
        }
    }

done:
    tal_free(rx_buf);
    if (rt == OPRT_RECV_ERR) {
        PR_ERR("acp connect timeout waiting for hello-ok");
    }
    return rt;
}

/* ---------------------------------------------------------------------------
 * Internal helpers – WS frame decode in the RX loop
 * --------------------------------------------------------------------------- */

/**
 * @brief Decode one WS frame from the head of rx_buf.
 *
 * Unmasked frames (server→client) are returned as-is.  The payload is
 * a pointer into rx_buf and is valid until the next call to this function.
 *
 * @param[in]  rx_buf      Input buffer.
 * @param[in]  rx_len      Bytes available in rx_buf.
 * @param[out] opcode      WS opcode byte.
 * @param[out] payload     Pointer to (unmasked) payload in rx_buf.
 * @param[out] payload_len Payload length in bytes.
 * @param[out] consumed    Total frame bytes consumed (header + payload).
 * @return OPRT_OK          Frame decoded successfully.
 * @return OPRT_RESOURCE_NOT_READY  Not enough data yet (partial frame).
 */
STATIC OPERATE_RET __ws_decode_frame(CONST uint8_t *rx_buf, size_t rx_len,
                                     uint8_t *opcode,
                                     CONST uint8_t **payload,
                                     size_t *payload_len,
                                     size_t *consumed)
{
    if (!rx_buf || !opcode || !payload || !payload_len || !consumed) {
        return OPRT_INVALID_PARM;
    }
    if (rx_len < 2) {
        return OPRT_RESOURCE_NOT_READY;
    }

    uint8_t  op     = (uint8_t)(rx_buf[0] & 0x0F);
    BOOL_T   masked = (rx_buf[1] & 0x80) != 0;
    uint64_t plen   = (uint64_t)(rx_buf[1] & 0x7F);
    size_t   off    = 2;

    if (plen == 126) {
        if (rx_len < off + 2) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = (uint64_t)((rx_buf[off] << 8) | rx_buf[off + 1]);
        off += 2;
    } else if (plen == 127) {
        if (rx_len < off + 8) {
            return OPRT_RESOURCE_NOT_READY;
        }
        plen = 0;
        for (INT_T i = 0; i < 8; i++) {
            plen = (plen << 8) | rx_buf[off + i];
        }
        off += 8;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (rx_len < off + 4) {
            return OPRT_RESOURCE_NOT_READY;
        }
        memcpy(mask, rx_buf + off, 4);
        off += 4;
    }

    if (plen > (uint64_t)(ACP_CLIENT_RX_BUF_SIZE - off)) {
        PR_WARN("acp rx frame too large plen=%llu, dropping", (unsigned long long)plen);
        *consumed    = off + (size_t)plen;
        *payload_len = 0;
        *opcode      = op;
        *payload     = NULL;
        return OPRT_OK;
    }

    size_t frame_len = off + (size_t)plen;
    if (rx_len < frame_len) {
        return OPRT_RESOURCE_NOT_READY;
    }

    /* Unmask in-place (server should never send masked, but handle it anyway) */
    if (masked && plen > 0) {
        uint8_t *data = (uint8_t *)(rx_buf + off);
        for (size_t i = 0; i < (size_t)plen; i++) {
            data[i] ^= mask[i % 4];
        }
    }

    *opcode      = op;
    *payload     = rx_buf + off;
    *payload_len = (size_t)plen;
    *consumed    = frame_len;
    return OPRT_OK;
}

/**
 * @brief Consume (remove) the first `consumed` bytes from rx_buf.
 * @param[in] consumed  Number of bytes to remove.
 * @return none
 */
STATIC VOID_T __rx_consume(size_t consumed)
{
    if (consumed == 0 || consumed > s_ctx.rx_len) {
        return;
    }
    if (consumed < s_ctx.rx_len) {
        memmove(s_ctx.rx_buf, s_ctx.rx_buf + consumed,
                s_ctx.rx_len - consumed);
    }
    s_ctx.rx_len -= consumed;
}

/* ---------------------------------------------------------------------------
 * Internal helpers – ACP frame dispatch
 * --------------------------------------------------------------------------- */

/**
 * @brief Send a pong response to a gateway tick heartbeat.
 *
 * The openclaw gateway sends a "tick" event every ~30 seconds.  Responding
 * with a "pong" req keeps the connection alive and prevents the gateway
 * from closing the session.
 *
 * @param[in] seq  Sequence number from the incoming tick event.
 * @return none
 */
VOID_T __acp_send_pong(UINT32_T seq)
{
    CHAR_T buf[128] = {0};
    snprintf(buf, sizeof(buf),
             "{\"type\":\"req\",\"id\":\"%u\",\"method\":\"pong\","
             "\"params\":{\"seq\":%u}}",
             (unsigned)(++s_ctx.req_id_counter), (unsigned)seq);
    (void)__ws_send_json(s_ctx.fd, buf);
    PR_DEBUG("acp pong sent seq=%u", (unsigned)seq);
}

/**
 * @brief Process one decoded ACP JSON text message.
 *
 * Handles:
 *   - type "event", event "chat":  accumulate content on delta; fire
 *     reply_cb when state == "final"
 *   - type "event", event "tick":  send pong to keep connection alive
 *   - type "res":                  log unexpected responses
 *
 * @param[in] text      NUL-terminated JSON payload.
 * @param[in] text_len  Length of text (informational).
 * @return none
 */
STATIC VOID_T __acp_dispatch(CONST CHAR_T *text, size_t text_len)
{
    (void)text_len;
    PR_DEBUG("acp dispatch: %.128s", text);
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        PR_WARN("acp dispatch json parse failed: %.64s", text);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(root, "event");
        if (!cJSON_IsString(event) || !event->valuestring) {
            cJSON_Delete(root);
            return;
        }

        /* ---- chat event: delta (streaming) or final (complete) ---- */
        if (strcmp(event->valuestring, "chat") == 0) {
            cJSON *evt_payload = cJSON_GetObjectItem(root, "payload");
            cJSON *state       = cJSON_GetObjectItem(evt_payload, "state");
            cJSON *message     = cJSON_GetObjectItem(evt_payload, "message");

            if (!cJSON_IsString(state) || !state->valuestring) {
                cJSON_Delete(root);
                return;
            }

            if (strcmp(state->valuestring, "delta") == 0 ||
                strcmp(state->valuestring, "final") == 0) {

                /* Extract text content from message.content[].text */
                cJSON *content_arr = cJSON_GetObjectItem(message, "content");
                if (cJSON_IsArray(content_arr)) {
                    cJSON *part = NULL;
                    cJSON_ArrayForEach(part, content_arr) {
                        cJSON *part_type = cJSON_GetObjectItem(part, "type");
                        cJSON *part_text = cJSON_GetObjectItem(part, "text");
                        if (cJSON_IsString(part_type) &&
                            strcmp(part_type->valuestring, "text") == 0 &&
                            cJSON_IsString(part_text) && part_text->valuestring) {

                            size_t clen = strlen(part_text->valuestring);
                            if (clen >= ACP_CLIENT_REPLY_BUF_SIZE) {
                                clen = ACP_CLIENT_REPLY_BUF_SIZE - 1;
                            }
                            memcpy(s_ctx.reply_buf, part_text->valuestring, clen);
                            s_ctx.reply_buf[clen] = '\0';
                            s_ctx.reply_len = clen;
                        }
                    }
                }

                if (strcmp(state->valuestring, "final") == 0) {
                    PR_INFO("acp reply final len=%u: %.64s",
                            (unsigned)s_ctx.reply_len, s_ctx.reply_buf);
                    if (s_ctx.sync_waiting && s_ctx.reply_len > 0) {
                        /* Sync caller (MCP tool) is waiting: copy reply and signal. */
                        size_t copy_len = s_ctx.reply_len;
                        if (copy_len >= ACP_CLIENT_REPLY_BUF_SIZE) {
                            copy_len = ACP_CLIENT_REPLY_BUF_SIZE - 1;
                        }
                        memcpy(s_ctx.sync_reply_buf, s_ctx.reply_buf, copy_len);
                        s_ctx.sync_reply_buf[copy_len] = '\0';
                        s_ctx.sync_reply_len = copy_len;
                        tal_semaphore_post(s_ctx.sync_sem);
                    } else if (s_ctx.reply_cb && s_ctx.reply_len > 0) {
                        /* Normal async path. */
                        s_ctx.reply_cb(s_ctx.reply_buf, s_ctx.reply_cb_data);
                    }
                    s_ctx.reply_len    = 0;
                    s_ctx.reply_buf[0] = '\0';
                }
            }
        }

        /* ---- tick heartbeat: no response needed, just log to confirm alive ---- */
        if (strcmp(event->valuestring, "tick") == 0) {
            cJSON *evt_payload = cJSON_GetObjectItem(root, "payload");
            cJSON *seq_field   = cJSON_GetObjectItem(evt_payload, "seq");
            UINT32_T seq = cJSON_IsNumber(seq_field) ? (UINT32_T)seq_field->valuedouble : 0;
            PR_DEBUG("acp tick seq=%u (keepalive, no response needed)", (unsigned)seq);
        }

    } else if (strcmp(type->valuestring, "res") == 0) {
        /* connect response is handled synchronously in __acp_connect;
         * log unexpected res frames here for debug. */
        cJSON *id      = cJSON_GetObjectItem(root, "id");
        cJSON *ok      = cJSON_GetObjectItem(root, "ok");
        cJSON *error   = cJSON_GetObjectItem(root, "error");
        cJSON *errcode = error ? cJSON_GetObjectItem(error, "code")    : NULL;
        cJSON *errmsg  = error ? cJSON_GetObjectItem(error, "message") : NULL;
        if (cJSON_IsBool(ok) && cJSON_IsTrue(ok)) {
            PR_DEBUG("acp res id=%s ok=true",
                     cJSON_IsString(id) ? id->valuestring : "?");
        } else {
            PR_WARN("acp res id=%s ok=false code=%s msg=%s",
                    cJSON_IsString(id)      ? id->valuestring      : "?",
                    cJSON_IsString(errcode) ? errcode->valuestring : "?",
                    cJSON_IsString(errmsg)  ? errmsg->valuestring  : "?");
        }
    }

    cJSON_Delete(root);
}

/* ---------------------------------------------------------------------------
 * Internal helpers – connection lifecycle
 * --------------------------------------------------------------------------- */

/**
 * @brief Open a TCP connection, perform WS upgrade, and ACP handshake.
 * @return OPRT_OK on success; the socket fd is stored in s_ctx.fd.
 */
STATIC OPERATE_RET __connect_and_handshake(VOID_T)
{
    TUYA_IP_ADDR_T ip_addr = 0;

    /* Resolve host – try str2addr first (works for dotted-decimal IPs) */
    ip_addr = tal_net_str2addr(ACP_GATEWAY_HOST);
    if (ip_addr == 0) {
        OPERATE_RET rt = tal_net_gethostbyname(ACP_GATEWAY_HOST, &ip_addr);
        if (rt != OPRT_OK || ip_addr == 0) {
            PR_ERR("acp dns resolve failed host=%s", ACP_GATEWAY_HOST);
            return OPRT_NETWORK_ERROR;
        }
    }

    INT_T fd = tal_net_socket_create(PROTOCOL_TCP);
    if (fd < 0) {
        PR_ERR("acp socket create failed");
        return OPRT_NETWORK_ERROR;
    }

    OPERATE_RET rt = tal_net_connect(fd, ip_addr, ACP_GATEWAY_PORT);
    if (rt != OPRT_OK) {
        PR_ERR("acp tcp connect failed rt=%d host=%s port=%u",
               rt, ACP_GATEWAY_HOST, (unsigned)ACP_GATEWAY_PORT);
        tal_net_close(fd);
        return rt;
    }
    PR_INFO("acp tcp connected fd=%d host=%s:%u",
            fd, ACP_GATEWAY_HOST, (unsigned)ACP_GATEWAY_PORT);

    rt = __ws_upgrade(fd, ACP_GATEWAY_HOST, ACP_GATEWAY_PORT);
    if (rt != OPRT_OK) {
        tal_net_close(fd);
        return rt;
    }

    rt = __acp_connect(fd, s_ctx.session_key, sizeof(s_ctx.session_key));
    if (rt != OPRT_OK) {
        tal_net_close(fd);
        return rt;
    }

    s_ctx.fd    = fd;
    s_ctx.state = ACP_STATE_CONNECTED;
    PR_INFO("acp client ready fd=%d session=%s", fd, s_ctx.session_key);
    return OPRT_OK;
}

/**
 * @brief Close the socket and reset connection state.
 * @return none
 */
STATIC VOID_T __disconnect(VOID_T)
{
    if (s_ctx.fd >= 0) {
        tal_net_close(s_ctx.fd);
        s_ctx.fd = -1;
    }
    s_ctx.state          = ACP_STATE_DISCONNECTED;
    s_ctx.session_key[0] = '\0';
    s_ctx.rx_len         = 0;
    s_ctx.reply_len      = 0;
    s_ctx.reply_buf[0]   = '\0';
}

/* ---------------------------------------------------------------------------
 * Background task
 * --------------------------------------------------------------------------- */

/**
 * @brief Main ACP client task: connect, recv loop, reconnect on error.
 * @param[in] arg  Unused.
 * @return none
 */
STATIC VOID_T acp_client_task(VOID_T *arg)
{
    (void)arg;
    PR_INFO("acp client task started");

    while (!s_ctx.stop_requested) {
        /* ---- Connection phase ---- */
        if (s_ctx.state == ACP_STATE_DISCONNECTED) {
            OPERATE_RET rt = __connect_and_handshake();
            if (rt != OPRT_OK) {
                PR_WARN("acp connect failed, retry in %dms", ACP_CLIENT_RECONNECT_MS);
                tal_system_sleep(ACP_CLIENT_RECONNECT_MS);
                continue;
            }
        }

        /* ---- Receive phase ---- */
        TUYA_FD_SET_T rfds;
        TAL_FD_ZERO(&rfds);
        TAL_FD_SET(s_ctx.fd, &rfds);

        INT_T ready = tal_net_select(s_ctx.fd + 1, &rfds, NULL, NULL,
                                     ACP_SELECT_TIMEOUT_MS);
        if (ready <= 0) {
            continue;
        }

        if (!TAL_FD_ISSET(s_ctx.fd, &rfds)) {
            continue;
        }

        if (s_ctx.rx_len >= sizeof(s_ctx.rx_buf)) {
            PR_WARN("acp rx buffer full, disconnecting");
            __disconnect();
            continue;
        }

        INT_T n = tal_net_recv(s_ctx.fd,
                               s_ctx.rx_buf + s_ctx.rx_len,
                               (UINT32_T)(sizeof(s_ctx.rx_buf) - s_ctx.rx_len));
        if (n == OPRT_RESOURCE_NOT_READY) {
            continue;
        }
        if (n <= 0) {
            PR_WARN("acp connection closed by server, reconnecting");
            __disconnect();
            continue;
        }
        s_ctx.rx_len += (size_t)n;

        /* ---- Frame decode loop ---- */
        while (s_ctx.rx_len > 0) {
            uint8_t       opcode      = 0;
            CONST uint8_t *payload    = NULL;
            size_t         payload_len = 0;
            size_t         consumed   = 0;

            OPERATE_RET rt = __ws_decode_frame(s_ctx.rx_buf, s_ctx.rx_len,
                                               &opcode, &payload,
                                               &payload_len, &consumed);
            if (rt == OPRT_RESOURCE_NOT_READY) {
                break; /* wait for more data */
            }
            if (rt != OPRT_OK) {
                PR_WARN("acp frame decode err rt=%d", rt);
                __disconnect();
                break;
            }

            switch (opcode) {
            case ACP_WS_OPCODE_TEXT:
                if (payload && payload_len > 0) {
                    /* Temporary NUL-terminated copy for JSON parse */
                    CHAR_T *tmp = (CHAR_T *)tal_malloc(payload_len + 1);
                    if (tmp) {
                        memcpy(tmp, payload, payload_len);
                        tmp[payload_len] = '\0';
                        __acp_dispatch(tmp, payload_len);
                        tal_free(tmp);
                    }
                }
                break;

            case ACP_WS_OPCODE_PING:
                /* Respond with a PONG to keep the connection alive */
                (void)__ws_send_masked(s_ctx.fd, ACP_WS_OPCODE_PONG,
                                       payload, payload_len);
                break;

            case ACP_WS_OPCODE_CLOSE:
                PR_INFO("acp server sent close frame");
                __disconnect();
                break;

            default:
                PR_DEBUG("acp unhandled opcode=0x%02x", opcode);
                break;
            }

            __rx_consume(consumed);
            if (s_ctx.state == ACP_STATE_DISCONNECTED) {
                break;
            }
        }
    }

    PR_INFO("acp client task stopped");
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Register a callback for incoming OpenClaw agent replies.
 * @param[in] cb         Callback; NULL clears.
 * @param[in] user_data  Forwarded pointer.
 * @return none
 */
VOID_T acp_client_set_reply_cb(acp_reply_cb_t cb, VOID_T *user_data)
{
    s_ctx.reply_cb      = cb;
    s_ctx.reply_cb_data = user_data;
}

/**
 * @brief Initialise the ACP client and start the background task.
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET acp_client_init(VOID_T)
{
    if (s_ctx.thread) {
        return OPRT_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.fd    = -1;
    s_ctx.state = ACP_STATE_DISCONNECTED;

    OPERATE_RET rt = tal_mutex_create_init(&s_ctx.tx_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("acp mutex create failed rt=%d", rt);
        return rt;
    }

    rt = tal_mutex_create_init(&s_ctx.sync_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("acp sync_mutex create failed rt=%d", rt);
        return rt;
    }

    rt = tal_semaphore_create_init(&s_ctx.sync_sem, 0, 1);
    if (rt != OPRT_OK) {
        PR_ERR("acp sync_sem create failed rt=%d", rt);
        return rt;
    }

    THREAD_CFG_T cfg = {0};
    cfg.stackDepth = ACP_CLIENT_STACK_SIZE;
    cfg.priority   = THREAD_PRIO_1;
    cfg.thrdname   = "acp_client";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    cfg.psram_mode = 1;
#endif

    rt = tal_thread_create_and_start(&s_ctx.thread, NULL, NULL,
                                     acp_client_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("acp thread create failed rt=%d", rt);
        return rt;
    }

    PR_INFO("acp client init ok host=%s port=%u",
            ACP_GATEWAY_HOST, (unsigned)ACP_GATEWAY_PORT);
    return OPRT_OK;
}

/**
 * @brief Send a user message to the OpenClaw agent and trigger an LLM reply.
 *
 * Sends a chat.send ACP request (triggers full agent response):
 * @code
 * {
 *   "type": "req", "id": "N", "method": "chat.send",
 *   "params": {
 *     "sessionKey": "<session_key>",
 *     "message": "<text>",
 *     "idempotencyKey": "device-<req_id>"
 *   }
 * }
 * @endcode
 *
 * @note chat.inject only appends context; chat.send actually triggers the agent.
 * @param[in] text  Message text (ASR result or user input).
 * @return OPRT_OK on success.
 */
OPERATE_RET acp_client_inject(CONST CHAR_T *text)
{
    if (!text || text[0] == '\0') {
        return OPRT_INVALID_PARM;
    }
    if (s_ctx.state != ACP_STATE_CONNECTED || s_ctx.fd < 0) {
        PR_WARN("acp inject skipped: not connected");
        return OPRT_RESOURCE_NOT_READY;
    }

    cJSON *root   = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    if (!root || !params) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        return OPRT_MALLOC_FAILED;
    }

    CHAR_T id_str[16]       = {0};
    CHAR_T idem_key[32]     = {0};
    UINT32_T req_id = (unsigned)(++s_ctx.req_id_counter);
    snprintf(id_str,   sizeof(id_str),   "%u", req_id);
    snprintf(idem_key, sizeof(idem_key), "device-%u", req_id);

    cJSON_AddStringToObject(root,   "type",   "req");
    cJSON_AddStringToObject(root,   "id",     id_str);
    cJSON_AddStringToObject(root,   "method", "chat.send");
    cJSON_AddStringToObject(params, "sessionKey",     s_ctx.session_key);
    cJSON_AddStringToObject(params, "message",        text);
    cJSON_AddStringToObject(params, "idempotencyKey", idem_key);
    cJSON_AddItemToObject(root, "params", params);

    CHAR_T *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return OPRT_MALLOC_FAILED;
    }

    tal_mutex_lock(s_ctx.tx_mutex);
    OPERATE_RET rt = __ws_send_json(s_ctx.fd, payload);
    tal_mutex_unlock(s_ctx.tx_mutex);

    cJSON_free(payload);

    if (rt != OPRT_OK) {
        PR_ERR("acp chat.send failed rt=%d", rt);
    } else {
        PR_INFO("acp chat.send sent idem=%s: %.64s", idem_key, text);
    }
    return rt;
}

/**
 * @brief Stop the ACP client and close the connection.
 * @return OPRT_OK on success.
 */
OPERATE_RET acp_client_stop(VOID_T)
{
    s_ctx.stop_requested = TRUE;
    __disconnect();
    PR_INFO("acp client stop requested");
    return OPRT_OK;
}

/**
 * @brief Query whether the ACP session is currently connected.
 *
 * Thread-safe: reads a single enum value that is only written from
 * the acp_client task under the receive loop.
 *
 * @return TRUE  if the ACP session is established (hello-ok received).
 * @return FALSE if disconnected or still connecting.
 */
BOOL_T acp_client_is_connected(VOID_T)
{
    return (s_ctx.state == ACP_STATE_CONNECTED) ? TRUE : FALSE;
}

/**
 * @brief Send a message to OpenClaw and block until the reply arrives.
 *
 * Sends a chat.send request and waits (up to @p timeout_ms) for the
 * agent's final reply.  The reply is copied into @p reply_buf on success.
 *
 * Only one sync caller may be active at a time; concurrent callers will
 * block on the sync mutex and execute sequentially.
 *
 * @param[in]  text        NUL-terminated user message.
 * @param[in]  timeout_ms  Maximum wait time in milliseconds.
 * @param[out] reply_buf   Buffer to receive the agent reply.
 * @param[in]  buf_len     Size of reply_buf (including NUL terminator).
 * @return OPRT_OK         Reply received and copied to reply_buf.
 * @return OPRT_TIMEOUT    No reply within timeout_ms.
 * @return OPRT_RESOURCE_NOT_READY if ACP is not connected.
 * @return OPRT_INVALID_PARM if any parameter is invalid.
 */
OPERATE_RET acp_client_send_and_wait(CONST CHAR_T *text, UINT32_T timeout_ms,
                                     CHAR_T *reply_buf, size_t buf_len)
{
    if (!text || text[0] == '\0' || !reply_buf || buf_len == 0) {
        return OPRT_INVALID_PARM;
    }
    if (s_ctx.state != ACP_STATE_CONNECTED) {
        PR_WARN("[acp] send_and_wait: not connected");
        return OPRT_RESOURCE_NOT_READY;
    }

    tal_mutex_lock(s_ctx.sync_mutex);

    s_ctx.sync_waiting       = TRUE;
    s_ctx.sync_reply_len     = 0;
    s_ctx.sync_reply_buf[0]  = '\0';
    reply_buf[0]             = '\0';

    PR_INFO("[acp] send_and_wait: sending msg=%.64s timeout=%ums", text, (unsigned)timeout_ms);

    OPERATE_RET rt = acp_client_inject(text);
    if (rt != OPRT_OK) {
        PR_ERR("[acp] send_and_wait: inject failed rt=%d", rt);
        s_ctx.sync_waiting = FALSE;
        tal_mutex_unlock(s_ctx.sync_mutex);
        return rt;
    }

    rt = tal_semaphore_wait(s_ctx.sync_sem, timeout_ms);
    s_ctx.sync_waiting = FALSE;

    if (rt == OPRT_OK && s_ctx.sync_reply_len > 0) {
        size_t copy_len = s_ctx.sync_reply_len;
        if (copy_len >= buf_len) {
            copy_len = buf_len - 1;
        }
        memcpy(reply_buf, s_ctx.sync_reply_buf, copy_len);
        reply_buf[copy_len] = '\0';
        PR_INFO("[acp] send_and_wait: reply len=%u: %.64s", (unsigned)copy_len, reply_buf);
    } else {
        PR_WARN("[acp] send_and_wait: timeout or empty reply rt=%d", rt);
        rt = OPRT_TIMEOUT;
    }

    tal_mutex_unlock(s_ctx.sync_mutex);
    return rt;
}

/**
 * @file acp_client.h
 * @brief ACP (Agent Client Protocol) WebSocket client for DuckyClaw.
 *
 * Connects to an openclaw gateway (@agentclientprotocol/sdk) as a WebSocket
 * client over a LAN connection and communicates using the ACP JSON-RPC-over-
 * WebSocket protocol.  The client:
 *   1. Establishes a TCP connection to ACP_GATEWAY_HOST:ACP_GATEWAY_PORT
 *   2. Performs the RFC 6455 WebSocket upgrade handshake
 *   3. Sends the ACP "connect" request with client descriptor and auth token;
 *      waits for the "hello-ok" event to obtain the sessionKey
 *   4. Exposes acp_client_inject() to push user messages via chat.inject
 *   5. Delivers the agent's final reply via a registered callback
 *   6. Responds to gateway "tick" heartbeat events with "pong" requests
 *
 * ACP frame types:
 *   Request  : {"type":"req",   "id":"N",  "method":"...", "params":{...}}
 *   Response : {"type":"res",   "id":"N",  "ok":true/false, "payload":{...}}
 *   Event    : {"type":"event", "event":"...", "payload":{...}, "seq":N}
 *
 * Configuration macros (defined in tuya_app_config_secrets.h):
 *   ACP_GATEWAY_HOST   - OpenClaw server IP or hostname
 *   ACP_GATEWAY_PORT   - OpenClaw gateway port (default 18789)
 *   ACP_GATEWAY_TOKEN  - Authentication token from openclaw.json gateway.auth.token
 *   ACP_DEVICE_ID      - Unique device identifier (used as displayName)
 *
 * Prerequisites on the Linux/openclaw side:
 *   Set gateway.bind to "lan" in ~/.openclaw/openclaw.json so the gateway
 *   listens on 0.0.0.0 instead of loopback, making it reachable from LAN.
 *   Set gateway.controlUi.dangerouslyDisableDeviceAuth to true so the
 *   device can connect as "openclaw-control-ui" with operator.admin scope
 *   without going through the interactive device-pairing flow.
 *
 * @version 2.0
 * @date 2026-03-19
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#ifndef __ACP_CLIENT_H__
#define __ACP_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

#ifndef ACP_CLIENT_RX_BUF_SIZE
#define ACP_CLIENT_RX_BUF_SIZE    (8 * 1024)
#endif

#ifndef ACP_CLIENT_REPLY_BUF_SIZE
#define ACP_CLIENT_REPLY_BUF_SIZE (8 * 1024)
#endif

#ifndef ACP_CLIENT_STACK_SIZE
#define ACP_CLIENT_STACK_SIZE     (12 * 1024)
#endif

#ifndef ACP_CLIENT_RECONNECT_MS
#define ACP_CLIENT_RECONNECT_MS   5000
#endif

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

/**
 * @brief Callback invoked when the OpenClaw agent finishes a reply.
 *
 * @param[in] text       Complete assistant reply text (NUL-terminated).
 * @param[in] user_data  Opaque pointer supplied at registration time.
 */
typedef void (*acp_reply_cb_t)(const char *text, void *user_data);

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Register a callback for incoming OpenClaw agent replies.
 *
 * @param[in] cb         Callback function; NULL clears the existing callback.
 * @param[in] user_data  Forwarded opaque pointer.
 * @return none
 * @note Must be called before acp_client_init().
 */
VOID_T acp_client_set_reply_cb(acp_reply_cb_t cb, VOID_T *user_data);

/**
 * @brief Initialise the ACP client and start the background task.
 *
 * Reads connection parameters from ACP_GATEWAY_HOST, ACP_GATEWAY_PORT,
 * ACP_GATEWAY_TOKEN, and ACP_DEVICE_ID (all defined in tuya_app_config.h).
 * The background task handles connection, reconnection, and frame I/O.
 *
 * @return OPRT_OK on success, error code on failure.
 * @note Call after the network link is up (e.g., inside EVENT_MQTT_CONNECTED).
 */
OPERATE_RET acp_client_init(VOID_T);

/**
 * @brief Inject a user message into the OpenClaw agent session.
 *
 * Sends a chat.inject ACP request to the connected OpenClaw gateway.
 * The agent reply will arrive asynchronously via the registered callback.
 *
 * @param[in] text  User message text (NUL-terminated, UTF-8).
 * @return OPRT_OK on success.
 * @return OPRT_RESOURCE_NOT_READY if the ACP session is not yet established.
 * @return OPRT_INVALID_PARM if text is NULL or empty.
 */
OPERATE_RET acp_client_inject(CONST CHAR_T *text);

/**
 * @brief Stop the ACP client and close the connection.
 *
 * @return OPRT_OK on success.
 */
OPERATE_RET acp_client_stop(VOID_T);

/**
 * @brief Query whether the ACP session is currently connected.
 *
 * @return TRUE  if the ACP session is established.
 * @return FALSE if disconnected or still connecting.
 */
BOOL_T acp_client_is_connected(VOID_T);

/**
 * @brief Send a message to OpenClaw and block until the reply arrives.
 *
 * Sends a chat.send ACP request and waits up to @p timeout_ms for the
 * agent's final reply.  Suitable for synchronous MCP tool callers.
 *
 * Only one sync caller may be active at a time; concurrent callers will
 * execute sequentially.
 *
 * @param[in]  text        NUL-terminated user message.
 * @param[in]  timeout_ms  Maximum wait time in milliseconds.
 * @param[out] reply_buf   Buffer to receive the agent reply (NUL-terminated).
 * @param[in]  buf_len     Size of reply_buf (must include space for NUL).
 * @return OPRT_OK                on success (reply in reply_buf).
 * @return OPRT_TIMEOUT           if no reply within timeout_ms.
 * @return OPRT_RESOURCE_NOT_READY if ACP session not connected.
 * @return OPRT_INVALID_PARM      if any parameter is invalid.
 */
OPERATE_RET acp_client_send_and_wait(CONST CHAR_T *text, UINT32_T timeout_ms,
                                     CHAR_T *reply_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* __ACP_CLIENT_H__ */

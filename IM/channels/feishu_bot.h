/**
 * @file feishu_bot.h
 * @brief Feishu (Lark) bot channel for IM component
 * @version 1.0
 * @date 2025-02-13
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __FEISHU_BOT_H__
#define __FEISHU_BOT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "im_platform.h"
#include "cJSON.h"

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */

/**
 * @brief Mention target for outbound @mention messages
 */
typedef struct {
    const char *open_id;
    const char *name;
} feishu_mention_t;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialize feishu bot (load credentials from config/NVS)
 * @return OPRT_OK on success
 */
OPERATE_RET feishu_bot_init(VOID_T);

/**
 * @brief Start feishu bot WebSocket service
 * @return OPRT_OK on success
 */
OPERATE_RET feishu_bot_start(VOID_T);

/**
 * @brief Unified Feishu message send interface.
 *
 * Automatically detects @mention targets from two sources and sends the
 * appropriate message format:
 *   1. mentions_json — pre-resolved mention targets from inbound context
 *      (JSON array: [{"open_id":"ou_xxx","name":"张三"}, ...])
 *   2. text scanning — scans the text for "@word" patterns and resolves them
 *      via bot name match, @all, or Feishu user search API (with cache)
 *
 * If any mentions are detected, sends a rich-text (post) message with @at
 * nodes; otherwise sends a plain text message.  Duplicate @name tokens in the
 * text body are automatically stripped when mentions are present.
 *
 * @param[in] chat_id       target chat_id (oc_xxx) or open_id (ou_xxx)
 * @param[in] text          message text body
 * @param[in] mentions_json optional pre-resolved mention JSON array (may be NULL)
 * @return OPRT_OK on success
 */
OPERATE_RET feishu_send_message(const char *chat_id, const char *text, const char *mentions_json);

/**
 * @brief Fetch all members of a Feishu group chat.
 *
 * Calls GET /open-apis/im/v1/chats/{chat_id}/members (handles pagination) and
 * returns a cJSON array of member objects:
 * @code
 * [{"open_id":"ou_xxx","name":"张三"}, {"open_id":"ou_yyy","name":"openclaw"}, ...]
 * @endcode
 * The caller is responsible for calling cJSON_Delete() on the returned array.
 *
 * Typical usage by the AI agent:
 *   1. Call feishu_get_members MCP tool → receives JSON member list.
 *   2. Choose the target member (e.g. "openclaw").
 *   3. Pass the member's open_id as mentions_json in openclaw_ctrl.
 *
 * @param[in]  chat_id   Group chat ID (oc_…).
 * @param[out] out_json  Heap-allocated cJSON array; caller must cJSON_Delete().
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET feishu_get_chat_members(const char *chat_id, cJSON **out_json);

/**
 * @brief Set feishu app_id credential
 * @param[in] app_id application ID
 * @return OPRT_OK on success
 */
OPERATE_RET feishu_set_app_id(const char *app_id);

/**
 * @brief Set feishu app_secret credential
 * @param[in] app_secret application secret
 * @return OPRT_OK on success
 */
OPERATE_RET feishu_set_app_secret(const char *app_secret);

/**
 * @brief Set allowed sender list (CSV of open_id/user_id)
 * @param[in] allow_from_csv comma-separated list of allowed sender IDs
 * @return OPRT_OK on success
 */
OPERATE_RET feishu_set_allow_from(const char *allow_from_csv);

#ifdef __cplusplus
}
#endif

#endif /* __FEISHU_BOT_H__ */

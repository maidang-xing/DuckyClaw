/**
 * @file tool_openclaw_ctrl.c
 * @brief MCP tools: openclaw_ctrl + feishu_get_members.
 *
 * Provides two MCP tools for the AI agent:
 *
 *   feishu_get_members
 *     Fetches the current Feishu group member list so the agent knows
 *     who is available to @mention.  Call this first when the agent needs
 *     to decide who to notify.
 *
 *   openclaw_ctrl
 *     Sends a message/command to the OpenClaw gateway (PC side).
 *     Transport priority (per dev7.md):
 *       1. WS/ACP – acp_client_send_and_wait(); reply is prefixed with
 *          "[来自 OpenClaw]" and pushed to the IM bus, then returned to
 *          the agent.
 *       2. Feishu fallback – when ACP is disconnected, sends a Feishu
 *          rich-text message with @mentions determined by the agent.
 *
 *     Parameters:
 *       message       (string, required)  – command / query text.
 *       mentions_json (string, optional)  – JSON array of @mention targets
 *                     chosen by the agent from the feishu_get_members list,
 *                     e.g. [{"open_id":"ou_xxx","name":"张三"}].
 *                     Used only on the Feishu fallback path.
 *
 * Memory: claw_malloc / claw_free (PSRAM-aware wrappers from tool_files.h).
 *
 * @version 2.0
 * @date 2026-03-19
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#include "tool_openclaw_ctrl.h"

#include "tool_files.h"                 /* claw_malloc / claw_free */
#include "ai_mcp_server.h"
#include "acp_client.h"
#include "app_im.h"
#include "channels/feishu_bot.h"
#include "im_config.h"
#include "cJSON.h"
#include "tal_log.h"
#include "tal_api.h"

#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */

/** Timeout waiting for an OpenClaw reply (ms). */
#define OPENCLAW_CTRL_REPLY_TIMEOUT_MS  (30000U)

/** Max size of the reply buffer (chars, including NUL). */
#define OPENCLAW_CTRL_REPLY_BUF_LEN     (4096U)

/** Prefix prepended to every reply forwarded to the IM bus. */
#define OPENCLAW_REPLY_PREFIX           "[来自 OpenClaw] "

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Retrieve a string property from an MCP property list.
 *
 * @param[in] props  MCP property list.
 * @param[in] name   Property name.
 * @return String value pointer, or NULL if not found.
 */
static const char *__get_str_prop(const MCP_PROPERTY_LIST_T *props,
                                  const char *name)
{
    const MCP_PROPERTY_T *p = ai_mcp_property_list_find(props, name);
    if (p && p->type == MCP_PROPERTY_TYPE_STRING && p->default_val.str_val) {
        return p->default_val.str_val;
    }
    return NULL;
}

/**
 * @brief Read the persisted Feishu chat_id from KV storage.
 *
 * @param[out] buf      Output buffer.
 * @param[in]  buf_size Buffer size (bytes).
 * @return TRUE if a non-empty chat_id was loaded.
 */
static BOOL_T __load_chat_id(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return FALSE;
    }
    buf[0] = '\0';
    if (im_kv_get_string(IM_NVS_BOT, "chat_id", buf, (uint32_t)buf_size) == OPRT_OK
            && buf[0] != '\0') {
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Auto-build a mentions_json string for a named target.
 *
 * Fetches the Feishu group member list for @p chat_id, searches for the first
 * member whose name contains @p target_name (case-insensitive), and builds a
 * JSON array string:  [{"open_id":"ou_xxx","name":"openclaw"}]
 *
 * If no matching member is found, the buffer is left as an empty string and
 * FALSE is returned; the caller should then send without @mention.
 *
 * @param[in]  chat_id      Group chat_id.
 * @param[in]  target_name  Name substring to search for (e.g. "openclaw").
 * @param[out] buf          Output buffer for the JSON string.
 * @param[in]  buf_size     Size of @p buf.
 * @return TRUE if a matching member was found and @p buf was populated.
 */
static BOOL_T __auto_resolve_mention(const char *chat_id, const char *target_name,
                                     char *buf, size_t buf_size)
{
    buf[0] = '\0';
    if (!chat_id || chat_id[0] == '\0' || !target_name || !buf || buf_size == 0) {
        return FALSE;
    }

    cJSON *members = NULL;
    OPERATE_RET rt = feishu_get_chat_members(chat_id, &members);
    if (rt != OPRT_OK || !members) {
        if (members) {
            cJSON_Delete(members);
        }
        return FALSE;
    }

    BOOL_T found = FALSE;
    int count = cJSON_GetArraySize(members);
    for (int i = 0; i < count; i++) {
        cJSON *m    = cJSON_GetArrayItem(members, i);
        cJSON *oid  = m ? cJSON_GetObjectItem(m, "open_id") : NULL;
        cJSON *name = m ? cJSON_GetObjectItem(m, "name")    : NULL;

        if (!cJSON_IsString(oid) || !oid->valuestring || oid->valuestring[0] == '\0') {
            continue;
        }
        if (!cJSON_IsString(name) || !name->valuestring) {
            continue;
        }

        /* Case-insensitive substring match */
        const char *n = name->valuestring;
        const char *t = target_name;

        /* Simple manual tolower search to avoid locale issues */
        size_t tlen = strlen(t);
        size_t nlen = strlen(n);
        BOOL_T match = FALSE;
        if (tlen <= nlen) {
            for (size_t j = 0; j <= nlen - tlen && !match; j++) {
                BOOL_T ok = TRUE;
                for (size_t k = 0; k < tlen && ok; k++) {
                    char nc = n[j + k];
                    char tc = t[k];
                    if (nc >= 'A' && nc <= 'Z') { nc += ('a' - 'A'); }
                    if (tc >= 'A' && tc <= 'Z') { tc += ('a' - 'A'); }
                    if (nc != tc) { ok = FALSE; }
                }
                if (ok) { match = TRUE; }
            }
        }

        if (match) {
            snprintf(buf, buf_size,
                     "[{\"open_id\":\"%s\",\"name\":\"%s\"}]",
                     oid->valuestring, name->valuestring);
            PR_INFO("[openclaw_ctrl] auto-mention: found '%s' open_id=%s",
                    name->valuestring, oid->valuestring);
            found = TRUE;
            break;
        }
    }

    cJSON_Delete(members);
    if (!found) {
        PR_WARN("[openclaw_ctrl] auto-mention: no member matching '%s' found", target_name);
    }
    return found;
}

/* ---------------------------------------------------------------------------
 * Tool: feishu_get_members
 * --------------------------------------------------------------------------- */

/**
 * @brief MCP tool handler: feishu_get_members.
 *
 * Fetches the Feishu group member list and returns it as a JSON array string.
 * The agent should call this tool before deciding who to @mention in
 * openclaw_ctrl.
 *
 * @param[in]  properties  Unused (no parameters).
 * @param[out] ret_val     JSON array string of members.
 * @param[in]  user_data   Unused.
 * @return OPRT_OK on success.
 */
static OPERATE_RET __tool_feishu_get_members(const MCP_PROPERTY_LIST_T *properties,
                                             MCP_RETURN_VALUE_T *ret_val,
                                             void *user_data)
{
    (void)properties;
    (void)user_data;

    char chat_id[96] = {0};
    if (!__load_chat_id(chat_id, sizeof(chat_id))) {
        PR_WARN("[feishu_get_members] chat_id not set");
        ai_mcp_return_value_set_str(ret_val,
            "Error: Feishu chat_id not configured. Send a message to the bot first.");
        return OPRT_RESOURCE_NOT_READY;
    }

    PR_INFO("[feishu_get_members] fetching members for chat_id=%s", chat_id);

    cJSON *members = NULL;
    OPERATE_RET rt = feishu_get_chat_members(chat_id, &members);
    if (rt != OPRT_OK || !members) {
        PR_ERR("[feishu_get_members] fetch failed rt=%d", rt);
        ai_mcp_return_value_set_str(ret_val, "Error: failed to fetch Feishu group members");
        if (members) {
            cJSON_Delete(members);
        }
        return rt;
    }

    char *json_str = cJSON_PrintUnformatted(members);
    cJSON_Delete(members);
    if (!json_str) {
        ai_mcp_return_value_set_str(ret_val, "Error: failed to serialize member list");
        return OPRT_MALLOC_FAILED;
    }

    PR_INFO("[feishu_get_members] returning %d chars", (int)strlen(json_str));
    ai_mcp_return_value_set_str(ret_val, json_str);
    cJSON_free(json_str);
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Tool: openclaw_ctrl
 * --------------------------------------------------------------------------- */

/**
 * @brief MCP tool handler: openclaw_ctrl.
 *
 * Primary path (ACP connected):
 *   Sends @p message to OpenClaw via WS and waits for the reply.
 *   The reply is prefixed with "[来自 OpenClaw]", pushed to the IM bus,
 *   and returned to the agent.
 *
 * Fallback path (ACP disconnected or reply timeout):
 *   Sends the message to Feishu using rich-text format.  @mentions are
 *   taken from the optional @p mentions_json parameter, which the agent
 *   should populate based on the result of feishu_get_members.
 *
 * @param[in]  properties  MCP property list (message, mentions_json).
 * @param[out] ret_val     Result string returned to the agent.
 * @param[in]  user_data   Unused.
 * @return OPRT_OK on success (including fallback).
 */
static OPERATE_RET __tool_openclaw_ctrl(const MCP_PROPERTY_LIST_T *properties,
                                        MCP_RETURN_VALUE_T *ret_val,
                                        void *user_data)
{
    (void)user_data;

    const char *message = __get_str_prop(properties, "message");
    if (!message || message[0] == '\0') {
        PR_WARN("[openclaw_ctrl] missing 'message' parameter");
        ai_mcp_return_value_set_str(ret_val, "Error: 'message' parameter is required");
        return OPRT_INVALID_PARM;
    }

    /* Optional @mention targets decided by the agent (used on fallback path). */
    const char *mentions_json = __get_str_prop(properties, "mentions_json");

    PR_INFO("[openclaw_ctrl] message=%.128s mentions=%s",
            message, mentions_json ? mentions_json : "(none)");

    /* -----------------------------------------------------------------------
     * Primary path: ACP / WS
     * ----------------------------------------------------------------------- */
    if (acp_client_is_connected()) {
        PR_INFO("[openclaw_ctrl] ACP connected, sending via WS");

        char *reply_buf = (char *)claw_malloc(OPENCLAW_CTRL_REPLY_BUF_LEN);
        if (!reply_buf) {
            PR_ERR("[openclaw_ctrl] malloc reply_buf failed");
            ai_mcp_return_value_set_str(ret_val, "Error: out of memory");
            return OPRT_MALLOC_FAILED;
        }
        reply_buf[0] = '\0';

        OPERATE_RET rt = acp_client_send_and_wait(message,
                                                  OPENCLAW_CTRL_REPLY_TIMEOUT_MS,
                                                  reply_buf,
                                                  OPENCLAW_CTRL_REPLY_BUF_LEN);
        if (rt == OPRT_OK && reply_buf[0] != '\0') {
            size_t prefix_len = strlen(OPENCLAW_REPLY_PREFIX);
            size_t reply_len  = strlen(reply_buf);
            char  *prefixed   = (char *)claw_malloc(prefix_len + reply_len + 1);
            if (!prefixed) {
                claw_free(reply_buf);
                ai_mcp_return_value_set_str(ret_val, "Error: out of memory");
                return OPRT_MALLOC_FAILED;
            }
            memcpy(prefixed, OPENCLAW_REPLY_PREFIX, prefix_len);
            memcpy(prefixed + prefix_len, reply_buf, reply_len);
            prefixed[prefix_len + reply_len] = '\0';

            PR_INFO("[openclaw_ctrl] forwarding reply: %.128s", prefixed);
            (void)app_im_bot_send_message(prefixed);
            ai_mcp_return_value_set_str(ret_val, prefixed);

            claw_free(prefixed);
            claw_free(reply_buf);
            return OPRT_OK;
        }

        PR_WARN("[openclaw_ctrl] ACP timeout/empty (rt=%d), falling back to Feishu", rt);
        claw_free(reply_buf);
    } else {
        PR_INFO("[openclaw_ctrl] ACP not connected, using Feishu fallback");
    }

    /* -----------------------------------------------------------------------
     * Fallback path: Feishu rich-text with @mentions
     *
     * Priority:
     *   1. Agent-provided mentions_json (explicit, from feishu_get_members).
     *   2. Auto-resolve: fetch members and search for "openclaw" by name.
     *   3. No @mention (plain text) if resolution fails.
     * ----------------------------------------------------------------------- */
    const char *effective_mentions = NULL;
    char        auto_mention_buf[256] = {0};

    if (mentions_json && mentions_json[0] != '\0') {
        /* Agent explicitly provided mentions. */
        effective_mentions = mentions_json;
        PR_INFO("[openclaw_ctrl] Feishu fallback using agent-provided mentions");
    } else {
        /* Try to auto-resolve @openclaw from the group member list. */
        char chat_id[96] = {0};
        if (__load_chat_id(chat_id, sizeof(chat_id))) {
            if (__auto_resolve_mention(chat_id, "openclaw",
                                       auto_mention_buf, sizeof(auto_mention_buf))) {
                effective_mentions = auto_mention_buf;
            }
        } else {
            PR_WARN("[openclaw_ctrl] chat_id not set, cannot auto-resolve @mention");
        }
    }

    PR_INFO("[openclaw_ctrl] Feishu fallback effective_mentions=%s msg=%.128s",
            effective_mentions ? effective_mentions : "(none)", message);

    OPERATE_RET fb_rt = app_im_bot_send_message_with_mentions(message, effective_mentions);
    if (fb_rt != OPRT_OK) {
        PR_ERR("[openclaw_ctrl] Feishu send failed rt=%d", fb_rt);
        ai_mcp_return_value_set_str(ret_val,
            "Error: WS unavailable and Feishu send failed");
        return fb_rt;
    }

    if (effective_mentions && effective_mentions[0] != '\0') {
        ai_mcp_return_value_set_str(ret_val,
            "Message sent via Feishu with @mention (OpenClaw WS not connected)");
    } else {
        ai_mcp_return_value_set_str(ret_val,
            "Message sent via Feishu (OpenClaw WS not connected, no @mention resolved)");
    }
    return OPRT_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * --------------------------------------------------------------------------- */

/**
 * @brief Register OpenClaw control and Feishu member MCP tools.
 *
 * Registers:
 *   feishu_get_members – fetch Feishu group member list
 *   openclaw_ctrl      – send command to OpenClaw / Feishu
 *   openclaw.ctrl      – dot-namespace alias
 *   pc_ctrl            – PC control alias
 *
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET tool_openclaw_ctrl_register(void)
{
    OPERATE_RET rt       = OPRT_OK;
    OPERATE_RET alias_rt = OPRT_OK;

    PR_INFO("[openclaw_ctrl] registering tools...");

    /* ---- feishu_get_members ---- */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "feishu_get_members",
        "Fetch the Feishu group member list. "
        "Returns a JSON array of members, each with 'open_id' and 'name'. "
        "Call this tool BEFORE openclaw_ctrl when you need to @mention a specific "
        "person (e.g. @openclaw or another member). "
        "Use the returned open_id in the mentions_json parameter of openclaw_ctrl.",
        __tool_feishu_get_members,
        NULL
    ));

    /* ---- openclaw_ctrl (primary) ---- */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "openclaw_ctrl",
        "Send a message or command to the OpenClaw AI gateway (PC side). "
        "Use this when the user wants to control the PC, query OpenClaw, or "
        "trigger a PC-side AI action. "
        "Priority: WS/LAN connection (waits for reply, forwards it to IM bus). "
        "Fallback: Feishu rich-text message with optional @mentions. "
        "To @mention someone, first call feishu_get_members to get their open_id, "
        "then pass it in mentions_json, e.g. [{\"open_id\":\"ou_xxx\",\"name\":\"openclaw\"}].",
        __tool_openclaw_ctrl,
        NULL,
        MCP_PROP_STR("message",
            "The message or command to send (e.g. 'create a text file on the desktop')."),
        MCP_PROP_STR_DEF("mentions_json",
            "Optional JSON array of @mention targets for Feishu fallback. "
            "Obtain open_id values from feishu_get_members first. "
            "Example: [{\"open_id\":\"ou_abc\",\"name\":\"openclaw\"}]. "
            "Leave empty to send without @mention.",
            "")
    ));

    /* ---- aliases ---- */
    alias_rt = AI_MCP_TOOL_ADD(
        "openclaw.ctrl",
        "Alias for openclaw_ctrl.",
        __tool_openclaw_ctrl,
        NULL,
        MCP_PROP_STR("message", "Command to send to OpenClaw."),
        MCP_PROP_STR_DEF("mentions_json",
            "Optional @mention targets JSON array for Feishu fallback.",
            "")
    );
    if (alias_rt != OPRT_OK) {
        PR_WARN("[openclaw_ctrl] alias 'openclaw.ctrl' failed rt=%d", alias_rt);
    }

    alias_rt = AI_MCP_TOOL_ADD(
        "pc_ctrl",
        "Alias for openclaw_ctrl: control the PC or OpenClaw gateway.",
        __tool_openclaw_ctrl,
        NULL,
        MCP_PROP_STR("message", "Command to send to OpenClaw / PC."),
        MCP_PROP_STR_DEF("mentions_json",
            "Optional @mention targets JSON array for Feishu fallback.",
            "")
    );
    if (alias_rt != OPRT_OK) {
        PR_WARN("[openclaw_ctrl] alias 'pc_ctrl' failed rt=%d", alias_rt);
    }

    PR_INFO("[openclaw_ctrl] registration done");
    return rt;
}

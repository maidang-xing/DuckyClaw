/**
 * @file tool_openclaw_ctrl.h
 * @brief MCP tools: openclaw_ctrl + feishu_get_members.
 *
 * Two MCP tools are registered:
 *
 *   feishu_get_members – fetches Feishu group member list (open_id + name).
 *     The agent calls this first when it needs to @mention someone.
 *
 *   openclaw_ctrl – sends a message/command to OpenClaw (WS-first, Feishu
 *     fallback).  The agent populates the optional "mentions_json" parameter
 *     using open_id values obtained from feishu_get_members.
 *
 * @version 2.0
 * @date 2026-03-19
 * @copyright Copyright (c) Tuya Inc. All Rights Reserved.
 */

#ifndef __TOOL_OPENCLAW_CTRL_H__
#define __TOOL_OPENCLAW_CTRL_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */

/**
 * @brief Register OpenClaw control and Feishu member MCP tools.
 *
 * Registers:
 *   - "feishu_get_members" – returns Feishu group member list as JSON array
 *   - "openclaw_ctrl"      – sends command to OpenClaw (WS-first, Feishu fallback)
 *   - "openclaw.ctrl"      – dot-namespace alias
 *   - "pc_ctrl"            – PC control alias
 *
 * @return OPRT_OK on success, error code on failure.
 */
OPERATE_RET tool_openclaw_ctrl_register(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOOL_OPENCLAW_CTRL_H__ */

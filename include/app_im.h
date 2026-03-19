/**
 * @file app_im.h
 * @brief app_im module is used to 
 * @version 0.1
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#ifndef __APP_IM_H__
#define __APP_IM_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/


/***********************************************************
***********************typedef define***********************
***********************************************************/


/***********************************************************
********************function declaration********************
***********************************************************/
OPERATE_RET app_im_init(void);

void app_im_set_chat_id(const char *chat_id);

OPERATE_RET app_im_bot_send_message(const char *message);

/**
 * @brief Send a bot message with @mention targets (Feishu only).
 *
 * @param[in] message      Message text.
 * @param[in] mentions_json JSON array string of mention targets, e.g.
 *                          [{"open_id":"ou_xxx","name":"张三"}].
 *                          Pass NULL or empty string to fall back to plain send.
 * @return OPRT_OK on success
 */
OPERATE_RET app_im_bot_send_message_with_mentions(const char *message, const char *mentions_json);

#ifdef __cplusplus
}
#endif

#endif /* __APP_IM_H__ */

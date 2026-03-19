#ifndef __MESSAGE_BUS_H__
#define __MESSAGE_BUS_H__

#include "im_platform.h"

#define IM_CHAN_TELEGRAM  "telegram"
#define IM_CHAN_DISCORD   "discord"
#define IM_CHAN_FEISHU    "feishu"

/**
 * @brief Inbound/outbound message on the IM bus.
 *
 * mentions_json (optional): JSON array of mention targets extracted from the
 * inbound Feishu event, serialised as:
 *   [{"open_id":"ou_xxx","name":"张三"}, ...]
 * Set by feishu_bot.c on inbound, consumed by app_im.c on outbound.
 * NULL when no mentions are present or for non-Feishu channels.
 */
typedef struct {
    char  channel[16];
    char  chat_id[96];
    char *content;
    char *mentions_json;
} im_msg_t;

OPERATE_RET message_bus_init(void);
OPERATE_RET message_bus_push_inbound(const im_msg_t *msg);
OPERATE_RET message_bus_pop_inbound(im_msg_t *msg, uint32_t timeout_ms);
OPERATE_RET message_bus_push_outbound(const im_msg_t *msg);
OPERATE_RET message_bus_pop_outbound(im_msg_t *msg, uint32_t timeout_ms);

#endif /* __MESSAGE_BUS_H__ */

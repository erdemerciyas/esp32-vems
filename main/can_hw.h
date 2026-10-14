#pragma once

/* CAN controller selected in menuconfig: same API names for the MCP2515 and the internal TWAI backend. */
#include "sdkconfig.h"

#if CONFIG_VEMS_CAN_HW_TWAI
#include "can_twai.h"
#define can_hw_config_t     can_twai_config_t
#define can_hw_init         can_twai_init
#define can_hw_send         can_twai_send
#define can_hw_recv         can_twai_recv
#define can_hw_is_online    can_twai_is_online
#define can_hw_go_offline   can_twai_go_offline
#define can_hw_get_errors   can_twai_get_errors
#else
#include "mcp2515.h"
#define can_hw_config_t     mcp2515_config_t
#define can_hw_init         mcp2515_init
#define can_hw_send         mcp2515_send
#define can_hw_recv         mcp2515_recv
#define can_hw_is_online    mcp2515_is_online
#define can_hw_go_offline   mcp2515_go_offline
#define can_hw_get_errors   mcp2515_get_errors
#endif

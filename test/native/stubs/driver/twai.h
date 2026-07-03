#pragma once
// Off-target stub for ESP-IDF TWAI (CAN) types used by the pure-function core.
#include <cstdint>

// POD CAN frame matching the fields the firmware core reads/writes:
// identifier, extd, rtr, data_length_code, and the 8-byte payload.
typedef struct {
  uint32_t identifier;
  uint8_t  extd;
  uint8_t  rtr;
  uint8_t  data_length_code;
  uint8_t  data[8];
} twai_message_t;

// Driver handle is opaque on-target; a void* is sufficient off-target.
typedef void *twai_handle_t;

// TWAI alert bit constants, mirroring ESP-IDF driver/twai_types.h values.
// Native never calls CAN functions; it only needs these constants so that
// OpenHaldexC6_defs.h's CAN_ALERTS_* masks and the can_alerts_indicate_failure
// predicate compile on host.
#define TWAI_ALERT_RECOVERY_IN_PROGRESS 0x00000020
#define TWAI_ALERT_RX_DATA              0x00000004
#define TWAI_ALERT_ABOVE_ERR_WARN       0x00000100
#define TWAI_ALERT_BUS_ERROR            0x00000200
#define TWAI_ALERT_TX_FAILED            0x00000400
#define TWAI_ALERT_RX_QUEUE_FULL        0x00000800
#define TWAI_ALERT_ERR_PASS             0x00001000
#define TWAI_ALERT_BUS_OFF              0x00002000
#define TWAI_ALERT_RX_FIFO_OVERRUN      0x00004000
#define TWAI_ALERT_BUS_RECOVERED        0x00010000

#pragma once
//
// TamaNav BLE protocol, shared by the firmware and the phone-side bridge.
//
// The watch is a BLE peripheral: the phone connects, subscribes to the TX
// characteristic and writes 16-byte guidance packets to RX.  Every packet fits
// the default 23-byte ATT MTU, so no MTU negotiation is required.
//
// The phone is the only thing that knows about the navigation app; the watch
// only draws an arrow, a distance and a time.  That keeps Korean fonts and
// road-name parsing off the device.
//
// UUIDs spell "NAV" in the fourth group so the service is easy to spot in a
// BLE scanner:
//   4e415600-0000-1000-8000-00805f9b34fb  service
//   4e415601-...                          RX  phone -> watch (write / write-nr)
//   4e415602-...                          TX  watch -> phone (notify)

#include <stdint.h>

#define NAV_PROTO_VERSION 1

#define NAV_SVC_UUID "4e415600-0000-1000-8000-00805f9b34fb"
#define NAV_RX_UUID "4e415601-0000-1000-8000-00805f9b34fb"
#define NAV_TX_UUID "4e415602-0000-1000-8000-00805f9b34fb"

#define NAV_UNKNOWN16 0xFFFFu
#define NAV_UNKNOWN8 0xFFu

// Turn icons.  The phone maps its own strings (좌회전, 우회전, ...) to these,
// so the watch never sees text.
enum {
  NAV_ICON_NONE = 0,
  NAV_ICON_STRAIGHT,
  NAV_ICON_SLIGHT_LEFT,
  NAV_ICON_LEFT,
  NAV_ICON_SHARP_LEFT,
  NAV_ICON_SLIGHT_RIGHT,
  NAV_ICON_RIGHT,
  NAV_ICON_SHARP_RIGHT,
  NAV_ICON_UTURN_LEFT,
  NAV_ICON_UTURN_RIGHT,
  NAV_ICON_ROUNDABOUT,
  NAV_ICON_MERGE_LEFT,
  NAV_ICON_MERGE_RIGHT,
  NAV_ICON_EXIT_LEFT,
  NAV_ICON_EXIT_RIGHT,
  NAV_ICON_FERRY,
  NAV_ICON_ARRIVE,
  NAV_ICON_OFF_ROUTE,
  NAV_ICON_RECALC,
  NAV_ICON_COUNT
};

#define NAV_FLAG_ARRIVED 0x01
#define NAV_FLAG_OFF_ROUTE 0x02
#define NAV_FLAG_SPEED_VALID 0x04
#define NAV_FLAG_ETA_VALID 0x08

#define NAV_SRC_UNKNOWN 0
#define NAV_SRC_NAVER 1
#define NAV_SRC_KAKAO 2
#define NAV_SRC_GOOGLE 3
#define NAV_SRC_OTHER 4

#pragma pack(push, 1)

// NAV_PKT_NAV: phone -> watch, 16 bytes (extra bytes are ignored).
typedef struct {
  uint8_t type;       // NAV_PKT_NAV
  uint8_t icon;       // NAV_ICON_*
  uint16_t dist_m;    // metres to the next manoeuvre, NAV_UNKNOWN16 = unknown
  uint16_t remain_s;  // seconds to destination, NAV_UNKNOWN16 = unknown
  uint8_t flags;      // NAV_FLAG_*
  uint8_t speed_kmh;  // NAV_UNKNOWN8 = unknown
  uint8_t source;     // NAV_SRC_*
  uint8_t seq;        // wraps, lets the phone spot lost writes
  uint16_t eta_min;   // local clock time of arrival, minutes since midnight
  uint16_t total_m;   // remaining trip distance, NAV_UNKNOWN16 = unknown
  uint16_t trip_s;    // remaining trip time, NAV_UNKNOWN16 = unknown
} NavPacket;

// NAV_PKT_HELLO: watch -> phone, 8 bytes, sent on every connect.
typedef struct {
  uint8_t type;  // NAV_PKT_HELLO
  uint8_t proto; // NAV_PROTO_VERSION
  uint8_t fw_major;
  uint8_t fw_minor;
  uint8_t fw_patch;
  uint8_t screen; // 0 = round 466 px
  uint8_t reserved;
  uint8_t checksum; // XOR of bytes 0..6
} NavHello;

// NAV_PKT_STATUS: watch -> phone, 8 bytes, on connect and on PING.
typedef struct {
  uint8_t type;      // NAV_PKT_STATUS
  uint8_t battery;   // percent, 255 = unknown
  uint8_t flags;     // bit0 = USB power, bit1 = charging
  uint8_t brightness;
  uint8_t uptime_h;  // hours since boot, wraps
  uint8_t reserved[2];
  uint8_t checksum;  // XOR of bytes 0..6
} NavStatus;

#pragma pack(pop)

enum {
  NAV_PKT_NAV = 0x01,    // phone -> watch
  NAV_PKT_CLEAR = 0x02,  // phone -> watch: back to the idle screen
  NAV_PKT_PING = 0x03,   // phone -> watch: reply with NAV_PKT_STATUS
  NAV_PKT_HELLO = 0x80,  // watch -> phone
  NAV_PKT_STATUS = 0x81  // watch -> phone
};

static inline uint8_t nav_checksum(const uint8_t *p, int n) {
  uint8_t x = 0;
  for (int i = 0; i < n; i++) x ^= p[i];
  return x;
}

static inline uint16_t nav_get16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

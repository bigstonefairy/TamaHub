// TamaNav - turn-by-turn display for TamaHub v2.
//
// Lives in any 2 MB ota slot, normally ota_4.  There is no GPS and no map on
// the board: a phone app reads the navigation notification (Naver Map, Kakao
// Map, Google Maps) and writes 16-byte guidance packets over BLE.  The watch
// only draws a turn arrow, the distance to the manoeuvre and the remaining
// time, so no Korean font or road-name parsing is needed on the device.
//
// See nav_proto.h for the wire format and docs/NAVIGATION.md for the phone
// side.
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash / 8 MB PSRAM)

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// pin_config.h must come before XPowersLib so the AXP2101 typedef exists.
#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "SensorPCF85063.hpp"
#include <XPowersLib.h>
#include "multiboot.h"
#include "nav_proto.h"

#define TAMAHUB_APP_NAME "TamaNav"
#define TAMAHUB_APP_VERSION "1.0.0"

// TamaHub scans the image for these tags, so the app can live in any slot.
__attribute__((used)) const char kHubAppId[] = "TAMAHUB_APP:" TAMAHUB_APP_NAME;
__attribute__((used)) const char kHubAppVer[] = "TAMAHUB_VER:" TAMAHUB_APP_VERSION;

// ---------------------------------------------------------------- display ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// ---------------------------------------------------------------- PMU / RTC --
static XPowersPMU pmu;
static SensorPCF85063 rtc;
static bool pmuOk = false;
static bool rtcOk = false;
static int batteryPct = -1;
static bool batteryUsb = false;
static uint32_t lastBattery = 0;

// ---------------------------------------------------------------- colours ----
static const uint16_t C_BG = 0x0000;
static const uint16_t C_FG = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_LINE = 0x39E7;
static const uint16_t C_ACCENT = 0xA81F;  // purple, matches the hub card
static const uint16_t C_GO = 0x07E0;
static const uint16_t C_WARN = 0xFD20;
static const uint16_t C_BAD = 0xF800;

// ---------------------------------------------------------------- state ------
struct NavState {
  uint8_t icon;
  uint16_t dist_m;
  uint16_t remain_s;
  uint8_t flags;
  uint8_t speed_kmh;
  uint8_t source;
  uint16_t eta_min;
  uint32_t updated;  // millis() when the packet arrived
  bool has;          // guidance is on screen
};

static NavState nav;
static portMUX_TYPE navMux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool linkUp = false;
static volatile bool sendHello = false;
static volatile bool sendStatusNow = false;
static uint32_t linkChangedAt = 0;

static BLEServer *bleServer = nullptr;
static BLECharacteristic *txChar = nullptr;

static uint8_t brightness = 200;
static Preferences prefs;
static bool dirty = true;
static String serialCmd;
static uint32_t lastStatus = 0;

static bool demo = false;
static uint32_t demoAt = 0;
static uint8_t demoStep = 0;

// ---------------------------------------------------------------- helpers ----
static void textAt(const char *s, int16_t x, int16_t y, uint8_t size,
                   uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static int16_t textW(const char *s, uint8_t size) {
  return (int16_t)strlen(s) * 6 * size;
}

static void textCenter(const char *s, int16_t y, uint8_t size, uint16_t color) {
  textAt(s, (LCD_WIDTH - textW(s, size)) / 2, y, size, color);
}

// Stamped circles make a line with a real thickness, which the default font
// and drawLine() cannot provide.
static void thickLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      int16_t w, uint16_t color) {
  const int16_t dx = x2 - x1;
  const int16_t dy = y2 - y1;
  const int16_t steps = max(abs(dx), abs(dy));
  if (steps == 0) {
    gfx->fillCircle(x1, y1, w / 2, color);
    return;
  }
  for (int16_t i = 0; i <= steps; i++) {
    gfx->fillCircle(x1 + (int32_t)dx * i / steps,
                    y1 + (int32_t)dy * i / steps, w / 2, color);
  }
}

// Arrow head whose tip is (tx, ty).  Angle 0 points up; positive turns
// clockwise (screen coordinates).
static void arrowHead(int16_t tx, int16_t ty, float angle, int16_t len,
                      int16_t halfW, uint16_t color) {
  const float dx = sinf(angle);
  const float dy = -cosf(angle);
  const float bx = tx - dx * len;
  const float by = ty - dy * len;
  const float px = cosf(angle);
  const float py = sinf(angle);
  gfx->fillTriangle(tx, ty, (int16_t)(bx + px * halfW),
                    (int16_t)(by + py * halfW), (int16_t)(bx - px * halfW),
                    (int16_t)(by - py * halfW), color);
  // Close the seam between the shaft and the head.
  thickLine((int16_t)bx, (int16_t)by, tx, ty, halfW > 16 ? 12 : 8, color);
}

static void ring(int16_t cx, int16_t cy, int16_t r, int16_t w, uint16_t color) {
  for (int16_t a = 0; a < 360; a += 4) {
    const float rad = a * 0.0174533f;
    gfx->fillCircle(cx + (int16_t)(cosf(rad) * r),
                    cy + (int16_t)(sinf(rad) * r), w / 2, color);
  }
}

// ---------------------------------------------------------------- icons ------
// All turn glyphs are drawn inside a 200 px tall box around x = 233.
static const int16_t IAX = 233;

static void drawTurnIcon(uint8_t icon, uint16_t color) {
  const float kPi = 3.14159265f;
  switch (icon) {
    case NAV_ICON_STRAIGHT:
      thickLine(IAX, 280, IAX, 158, 24, color);
      arrowHead(IAX, 138, 0, 46, 40, color);
      break;

    case NAV_ICON_SLIGHT_LEFT:
      thickLine(IAX, 280, IAX - 18, 160, 24, color);
      arrowHead(IAX - 20, 140, -0.14f, 46, 40, color);
      break;

    case NAV_ICON_SLIGHT_RIGHT:
      thickLine(IAX, 280, IAX + 18, 160, 24, color);
      arrowHead(IAX + 20, 140, 0.14f, 46, 40, color);
      break;

    case NAV_ICON_LEFT:
      thickLine(IAX, 280, IAX, 226, 24, color);
      thickLine(IAX, 226, IAX - 62, 226, 24, color);
      arrowHead(IAX - 76, 226, -kPi / 2, 44, 38, color);
      break;

    case NAV_ICON_RIGHT:
      thickLine(IAX, 280, IAX, 226, 24, color);
      thickLine(IAX, 226, IAX + 62, 226, 24, color);
      arrowHead(IAX + 76, 226, kPi / 2, 44, 38, color);
      break;

    case NAV_ICON_SHARP_LEFT:
      thickLine(IAX, 280, IAX, 246, 24, color);
      thickLine(IAX, 246, IAX - 46, 192, 24, color);
      arrowHead(IAX - 60, 176, -0.785f, 44, 38, color);
      break;

    case NAV_ICON_SHARP_RIGHT:
      thickLine(IAX, 280, IAX, 246, 24, color);
      thickLine(IAX, 246, IAX + 46, 192, 24, color);
      arrowHead(IAX + 60, 176, 0.785f, 44, 38, color);
      break;

    case NAV_ICON_UTURN_LEFT:
    case NAV_ICON_UTURN_RIGHT: {
      const int16_t dir = (icon == NAV_ICON_UTURN_LEFT) ? -1 : 1;
      thickLine(IAX - dir * 42, 280, IAX - dir * 42, 218, 24, color);
      for (int16_t a = 0; a <= 180; a += 3) {
        const float rad = a * 0.0174533f;
        gfx->fillCircle(IAX - dir * (int16_t)(cosf(rad) * 42),
                        218 - (int16_t)(sinf(rad) * 42), 12, color);
      }
      thickLine(IAX + dir * 42, 218, IAX + dir * 42, 242, 24, color);
      arrowHead(IAX + dir * 42, 262, kPi, 42, 38, color);
      break;
    }

    case NAV_ICON_ROUNDABOUT:
    case NAV_ICON_RECALC: {
      const bool recalc = (icon == NAV_ICON_RECALC);
      const int16_t cx = IAX;
      const int16_t cy = recalc ? 200 : 208;
      const int16_t r = recalc ? 58 : 52;
      for (int16_t a = recalc ? 30 : 40; a <= (recalc ? 300 : 280); a += 4) {
        const float rad = a * 0.0174533f;
        gfx->fillCircle(cx + (int16_t)(cosf(rad) * r),
                        cy + (int16_t)(sinf(rad) * r), 12, color);
      }
      const float endRad = (recalc ? 330 : 310) * 0.0174533f;
      const int16_t hx = cx + (int16_t)(cosf(endRad) * r);
      const int16_t hy = cy + (int16_t)(sinf(endRad) * r);
      arrowHead(hx, hy, endRad + kPi / 2, 34, 26, color);
      break;
    }

    case NAV_ICON_MERGE_LEFT:
      thickLine(IAX, 282, IAX, 146, 22, color);
      arrowHead(IAX, 128, 0, 44, 38, color);
      thickLine(IAX - 62, 258, IAX - 8, 198, 18, color);
      break;

    case NAV_ICON_MERGE_RIGHT:
      thickLine(IAX, 282, IAX, 146, 22, color);
      arrowHead(IAX, 128, 0, 44, 38, color);
      thickLine(IAX + 62, 258, IAX + 8, 198, 18, color);
      break;

    case NAV_ICON_EXIT_LEFT:
      thickLine(IAX, 282, IAX, 206, 22, color);
      thickLine(IAX, 206, IAX - 58, 150, 22, color);
      arrowHead(IAX - 70, 138, -0.76f, 44, 38, color);
      break;

    case NAV_ICON_EXIT_RIGHT:
      thickLine(IAX, 282, IAX, 206, 22, color);
      thickLine(IAX, 206, IAX + 58, 150, 22, color);
      arrowHead(IAX + 70, 138, 0.76f, 44, 38, color);
      break;

    case NAV_ICON_FERRY: {
      thickLine(IAX, 276, IAX, 276, 24, color);
      gfx->fillTriangle(IAX - 64, 232, IAX + 64, 232, IAX + 40, 272, color);
      gfx->fillRect(IAX - 34, 206, 68, 18, color);
      thickLine(IAX, 206, IAX, 162, 12, color);
      for (int16_t y = 286; y <= 300; y += 14) {
        thickLine(IAX - 56, y, IAX - 20, y, 6, color);
        thickLine(IAX - 4, y, IAX + 32, y, 6, color);
        thickLine(IAX + 48, y, IAX + 62, y, 6, color);
      }
      break;
    }

    case NAV_ICON_ARRIVE:
      thickLine(IAX - 40, 284, IAX - 40, 130, 16, color);
      gfx->fillTriangle(IAX - 31, 132, IAX - 31, 202, IAX + 54, 167, color);
      break;

    case NAV_ICON_OFF_ROUTE:
      thickLine(IAX - 56, 142, IAX + 56, 254, 26, color);
      thickLine(IAX + 56, 142, IAX - 56, 254, 26, color);
      break;

    default:
      for (int16_t y = 280; y > 150; y -= 30) {
        thickLine(IAX, y, IAX, y - 16, 20, color);
      }
      arrowHead(IAX, 140, 0, 42, 36, color);
      break;
  }
}

// ---------------------------------------------------------------- formatting --
static void fmtDistance(char *out, size_t n, uint16_t m) {
  if (m == NAV_UNKNOWN16) {
    snprintf(out, n, "--");
  } else if (m < 1000) {
    snprintf(out, n, "%u m", (unsigned)m);
  } else if (m < 10000) {
    snprintf(out, n, "%u.%u km", (unsigned)(m / 1000),
             (unsigned)((m % 1000) / 100));
  } else {
    snprintf(out, n, "%u km", (unsigned)((m + 500) / 1000));
  }
}

static void fmtRemain(char *out, size_t n, uint16_t s) {
  if (s == NAV_UNKNOWN16) {
    snprintf(out, n, "--");
  } else if (s < 60) {
    snprintf(out, n, "<1 min");
  } else if (s < 3600) {
    snprintf(out, n, "%u min", (unsigned)(s / 60));
  } else {
    snprintf(out, n, "%uh %02um", (unsigned)(s / 3600),
             (unsigned)((s % 3600) / 60));
  }
}

static bool clockValid(const RTC_DateTime &t) {
  return t.getYear() >= 2025 && t.getYear() <= 2120;
}

static void etaText(char *out, size_t n, const NavState &s) {
  int mins = -1;
  if ((s.flags & NAV_FLAG_ETA_VALID) && s.eta_min < 1440) {
    mins = s.eta_min;
  } else if (s.remain_s != NAV_UNKNOWN16 && rtcOk) {
    RTC_DateTime t = rtc.getDateTime();
    if (clockValid(t)) {
      struct tm tmv = {};
      tmv.tm_year = t.getYear() - 1900;
      tmv.tm_mon = t.getMonth() - 1;
      tmv.tm_mday = t.getDay();
      tmv.tm_hour = t.getHour();
      tmv.tm_min = t.getMinute();
      tmv.tm_sec = t.getSecond();
      const time_t at = mktime(&tmv) + s.remain_s;
      struct tm o;
      localtime_r(&at, &o);
      mins = o.tm_hour * 60 + o.tm_min;
    }
  }
  if (mins < 0) {
    snprintf(out, n, "ETA --:--");
  } else {
    snprintf(out, n, "ETA %02d:%02d", mins / 60, mins % 60);
  }
}

static const char *sourceName(uint8_t src) {
  switch (src) {
    case NAV_SRC_NAVER: return "NAVER";
    case NAV_SRC_KAKAO: return "KAKAO";
    case NAV_SRC_GOOGLE: return "GOOGLE";
    case NAV_SRC_OTHER: return "PHONE";
    default: return "NAV";
  }
}

// ---------------------------------------------------------------- drawing ----
static void drawBattery() {
  const int16_t x = 288, y = 60, w = 24, h = 14;
  gfx->drawRect(x, y, w, h, C_DIM);
  gfx->fillRect(x + w, y + 4, 3, 6, C_DIM);
  if (batteryUsb) {
    gfx->fillRect(x + 2, y + 2, w - 4, h - 4, C_GO);
  } else if (batteryPct >= 0) {
    const uint16_t c = batteryPct <= 15 ? C_BAD : (batteryPct <= 30 ? C_WARN : C_GO);
    const int16_t inner = (w - 4) * batteryPct / 100;
    if (inner > 0) gfx->fillRect(x + 2, y + 2, inner, h - 4, c);
  }
}

static void drawIdle() {
  gfx->fillScreen(C_BG);
  gfx->drawCircle(233, 233, 224, C_LINE);

  textCenter("TamaNav", 78, 5, C_FG);
  textCenter("TURN-BY-TURN", 132, 2, C_ACCENT);

  gfx->drawCircle(233, 246, 72, C_LINE);
  thickLine(233, 302, 233, 220, 16, C_ACCENT);
  arrowHead(233, 198, 0, 40, 34, C_ACCENT);

  textCenter("PAIR YOUR PHONE", 342, 3, C_FG);
  textCenter("BLE: TamaNav", 376, 2, C_DIM);
  textCenter("Run TamaNav app", 398, 2, C_DIM);
  textCenter("BOOT 2s: HUB", 420, 1, C_DIM);

  drawBattery();
  gfx->flush();
  dirty = false;
}

static void drawGuidance(const NavState &s) {
  gfx->fillScreen(C_BG);
  gfx->drawCircle(233, 233, 224, C_LINE);

  const bool arrived = (s.flags & NAV_FLAG_ARRIVED) || s.icon == NAV_ICON_ARRIVE;
  const bool offRoute = (s.flags & NAV_FLAG_OFF_ROUTE) ||
                        s.icon == NAV_ICON_OFF_ROUTE;
  const bool stale = (uint32_t)(millis() - s.updated) > 15000;

  // Source label with a link dot.
  const char *src = sourceName(s.source);
  const int16_t sw = textW(src, 2);
  textAt(src, (LCD_WIDTH - sw) / 2, 64, 2, stale ? C_DIM : C_FG);
  const uint16_t dot = linkUp ? (stale ? C_WARN : C_GO) : C_BAD;
  gfx->fillCircle((LCD_WIDTH - sw) / 2 - 20, 70, 5, dot);
  drawBattery();

  // Turn glyph.
  drawTurnIcon(arrived ? NAV_ICON_ARRIVE : s.icon,
               offRoute ? C_WARN : C_ACCENT);

  if (offRoute) {
    textCenter("OFF ROUTE", 278, 2, C_WARN);
  } else if (!linkUp && stale) {
    textCenter("NO LINK", 278, 2, C_BAD);
  } else if (!arrived && (s.flags & NAV_FLAG_SPEED_VALID) &&
             s.speed_kmh != NAV_UNKNOWN8) {
    char sp[16];
    snprintf(sp, sizeof(sp), "%u km/h", (unsigned)s.speed_kmh);
    textCenter(sp, 278, 2, C_DIM);
  }

  if (arrived) {
    textCenter("ARRIVED", 312, 6, C_GO);
  } else {
    char dist[16];
    fmtDistance(dist, sizeof(dist), s.dist_m);
    textCenter(dist, 306, 7, offRoute ? C_WARN : C_FG);

    char rem[16];
    fmtRemain(rem, sizeof(rem), s.remain_s);
    textCenter(rem, 372, 3, C_DIM);

    char eta[20];
    etaText(eta, sizeof(eta), s);
    textCenter(eta, 400, 2, C_DIM);
  }

  gfx->flush();
  dirty = false;
}

// ---------------------------------------------------------------- BLE --------
class NavServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    linkUp = true;
    linkChangedAt = millis();
    sendHello = true;
    dirty = true;
  }

  void onDisconnect(BLEServer *server) override {
    linkUp = false;
    linkChangedAt = millis();
    dirty = true;
    BLEDevice::startAdvertising();
  }
};

static void storeNav(const NavState &in) {
  portENTER_CRITICAL(&navMux);
  nav = in;
  portEXIT_CRITICAL(&navMux);
}

static void parsePacket(const uint8_t *d, size_t n) {
  if (n < 10) return;

  NavState t = {};
  t.icon = d[1];
  t.dist_m = nav_get16(d + 2);
  t.remain_s = nav_get16(d + 4);
  t.flags = d[6];
  t.speed_kmh = d[7];
  t.source = d[8];
  t.eta_min = (n >= 12) ? nav_get16(d + 10) : NAV_UNKNOWN16;
  t.updated = millis();
  t.has = true;

  if (t.icon >= NAV_ICON_COUNT) t.icon = NAV_ICON_NONE;
  if (t.source > NAV_SRC_OTHER) t.source = NAV_SRC_UNKNOWN;

  portENTER_CRITICAL(&navMux);
  nav = t;
  portEXIT_CRITICAL(&navMux);
  dirty = true;
}

static void clearNav() {
  portENTER_CRITICAL(&navMux);
  nav = NavState{};
  portEXIT_CRITICAL(&navMux);
  dirty = true;
}

class NavRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const uint8_t *d = c->getData();
    const size_t n = c->getLength();
    if (!n) return;
    switch (d[0]) {
      case NAV_PKT_NAV: parsePacket(d, n); break;
      case NAV_PKT_CLEAR: clearNav(); break;
      case NAV_PKT_PING: sendStatusNow = true; break;
      default: break;
    }
  }
};

static uint8_t statusChecksum(const uint8_t *p) { return nav_checksum(p, 7); }

static void bleBegin() {
  BLEDevice::init("TamaNav");
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new NavServerCallbacks());

  BLEService *svc = bleServer->createService(BLEUUID(NAV_SVC_UUID));

  BLECharacteristic *rx = svc->createCharacteristic(
      BLEUUID(NAV_RX_UUID),
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new NavRxCallbacks());

  txChar = svc->createCharacteristic(
      BLEUUID(NAV_TX_UUID),
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);
  txChar->addDescriptor(new BLE2902());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLEUUID(NAV_SVC_UUID));
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

static void bleSendStatus() {
  if (!txChar || !linkUp) return;
  NavStatus st = {};
  st.type = NAV_PKT_STATUS;
  st.battery = (batteryPct >= 0) ? (uint8_t)batteryPct : NAV_UNKNOWN8;
  st.flags = (batteryUsb ? 1 : 0);
  st.brightness = brightness;
  st.uptime_h = (uint8_t)(millis() / 3600000UL);
  st.checksum = statusChecksum((const uint8_t *)&st);
  txChar->setValue((const uint8_t *)&st, sizeof(st));
  txChar->notify();
}

static void bleSendHello() {
  if (!txChar || !linkUp) return;
  NavHello h = {};
  h.type = NAV_PKT_HELLO;
  h.proto = NAV_PROTO_VERSION;
  h.fw_major = 1;
  h.fw_minor = 0;
  h.fw_patch = 0;
  h.screen = 0;
  h.checksum = nav_checksum((const uint8_t *)&h, 7);
  txChar->setValue((const uint8_t *)&h, sizeof(h));
  txChar->notify();
}

// ---------------------------------------------------------------- PMU --------
static void readBattery() {
  if (!pmuOk) {
    batteryPct = -1;
    batteryUsb = false;
    return;
  }
  batteryUsb = pmu.isVbusIn();
  batteryPct = pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1;
}

static void applyBrightness(uint8_t b) {
  brightness = b;
  panel->setBrightness(b);
  prefs.putUChar("bright", b);
}

// ---------------------------------------------------------------- serial -----
static void applyDemoStep() {
  static const uint8_t icons[] = {NAV_ICON_STRAIGHT,  NAV_ICON_SLIGHT_RIGHT,
                                  NAV_ICON_RIGHT,     NAV_ICON_ROUNDABOUT,
                                  NAV_ICON_LEFT,      NAV_ICON_SHARP_LEFT,
                                  NAV_ICON_UTURN_LEFT, NAV_ICON_MERGE_RIGHT,
                                  NAV_ICON_EXIT_LEFT, NAV_ICON_ARRIVE};
  NavState t = nav;
  t.has = true;
  t.icon = icons[demoStep % (sizeof(icons) / sizeof(icons[0]))];
  t.dist_m = (uint16_t)(80 + (demoStep % 12) * 120);
  t.remain_s = (uint16_t)((12 - (demoStep % 12)) * 60);
  t.flags = NAV_FLAG_SPEED_VALID | NAV_FLAG_ETA_VALID;
  t.speed_kmh = 42;
  t.source = NAV_SRC_NAVER;
  t.eta_min = 18 * 60 + 42;
  t.updated = millis();
  storeNav(t);
  demoStep++;
  dirty = true;
}

static void handleCommand(const String &c) {
  if (c == "STATUS" || c == "R") {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    Serial.printf("nav has=%d icon=%u dist=%u remain=%u flags=0x%02x spd=%u "
                  "src=%u link=%d bright=%u bat=%d%% usb=%d demo=%d\n",
                  s.has, (unsigned)s.icon, (unsigned)s.dist_m,
                  (unsigned)s.remain_s, (unsigned)s.flags,
                  (unsigned)s.speed_kmh, (unsigned)s.source, linkUp ? 1 : 0,
                  (unsigned)brightness, batteryPct, batteryUsb ? 1 : 0,
                  demo ? 1 : 0);
  } else if (c.startsWith("ICON=")) {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.icon = (uint8_t)constrain(c.substring(5).toInt(), 0, NAV_ICON_COUNT - 1);
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c.startsWith("DIST=")) {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.dist_m = (uint16_t)constrain(c.substring(5).toInt(), 0, 65534);
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c.startsWith("REMAIN=")) {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.remain_s = (uint16_t)constrain(c.substring(7).toInt(), 0, 65534);
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c.startsWith("SPD=")) {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.speed_kmh = (uint8_t)constrain(c.substring(4).toInt(), 0, 254);
    s.flags |= NAV_FLAG_SPEED_VALID;
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c.startsWith("SRC=")) {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.source = (uint8_t)constrain(c.substring(4).toInt(), 0, NAV_SRC_OTHER);
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c == "ARRIVE") {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.icon = NAV_ICON_ARRIVE;
    s.flags |= NAV_FLAG_ARRIVED;
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c == "OFFROUTE") {
    NavState s;
    portENTER_CRITICAL(&navMux);
    s = nav;
    portEXIT_CRITICAL(&navMux);
    s.has = true;
    s.icon = NAV_ICON_OFF_ROUTE;
    s.flags |= NAV_FLAG_OFF_ROUTE;
    s.updated = millis();
    storeNav(s);
    dirty = true;
  } else if (c == "CLR") {
    demo = false;
    clearNav();
  } else if (c == "DEMO") {
    demo = !demo;
    demoAt = 0;
    if (!demo) clearNav();
    Serial.printf("demo %s\n", demo ? "on" : "off");
  } else if (c.startsWith("BRIGHT=")) {
    applyBrightness((uint8_t)constrain(c.substring(7).toInt(), 1, 255));
  } else if (c.length()) {
    Serial.println("commands: STATUS | ICON=n | DIST=m | REMAIN=s | SPD=k | SRC=n |"
                   " ARRIVE | OFFROUTE | CLR | DEMO | BRIGHT=n");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    const char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      serialCmd.trim();
      if (serialCmd.length()) handleCommand(serialCmd);
      serialCmd = "";
    } else if (serialCmd.length() < 63) {
      serialCmd += ch;
    }
  }
}

// ---------------------------------------------------------------- setup ------
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(150);
  Serial.println();
  Serial.printf("TamaNav v%s\n", TAMAHUB_APP_VERSION);
  Serial.printf("hub tag: %s %s\n", kHubAppId, kHubAppVer);

  mboot::begin();
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setTimeOut(50);

  // Panel power (BLDO1) must be on before gfx->begin().
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    pmu.setBLDO1Voltage(3300);
    pmu.enableBLDO1();
  } else {
    Serial.println("AXP2101 not detected");
  }

  if (!gfx->begin(80000000)) Serial.println("gfx->begin() failed");

  prefs.begin("tamanav", false);
  brightness = prefs.getUChar("bright", 200);
  if (brightness < 10) brightness = 200;
  panel->setBrightness(brightness);

  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 not detected");

  readBattery();
  bleBegin();
  Serial.println("BLE up as TamaNav");

  drawIdle();
}

// ---------------------------------------------------------------- loop -------
void loop() {
  static mboot::Hold mbHold;
  if (mbHold.reached()) {
    panel->setBrightness(180);
    mboot::screenMessage(gfx, "RETURNING", "release the button", C_GO);
    mboot::waitRelease();
    if (!mboot::hubLooksValid()) {
      Serial.println("multiboot: factory slot is empty, staying in TamaNav");
      mboot::screenMessage(gfx, "HUB MISSING", "reinstall from a PC", C_WARN);
      delay(2500);
      panel->setBrightness(brightness);
      dirty = true;
    } else {
      mboot::returnToHub();
    }
  }

  if (mbHold.shortPress()) {
    static const uint8_t levels[] = {60, 120, 200, 255};
    const size_t count = sizeof(levels) / sizeof(levels[0]);
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
      if (brightness >= levels[i]) index = i;
    }
    applyBrightness(levels[(index + 1) % count]);
    dirty = true;
  }

  pollSerial();

  if (sendHello) {
    sendHello = false;
    bleSendHello();
    bleSendStatus();
  }
  if (sendStatusNow) {
    sendStatusNow = false;
    bleSendStatus();
  }

  const uint32_t now = millis();
  if (now - lastBattery >= 5000) {
    lastBattery = now;
    const int before = batteryPct;
    readBattery();
    if (batteryPct != before) dirty = true;
    if (linkUp && now - lastStatus >= 30000) {
      lastStatus = now;
      bleSendStatus();
    }
  }

  if (demo && now - demoAt >= 3000) {
    demoAt = now;
    applyDemoStep();
  }

  NavState s;
  portENTER_CRITICAL(&navMux);
  s = nav;
  portEXIT_CRITICAL(&navMux);

  // Re-render every second while navigating so the link/staleness dot and the
  // silent ETA stay current, and immediately on any state change.
  static uint32_t lastTick = 0;
  static NavState lastDrawn = {};
  static bool wasLinkUp = false;
  if (dirty || now - lastTick >= 1000 || linkUp != wasLinkUp ||
      memcmp(&s, &lastDrawn, sizeof(s)) != 0) {
    lastTick = now;
    lastDrawn = s;
    wasLinkUp = linkUp;
    if (s.has) {
      drawGuidance(s);
    } else {
      drawIdle();
    }
  }

  delay(20);
}

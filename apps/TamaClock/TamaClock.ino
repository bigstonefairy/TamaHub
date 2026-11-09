// TamaClock - a standalone RTC clock for TamaHub v2.
//
// Lives in any 2 MB ota slot.  It reuses the PCF85063 that the games already
// keep set, so no new time source is needed: whichever game last adjusted the
// clock is the source of truth.  The serial commands below are for a board
// whose RTC has never been set.
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash / 8 MB PSRAM)

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

// pin_config.h must come before XPowersLib so the AXP2101 typedef exists.
#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "SensorPCF85063.hpp"
#include <XPowersLib.h>
#include "multiboot.h"

#define TAMAHUB_APP_NAME "TamaClock"
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

static const uint16_t C_BG = 0x0000;
static const uint16_t C_FG = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_ACCENT = 0xFFE0;  // yellow
static const uint16_t C_WARN = 0xFD20;    // orange

static bool dirty = true;
static bool use24h = true;
static int batteryPct = -1;
static bool batteryUsb = false;
static uint32_t lastBattery = 0;
static int lastSecond = -1;
static String serialCmd;

// ---------------------------------------------------------------- helpers ----
static void text(const char *s, int16_t x, int16_t y, uint8_t size,
                 uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void textCenter(const char *s, int16_t y, uint8_t size, uint16_t color) {
  int16_t w = (int16_t)strlen(s) * 6 * size;
  text(s, (LCD_WIDTH - w) / 2, y, size, color);
}

static void readBattery() {
  if (!pmuOk) {
    batteryPct = -1;
    batteryUsb = false;
    return;
  }
  batteryUsb = pmu.isVbusIn();
  batteryPct = pmu.getBatteryPercent();
}

static bool clockValid(const RTC_DateTime &t) {
  return t.getYear() >= 2025 && t.getYear() <= 2120;
}

static int daysInMonth(int y, int m) {
  static const uint8_t days[12] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return days[m - 1];
}

// The PCF85063 stores a weekday register, but the games set the date through
// the library's default (week = 0), so derive it from the date instead.
static int weekdayOf(int y, int m, int d) {
  struct tm tmv = {};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = m - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = 12;
  mktime(&tmv);
  return tmv.tm_wday;  // 0 = Sunday
}

static bool applyDateTime(int y, int mo, int d, int h, int mi, int s) {
  if (y < 2025 || y > 2120 || mo < 1 || mo > 12 || d < 1 ||
      d > daysInMonth(y, mo) || h < 0 || h > 23 || mi < 0 || mi > 59 ||
      s < 0 || s > 59) {
    return false;
  }
  struct tm tmv = {};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = h;
  tmv.tm_min = mi;
  tmv.tm_sec = s;
  mktime(&tmv);
  rtc.setDateTime(RTC_DateTime((uint16_t)y, (uint8_t)mo, (uint8_t)d,
                               (uint8_t)h, (uint8_t)mi, (uint8_t)s,
                               (uint8_t)tmv.tm_wday));
  dirty = true;
  return true;
}

// ---------------------------------------------------------------- drawing ----
static void draw() {
  gfx->fillScreen(C_BG);
  gfx->drawCircle(233, 233, 196, C_ACCENT);

  RTC_DateTime t = rtc.getDateTime();
  const bool ok = rtcOk && clockValid(t);

  if (!ok) {
    textCenter("TamaClock", 116, 3, C_FG);
    textCenter("--:--", 172, 8, C_ACCENT);
    textCenter("SET TIME VIA USB", 292, 2, C_DIM);
    textCenter("SET=YYYY-MM-DD HH:MM:SS", 318, 1, C_DIM);
    textCenter("115200 baud", 338, 2, C_DIM);
  } else {
    const int hour = t.getHour();
    const int minute = t.getMinute();
    char hm[10];
    if (use24h) {
      snprintf(hm, sizeof(hm), "%02d:%02d", hour, minute);
    } else {
      int h12 = hour % 12;
      if (h12 == 0) h12 = 12;
      snprintf(hm, sizeof(hm), "%02d:%02d", h12, minute);
      textCenter(hour < 12 ? "AM" : "PM", 134, 2, C_ACCENT);
    }
    textCenter(hm, 170, 8, C_FG);

    char ss[8];
    snprintf(ss, sizeof(ss), ":%02d", t.getSecond());
    textCenter(ss, 246, 3, C_ACCENT);

    char date[16];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", t.getYear(), t.getMonth(),
             t.getDay());
    textCenter(date, 300, 3, C_FG);

    static const char *const WD[] = {"SUNDAY",    "MONDAY",   "TUESDAY",
                                     "WEDNESDAY", "THURSDAY", "FRIDAY",
                                     "SATURDAY"};
    const int wd = weekdayOf(t.getYear(), t.getMonth(), t.getDay());
    textCenter(WD[wd % 7], 334, 2, C_DIM);
  }

  char bat[24];
  if (!pmuOk) {
    snprintf(bat, sizeof(bat), "BAT --");
  } else if (batteryUsb) {
    snprintf(bat, sizeof(bat), "USB");
  } else if (batteryPct >= 0) {
    snprintf(bat, sizeof(bat), "BAT %d%%", batteryPct);
  } else {
    snprintf(bat, sizeof(bat), "BAT --");
  }
  const bool low = batteryPct >= 0 && batteryPct <= 15 && !batteryUsb;
  textCenter(bat, 372, 2, low ? C_WARN : C_DIM);

  textCenter("TamaClock v" TAMAHUB_APP_VERSION, 402, 1, C_DIM);
  textCenter("BOOT 2s: HUB   /   BOOT: 12/24H", 422, 1, C_DIM);

  gfx->flush();
  dirty = false;
}

// ---------------------------------------------------------------- serial -----
static void handleCommand(const String &c) {
  if (c.startsWith("SET=")) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const char *p = c.c_str() + 4;
    const bool parsed = sscanf(p, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 ||
                        sscanf(p, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6;
    if (!parsed) {
      Serial.println("use SET=YYYY-MM-DD HH:MM:SS");
    } else if (applyDateTime(y, mo, d, h, mi, s)) {
      Serial.println("time set");
    } else {
      Serial.println("bad date/time");
    }
  } else if (c.startsWith("TIME=")) {
    const time_t tt = (time_t)strtoul(c.c_str() + 5, nullptr, 10);
    struct tm tmv = {};
    gmtime_r(&tt, &tmv);
    if (applyDateTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                      tmv.tm_hour, tmv.tm_min, tmv.tm_sec)) {
      Serial.println("time set (UTC fields)");
    } else {
      Serial.println("bad epoch");
    }
  } else if (c == "12H") {
    use24h = false;
    dirty = true;
    Serial.println("12h");
  } else if (c == "24H") {
    use24h = true;
    dirty = true;
    Serial.println("24h");
  } else if (c == "STATUS" || c == "R") {
    RTC_DateTime t = rtc.getDateTime();
    Serial.printf("rtc=%s %04u-%02u-%02u %02u:%02u:%02u bat=%d%% usb=%d\n",
                  rtcOk && clockValid(t) ? "ok" : "unset",
                  (unsigned)t.getYear(), (unsigned)t.getMonth(),
                  (unsigned)t.getDay(), (unsigned)t.getHour(),
                  (unsigned)t.getMinute(), (unsigned)t.getSecond(), batteryPct,
                  batteryUsb ? 1 : 0);
  } else if (c.length()) {
    Serial.println("commands: SET=YYYY-MM-DD HH:MM:SS | TIME=<epoch> | 12H | 24H | STATUS");
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
  Serial.printf("TamaClock v%s\n", TAMAHUB_APP_VERSION);
  Serial.printf("hub tag: %s %s\n", kHubAppId, kHubAppVer);  // keep tags in the image
  Serial.println("commands: SET=YYYY-MM-DD HH:MM:SS | TIME=<epoch> | 12H | 24H | STATUS");

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
  panel->setBrightness(180);

  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 not detected");

  readBattery();
  draw();
}

// ---------------------------------------------------------------- loop -------
void loop() {
  static mboot::Hold mbHold;
  if (mbHold.reached()) {
    panel->setBrightness(180);
    mboot::screenMessage(gfx, "RETURNING", "release the button", 0x07E0);
    mboot::waitRelease();
    if (!mboot::hubLooksValid()) {
      Serial.println("multiboot: factory slot is empty, staying in clock");
      mboot::screenMessage(gfx, "HUB MISSING", "reinstall from a PC", 0xFD20);
      delay(2500);
      dirty = true;
    } else {
      mboot::returnToHub();
    }
  }

  if (mbHold.shortPress()) {
    use24h = !use24h;
    dirty = true;
  }

  pollSerial();

  uint32_t now = millis();
  RTC_DateTime t = rtc.getDateTime();
  const int sec = (rtcOk && clockValid(t)) ? t.getSecond() : -1;
  if (sec != lastSecond) {
    lastSecond = sec;
    dirty = true;
  }
  if (now - lastBattery >= 5000) {
    lastBattery = now;
    readBattery();
    dirty = true;
  }
  if (dirty) draw();
  delay(40);
}

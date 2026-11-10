// TamaClock v1.1 - a standalone RTC clock for TamaHub v2.
//
// Lives in any 2 MB ota slot.  It reuses the PCF85063 the games already keep
// running, so no new time source is needed.  The games seed the RTC with a
// fixed date (they only care about elapsed time), so this app keeps a
// whole-day display offset computed from its build date: the date is real,
// the games' clock is untouched.  See docs/CLOCK.md.
//
// Gestures:
//   swipe up / down  previous / next face
//   long press       next theme colour
//   tap             12 / 24 hour
//   PWR (short)     screen off / on
//   BOOT (short)    12 / 24 hour, or wake
//   BOOT (2 s)      return to TamaHub
//
// Faces: digital, Pokemon, Digimon, Mario and Zelda.  Themed faces play an
// hourly animation; an SD pack in /tamaclock/<name>.tnm replaces the built-in
// procedural scene.
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash / 8 MB PSRAM)

#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <FS.h>
#include <SD_MMC.h>

// pin_config.h must come before XPowersLib so the AXP2101 typedef exists.
#include "pin_config.h"
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorPCF85063.hpp"
#include <XPowersLib.h>
#include "multiboot.h"
#include "clockpack.h"
#include "scenes.h"

#define TAMAHUB_APP_NAME "TamaClock"
#define TAMAHUB_APP_VERSION "1.1.0"

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
static bool batteryChg = false;
static uint32_t lastBattery = 0;

// ---------------------------------------------------------------- touch ------
static TouchDrvCST92xx touch;
#define TOUCH_ADDR 0x5A
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }

// ---------------------------------------------------------------- colours ----
static const uint16_t C_BG = 0x0000;
static const uint16_t C_FG = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_LINE = 0x39E7;
static const uint16_t C_GO = 0x07E0;
static const uint16_t C_WARN = 0xFD20;
static const uint16_t C_BAD = 0xF800;
static const uint16_t C_FILL = 0x2104;

struct Theme {
  uint16_t color;
  const char *name;
};

// Yellow first: it is the colour the clock shipped with.
static const Theme THEMES[] = {
    {0xFFE0, "YELLOW"}, {0x07FF, "CYAN"},  {0x07E0, "GREEN"},
    {0xFD20, "ORANGE"}, {0xF81F, "PINK"},  {0xFFFF, "WHITE"},
    {0xA81F, "PURPLE"},
};
static const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

// ---------------------------------------------------------------- faces ------
enum Face {
  FACE_DIGITAL = 0,
  FACE_POKE,
  FACE_DIGI,
  FACE_MARIO,
  FACE_ZELDA,
  FACE_COUNT
};
static const char *const FACE_NAMES[FACE_COUNT] = {"DIGITAL", "POKEMON",
                                                   "DIGIMON", "MARIO",
                                                   "ZELDA"};
static const char *const PACK_NAMES[FACE_COUNT] = {"", "poke", "digi", "mario",
                                                   "zelda"};

// ---------------------------------------------------------------- state ------
static Preferences prefs;
static ClockPack pack;
static bool sdReady = false;

static uint8_t face = FACE_DIGITAL;
static uint8_t theme = 0;
static uint8_t brightness = 200;
static bool use24h = true;
static bool asleep = false;

static int32_t dateOffset = 0;
static bool clockOk = false;

// Cached wall clock: the RTC is on I2C, so the render path must not poll it
// every loop iteration.
static struct tm gTm = {};
static uint32_t gEpoch = 0;
static bool gClockOk = false;
static uint32_t gClockAt = 0;

static bool dirty = true;
static uint32_t lastDraw = 0;
static uint32_t lastSecond = 0xFFFFFFFF;

static char toastMsg[20] = "";
static uint32_t toastUntil = 0;

static uint32_t forceShowStart = 0;

static String serialCmd;

// Touch gesture state.
static bool touchWas = false;
static int16_t touchX0 = 0, touchY0 = 0;
static int16_t touchXl = 0, touchYl = 0;
static uint32_t touchT0 = 0;
static bool touchConsumed = false;
static uint32_t lastTouchPoll = 0;

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

static uint16_t accent() { return THEMES[theme].color; }

static void flashToast(const char *msg) {
  snprintf(toastMsg, sizeof(toastMsg), "%s", msg);
  toastUntil = millis() + 900;
  dirty = true;
}

static void drawToast(int16_t y) {
  if (!toastMsg[0] || (int32_t)(millis() - toastUntil) >= 0) return;
  const int16_t w = textW(toastMsg, 2) + 16;
  gfx->fillRoundRect((LCD_WIDTH - w) / 2, y - 4, w, 22, 10, C_FILL);
  textCenter(toastMsg, y, 2, accent());
}

// ---------------------------------------------------------------- clock ------
static uint32_t buildEpoch() {
  static uint32_t cached = 0;
  if (cached) return cached;
  char mon[4] = {0};
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) return 0;
  if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return 0;
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
  int m = -1;
  for (int i = 0; i < 12; i++) {
    if (!strcmp(mon, MONTHS[i])) m = i;
  }
  if (m < 0) return 0;
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = m;
  t.tm_mday = day;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  cached = (uint32_t)mktime(&t);
  return cached;
}

static bool rtcNow(struct tm *out) {
  if (!rtcOk) return false;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2025 || t.getYear() > 2120) return false;
  memset(out, 0, sizeof(*out));
  out->tm_year = t.getYear() - 1900;
  out->tm_mon = t.getMonth() - 1;
  out->tm_mday = t.getDay();
  out->tm_hour = t.getHour();
  out->tm_min = t.getMinute();
  out->tm_sec = t.getSecond();
  mktime(out);  // normalises tm_wday / tm_yday
  return true;
}

static uint32_t rtcEpochNow() {
  struct tm t;
  if (!rtcNow(&t)) return 0;
  return (uint32_t)mktime(&t);
}

// Seconds added to the RTC clock for display.  Always a whole number of days
// so the time of day stays exactly what the games maintain.
static uint32_t displayEpoch() {
  const uint32_t e = rtcEpochNow();
  if (!e) return 0;
  return (uint32_t)((int64_t)e + dateOffset);
}

// The games seed a virgin RTC with a fixed date, so every TamaClock build
// recalibrates the offset from its own build date.  Reflashing keeps the date
// right; if the RTC ever holds a real date the offset collapses to zero.
static void calibrateDate() {
  const uint32_t be = buildEpoch();
  const uint32_t re = rtcEpochNow();
  const uint32_t stamp = prefs.getUInt("stamp", 0);
  if (!be || !re) {
    dateOffset = prefs.getInt("off", 0);
    return;
  }
  if (stamp != be) {
    const int64_t buildDay = (int64_t)be / 86400;
    const int64_t rtcDay = (int64_t)re / 86400;
    dateOffset = (int32_t)((buildDay - rtcDay) * 86400);
    prefs.putInt("off", dateOffset);
    prefs.putUInt("stamp", be);
    Serial.printf("date: offset %+d day(s) from the build stamp\n",
                  (int)(buildDay - rtcDay));
  } else {
    dateOffset = prefs.getInt("off", 0);
  }
}

// Moves the displayed clock without touching the RTC the games use.
static void shiftDisplayTo(uint32_t target) {
  const uint32_t nowE = displayEpoch();
  if (!nowE) {
    flashToast("RTC UNSET");
    return;
  }
  dateOffset += (int32_t)(target - nowE);
  prefs.putInt("off", dateOffset);
  dirty = true;
}

// 5 Hz is plenty for a clock and keeps I2C traffic low.
static void refreshClock() {
  const uint32_t now = millis();
  if (gClockAt && now - gClockAt < 200) return;
  gClockAt = now ? now : 1;
  gEpoch = displayEpoch();
  gClockOk = gEpoch != 0;
  if (gClockOk) {
    time_t tt = (time_t)gEpoch;
    gmtime_r(&tt, &gTm);
  }
}

// ---------------------------------------------------------------- battery ----
static void readBattery() {
  if (!pmuOk) {
    batteryPct = -1;
    batteryUsb = false;
    batteryChg = false;
    return;
  }
  batteryUsb = pmu.isVbusIn();
  batteryChg = pmu.isCharging();
  batteryPct = pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1;
}

static void drawBattery() {
  const int16_t w = 26, h = 14, y = 40;
  const int16_t x = 199;
  const uint16_t level = batteryPct <= 15 ? C_BAD
                        : batteryPct <= 30 ? C_WARN
                                          : C_GO;
  gfx->drawRect(x, y, w, h, C_DIM);
  gfx->fillRect(x + w, y + 4, 3, 6, C_DIM);
  if (batteryUsb) {
    gfx->fillRect(x + 2, y + 2, w - 4, h - 4, batteryChg ? C_GO : C_LINE);
    // Small bolt so USB power is unmistakable even at 100 %.
    gfx->fillTriangle(x + 15, y + 2, x + 9, y + 8, x + 14, y + 8, C_BG);
    gfx->fillTriangle(x + 10, y + 12, x + 16, y + 6, x + 11, y + 6, C_BG);
  } else if (batteryPct >= 0) {
    const int16_t inner = (w - 4) * batteryPct / 100;
    if (inner > 0) gfx->fillRect(x + 2, y + 2, inner, h - 4, level);
  }

  char buf[8];
  if (batteryPct >= 0) {
    snprintf(buf, sizeof(buf), "%d%%", batteryPct);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  textAt(buf, x + w + 8, y + 1, 2, batteryUsb ? C_GO : C_DIM);
}

// ---------------------------------------------------------------- chrome -----
static void drawRing() {
  gfx->drawCircle(233, 233, 224, C_LINE);
}

static void drawFaceDots(int16_t y) {
  const int16_t spacing = 24;
  const int16_t x0 = 233 - (FACE_COUNT - 1) * spacing / 2;
  for (int i = 0; i < FACE_COUNT; i++) {
    gfx->fillCircle(x0 + i * spacing, y, i == face ? 6 : 4,
                    i == face ? accent() : C_LINE);
  }
}

// Seconds as 60 ticks just inside the ring; that keeps the big time text
// uncluttered.
static void drawSecondTicks(int sec) {
  for (int i = 0; i < 60; i += 2) {
    const float a = (i * 6.0f - 90.0f) * 0.0174533f;
    const int16_t x = 233 + (int16_t)(cosf(a) * 213);
    const int16_t y = 233 + (int16_t)(sinf(a) * 213);
    gfx->fillCircle(x, y, i <= sec ? 3 : 2, i <= sec ? accent() : C_LINE);
  }
}

static int hour12(int h) {
  int r = h % 12;
  return r == 0 ? 12 : r;
}

// ---------------------------------------------------------------- digital ----
static void drawDigitalFace() {
  gfx->fillScreen(C_BG);
  drawRing();
  drawBattery();

  struct tm t;
  refreshClock();
  clockOk = gClockOk && gEpoch != 0;
  t = gTm;
  if (clockOk) {
    time_t tt = (time_t)gEpoch;
    gmtime_r(&tt, &t);
  }

  char hm[10] = "--:--";
  if (clockOk) {
    if (use24h) {
      snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
      snprintf(hm, sizeof(hm), "%d:%02d", hour12(t.tm_hour), t.tm_min);
    }
  }
  textCenter(hm, 92, 12, accent());
  drawSecondTicks(clockOk ? t.tm_sec : 0);

  char date[24] = "RTC NOT SET";
  char wd[16] = "";
  if (clockOk) {
    if (use24h) {
      snprintf(date, sizeof(date), "%04d-%02d-%02d", t.tm_year + 1900,
               t.tm_mon + 1, t.tm_mday);
    } else {
      snprintf(date, sizeof(date), "%04d-%02d-%02d %s", t.tm_year + 1900,
               t.tm_mon + 1, t.tm_mday, t.tm_hour < 12 ? "AM" : "PM");
    }
    static const char *const WD[] = {"SUNDAY",    "MONDAY",   "TUESDAY",
                                     "WEDNESDAY", "THURSDAY", "FRIDAY",
                                     "SATURDAY"};
    snprintf(wd, sizeof(wd), "%s", WD[t.tm_wday % 7]);
  }
  textCenter(date, 202, 3, C_FG);
  if (clockOk) textCenter(wd, 234, 2, C_DIM);

  drawFaceDots(282);
  textCenter("SWIPE: FACE   TAP: 12/24", 342, 2, C_DIM);
  textCenter("HOLD: COLOR   PWR: SLEEP", 366, 2, C_DIM);
  textCenter("TamaClock v" TAMAHUB_APP_VERSION, 400, 1, C_DIM);
  drawToast(310);

  gfx->flush();
  dirty = false;
}

// ---------------------------------------------------------------- themed -----
static float hourPhase() {
  const uint32_t now = millis();
  if (forceShowStart && now - forceShowStart < 20000) {
    return (now - forceShowStart) / 20000.0f;
  }
  refreshClock();
  if (!gClockOk) return -1.0f;
  if (gTm.tm_min == 0 && gTm.tm_sec < 20) return gTm.tm_sec / 20.0f;
  return -1.0f;
}

static void drawThemedFace() {
  gfx->fillScreen(C_BG);
  drawRing();
  drawBattery();

  refreshClock();
  struct tm t = {};
  clockOk = gClockOk && gEpoch != 0;
  if (clockOk) {
    time_t tt = (time_t)gEpoch;
    gmtime_r(&tt, &t);
  }

  char hm[10] = "--:--";
  if (clockOk) {
    if (use24h) {
      snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
    } else {
      snprintf(hm, sizeof(hm), "%d:%02d", hour12(t.tm_hour), t.tm_min);
    }
  }
  textCenter(hm, 62, 8, accent());

  char date[24] = "";
  if (clockOk) {
    snprintf(date, sizeof(date), "%04d-%02d-%02d  %s", t.tm_year + 1900,
             t.tm_mon + 1, t.tm_mday, FACE_NAMES[face]);
  } else {
    snprintf(date, sizeof(date), "RTC NOT SET");
  }
  textCenter(date, 344, 2, C_DIM);

  const float hp = hourPhase();
  bool packed = false;
  if (pack.ready()) {
    uint16_t frame = 0;
    const uint16_t fps = pack.fps();
    if (hp >= 0 && pack.hourCount() > 0) {
      uint16_t idx = (uint16_t)(hp * pack.hourCount());
      if (idx >= pack.hourCount()) idx = pack.hourCount() - 1;
      frame = pack.hourStart() + idx;
    } else {
      const uint16_t idle = pack.hourStart() ? pack.hourStart() : pack.frames();
      // Split the multiply so millis() * fps cannot overflow after a week.
      const uint32_t secs = millis() / 1000UL;
      const uint32_t frac = (millis() % 1000UL) * fps / 1000UL;
      frame = (uint16_t)((secs * fps + frac) % idle);
    }
    packed = pack.draw(gfx, frame, 233, 238);
  }
  if (!packed) {
    switch (face) {
      case FACE_POKE:
        scenes::poke(gfx, 233, 238, millis(), hp, accent());
        break;
      case FACE_DIGI:
        scenes::digi(gfx, 233, 238, millis(), hp, accent());
        break;
      case FACE_MARIO:
        scenes::mario(gfx, 233, 238, millis(), hp, accent());
        break;
      case FACE_ZELDA:
        scenes::zelda(gfx, 233, 238, millis(), hp, accent());
        break;
      default:
        break;
    }
  }

  drawFaceDots(376);
  drawToast(310);
  gfx->flush();
  dirty = false;
}

static void draw() {
  if (face == FACE_DIGITAL) {
    drawDigitalFace();
  } else {
    drawThemedFace();
  }
  lastDraw = millis();
}

// ---------------------------------------------------------------- SD ---------
static void loadPack() {
  pack.close();
  if (!sdReady || face == FACE_DIGITAL) return;
  char path[40];
  snprintf(path, sizeof(path), "/tamaclock/%s.tnm", PACK_NAMES[face]);
  if (!pack.open(path, face + 1)) {
    Serial.printf("no pack at %s (procedural scene)\n", path);
  }
}

static bool sdMount() {
  SD_MMC.end();
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  sdReady = SD_MMC.begin("/sdcard", true /* 1-bit */, false /* never format */);
  if (sdReady) {
    if (!SD_MMC.exists("/tamaclock")) SD_MMC.mkdir("/tamaclock");
    Serial.printf("SD: %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  } else {
    Serial.println("SD: not detected (procedural faces only)");
  }
  return sdReady;
}

// ---------------------------------------------------------------- sleep ------
static void enterSleep() {
  if (asleep) return;
  asleep = true;
  panel->setBrightness(0);
  panel->displayOff();
  Serial.println("sleep");
}

static void leaveSleep() {
  if (!asleep) return;
  asleep = false;
  panel->displayOn();
  panel->setBrightness(brightness);
  dirty = true;
  Serial.println("wake");
}

static void applyBrightness(uint8_t b) {
  brightness = b;
  panel->setBrightness(b);
  prefs.putUChar("bright", b);
}

static void setFace(int f) {
  if (f < 0) f = FACE_COUNT - 1;
  face = (uint8_t)(f % FACE_COUNT);
  prefs.putUChar("face", face);
  loadPack();
  flashToast(FACE_NAMES[face]);
  dirty = true;
}

static void setTheme(int t) {
  if (t < 0) t = THEME_COUNT - 1;
  theme = (uint8_t)(t % THEME_COUNT);
  prefs.putUChar("theme", theme);
  flashToast(THEMES[theme].name);
  dirty = true;
}

// ---------------------------------------------------------------- touch ------
static void pollTouch() {
  const uint32_t now = millis();
  if (now - lastTouchPoll < 20) return;
  lastTouchPoll = now;

  if (!gTouchIrq && !touchWas) return;
  gTouchIrq = false;

  // A bare address cycle wakes a sleeping CST9217; without it getPoint() can
  // block for a full second (SensorLib does not honour Wire's timeout).
  Wire.beginTransmission(TOUCH_ADDR);
  if (Wire.endTransmission() != 0) return;

  int16_t x = 0, y = 0;
  const bool pressed = touch.getPoint(&x, &y, 1) > 0;
  const bool wasPressed = touchWas;
  touchWas = pressed;

  if (asleep) {
    if (pressed && !wasPressed) {
      leaveSleep();
      touchConsumed = true;
    }
    return;
  }

  if (pressed && !wasPressed) {
    touchX0 = touchXl = x;
    touchY0 = touchYl = y;
    touchT0 = now;
    touchConsumed = false;
    return;
  }

  if (pressed) {
    touchXl = x;
    touchYl = y;
    const int16_t dy = touchYl - touchY0;
    const int16_t dx = touchXl - touchX0;
    if (!touchConsumed && abs(dy) >= 60 && abs(dy) > abs(dx)) {
      setFace(face + (dy < 0 ? 1 : -1));
      touchConsumed = true;
    } else if (!touchConsumed && now - touchT0 >= 800) {
      setTheme(theme + 1);
      touchConsumed = true;
    }
    return;
  }

  if (!wasPressed) return;

  if (!touchConsumed) {
    const int16_t dx = abs(touchXl - touchX0);
    const int16_t dy = abs(touchYl - touchY0);
    const uint32_t held = now - touchT0;
    if (held < 600 && dx < 30 && dy < 30) {
      use24h = !use24h;
      prefs.putBool("h24", use24h);
      flashToast(use24h ? "24 HOUR" : "12 HOUR");
      dirty = true;
    }
  }
  touchConsumed = false;
}

// ---------------------------------------------------------------- power ------
static void pwrSetup() {
  if (!pmuOk) return;
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  pmu.clearIrqStatus();
}

static bool pwrShortPressed() {
  if (!pmuOk) return false;
  pmu.getIrqStatus();
  const bool hit = pmu.isPekeyShortPressIrq();
  if (hit) pmu.clearIrqStatus();
  return hit;
}

// ---------------------------------------------------------------- serial -----
static void handleCommand(const String &c) {
  if (c == "STATUS" || c == "R") {
    uint32_t e = displayEpoch();
    time_t tt = (time_t)e;
    struct tm t = {};
    if (e) gmtime_r(&tt, &t);
    Serial.printf("face=%u(%s) theme=%u(%s) bright=%u %s rtc=%s "
                  "display=%04d-%02d-%02d %02d:%02d:%02d off=%d days "
                  "bat=%d%% usb=%d sd=%d pack=%d\n",
                  face, FACE_NAMES[face], theme, THEMES[theme].name,
                  (unsigned)brightness, use24h ? "24h" : "12h",
                  e ? "ok" : "unset", t.tm_year + 1900, t.tm_mon + 1,
                  t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
                  (int)(dateOffset / 86400), batteryPct, batteryUsb ? 1 : 0,
                  sdReady ? 1 : 0, pack.ready() ? 1 : 0);
  } else if (c.startsWith("SET=")) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const char *p = c.c_str() + 4;
    const bool parsed =
        sscanf(p, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 ||
        sscanf(p, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6;
    if (!parsed) {
      Serial.println("use SET=YYYY-MM-DD HH:MM:SS");
    } else {
      struct tm t = {};
      t.tm_year = y - 1900;
      t.tm_mon = mo - 1;
      t.tm_mday = d;
      t.tm_hour = h;
      t.tm_min = mi;
      t.tm_sec = s;
      shiftDisplayTo((uint32_t)mktime(&t));
      Serial.println("display time set (RTC untouched)");
    }
  } else if (c.startsWith("TIME=")) {
    const uint32_t e = (uint32_t)strtoul(c.c_str() + 5, nullptr, 10);
    shiftDisplayTo(e);
    Serial.println("display time set (RTC untouched)");
  } else if (c == "12H") {
    use24h = false;
    prefs.putBool("h24", use24h);
    dirty = true;
  } else if (c == "24H") {
    use24h = true;
    prefs.putBool("h24", use24h);
    dirty = true;
  } else if (c == "AUTO") {
    prefs.putUInt("stamp", 0);
    calibrateDate();
    Serial.println("date offset recalibrated from the build stamp");
  } else if (c == "REAL") {
    dateOffset = 0;
    prefs.putInt("off", dateOffset);
    dirty = true;
    Serial.println("date offset cleared (display == RTC)");
  } else if (c.startsWith("FACE=")) {
    String v = c.substring(5);
    v.toUpperCase();
    if (v == "NEXT") {
      setFace(face + 1);
    } else if (v == "PREV") {
      setFace(face - 1);
    } else {
      setFace(v.toInt());
    }
  } else if (c.startsWith("COLOR=")) {
    String v = c.substring(6);
    v.toUpperCase();
    if (v == "NEXT") {
      setTheme(theme + 1);
    } else {
      setTheme(v.toInt());
    }
  } else if (c.startsWith("BRIGHT=")) {
    applyBrightness((uint8_t)constrain(c.substring(7).toInt(), 1, 255));
    dirty = true;
  } else if (c == "SLEEP") {
    enterSleep();
  } else if (c == "WAKE") {
    leaveSleep();
  } else if (c == "SHOW") {
    forceShowStart = millis();
    if (forceShowStart == 0) forceShowStart = 1;
    dirty = true;
  } else if (c == "SD") {
    sdMount();
    loadPack();
    dirty = true;
  } else if (c == "PACK") {
    loadPack();
    dirty = true;
  } else if (c.length()) {
    Serial.println("commands: STATUS | SET=YYYY-MM-DD HH:MM:SS | 12H | 24H | "
                   "AUTO | REAL | FACE=n|NEXT|PREV | COLOR=n|NEXT | "
                   "BRIGHT=n | SLEEP | WAKE | SHOW | SD | PACK");
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

  prefs.begin("tamaclock", false);
  face = prefs.getUChar("face", FACE_DIGITAL);
  if (face >= FACE_COUNT) face = FACE_DIGITAL;
  theme = prefs.getUChar("theme", 0);
  if (theme >= THEME_COUNT) theme = 0;
  brightness = prefs.getUChar("bright", 200);
  if (brightness < 10) brightness = 200;
  use24h = prefs.getBool("h24", true);
  panel->setBrightness(brightness);

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {
    touchOk = touch.begin(Wire, TOUCH_ADDR, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (touchOk) {
    touch.reset();
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);  // panel mounted at 180 degrees
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);
    Serial.println("CST9217 ok");
  } else {
    Serial.println("CST9217 not detected (BOOT button still works)");
  }

  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 not detected");
  calibrateDate();

  readBattery();
  sdMount();
  loadPack();

  pwrSetup();
  draw();
}

// ---------------------------------------------------------------- loop -------
void loop() {
  static mboot::Hold mbHold;
  if (mbHold.reached()) {
    leaveSleep();
    panel->setBrightness(180);
    mboot::screenMessage(gfx, "RETURNING", "release the button", C_GO);
    mboot::waitRelease();
    if (!mboot::hubLooksValid()) {
      Serial.println("multiboot: factory slot is empty, staying in TamaClock");
      mboot::screenMessage(gfx, "HUB MISSING", "reinstall from a PC", C_WARN);
      delay(2500);
      panel->setBrightness(brightness);
      dirty = true;
    } else {
      mboot::returnToHub();
    }
  }

  if (mbHold.shortPress()) {
    if (asleep) {
      leaveSleep();
    } else {
      use24h = !use24h;
      prefs.putBool("h24", use24h);
      dirty = true;
    }
  }

  static uint32_t lastPwrPoll = 0;
  if (millis() - lastPwrPoll >= 100) {
    lastPwrPoll = millis();
    if (pwrShortPressed()) {
      if (asleep) {
        leaveSleep();
      } else {
        enterSleep();
      }
    }
  }

  pollSerial();
  pollTouch();

  if (asleep) {
    delay(30);
    return;
  }

  const uint32_t now = millis();
  refreshClock();

  if (now - lastBattery >= 5000) {
    lastBattery = now;
    const int before = batteryPct;
    const bool usbBefore = batteryUsb;
    readBattery();
    if (batteryPct != before || batteryUsb != usbBefore) dirty = true;
  }

  // Toast expiry needs one redraw to erase the pill.
  if (toastMsg[0] && (int32_t)(now - toastUntil) >= 0) {
    toastMsg[0] = 0;
    dirty = true;
  }

  // Digital face: one redraw per second is enough (second ticks + date).
  // Themed faces run at about 8 fps idle and 12 fps during the hourly show.
  if (face == FACE_DIGITAL) {
    if (gClockOk && (uint32_t)gTm.tm_sec != lastSecond) {
      lastSecond = gTm.tm_sec;
      dirty = true;
    }
    if (now - lastDraw >= 1000) dirty = true;
  } else {
    const uint32_t interval = hourPhase() >= 0 ? 80 : 120;
    if (now - lastDraw >= interval) dirty = true;
  }

  if (dirty) draw();
  delay(20);
}

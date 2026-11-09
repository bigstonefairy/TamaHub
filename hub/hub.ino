// TamaHub - boot-time game selector for the TamaPoke / TamaDigi multi-game build.
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash / 8 MB PSRAM)
//
// This sketch lives in the `factory` partition.  The two games live in ota_0
// and ota_1 (ota_2 is a spare slot).  The ESP32 bootloader boots `factory`
// whenever otadata is empty, which is exactly the first install and every
// return from a game.  After a game has been picked, otadata points at it and
// the next power-on goes straight back into that game.
//
// Launching a game:
//   1. write `resumeAt = now` into that game's NVS namespace, so the game can
//      skip the time spent in this menu / in the other game,
//   2. esp_ota_set_boot_partition(slot),
//   3. esp_restart().
//
// The games return here by erasing otadata and restarting (see multiboot.h).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>

// pin_config.h defines XPOWERS_CHIP_AXP2101; it must come before XPowersLib.h
// so the library exposes the XPowersPMU typedef for this board.
#include "pin_config.h"

#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorPCF85063.hpp"
#include <XPowersLib.h>

// ---------------------------------------------------------------- versions --
#define HUB_VERSION "1.1"

// ---------------------------------------------------------------- display ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// ---------------------------------------------------------------- touch ------
TouchDrvCST92xx touch;
#define TOUCH_ADDR 0x5A
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }

// ---------------------------------------------------------------- PMU / RTC --
static XPowersPMU pmu;
static SensorPCF85063 rtc;
static bool pmuOk = false;
static bool rtcOk = false;

// ---------------------------------------------------------------- colours ----
static const uint16_t C_BG = 0x0000;
static const uint16_t C_FG = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_CARD = 0x18E3;
static const uint16_t C_LINE = 0x39E7;
static const uint16_t C_POKE = 0xF800;   // red
static const uint16_t C_DIGI = 0x04BF;   // blue
static const uint16_t C_EXTRA = 0x07E0;  // green
static const uint16_t C_WARN = 0xFD20;   // orange

// ---------------------------------------------------------------- slots ------
//
// subtype / namespace / display name / letter / accent.
// The namespace is where `resumeAt` is written before launching; leave it
// nullptr for a slot whose game does not understand the pause protocol.
struct Slot {
  uint8_t subtype;
  const char *ns;
  const char *name;
  char letter;
  uint16_t accent;
  // runtime
  const esp_partition_t *part;
  bool valid;
  char version[24];
};

static Slot slots[3] = {
    {ESP_PARTITION_SUBTYPE_APP_OTA_0, "tamapoke", "POKEMON", 'P', C_POKE, nullptr, false, ""},
    {ESP_PARTITION_SUBTYPE_APP_OTA_1, "tamadigi", "DIGIMON", 'D', C_DIGI, nullptr, false, ""},
    {ESP_PARTITION_SUBTYPE_APP_OTA_2, "tamanaruto", "NARUTO", 'N', C_EXTRA, nullptr, false, ""},
};
static const int SLOT_COUNT = 3;
static int selected = 0;

// ---------------------------------------------------------------- state ------
static bool dirty = true;
static uint32_t lastDraw = 0;
static uint32_t lastBattery = 0;
static int batteryPct = -1;
static bool batteryUsb = false;

static bool bootDown = false;
static uint32_t bootStart = 0;

static bool touchWas = false;
static int16_t touchX0 = 0, touchY0 = 0;   // press point
static int16_t touchXl = 0, touchYl = 0;   // last point while held
static uint32_t lastTouchPoll = 0;

static String serialCmd;

// ---------------------------------------------------------------- helpers ----
static void text(const char *s, int16_t x, int16_t y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static void textCenter(const char *s, int16_t y, uint8_t size, uint16_t color) {
  int16_t w = (int16_t)strlen(s) * 6 * size;
  text(s, (LCD_WIDTH - w) / 2, y, size, color);
}

// The panel is round: a title near the top has much less usable width than
// 466 px.  Shrink the font until the string fits maxWidth.
static void textCenterFit(const char *s, int16_t y, uint8_t maxSize,
                          uint16_t color, int16_t maxWidth) {
  uint8_t size = maxSize;
  while (size > 1 && (int16_t)strlen(s) * 6 * size > maxWidth) size--;
  int16_t w = (int16_t)strlen(s) * 6 * size;
  text(s, (LCD_WIDTH - w) / 2, y, size, color);
}

static uint32_t rtcEpoch() {
  if (!rtcOk) return 0;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2025 || t.getYear() > 2120) return 0;
  struct tm tmv = {};
  tmv.tm_year = t.getYear() - 1900;
  tmv.tm_mon = t.getMonth() - 1;
  tmv.tm_mday = t.getDay();
  tmv.tm_hour = t.getHour();
  tmv.tm_min = t.getMinute();
  tmv.tm_sec = t.getSecond();
  time_t e = mktime(&tmv);
  return e > 0 ? (uint32_t)e : 0;
}

static void readBattery() {
  if (!pmuOk) {
    batteryPct = -1;
    batteryUsb = false;
    return;
  }
  batteryUsb = pmu.isVbusIn();
  batteryPct = pmu.getBatteryPercent();
  if (batteryPct < 0 && !batteryUsb) batteryPct = -1;
}

static void scanSlots() {
  for (int i = 0; i < SLOT_COUNT; i++) {
    Slot &s = slots[i];
    s.part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)s.subtype, nullptr);
    s.valid = false;
    s.version[0] = 0;
    if (!s.part) continue;

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(s.part, &desc) == ESP_OK) {
      s.valid = true;
      snprintf(s.version, sizeof(s.version), "%s", desc.version);
    } else {
      // Fall back to the image magic byte: useful when the app is valid but
      // its description cannot be parsed.
      uint8_t magic = 0;
      if (esp_partition_read(s.part, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
        s.valid = true;
      }
    }
  }
}

static void printSlots() {
  Serial.printf("TamaHub v%s factory=0x%06x\n", HUB_VERSION,
                (unsigned)(esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                    ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                    nullptr)
                               ? esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                          ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                                          nullptr)
                                     ->address
                               : 0));
  for (int i = 0; i < SLOT_COUNT; i++) {
    const Slot &s = slots[i];
    if (s.part) {
      Serial.printf("  %d %-12s @0x%06x %s%s%s\n", i + 1, s.name,
                    (unsigned)s.part->address, s.valid ? "ready" : "EMPTY",
                    s.version[0] ? " v" : "", s.version);
    } else {
      Serial.printf("  %d %-12s no partition\n", i + 1, s.name);
    }
  }
}

// ---------------------------------------------------------------- drawing ----
static void drawCard(int i, int16_t y) {
  Slot &s = slots[i];
  const int16_t x = 53, w = 360, h = 98;
  uint16_t accent = s.accent;

  gfx->fillRoundRect(x, y, w, h, 16, C_CARD);
  gfx->drawRoundRect(x, y, w, h, 16, (i == selected) ? accent : C_LINE);
  if (i == selected) {
    gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 14, accent);
  }

  // icon badge
  gfx->fillCircle(x + 40, y + h / 2, 24, s.valid ? accent : C_LINE);
  char letter[2] = {s.letter, 0};
  text(letter, x + 40 - 6 * 3, y + h / 2 - 8 * 3, 3, 0x0000);

  // name
  text(s.name, x + 76, y + 16, 3, s.valid ? C_FG : C_DIM);

  // state / version
  char st[40];
  if (!s.valid) {
    snprintf(st, sizeof(st), "NOT INSTALLED");
  } else if (s.version[0] && strchr(s.version, '.')) {
    // The ESP-IDF description carries a build hash on this toolchain; only
    // show something that looks like a real version.
    snprintf(st, sizeof(st), "READY  v%s", s.version);
  } else {
    snprintf(st, sizeof(st), "READY");
  }
  text(st, x + 76, y + 52, 2, s.valid ? C_DIM : C_LINE);

  // selected marker
  if (i == selected) {
    text(">", x + w - 24, y + 34, 3, accent);
  }
}

static void drawFooter() {
  char clock[16] = "--:--";
  uint32_t e = rtcEpoch();
  if (e) {
    snprintf(clock, sizeof(clock), "%02u:%02u", (unsigned)((e / 3600) % 24),
             (unsigned)((e / 60) % 60));
  }

  char bat[24];
  if (batteryUsb) {
    snprintf(bat, sizeof(bat), "USB");
  } else if (batteryPct >= 0) {
    snprintf(bat, sizeof(bat), "BAT %d%%", batteryPct);
  } else {
    snprintf(bat, sizeof(bat), "BAT --");
  }

  text(clock, 90, 432, 1, C_DIM);
  text(bat, 330, 432, 1, batteryPct >= 0 && batteryPct <= 15 ? C_WARN : C_DIM);
  textCenter("TAP A CARD TO PLAY   /   BOOT: SELECT", 414, 1, C_DIM);
}

static void draw() {
  gfx->fillScreen(C_BG);
  // Keep the title inside the round panel's top chord: size 3 at y=40 is
  // 234 px wide, while the usable chord there is ~261 px.
  textCenterFit("TAMA HUB v" HUB_VERSION, 40, 3, C_FG, 300);
  textCenter("choose a game", 70, 2, C_DIM);

  drawCard(0, 92);
  drawCard(1, 198);
  drawCard(2, 304);

  drawFooter();
  gfx->flush();
  lastDraw = millis();
  dirty = false;
}

static void showMessage(const char *line1, const char *line2, uint16_t color) {
  gfx->fillScreen(C_BG);
  gfx->drawCircle(233, 233, 190, color);
  textCenter(line1, 200, 3, C_FG);
  textCenter(line2, 242, 2, C_DIM);
  gfx->flush();
}

// ---------------------------------------------------------------- launch -----
static void launch(int i) {
  if (i < 0 || i >= SLOT_COUNT) return;
  Slot &s = slots[i];
  if (!s.valid || !s.part) {
    showMessage("SLOT IS EMPTY", "flash a game into it first", C_WARN);
    delay(1500);
    dirty = true;
    return;
  }

  uint32_t now = rtcEpoch();
  if (s.ns && now) {
    // The inactive game was paused while the device was on.  Tell it where to
    // resume so it does not age by the time spent in the other game.
    Preferences p;
    if (p.begin(s.ns, false)) {
      p.putUInt("resumeAt", now);
      p.end();
      Serial.printf("resumeAt=%u -> %s\n", (unsigned)now, s.ns);
    }
  }

  showMessage("STARTING", s.name, s.accent);
  delay(250);

  esp_err_t err = esp_ota_set_boot_partition(s.part);
  if (err != ESP_OK) {
    Serial.printf("set_boot_partition failed: 0x%x\n", (unsigned)err);
    showMessage("CANNOT START", "image rejected by bootloader", C_WARN);
    delay(1800);
    dirty = true;
    return;
  }

  Serial.printf("booting %s from 0x%06x\n", s.name, (unsigned)s.part->address);
  Serial.flush();
  delay(120);
  esp_restart();
}

// ---------------------------------------------------------------- input ------
static void pollBootButton() {
  bool down = (digitalRead(0) == LOW);
  uint32_t now = millis();

  if (down && !bootDown) {
    bootDown = true;
    bootStart = now;
  } else if (!down && bootDown) {
    bootDown = false;
    uint32_t held = now - bootStart;
    // Launch on RELEASE, never while the button is held: GPIO0 low at reset
    // puts the chip in serial download mode.
    if (held >= 1200) {
      launch(selected);
    } else if (held >= 60) {
      selected = (selected + 1) % SLOT_COUNT;
      dirty = true;
    }
  }
}

static void pollTouch() {
  uint32_t now = millis();
  if (now - lastTouchPoll < 20) return;
  lastTouchPoll = now;

  if (!gTouchIrq && !touchWas) return;
  gTouchIrq = false;

  // A bare address cycle wakes a sleeping CST9217; without it getPoint() can
  // block for a full second (SensorLib does not honour Wire's timeout).
  Wire.beginTransmission(TOUCH_ADDR);
  if (Wire.endTransmission() != 0) return;

  int16_t x = 0, y = 0;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  const bool wasPressed = touchWas;
  touchWas = pressed;

  if (pressed && !wasPressed) {
    touchX0 = touchXl = x;
    touchY0 = touchYl = y;
    return;
  }
  if (pressed) {
    // Keep the last valid position: on release getPoint() usually reports
    // nothing and leaves x/y untouched.
    touchXl = x;
    touchYl = y;
    return;
  }
  if (!wasPressed) return;

  // Released. A tap is a release near the press point; the card hit test uses
  // the position where the finger was last seen.
  if (abs(touchXl - touchX0) >= 40 || abs(touchYl - touchY0) >= 40) return;
  const int16_t cardX = 53, cardW = 360, cardH = 98;
  if (touchXl < cardX || touchXl > cardX + cardW) return;
  const int16_t ys[3] = {92, 198, 304};
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (touchYl >= ys[i] && touchYl <= ys[i] + cardH) {
      selected = i;
      dirty = true;
      draw();
      launch(i);
      return;
    }
  }
}

static void handleCommand(const String &c) {
  if (c == "1") {
    launch(0);
  } else if (c == "2") {
    launch(1);
  } else if (c == "3") {
    launch(2);
  } else if (c == "R" || c == "r" || c == "SLOTS") {
    scanSlots();
    printSlots();
    dirty = true;
  } else if (c.length()) {
    Serial.println("commands: 1 2 3 | SLOTS | R");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      serialCmd.trim();
      if (serialCmd.length()) handleCommand(serialCmd);
      serialCmd = "";
    } else if (serialCmd.length() < 32) {
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
  Serial.println("=== TamaHub ===");
  Serial.printf("hub v%s\n", HUB_VERSION);

  pinMode(0, INPUT_PULLUP);
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

  readBattery();
  scanSlots();
  printSlots();

  // Pick the first installed slot so a BOOT long-press does something useful.
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (slots[i].valid) {
      selected = i;
      break;
    }
  }

  draw();
}

// ---------------------------------------------------------------- loop -------
void loop() {
  pollBootButton();
  pollTouch();
  pollSerial();

  uint32_t now = millis();
  if (now - lastBattery >= 2000) {
    lastBattery = now;
    readBattery();
    dirty = true;
  }
  if (dirty || now - lastDraw >= 1000) {
    draw();
  }
  delay(2);
}

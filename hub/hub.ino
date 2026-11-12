// TamaHub - boot-time app selector for the TamaHub multi-app build.
//
// Board: Waveshare ESP32-S3-Touch-AMOLED-1.75 (16 MB flash / 8 MB PSRAM)
//
// This sketch lives in the `factory` partition.  v2 gives the device five
// equal 2 MB ota slots, and the user decides which app goes in which slot.
// The ESP32 bootloader boots `factory` whenever otadata is empty, which is
// exactly the first install and every return from an app.  After an app has
// been picked, otadata points at it and the next power-on goes straight back
// into that app.
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
#define HUB_VERSION "1.2"

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
static const uint16_t C_EXTRA = 0x07E0;  // green (unknown app fallback)
static const uint16_t C_CLOCK = 0xFFE0;  // yellow
static const uint16_t C_IDLEP = 0xFC60;  // orange, IdlePoke
static const uint16_t C_IDLED = 0x05FF;  // cyan, IdleDigi
static const uint16_t C_WARN = 0xFD20;   // orange

// ---------------------------------------------------------------- slots ------
//
// Nothing here is tied to an offset.  Every image carries a
// "TAMAHUB_APP:<name>" tag and a "TAMAHUB_VER:<version>" tag in .rodata, so
// the hub can label whichever app the user flashed into a slot.  The
// catalogue below only supplies the pretty name, letter and colour; apps it
// does not know are still launchable and show their own tag.
struct AppInfo {
  const char *id;      // value after "TAMAHUB_APP:"
  const char *name;    // card label
  char letter;
  uint16_t accent;
  const char *ns;      // NVS namespace for resumeAt, or nullptr
};

static const AppInfo APP_INFO[] = {
    {"TamaClock", "CLOCK", 'C', C_CLOCK, nullptr},
    {"IdlePoke", "IDLEPOKE", 'P', C_IDLEP, "idlepoke"},
    {"IdleDigi", "IDLEDIGI", 'D', C_IDLED, "idledigi"},
    {"TamaPoke", "POKEMON", 'P', C_POKE, "tamapoke"},
    {"TamaDigi", "DIGIMON", 'D', C_DIGI, "tamadigi"},
};

struct Slot {
  uint8_t subtype;
  // runtime
  const esp_partition_t *part;
  bool valid;
  char name[12];
  char letter;
  uint16_t accent;
  const char *ns;
  char version[24];
};

static Slot slots[5] = {
    {ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr, false, "EMPTY", '-', C_LINE, nullptr, ""},
    {ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr, false, "EMPTY", '-', C_LINE, nullptr, ""},
    {ESP_PARTITION_SUBTYPE_APP_OTA_2, nullptr, false, "EMPTY", '-', C_LINE, nullptr, ""},
    {ESP_PARTITION_SUBTYPE_APP_OTA_3, nullptr, false, "EMPTY", '-', C_LINE, nullptr, ""},
    {ESP_PARTITION_SUBTYPE_APP_OTA_4, nullptr, false, "EMPTY", '-', C_LINE, nullptr, ""},
};
static const int SLOT_COUNT = 5;
static int selected = 0;

// Card geometry shared by draw() and the touch hit test.
static const int16_t CARD_X = 53;
static const int16_t CARD_W = 360;
static const int16_t CARD_H = 52;
static const int16_t CARD_Y0 = 86;
static const int16_t CARD_GAP = 6;
static int16_t cardY(int i) { return CARD_Y0 + i * (CARD_H + CARD_GAP); }

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

// Find `tag` in the first part of an app image and copy the NUL-terminated
// text after it.  Reading a few KB at a time keeps this off the heap; the
// overlap keeps a tag that straddles a chunk boundary.
static bool readTag(const esp_partition_t *part, const char *tag, char *out,
                    size_t outLen) {
  if (!part || !out || outLen == 0) return false;
  out[0] = 0;
  const size_t tagLen = strlen(tag);
  if (tagLen == 0) return false;

  static uint8_t buf[4096 + 64];
  const size_t chunk = 4096;
  size_t carry = 0;
  size_t limit = part->size;
  if (limit > 256 * 1024) limit = 256 * 1024;

  for (size_t off = 0; off < limit; off += chunk) {
    size_t n = limit - off;
    if (n > chunk) n = chunk;
    if (esp_partition_read(part, off, buf + carry, n) != ESP_OK) return false;
    const size_t total = carry + n;

    for (size_t i = 0; i + tagLen < total; i++) {
      if (memcmp(buf + i, tag, tagLen) != 0) continue;
      size_t src = i + tagLen;
      size_t dst = 0;
      while (src < total && dst + 1 < outLen) {
        const char c = (char)buf[src++];
        if (c == 0) break;
        out[dst++] = c;
      }
      out[dst] = 0;
      return dst > 0;
    }

    carry = total >= tagLen ? tagLen : total;
    memmove(buf, buf + total - carry, carry);
  }
  return false;
}

// Returns an index, not a pointer: the Arduino preprocessor emits a prototype
// for this function above the AppInfo definition, so the return type must not
// name a type that is declared later in the sketch.
static int findApp(const char *id) {
  for (size_t i = 0; i < sizeof(APP_INFO) / sizeof(APP_INFO[0]); i++) {
    if (strcmp(APP_INFO[i].id, id) == 0) return (int)i;
  }
  return -1;
}

static void scanSlots() {
  for (int i = 0; i < SLOT_COUNT; i++) {
    Slot &s = slots[i];
    s.part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)s.subtype, nullptr);
    s.valid = false;
    s.version[0] = 0;
    strcpy(s.name, "EMPTY");
    s.letter = '-';
    s.accent = C_LINE;
    s.ns = nullptr;
    if (!s.part) continue;

    esp_app_desc_t desc = {};
    bool hasDesc = (esp_ota_get_partition_description(s.part, &desc) == ESP_OK);
    bool bootable = hasDesc;
    if (!bootable) {
      // Fall back to the image magic byte: useful when the app is valid but
      // its description cannot be parsed.
      uint8_t magic = 0;
      if (esp_partition_read(s.part, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
        bootable = true;
      }
    }
    if (!bootable) continue;
    s.valid = true;

    char id[24] = "";
    if (readTag(s.part, "TAMAHUB_APP:", id, sizeof(id))) {
      const int ai = findApp(id);
      if (ai >= 0) {
        const AppInfo &info = APP_INFO[ai];
        snprintf(s.name, sizeof(s.name), "%s", info.name);
        s.letter = info.letter;
        s.accent = info.accent;
        s.ns = info.ns;
      } else {
        snprintf(s.name, sizeof(s.name), "%s", id);
        char c = id[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        s.letter = c ? c : '?';
        s.accent = C_EXTRA;
      }
    } else {
      // An app built before the tag existed is still launchable.
      snprintf(s.name, sizeof(s.name), "%s", "APP");
      s.letter = '?';
      s.accent = C_LINE;
    }

    char ver[24] = "";
    if (readTag(s.part, "TAMAHUB_VER:", ver, sizeof(ver))) {
      snprintf(s.version, sizeof(s.version), "%s", ver);
    } else if (hasDesc && desc.version[0] && strchr(desc.version, '.')) {
      // This core fills the description with a build hash; only accept
      // something that looks like a real version.
      snprintf(s.version, sizeof(s.version), "%s", desc.version);
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
  const int16_t cy = y + CARD_H / 2;
  uint16_t accent = s.accent;

  gfx->fillRoundRect(CARD_X, y, CARD_W, CARD_H, 14, C_CARD);
  gfx->drawRoundRect(CARD_X, y, CARD_W, CARD_H, 14,
                     (i == selected) ? accent : C_LINE);
  if (i == selected) {
    gfx->drawRoundRect(CARD_X + 2, y + 2, CARD_W - 4, CARD_H - 4, 12, accent);
  }

  // icon badge
  gfx->fillCircle(CARD_X + 26, cy, 17, s.valid ? accent : C_LINE);
  char letter[2] = {s.letter, 0};
  text(letter, CARD_X + 26 - 6, cy - 8, 2, 0x0000);

  // name
  text(s.name, CARD_X + 50, y + 7, 2, s.valid ? C_FG : C_DIM);

  // state / version
  char st[40];
  if (!s.valid) {
    snprintf(st, sizeof(st), "NOT INSTALLED");
  } else if (s.version[0] && strchr(s.version, '.')) {
    snprintf(st, sizeof(st), "READY  v%s", s.version);
  } else if (s.version[0]) {
    snprintf(st, sizeof(st), "READY  %s", s.version);
  } else {
    snprintf(st, sizeof(st), "READY");
  }
  text(st, CARD_X + 50, y + 29, 1, s.valid ? C_DIM : C_LINE);

  // selected marker
  if (i == selected) {
    text(">", CARD_X + CARD_W - 20, y + 18, 2, accent);
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
  textCenter("choose an app", 68, 2, C_DIM);

  for (int i = 0; i < SLOT_COUNT; i++) drawCard(i, cardY(i));

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
  if (touchXl < CARD_X || touchXl > CARD_X + CARD_W) return;
  for (int i = 0; i < SLOT_COUNT; i++) {
    const int16_t y = cardY(i);
    if (touchYl >= y && touchYl <= y + CARD_H) {
      selected = i;
      dirty = true;
      draw();
      launch(i);
      return;
    }
  }
}

static void handleCommand(const String &c) {
  if (c.length() == 1 && c[0] >= '1' && c[0] < '1' + SLOT_COUNT) {
    launch(c[0] - '1');
  } else if (c == "R" || c == "r" || c == "SLOTS") {
    scanSlots();
    printSlots();
    dirty = true;
  } else if (c.length()) {
    Serial.printf("commands: 1-%d | SLOTS | R\n", SLOT_COUNT);
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

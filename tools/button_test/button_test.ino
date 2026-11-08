// button_test.ino
// 아래쪽 물리 버튼이 BOOT(GPIO0)인지 PWR(AXP2101/TCA9554)인지 확인하는 테스트.
//
// 보드: Waveshare ESP32-S3-Touch-AMOLED-1.75
// 라이브러리(GFX Library for Arduino, XPowersLib)는 TamaPoke와 동일.
//
// 화면에 두 줄이 뜬다.
//   BOOT / GPIO0      : 아래쪽 버튼이 BOOT를 누르면 PRESSED
//   PWR  / TCA9554 P4 : 아래쪽 버튼이 PWR을 누르면 PRESSED
//
// 2초 게이지가 끝까지 차면 "LONG PRESS OK"가 표시된다.
// PWR은 4초 이상 누르면 AXP2101이 전원을 차단하므로, 그 전에 손을 뗄 것.

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include <XPowersLib.h>
#include "pin_config.h"

// ---------- 화면 ----------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// ---------- PMU (패널 전원) ----------
XPowersPMU pmu;
static bool pmuOk = false;

// ---------- TCA9554 (PWR 상태) ----------
#define TCA_ADDR 0x20
#define TCA_INPUT_REG 0x00
#define TCA_PWR_BIT 4  // EXIO4 = conditioned PWR button, HIGH while pressed

static bool tcaOk = false;

// ---------- 색 ----------
static const uint16_t C_BG = 0x0000;
static const uint16_t C_FG = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_PANEL = 0x2124;
static const uint16_t C_GREEN = 0x07E0;
static const uint16_t C_RED = 0xF800;
static const uint16_t C_YELLOW = 0xFFE0;

// ---------- 버튼 상태 ----------
struct Btn {
  bool pressed = false;
  bool rawLast = false;
  uint8_t stable = 0;
  uint32_t since = 0;
  bool longFired = false;
};

static Btn bootBtn;  // GPIO0
static Btn pwrBtn;   // TCA9554 P4

static uint8_t tcaRaw = 0xFF;
static uint32_t lastPoll = 0;
static uint32_t lastDraw = 0;
static bool dirty = true;

static void drawText(const char *s, int16_t x, int16_t y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

static bool tcaRead(uint8_t &value) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(TCA_INPUT_REG);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TCA_ADDR, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

// 10ms 간격으로 호출. 3회 연속 같은 값이면 확정.
static void updateButton(Btn &b, bool raw, uint32_t now) {
  if (raw != b.rawLast) {
    b.rawLast = raw;
    b.stable = 0;
  } else if (b.stable < 3) {
    b.stable++;
  }

  bool next = (b.stable >= 3) ? raw : b.pressed;
  if (next != b.pressed) {
    b.pressed = next;
    b.since = now;
    b.longFired = false;
    dirty = true;
    Serial.printf("%s %s\n", &b == &bootBtn ? "BOOT" : "PWR",
                  b.pressed ? "PRESSED" : "released");
  }

  if (b.pressed && !b.longFired && now - b.since >= 2000) {
    b.longFired = true;
    dirty = true;
    Serial.printf("%s LONG PRESS (2s)\n", &b == &bootBtn ? "BOOT" : "PWR");
  }
}

static uint32_t holdMs(const Btn &b, uint32_t now) {
  return b.pressed ? (now - b.since) : 0;
}

static void drawPanel(int16_t y, const char *label, const Btn &b,
                      uint32_t now, bool pwrWarning) {
  const int16_t x = 33, w = 400, h = 118;
  gfx->fillRoundRect(x, y, w, h, 16, C_PANEL);
  gfx->drawRoundRect(x, y, w, h, 16, b.pressed ? C_GREEN : C_DIM);

  drawText(label, x + 18, y + 12, 2, C_FG);

  const char *state = b.pressed ? "PRESSED" : "released";
  drawText(state, x + 18, y + 40, 3, b.pressed ? C_GREEN : C_DIM);

  char hold[32];
  uint32_t ms = holdMs(b, now);
  snprintf(hold, sizeof(hold), "hold %.2fs", ms / 1000.0f);
  drawText(hold, x + 270, y + 48, 2, b.pressed ? C_YELLOW : C_DIM);

  // 2초 게이지
  const int16_t bx = x + 18, by = y + 88, bw = w - 36, bh = 10;
  gfx->fillRoundRect(bx, by, bw, bh, 5, 0x0000);
  uint32_t pct = ms > 2000 ? 2000 : ms;
  int16_t fill = (int16_t)((int64_t)bw * pct / 2000);
  if (fill > 0) {
    gfx->fillRoundRect(bx, by, fill, bh, 5, b.longFired ? C_GREEN : C_YELLOW);
  }
  if (b.longFired) drawText("LONG PRESS OK", x + 18, y + 68, 2, C_GREEN);

  if (pwrWarning) {
    drawText("! PWR CUTOFF AT 4s - RELEASE NOW", x + 18, y + 104, 1, C_RED);
  }
}

static void draw() {
  uint32_t now = millis();
  gfx->fillScreen(C_BG);

  drawText("BUTTON TEST", 110, 24, 3, C_FG);
  drawText("Press the LOWER case button", 66, 62, 2, C_DIM);

  drawPanel(90, "BOOT / GPIO0", bootBtn, now, false);

  char pwrLabel[48];
  snprintf(pwrLabel, sizeof(pwrLabel), "PWR / TCA9554 P4  raw=0x%02X", tcaRaw);
  drawPanel(226, pwrLabel, pwrBtn, now, pwrBtn.pressed && holdMs(pwrBtn, now) > 3200);

  if (!tcaOk) {
    drawText("TCA9554 not responding at 0x20", 40, 356, 2, C_RED);
  } else {
    drawText("TCA9554 OK at 0x20", 40, 356, 2, C_DIM);
  }

  drawText("Green = pressed", 143, 388, 2, C_DIM);
  drawText("BOOT safe / PWR 4s = off", 77, 416, 2, C_DIM);

  gfx->flush();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(200);
  Serial.println();
  Serial.println("=== Tama button test ===");
  Serial.println("Press the LOWER case button.");
  Serial.println("BOOT = GPIO0 / PWR = TCA9554 P4");
  Serial.println("PWR held 4s = AXP2101 hardware power off. Release before that.");
  Serial.println();

  pinMode(0, INPUT_PULLUP);  // BOOT, external pull-up on the board

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setTimeOut(50);

  // TamaPoke/TamaDigi와 동일: 패널 전원(BLDO1)을 먼저 켜야 화면이 나온다.
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuOk) {
    pmu.setBLDO1Voltage(3300);
    pmu.enableBLDO1();
  } else {
    Serial.println("AXP2101 not detected (panel power may stay off)");
  }

  if (!gfx->begin(80000000)) Serial.println("gfx->begin() failed");
  panel->setBrightness(180);

  uint8_t v = 0;
  tcaOk = tcaRead(v);
  tcaRaw = v;
  Serial.printf("TCA9554: %s raw=0x%02X\n", tcaOk ? "OK" : "NOT FOUND", tcaRaw);

  lastPoll = millis();
  lastDraw = 0;
  dirty = true;
}

void loop() {
  uint32_t now = millis();

  if (now - lastPoll >= 10) {
    lastPoll = now;

    bool bootRaw = (digitalRead(0) == LOW);

    uint8_t v = tcaRaw;
    bool ok = tcaRead(v);
    if (ok) {
      tcaRaw = v;
      tcaOk = true;
    } else {
      tcaOk = false;
    }
    bool pwrRaw = tcaOk && ((tcaRaw >> TCA_PWR_BIT) & 1);

    updateButton(bootBtn, bootRaw, now);
    updateButton(pwrBtn, pwrRaw, now);
  }

  // 상태가 바뀌었거나 100ms마다 갱신 (hold 시간 표시)
  if (dirty || now - lastDraw >= 100) {
    dirty = false;
    lastDraw = now;
    draw();
  }

  delay(2);
}

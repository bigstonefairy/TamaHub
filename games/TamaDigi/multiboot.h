#pragma once
//
// Shared multi-game glue for TamaPoke / TamaDigi.
//
// The lower case button is BOOT/GPIO0.  A 2 s hold opens the game menu:
//   1. the game draws a SAVING screen and forces a save,
//   2. the player is asked to release the button,
//   3. otadata is erased and the chip restarts into `factory` (TamaHub).
//
// The restart happens only after the button is released: GPIO0 held low at
// reset would put the ESP32-S3 into serial download mode instead of booting.
//
// TamaHub writes `resumeAt` into this game's NVS namespace before launching
// it.  pet.cpp consumes it in syncClock(), so time spent in the other game is
// not counted as offline progression.

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

namespace mboot {

constexpr uint8_t kPin = 0;          // BOOT / lower case button
constexpr uint32_t kHoldMs = 2000;   // hold time that opens the menu
constexpr uint32_t kShortMinMs = 40;  // ignore contact bounce
constexpr uint32_t kShortMaxMs = 600; // a tap, not a hold in progress

inline void begin() { pinMode(kPin, INPUT_PULLUP); }

// Edge-triggered hold + tap detector.  Call reached() once per loop() and
// shortPress() right after it.
class Hold {
 public:
  // Returns true exactly once, when the button has been held for kHoldMs.
  bool reached() {
    const bool down = (digitalRead(kPin) == LOW);
    if (!down) {
      if (active_ && !fired_) {
        const uint32_t held = millis() - start_;
        if (held >= kShortMinMs && held <= kShortMaxMs) releasedShort_ = true;
      }
      active_ = false;
      return false;
    }
    if (!active_) {
      active_ = true;
      fired_ = false;
      releasedShort_ = false;
      start_ = millis();
      return false;
    }
    if (!fired_ && millis() - start_ >= kHoldMs) {
      fired_ = true;
      return true;
    }
    return false;
  }

  // Returns true exactly once, on the loop after a short press is released.
  // A real hold never reports a tap: fired_ suppresses it.
  bool shortPress() {
    if (!releasedShort_) return false;
    releasedShort_ = false;
    return true;
  }

 private:
  bool active_ = false;
  bool fired_ = false;
  bool releasedShort_ = false;
  uint32_t start_ = 0;
};

// Never restart while GPIO0 is held, or the ROM enters download mode.
inline void waitRelease() {
  while (digitalRead(kPin) == LOW) delay(10);
  delay(40);  // debounce
}

inline void screenMessage(Arduino_GFX *g, const char *line1, const char *line2,
                          uint16_t accent) {
  if (!g) return;
  g->fillScreen(0x0000);
  g->drawCircle(233, 233, 196, accent);

  g->setTextSize(3);
  g->setTextColor(0xFFFF);
  int16_t w1 = (int16_t)strlen(line1) * 18;
  g->setCursor((466 - w1) / 2, 196);
  g->print(line1);

  g->setTextSize(2);
  g->setTextColor(0x8410);
  int16_t w2 = (int16_t)strlen(line2) * 12;
  g->setCursor((466 - w2) / 2, 238);
  g->print(line2);

  g->flush();
}

inline void returnToHub() {
  const esp_partition_t *ota = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (ota) {
    esp_err_t err = esp_partition_erase_range(ota, 0, ota->size);
    Serial.printf("multiboot: otadata erase 0x%x\n", (unsigned)err);
  } else {
    Serial.println("multiboot: no otadata partition");
  }
  delay(50);
  esp_restart();
}

// True when the factory slot contains a bootable image.  Checked before
// erasing otadata: if the hub was never flashed, erasing would leave the chip
// with nothing to boot.
inline bool hubLooksValid() {
  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory) return false;
  uint8_t magic = 0;
  if (esp_partition_read(factory, 0, &magic, 1) != ESP_OK) return false;
  return magic == 0xE9;  // ESP application image magic
}

}  // namespace mboot

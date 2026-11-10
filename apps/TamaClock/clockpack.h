#pragma once
//
// TamaClock SD animation packs.
//
// One pack per themed face, stored as /tamaclock/<name>.tnm:
//
//   offset  size  field
//   0       4     magic "TNM1"
//   4       1     format version (1)
//   5       1     theme id (1 Pokemon, 2 Digimon, 3 Mario, 4 Zelda)
//   6       1     frames per second (1..30)
//   7       1     reserved
//   8       2     frame count           (LE)
//   10      2     frame width           (LE, 8..280)
//   12      2     frame height          (LE, 8..200)
//   14      2     first hour frame      (LE, 0 = no hourly animation)
//   16      2     hour frame count      (LE)
//   18      2     reserved
//   20      ...   frames, RGB565 little-endian, w*h*2 bytes each
//
// The frame buffer lives in PSRAM and frames are seek()ed from the card on
// demand, so a pack can be much larger than RAM.  Draw with draw16bitRGBBitmap
// so the panel keeps its native colour order.

#include <Arduino.h>
#include "Arduino_GFX_Library.h"

class ClockPack {
 public:
  bool open(const char *path, uint8_t theme) {
    close();
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    if (f.size() < 20) {
      f.close();
      return false;
    }

    uint8_t h[20];
    if (f.read(h, sizeof(h)) != sizeof(h)) {
      f.close();
      return false;
    }
    if (memcmp(h, "TNM1", 4) != 0 || h[4] != 1) {
      Serial.printf("pack %s: bad magic/version\n", path);
      f.close();
      return false;
    }
    if (h[5] != 0 && h[5] != theme) {
      Serial.printf("pack %s: theme %u, expected %u\n", path, h[5], theme);
      f.close();
      return false;
    }

    w_ = (uint16_t)h[10] | ((uint16_t)h[11] << 8);
    h_ = (uint16_t)h[12] | ((uint16_t)h[13] << 8);
    frames_ = (uint16_t)h[8] | ((uint16_t)h[9] << 8);
    fps_ = h[6];
    hourStart_ = (uint16_t)h[14] | ((uint16_t)h[15] << 8);
    hourCount_ = (uint16_t)h[16] | ((uint16_t)h[17] << 8);

    if (w_ < 8 || w_ > 280 || h_ < 8 || h_ > 200 || frames_ == 0) {
      Serial.printf("pack %s: %ux%u x%u is out of range\n", path, w_, h_,
                    frames_);
      f.close();
      return false;
    }
    if (fps_ < 1 || fps_ > 30) fps_ = 8;
    if (hourStart_ > frames_) hourStart_ = 0;
    if (hourCount_ > frames_ - hourStart_) hourCount_ = frames_ - hourStart_;

    frameBytes_ = (uint32_t)w_ * h_ * 2;
    const uint32_t need = 20UL + frameBytes_ * frames_;
    if (f.size() < need) {
      Serial.printf("pack %s: truncated (%u < %u)\n", path, (unsigned)f.size(),
                    (unsigned)need);
      f.close();
      return false;
    }

    buf_ = (uint16_t *)heap_caps_malloc(frameBytes_, MALLOC_CAP_SPIRAM);
    if (!buf_) buf_ = (uint16_t *)malloc(frameBytes_);
    if (!buf_) {
      Serial.println("pack: no memory for a frame buffer");
      f.close();
      return false;
    }

    file_ = f;
    Serial.printf("pack %s: %ux%u, %u frames @%u fps\n", path, w_, h_, frames_,
                  fps_);
    return true;
  }

  void close() {
    if (file_) file_.close();
    if (buf_) {
      free(buf_);
      buf_ = nullptr;
    }
    w_ = h_ = frames_ = fps_ = hourStart_ = hourCount_ = 0;
    frameBytes_ = 0;
  }

  bool ready() const { return buf_ != nullptr; }
  uint16_t frames() const { return frames_; }
  uint16_t fps() const { return fps_ ? fps_ : 8; }
  uint16_t hourStart() const { return hourStart_; }
  uint16_t hourCount() const { return hourCount_; }

  // Draws `index` centred on (cx, cy).  Returns false if the read failed.
  bool draw(Arduino_GFX *g, uint16_t index, int16_t cx, int16_t cy) {
    if (!ready() || index >= frames_) return false;
    if (!file_.seek(20UL + (uint32_t)index * frameBytes_)) return false;
    if (file_.read((uint8_t *)buf_, frameBytes_) != (int)frameBytes_) {
      // A card pulled mid-frame must not leave garbage on screen.
      memset(buf_, 0, frameBytes_);
      return false;
    }
    g->draw16bitRGBBitmap(cx - w_ / 2, cy - h_ / 2, buf_, w_, h_);
    return true;
  }

 private:
  File file_;
  uint16_t w_ = 0, h_ = 0, frames_ = 0, fps_ = 0;
  uint16_t hourStart_ = 0, hourCount_ = 0;
  uint32_t frameBytes_ = 0;
  uint16_t *buf_ = nullptr;
};

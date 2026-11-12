#pragma once
//
// Minimal PMD sprite reader for the TamaClock creature faces.
//
// The clock does NOT ship its own art.  It reads the exact same sprite files
// the games already put on the card:
//
//   /tamapoke/mons/pNNN.bin   (TPK2, TamaPoke expanded, dex 1..386)
//   /tamadigi/mons/pNNN.bin   (TPK2, TamaDigi, dex 1..87)
//
// TPK2 is the games' multi-action PMD format.  The header is:
//
//   "TPK2" | actCount u8 | palCount u16 | pal[palCount] u16 (RGB565)
//   then per act: id u8, w u8, h u8, frames u8,
//                 ms[frames] u16, indices[frames * w * h] (0xFF = transparent)
//
// Only one creature is held in PSRAM at a time, and only the idle act is used;
// the clock has no need for battle, walk or sleep animations.  Drawing is the
// same integer-scaled, feet-anchored blit the games use, so a sprite looks the
// way it does in TamaPoke/TamaDigi.
//
// This file is self-contained on purpose: TamaClock must not depend on either
// game's sources, which live in different sketches and keep changing.

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include "Arduino_GFX_Library.h"

// The action ids are shared by both games (see their sdmon.h).
enum : uint8_t {
  PMD_IDLE = 0, PMD_WALKL, PMD_WALKR, PMD_SLEEP, PMD_EAT, PMD_HURT,
  PMD_ATTACK, PMD_POSE, PMD_HOP, PMD_NOD, PMD_BREATH, PMD_SIT,
  PMD_NACTS
};

struct PmdAct {
  uint8_t w = 0, h = 0, frames = 0;
  uint8_t top = 0;              // first row that holds a pixel
  uint8_t base = 0;             // lowest used row + 1: anchor by the feet
  uint16_t ms[24] = {0};
  const uint8_t *data = nullptr;
};

struct MonsPack {
  uint8_t *blob = nullptr;
  uint16_t palCount = 0;
  uint16_t pal[256] = {0};
  PmdAct acts[PMD_NACTS];

  bool ready() const { return blob != nullptr; }

  void close() {
    if (blob) {
      free(blob);
      blob = nullptr;
    }
    for (auto &a : acts) {
      a.w = a.h = a.frames = a.base = a.top = 0;
      a.data = nullptr;
    }
    palCount = 0;
  }

  // Opens <root>/pNNN.bin.  The blob keeps pointers into itself, so it lives
  // as long as this object does.
  bool open(const char *root, uint16_t dex) {
    close();
    char path[40];
    snprintf(path, sizeof(path), "%s/p%03u.bin", root, (unsigned)dex);
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    const uint32_t size = f.size();
    if (size < 7 || size > 3UL * 1024 * 1024) {
      f.close();
      return false;
    }
    blob = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!blob) blob = (uint8_t *)malloc(size);
    if (!blob) {
      f.close();
      return false;
    }
    const bool whole = f.read(blob, size) == (int)size;
    f.close();
    if (!whole || memcmp(blob, "TPK2", 4) != 0) {
      close();
      return false;
    }

    const uint8_t nActs = blob[4];
    memcpy(&palCount, blob + 5, 2);
    if (palCount > 256 || (uint32_t)7 + palCount * 2 > size) {
      close();
      return false;
    }
    memcpy(pal, blob + 7, palCount * 2);

    const uint8_t *p = blob + 7 + palCount * 2;
    const uint8_t *end = blob + size;
    for (uint8_t i = 0; i < nActs && p + 4 <= end; i++) {
      const uint8_t id = p[0], w = p[1], h = p[2], nf = p[3];
      p += 4;
      if (id >= PMD_NACTS || nf > 24) {
        close();
        return false;
      }
      const uint32_t bytes = (uint32_t)nf * 2 + (uint32_t)w * h * nf;
      if (!w || !h || !nf || p + bytes > end) {
        close();
        return false;
      }
      PmdAct &a = acts[id];
      a.w = w;
      a.h = h;
      a.frames = nf;
      for (uint8_t k = 0; k < nf; k++) {
        a.ms[k] = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
      }
      a.data = p;
      p += (uint32_t)w * h * nf;

      // Row range that actually holds pixels, across every frame: the games
      // anchor a sprite by the lowest such row so actions with different
      // padding still stand on the same ground line.  The top row lets the
      // clock size the creature by its visible height, not its canvas.
      uint8_t base = 1;
      uint8_t top = h;
      for (uint8_t fr = 0; fr < nf; fr++) {
        const uint8_t *frm = a.data + (uint32_t)fr * w * h;
        for (int r = h - 1; r >= 0; r--) {
          bool any = false;
          for (int c = 0; c < w && !any; c++)
            if (frm[r * w + c] != 0xFF) any = true;
          if (any) {
            if (r + 1 > base) base = (uint8_t)(r + 1);
            break;
          }
        }
        for (int r = 0; r < h; r++) {
          bool any = false;
          for (int c = 0; c < w && !any; c++)
            if (frm[r * w + c] != 0xFF) any = true;
          if (any) {
            if (r < top) top = (uint8_t)r;
            break;
          }
        }
      }
      a.base = base;
      a.top = top;
    }
    return true;
  }

  // First act that actually has frames, or PMD_NACTS if the file is empty.
  uint8_t firstAct() const {
    for (uint8_t i = 0; i < PMD_NACTS; i++)
      if (acts[i].frames) return i;
    return PMD_NACTS;
  }

  bool draw(Arduino_GFX *g, uint8_t actId, int16_t cx, int16_t groundY,
            uint32_t t, uint8_t maxScale = 6) const {
    if (!ready() || actId >= PMD_NACTS) return false;
    const PmdAct &a = acts[actId];
    if (!a.frames) return false;

    // Size the silhouette by its *visible* height, so a canvas with lots of
    // transparent padding does not shrink the creature.
    const PmdAct &ref = acts[PMD_IDLE].frames ? acts[PMD_IDLE] : a;
    const uint8_t span = (ref.base > ref.top) ? (uint8_t)(ref.base - ref.top) : ref.h;
    uint8_t s = span ? (uint8_t)((185 + span / 2) / span) : 3;
    if (s < 2) s = 2;
    while (s > 1 && (a.w * s > 400 || span * s > 190)) s--;
    if (s > maxScale) s = maxScale;

    const uint8_t fi = frameAt(a, t);
    const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
    const int16_t x0 = cx - a.w * s / 2;
    const int16_t y0 = groundY - (a.base ? a.base : a.h) * s;
    for (int r = 0; r < a.h; r++) {
      const uint8_t *row = fr + r * a.w;
      for (int c = 0; c < a.w; c++) {
        const uint8_t idx = row[c];
        if (idx == 0xFF) continue;
        g->fillRect(x0 + c * s, y0 + r * s, s, s, pal[idx]);
      }
    }
    return true;
  }

 private:
  static uint32_t actMs(const PmdAct &a) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < a.frames; i++) total += a.ms[i];
    return total ? total : 100;
  }

  static uint8_t frameAt(const PmdAct &a, uint32_t t) {
    t %= actMs(a);
    uint8_t i = 0;
    while (t >= a.ms[i]) {
      t -= a.ms[i];
      i = (uint8_t)((i + 1) % a.frames);
    }
    return i;
  }
};

// Fills `out` with the dex numbers that have pNNN.bin in `root`.
// Shiny files (psNNN.bin) are skipped: they duplicate the normal entry.
static inline int monsScan(const char *root, uint16_t *out, int cap) {
  File dir = SD_MMC.open(root);
  if (!dir || !dir.isDirectory()) return 0;
  int n = 0;
  File e;
  while (n < cap && (e = dir.openNextFile())) {
    if (!e.isDirectory()) {
      const char *name = e.name();
      const char *base = strrchr(name, '/');
      base = base ? base + 1 : name;
      if (base[0] == 'p' && base[1] != 's') {
        int dex = 0, i = 1;
        while (base[i] >= '0' && base[i] <= '9' && i < 5) {
          dex = dex * 10 + (base[i] - '0');
          i++;
        }
        if (dex >= 1 && !strcmp(base + i, ".bin")) out[n++] = (uint16_t)dex;
      }
    }
    e.close();
  }
  dir.close();
  return n;
}

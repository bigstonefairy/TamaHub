#pragma once
//
// Procedural faces for TamaClock.
//
// These are original, primitive-drawn scenes so the clock works with no SD
// card at all.  A pack on the SD card can replace any of them (see
// clockpack.h); the firmware falls back to these when the pack is missing.
//
// `hp` (hour phase) is 0..1 while the hourly animation plays and negative
// otherwise.  All drawing stays inside a 260 x 190 box centred on (cx, cy).

#include <Arduino.h>
#include <math.h>
#include "Arduino_GFX_Library.h"

namespace scenes {

static const float PI_F = 3.14159265f;

// Half of a filled disc, flat side on the centre line.  `upper` selects the
// top half (screen up), which fillArc() cannot express portably.
static inline void semiDisc(Arduino_GFX *g, int16_t cx, int16_t cy, int16_t r,
                            bool upper, uint16_t color) {
  for (int16_t dy = 0; dy <= r; dy++) {
    const int16_t w = (int16_t)sqrtf((float)(r * r - dy * dy));
    if (w <= 0) continue;
    g->drawFastHLine(cx - w, upper ? cy - dy : cy + dy, 2 * w + 1, color);
  }
}

static inline void star(Arduino_GFX *g, int16_t x, int16_t y, int16_t r,
                        uint16_t color) {
  g->fillTriangle(x, y - r, x - r / 3, y, x + r / 3, y, color);
  g->fillTriangle(x, y + r, x - r / 3, y, x + r / 3, y, color);
  g->fillTriangle(x - r, y, x, y - r / 3, x, y + r / 3, color);
  g->fillTriangle(x + r, y, x, y - r / 3, x, y + r / 3, color);
}

static inline void heart(Arduino_GFX *g, int16_t x, int16_t y, int16_t s,
                         uint16_t color) {
  g->fillCircle(x - s / 2, y - s / 4, s / 2, color);
  g->fillCircle(x + s / 2, y - s / 4, s / 2, color);
  g->fillTriangle(x - s, y - s / 8, x + s, y - s / 8, x, y + s, color);
}

// ---------------------------------------------------------------- Pokemon ---
static inline void poke(Arduino_GFX *g, int16_t cx, int16_t cy, uint32_t ms,
                        float hp, uint16_t accent) {
  const int16_t ground = cy + 84;
  g->fillEllipse(cx, ground + 6, 66, 8, 0x18C3);
  // Grass tufts.
  g->fillTriangle(cx - 92, ground, cx - 86, ground - 14, cx - 80, ground,
                  0x04A0);
  g->fillTriangle(cx + 80, ground, cx + 86, ground - 12, cx + 92, ground,
                  0x04A0);

  const int16_t bob = (int16_t)(sinf(ms / 520.0f) * 5.0f);
  const int16_t by = cy + 18 + bob;
  const int16_t r = 46;
  const int16_t lift = (hp >= 0) ? (int16_t)(sinf(hp * PI_F) * 74.0f) : 0;

  if (hp >= 0 && hp > 0.15f && hp < 0.85f) {
    const int16_t glow = (int16_t)(40 + 30 * sinf(hp * PI_F));
    g->fillCircle(cx, by, r + glow, 0x2945);
    for (int i = 0; i < 6; i++) {
      const float a = (i * 1.0472f) + hp * 6.0f;
      star(g, cx + (int16_t)(cosf(a) * (r + 26)),
           by + (int16_t)(sinf(a) * (r + 20)), 7, 0xFFE0);
    }
  }

  // Bottom half, band, then the lifted top half.
  semiDisc(g, cx, by, r, false, 0xFFDE);
  g->fillRect(cx - r, by - 6, 2 * r, 12, 0x0000);
  if (lift > 0) {
    semiDisc(g, cx, by - lift, r, true, accent);
    g->fillRect(cx - r, by - lift - 6, 2 * r, 12, 0x0000);
  } else {
    semiDisc(g, cx, by, r, true, accent);
    g->fillRect(cx - r, by - 6, 2 * r, 12, 0x0000);
  }

  const int16_t btnY = lift > 0 ? by - 6 : by;
  g->fillCircle(cx, btnY, 15, 0x0000);
  g->fillCircle(cx, btnY, 11, 0xFFFF);
  g->fillCircle(cx, btnY, 5, 0x0000);
}

// ---------------------------------------------------------------- Digimon ---
static inline void digi(Arduino_GFX *g, int16_t cx, int16_t cy, uint32_t ms,
                        float hp, uint16_t accent) {
  const int16_t ground = cy + 88;
  g->fillEllipse(cx, ground, 62, 9, 0x18C3);

  // Pulsing digital ring.
  for (int16_t a = 0; a < 360; a += 6) {
    const float rad = a * 0.0174533f;
    const int16_t rr = 96 + (int16_t)(sinf(ms / 400.0f + a * 0.05f) * 4);
    g->fillCircle(cx + (int16_t)(cosf(rad) * rr),
                  cy - 4 + (int16_t)(sinf(rad) * rr * 0.72f), 2, 0x2945);
  }

  const int16_t ey = cy - 4;
  const float wobble =
      (hp >= 0) ? sinf(hp * PI_F * 7.0f) * 4.0f : sinf(ms / 900.0f) * 1.5f;
  const int16_t ex = cx + (int16_t)wobble;

  // Egg.
  g->fillEllipse(ex, ey, 44, 58, 0xFFDE);
  g->fillEllipse(ex, ey - 18, 36, 38, 0xFFFF);
  for (int16_t i = -2; i <= 2; i++) {
    g->fillTriangle(ex + i * 14 - 7, ey - 6, ex + i * 14 + 7, ey - 6,
                    ex + i * 14, ey + 10, accent);
  }

  if (hp >= 0.45f && hp < 0.95f) {
    // Cracks.
    const int16_t k = 7;
    g->fillTriangle(ex - 2, ey - 40, ex + 12, ey - 26, ex - 10, ey - 18, 0x0000);
    g->fillTriangle(ex + 12, ey - 26, ex - 10, ey - 18, ex + 4, ey - 4, 0x0000);
    g->fillTriangle(ex - k, ey + 10, ex + 16, ey + 20, ex - 6, ey + 34, 0x0000);
  }
  if (hp >= 0.9f) {
    // Hatchling: round body, eyes, antenna.
    const int16_t hy = ey + 6;
    g->fillEllipse(ex, hy, 32, 30, 0xFFFF);
    g->fillCircle(ex - 12, hy - 4, 4, 0x0000);
    g->fillCircle(ex + 12, hy - 4, 4, 0x0000);
    g->fillTriangle(ex - 6, hy + 8, ex + 6, hy + 8, ex, hy + 16, accent);
    g->fillRect(ex - 2, hy - 44, 4, 16, accent);
    g->fillCircle(ex, hy - 48, 6, 0xFFE0);
  } else {
    // Eyes peek through the shell.
    g->fillCircle(ex - 13, ey + 8, 4, 0x0000);
    g->fillCircle(ex + 13, ey + 8, 4, 0x0000);
  }
}

// ---------------------------------------------------------------- Mario -----
static inline void guy(Arduino_GFX *g, int16_t x, int16_t y, uint32_t ms,
                       bool jumping) {
  // y is the ground line under the feet.
  const int16_t leg = jumping ? 2 : (int16_t)(sinf(ms / 140.0f) * 5);
  g->fillRect(x - 10, y - 24, 7, 18 + leg, 0x0014);   // left leg
  g->fillRect(x + 3, y - 24, 7, 18 - leg, 0x0014);    // right leg
  g->fillRect(x - 8, y - 40, 16, 18, 0x04BF);         // overalls
  g->fillRect(x - 12, y - 56, 24, 18, 0xD820);        // shirt/sleeves
  g->fillCircle(x, y - 66, 12, 0xFFDE);               // head
  g->fillRect(x - 14, y - 78, 28, 8, 0xD800);         // cap
  g->fillRect(x + 2, y - 74, 18, 5, 0xD800);          // brim
  g->fillRect(x - 6, y - 60, 12, 3, 0x39E7);          // moustache
  g->fillCircle(x - 4, y - 68, 2, 0x0000);
  g->fillCircle(x + 4, y - 68, 2, 0x0000);
}

static inline void mario(Arduino_GFX *g, int16_t cx, int16_t cy, uint32_t ms,
                         float hp, uint16_t accent) {
  const int16_t ground = cy + 76;
  // Brick floor.
  for (int i = 0; i < 6; i++) {
    const int16_t x = cx - 126 + i * 42;
    g->fillRect(x, ground, 40, 16, 0x9A60);
    g->drawRect(x, ground, 40, 16, 0x6300);
  }

  // Question block.
  const int16_t bx = cx - 30, by = cy - 78, bs = 58;
  g->fillRect(bx, by, bs, bs, 0xFDE0);
  g->drawRect(bx, by, bs, bs, 0xA2A0);
  g->drawRect(bx + 4, by + 4, bs - 8, bs - 8, 0xA2A0);
  g->setTextSize(5);
  g->setTextColor(0xA2A0);
  g->setCursor(bx + 17, by + 9);
  g->print("?");

  // Coin: rises and spins during the hourly show.
  const float ct = (hp >= 0) ? hp : fmodf(ms / 2600.0f, 1.0f);
  const int16_t coinY = by - 26 - (int16_t)(sinf(ct * PI_F) * 46.0f);
  const int16_t cw = 6 + (int16_t)(fabsf(cosf(ct * 2.0f * PI_F)) * 14.0f);
  g->fillEllipse(cx, coinY, cw, 17, 0xFFE0);
  g->drawEllipse(cx, coinY, cw, 17, 0xC600);
  if (cw > 10) g->fillRect(cx - 2, coinY - 8, 4, 16, 0xC600);

  // Walker.
  const int16_t wx =
      cx - 84 + (int16_t)((sinf(ms / 1100.0f) * 0.5f + 0.5f) * 112.0f);
  int16_t wy = ground;
  bool jumping = false;
  if (hp >= 0) {
    wy -= (int16_t)(sinf(hp * PI_F) * 66.0f);
    jumping = true;
  }
  guy(g, wx, wy, ms, jumping);

  if (hp >= 0 && hp > 0.2f && hp < 0.8f) {
    g->setTextSize(2);
    g->setTextColor(accent);
    g->setCursor(cx + 46, coinY - 34);
    g->print("+100");
  }
}

// ---------------------------------------------------------------- Zelda -----
static inline void zelda(Arduino_GFX *g, int16_t cx, int16_t cy, uint32_t ms,
                         float hp, uint16_t accent) {
  const int16_t ty = cy - 64;
  const uint16_t gold = 0xFEA0;

  // Three pieces light up in sequence during the hourly show.
  int lit = 3;
  if (hp >= 0) {
    lit = (int)(hp * 3.0f) + 1;
    if (lit > 3) lit = 3;
  }
  const uint16_t c1 = lit >= 1 ? gold : 0x52AA;
  const uint16_t c2 = lit >= 2 ? gold : 0x52AA;
  const uint16_t c3 = lit >= 3 ? gold : 0x52AA;

  g->fillTriangle(cx, ty - 34, cx - 34, ty + 20, cx + 34, ty + 20, c1);
  g->fillTriangle(cx - 36, ty + 26, cx - 70, ty + 80, cx - 2, ty + 80, c2);
  g->fillTriangle(cx + 36, ty + 26, cx + 2, ty + 80, cx + 70, ty + 80, c3);

  // Sword in the pedestal.
  const int16_t rise = (hp >= 0) ? (int16_t)(sinf(hp * PI_F) * 46.0f) : 0;
  const int16_t sy = cy + 26 - rise;
  g->fillTriangle(cx, sy - 72, cx - 9, sy - 4, cx + 9, sy - 4, 0xE7FF);
  g->fillRect(cx - 2, sy - 8, 4, 34, 0x8C71);
  g->fillRect(cx - 24, sy + 20, 48, 8, gold);
  g->fillCircle(cx, sy + 36, 8, gold);
  if (hp >= 0) {
    for (int i = 0; i < 5; i++) {
      const float a = hp * 6.0f + i * 1.2566f;
      star(g, cx + (int16_t)(cosf(a) * 70), sy - 20 + (int16_t)(sinf(a) * 46),
           6, 0xFFFF);
    }
  }

  // Pedestal.
  g->fillRect(cx - 58, cy + 62, 116, 12, 0x8410);
  g->fillRect(cx - 44, cy + 74, 88, 10, 0x630C);

  // Hearts.
  heart(g, cx - 84, cy + 34, 8, 0xD800);
  heart(g, cx + 84, cy + 34, 8, 0xD800);

  // Ambient sparkles.
  for (int i = 0; i < 3; i++) {
    const float t = ms / 1000.0f + i * 2.1f;
    star(g, cx + (int16_t)(sinf(t * 0.7f) * 104),
         cy - 20 + (int16_t)(cosf(t * 0.9f) * 60), 4, 0xFFFF);
  }
}

}  // namespace scenes

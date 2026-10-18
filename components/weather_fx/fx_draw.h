#pragma once

// Общее для частей погодного фона, которые рисуют готовыми картинками
// (дождь, облака, снег, листья, солнце): буфер картинки и мелкие помощники.

#include <algorithm>
#include <cmath>
#include <cstring>

#include <lvgl.h>
// lv_image_cache_drop и lv_layer_t — во внутренних заголовках LVGL
#include <lvgl_private.h>

#include "weather_fx.h"

namespace esphome {
namespace weather_fx {

// Круглый экран: центр и радиус
static const float FX_CX = 233.0f, FX_CY = 233.0f, FX_R = 233.0f;

static inline float fx_clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static inline float fx_smooth(float e0, float e1, float x) {
  const float t = fx_clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}

// Нижний край круга под точкой x
static inline float fx_bottom(float x) {
  const float d = x - FX_CX;
  return FX_CY + sqrtf(std::max(0.0f, FX_R * FX_R - d * d));
}

static inline bool fx_hit(const lv_area_t &a, const lv_area_t &clip) {
  return a.x1 <= clip.x2 && a.x2 >= clip.x1 && a.y1 <= clip.y2 && a.y2 >= clip.y1;
}

// Буфер картинки нужного размера: маска A8 (цвет — при рисовании) или
// RGB565A8. Память — из кучи LVGL (на плате это PSRAM), выделяется один раз и
// потом только переиспользуется. Буфер обнуляется
static inline bool fx_sprite_init(FxSprite &s, int w, int h, bool rgb) {
  const int bytes = w * h * (rgb ? 3 : 1);
  if (s.cap < bytes) {
    if (s.buf)
      lv_free(s.buf);
    s.buf = static_cast<uint8_t *>(lv_malloc(bytes));
    s.cap = s.buf ? bytes : 0;
    if (!s.buf)
      return false;
  }
  memset(s.buf, 0, bytes);
  lv_image_dsc_t &d = s.dsc;
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = rgb ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_A8;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = rgb ? w * 2 : w;
  d.data = s.buf;
  d.data_size = bytes;
  return true;
}

// Картинка дорисована — LVGL должен забыть прежнюю
static inline void fx_sprite_done(FxSprite &s) { lv_image_cache_drop(&s.dsc); }

// Картинка больше не нужна — вернуть память
static inline void fx_sprite_free(FxSprite &s) {
  if (!s.buf)
    return;
  lv_image_cache_drop(&s.dsc);
  lv_free(s.buf);
  s.buf = nullptr;
  s.cap = 0;
}

// Прямоугольник картинки, привязанной точкой (ax, ay) к (x, y)
static inline lv_area_t fx_sprite_area(const FxSprite &s, int x, int y) {
  const int x1 = x - s.ax, y1 = y - s.ay;
  return {x1, y1, x1 + (int32_t) s.dsc.header.w - 1, y1 + (int32_t) s.dsc.header.h - 1};
}

// Нарисовать картинку в экранном прямоугольнике a. Маска A8 — цветом c
static inline void fx_draw(lv_layer_t *layer, const FxSprite &s, const lv_area_t &a, lv_opa_t opa,
                           lv_color_t c = lv_color_white()) {
  if (!s.buf || opa <= LV_OPA_MIN)
    return;
  lv_draw_image_dsc_t im;
  lv_draw_image_dsc_init(&im);
  im.src = &s.dsc;
  im.opa = opa;
  im.recolor = c;
  im.image_area = a;
  lv_draw_image(layer, &im, &a);
}

// Быстрый atan2 (ошибка до 0,012°): для картинок, где угол считается
// для каждого пикселя
static inline float fx_atan2(float y, float x) {
  const float ax = fabsf(x), ay = fabsf(y);
  const float mx = std::max(ax, ay), mn = std::min(ax, ay);
  if (mx <= 0)
    return 0;
  const float a = mn / mx, s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax)
    r = 1.57079637f - r;
  if (x < 0)
    r = 3.14159274f - r;
  return y < 0 ? -r : r;
}

// Упорядоченный шум 4×4 (Байер), −0,5..0,5: плавный переход прозрачности
// на RGB565 иначе ложится заметными ступенями-кольцами
static inline float fx_dither(int x, int y) {
  static const uint8_t B[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  return (B[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
}

// Простой генератор случайных чисел с зерном — для рисунков, которые должны
// выходить одинаковыми при каждом запуске (облака, кристаллы снежинок)
struct FxRand {
  uint32_t s;
  explicit FxRand(uint32_t seed) : s(seed) {}
  float f() {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) / 16777216.0f;
  }
  float range(float lo, float hi) { return lo + (hi - lo) * this->f(); }
};

}  // namespace weather_fx
}  // namespace esphome

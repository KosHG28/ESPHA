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

// Набор картинок: вернуть память
static inline void fx_sheet_free(FxSheet &s) {
  if (s.dsc) {
    for (int i = 0; i < s.n; i++)
      if (s.ready && (s.ready[i >> 3] & (1 << (i & 7))))
        lv_image_cache_drop(&s.dsc[i]);
    lv_free(s.dsc);
  }
  if (s.buf)
    lv_free(s.buf);
  if (s.ready)
    lv_free(s.ready);
  if (s.crop)
    lv_free(s.crop);
  s = FxSheet{};
}

// Набор из n картинок w×h (маски A8 или RGB565A8): память на все сразу,
// рисуются по мере надобности (fx_sheet_take)
static inline bool fx_sheet_init(FxSheet &s, int n, int w, int h, bool rgb) {
  fx_sheet_free(s);
  const int px = w * h * (rgb ? 3 : 1);
  s.dsc = static_cast<lv_image_dsc_t *>(lv_malloc(n * sizeof(lv_image_dsc_t)));
  s.buf = static_cast<uint8_t *>(lv_malloc(n * px));
  s.ready = static_cast<uint8_t *>(lv_malloc((n + 7) / 8));
  s.crop = static_cast<int16_t *>(lv_malloc(2 * n * sizeof(int16_t)));
  if (!s.dsc || !s.buf || !s.ready || !s.crop) {
    fx_sheet_free(s);
    return false;
  }
  memset(s.dsc, 0, n * sizeof(lv_image_dsc_t));
  memset(s.ready, 0, (n + 7) / 8);
  memset(s.crop, 0, 2 * n * sizeof(int16_t));
  for (int i = 0; i < n; i++) {
    lv_image_dsc_t &d = s.dsc[i];
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = rgb ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_A8;
    d.header.w = w;
    d.header.h = h;
    d.header.stride = rgb ? w * 2 : w;
    d.data = s.buf + i * px;
    d.data_size = px;
  }
  s.n = n;
  s.w = w;
  s.h = h;
  s.rgb = rgb;
  return true;
}

// Картинку i ещё не рисовали? Тогда вернуть её пиксели (обнулённые) — нарисовать
// и отдать fx_sheet_done; уже нарисована — nullptr
static inline uint8_t *fx_sheet_take(FxSheet &s, int i) {
  if (!s.buf || i < 0 || i >= s.n || (s.ready[i >> 3] & (1 << (i & 7))))
    return nullptr;
  s.ready[i >> 3] |= (uint8_t) (1 << (i & 7));
  // Прежде картинка могла быть обрезана — снова во весь размер
  lv_image_dsc_t &d = s.dsc[i];
  d.header.w = s.w;
  d.header.h = s.h;
  d.header.stride = s.rgb ? s.w * 2 : s.w;
  d.data_size = s.w * s.h * (s.rgb ? 3 : 1);
  s.crop[2 * i] = s.crop[2 * i + 1] = 0;
  uint8_t *p = const_cast<uint8_t *>(static_cast<const uint8_t *>(d.data));
  memset(p, 0, d.data_size);
  return p;
}

// Только что нарисованную картинку i обрезать по рисунку: прозрачные поля
// не попадают ни в участок перерисовки, ни в смешивание при каждом выводе.
// Строки сдвигаются к началу на месте
static inline void fx_sheet_crop(FxSheet &s, int i) {
  lv_image_dsc_t &d = s.dsc[i];
  uint8_t *p = const_cast<uint8_t *>(static_cast<const uint8_t *>(d.data));
  const uint8_t *al = s.rgb ? p + s.w * s.h * 2 : p;
  int x0 = s.w, y0 = s.h, x1 = -1, y1 = -1;
  for (int y = 0; y < s.h; y++) {
    for (int x = 0; x < s.w; x++) {
      if (al[y * s.w + x]) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = y;
      }
    }
  }
  if (x1 < 0)
    x0 = y0 = x1 = y1 = 0;  // пустая — одна прозрачная точка
  const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
  if (cw == s.w && ch == s.h)
    return;
  if (s.rgb) {
    for (int y = 0; y < ch; y++)
      memmove(p + y * cw * 2, p + ((y + y0) * s.w + x0) * 2, cw * 2);
    for (int y = 0; y < ch; y++)
      memmove(p + cw * ch * 2 + y * cw, al + (y + y0) * s.w + x0, cw);
  } else {
    for (int y = 0; y < ch; y++)
      memmove(p + y * cw, p + (y + y0) * s.w + x0, cw);
  }
  d.header.w = cw;
  d.header.h = ch;
  d.header.stride = s.rgb ? cw * 2 : cw;
  d.data_size = cw * ch * (s.rgb ? 3 : 1);
  s.crop[2 * i] = (int16_t) x0;
  s.crop[2 * i + 1] = (int16_t) y0;
}

static inline void fx_sheet_done(FxSheet &s, int i) { lv_image_cache_drop(&s.dsc[i]); }

// Картинка i уже нарисована?
static inline bool fx_sheet_ready(const FxSheet &s, int i) {
  return s.buf && i >= 0 && i < s.n && (s.ready[i >> 3] & (1 << (i & 7)));
}

// Сменились цвета — все картинки нарисуются заново, когда понадобятся
static inline void fx_sheet_reset(FxSheet &s) {
  if (!s.buf)
    return;
  for (int i = 0; i < s.n; i++)
    if (s.ready[i >> 3] & (1 << (i & 7)))
      lv_image_cache_drop(&s.dsc[i]);
  memset(s.ready, 0, (s.n + 7) / 8);
}

// Прямоугольник картинки i набора (с учётом обрезки), привязанной точкой
// (ax, ay) к (x, y)
static inline lv_area_t fx_sheet_area(const FxSheet &s, int i, int x, int y) {
  if (!s.buf || i < 0 || i >= s.n)
    return {x - s.ax, y - s.ay, x - s.ax + s.w - 1, y - s.ay + s.h - 1};
  const int x1 = x - s.ax + s.crop[2 * i], y1 = y - s.ay + s.crop[2 * i + 1];
  return {x1, y1, x1 + (int32_t) s.dsc[i].header.w - 1, y1 + (int32_t) s.dsc[i].header.h - 1};
}

// Нарисовать картинку i набора в прямоугольнике a. Маска A8 — цветом c
static inline void fx_sheet_draw(lv_layer_t *layer, const FxSheet &s, int i, const lv_area_t &a, lv_opa_t opa,
                                 lv_color_t c = lv_color_white()) {
  if (!s.buf || i < 0 || i >= s.n || opa <= LV_OPA_MIN)
    return;
  lv_draw_image_dsc_t im;
  lv_draw_image_dsc_init(&im);
  im.src = &s.dsc[i];
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

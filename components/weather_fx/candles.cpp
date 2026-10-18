// «Романтика»: свечи по нижнему краю.
//
// Готовые картинки: восковое тело свечи (своей высоты у каждой) — цилиндр
// со светом слева, потёком воска и верхом, просвечивающим от пламени;
// пламя — капля с голубоватым основанием, оранжевым краем и белым ядром,
// нарисованная заранее в четырёх высотах и пяти наклонах вместе с фитилём;
// тёплый ореол — мягкое пятно, его яркость дрожит вместе с пламенем. Прежде
// ореол был тремя сглаженными кругами LVGL на каждую свечу — больше
// половины всей работы сцены.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

// Основания свечей — на дуге радиусом 200 px по низу круга, высота у каждой
// своя; пламя — над фитилём
static const int CND_X[5] = {75, 145, 233, 321, 391}, CND_Y[5] = {356, 413, 433, 413, 356};
static const int CND_H[5] = {30, 38, 26, 36, 32};
static const int CND_GLOW = 26;  // радиус ореола
// Пламя: высоты и наклоны, нарисованные заранее
static const float FLAME_H[4] = {10.0f, 12.0f, 14.0f, 16.0f};
static const float FLAME_DX[5] = {-1.6f, -0.8f, 0.0f, 0.8f, 1.6f};
static const int FLAME_W = 17, FLAME_HGT = 26, FLAME_AY = 21;  // картинка, привязка — верх фитиля

// Пламя высотой h с наклоном dx (смещение кончика): капля, к кончику
// острее; внутри — белое ядро, к краю оранжевее, у основания — голубизна.
// Фитиль — тёмная чёрточка снизу
static void draw_flame(uint8_t *p, float h, float dx) {
  uint16_t *rgb = reinterpret_cast<uint16_t *>(p);
  uint8_t *alpha = p + FLAME_W * FLAME_HGT * 2;
  const float cx = FLAME_W / 2.0f;
  for (int y = 0; y < FLAME_HGT; y++) {
    for (int x = 0; x < FLAME_W; x++) {
      float a = 0, r = 0, g = 0, b = 0;
      for (int q = 0; q < 4; q++) {
        const float px = x + 0.25f + 0.5f * (q & 1), py = y + 0.25f + 0.5f * (q >> 1);
        // Высота над верхом фитиля (вверх — плюс)
        const float v = FLAME_AY - py;
        // Фитиль: от верха свечи (4 px ниже) до основания пламени
        if (v < 0.5f && v > -4.0f) {
          const float wx = cx + dx * 0.4f * (v + 4.0f) / 4.0f;
          if (fabsf(px - wx) < 0.8f) {
            a += 1.0f;
            r += 58;
            g += 42;
            b += 26;
          }
          continue;
        }
        const float t = (v + 1.5f) / (h + 1.5f);  // 0 — низ капли, 1 — кончик
        if (t < 0 || t > 1)
          continue;
        const float axis = cx + dx * t * sqrtf(t) * 1.5f;
        const float w = 4.3f * powf(std::max(0.0f, 1.0f - powf(t, 1.6f)), 0.7f) * fx_smooth(-0.05f, 0.22f, t);
        const float d = fabsf(px - axis);
        if (d >= w)
          continue;
        // Цвет: ядро у основания и посередине — белое, к краю и кончику —
        // оранжевее; у самого низа края голубоватые
        const float e = d / w;
        const float core = (1.0f - fx_smooth(0.0f, 0.55f, e)) * (1.0f - fx_smooth(0.2f, 0.6f, t));
        float cr = 255, cg = 150 + 70 * (1 - e) + 30 * core, cb = 30 + 200 * core;
        const float blue = (1.0f - fx_smooth(0.05f, 0.2f, t)) * fx_smooth(0.4f, 0.9f, e);
        cr = cr * (1 - blue) + 110 * blue;
        cg = cg * (1 - blue) + 150 * blue;
        cb = cb * (1 - blue) + 255 * blue;
        // К кончику и краю пламя прозрачнее
        const float k = (1.0f - 0.55f * fx_smooth(0.6f, 1.0f, t)) * (1.0f - 0.35f * fx_smooth(0.7f, 1.0f, e));
        a += k;
        r += cr * k;
        g += cg * k;
        b += cb * k;
      }
      if (a <= 0)
        continue;
      rgb[y * FLAME_W + x] = lv_color_to_u16(lv_color_make(
          (uint8_t) std::min(255.0f, r / a), (uint8_t) std::min(255.0f, g / a), (uint8_t) std::min(255.0f, b / a)));
      alpha[y * FLAME_W + x] = (uint8_t) std::min(255.0f, a / 4.0f * 255.0f + 0.5f);
    }
  }
}

// Тело свечи высотой hgt: цилиндр 13 px — светлее слева, верх просвечивает
// тёплым от пламени, потёк воска слева сверху. Привязка — середина верха
static void draw_body(FxSprite &s, int hgt) {
  const int w = 13, h = hgt + 1;
  if (!fx_sprite_init(s, w, h, true))
    return;
  s.ax = w / 2;
  s.ay = 0;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + w * h * 2;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      // Скруглённые углы — радиус 3
      const float cxl = std::min(std::max(x + 0.5f, 3.0f), w - 3.0f),
                  cyl = std::min(std::max(y + 0.5f, 3.0f), h - 3.0f);
      const float d = sqrtf((x + 0.5f - cxl) * (x + 0.5f - cxl) + (y + 0.5f - cyl) * (y + 0.5f - cyl));
      const float cov = fx_clamp01(3.5f - d);
      if (cov <= 0)
        continue;
      // Цилиндр: свет слева, тень справа
      const float u = (x + 0.5f) / w * 2.0f - 1.0f;
      float k = 1.02f - 0.22f * u - 0.18f * u * u * u * u;
      // Верх просвечивает тёплым
      const float warm = 1.0f - fx_smooth(0.0f, 9.0f, (float) y);
      float r = 243 * k, g = 227 * k, b = 195 * k;
      r += (255 - r) * 0.5f * warm;
      g += (214 - g) * 0.45f * warm;
      b += (150 - b) * 0.5f * warm;
      // Потёк воска слева сверху — светлее
      const float dx = (x + 0.5f - 2.5f) / 1.6f, dy = (y + 0.5f - 6.0f) / 4.5f;
      if (dx * dx + dy * dy < 1.0f) {
        r = std::min(255.0f, r * 1.06f + 8);
        g = std::min(255.0f, g * 1.06f + 8);
        b = std::min(255.0f, b * 1.06f + 8);
      }
      rgb[y * w + x] = lv_color_to_u16(
          lv_color_make((uint8_t) std::min(255.0f, r), (uint8_t) std::min(255.0f, g), (uint8_t) std::min(255.0f, b)));
      alpha[y * w + x] = (uint8_t) (cov * 255.0f);
    }
  }
  fx_sprite_done(s);
}

// Мягкое пятно ореола
static void draw_glow(FxSprite &s) {
  const int h = CND_GLOW, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float d = sqrtf((float) ((x - h) * (x - h) + (y - h) * (y - h))) / h;
      const float v = 1.0f - fx_clamp01(d);
      if (v > 0)
        s.buf[y * n + x] = (uint8_t) std::min(255.0f, std::max(0.0f, 255.0f * powf(v, 1.3f) + fx_dither(x, y) * 5.0f));
    }
  }
  fx_sprite_done(s);
}

// ---------------------------------------------------------------------------

void WeatherFx::candles_apply_(bool on) {
  this->cnd_on_ = on;
  if (on && !this->cnd_glow_spr_.buf) {
    draw_glow(this->cnd_glow_spr_);
    for (int i = 0; i < NCND; i++)
      draw_body(this->cnd_body_[i], CND_H[i]);
    if (fx_sheet_init(this->cnd_flame_, 4 * 5, FLAME_W, FLAME_HGT, true)) {
      this->cnd_flame_.ax = FLAME_W / 2;
      this->cnd_flame_.ay = FLAME_AY;
    }
  } else if (!on) {
    fx_sprite_free(this->cnd_glow_spr_);
    for (auto &b : this->cnd_body_)
      fx_sprite_free(b);
    fx_sheet_free(this->cnd_flame_);
  }
  for (int i = 0; i < NCND; i++) {
    this->cnd_fh_[i] = 13;
    this->cnd_dx_[i] = 0;
    this->cnd_glow_[i] = 0.8f;
    const int x = CND_X[i], top = CND_Y[i] - CND_H[i];
    // Ореол вокруг пламени, само пламя и тело свечи
    const int y1 = top - 11 - CND_GLOW;
    this->mark_(this->cnd_spot_[i], on, x - CND_GLOW, y1, 2 * CND_GLOW + 1, CND_Y[i] - y1 + 1);
  }
}

void WeatherFx::candles_(uint32_t now) {
  if (!this->cnd_on_ || now - this->cnd_ms_ < 70)
    return;
  this->cnd_ms_ = now;
  // Пламя дрожит: высота, наклон и яркость ореола бродят понемногу.
  // Перерисовывается ореол с пламенем — тело свечи ниже не меняется
  for (int i = 0; i < NCND; i++) {
    this->cnd_fh_[i] = std::min(16.0f, std::max(10.0f, this->cnd_fh_[i] + rnd_(-15, 16) / 10.0f));
    this->cnd_dx_[i] = std::min(1.6f, std::max(-1.6f, this->cnd_dx_[i] * 0.7f + rnd_(-10, 11) / 10.0f));
    this->cnd_glow_[i] = std::min(1.0f, std::max(0.6f, this->cnd_glow_[i] + rnd_(-12, 13) / 100.0f));
    const int x = CND_X[i], fy = CND_Y[i] - CND_H[i] - 11;
    const lv_area_t a = {x - CND_GLOW, fy - CND_GLOW, x + CND_GLOW, fy + CND_GLOW};
    this->invalidate_(a);
  }
}

void WeatherFx::candles_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->cnd_on_ || !this->cnd_glow_spr_.buf)
    return;
  const bool dim = this->dim_;
  for (int i = 0; i < NCND; i++) {
    const Spot &s = this->cnd_spot_[i];
    if (!s.on)
      continue;
    const lv_area_t box = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(box, clip))
      continue;
    const int x = CND_X[i] + oc.x1, top = CND_Y[i] - CND_H[i] + oc.y1;
    // Ореол — с середины пламени
    const int fy = top - 11;
    fx_draw(layer, this->cnd_glow_spr_, fx_sprite_area(this->cnd_glow_spr_, x, fy),
            (lv_opa_t) ((dim ? 175.0f : 135.0f) * this->cnd_glow_[i]), lv_color_hex(0xFF9F43));
    fx_draw(layer, this->cnd_body_[i], fx_sprite_area(this->cnd_body_[i], x, top), LV_OPA_COVER);
    // Пламя: ближайшие нарисованные высота и наклон
    int hi = 0, di = 0;
    for (int k = 1; k < 4; k++)
      if (fabsf(FLAME_H[k] - this->cnd_fh_[i]) < fabsf(FLAME_H[hi] - this->cnd_fh_[i]))
        hi = k;
    for (int k = 1; k < 5; k++)
      if (fabsf(FLAME_DX[k] - this->cnd_dx_[i]) < fabsf(FLAME_DX[di] - this->cnd_dx_[i]))
        di = k;
    const int idx = hi * 5 + di;
    if (uint8_t *p = fx_sheet_take(this->cnd_flame_, idx)) {
      draw_flame(p, FLAME_H[hi], FLAME_DX[di]);
      fx_sheet_crop(this->cnd_flame_, idx);
      fx_sheet_done(this->cnd_flame_, idx);
    }
    fx_sheet_draw(layer, this->cnd_flame_, idx, fx_sheet_area(this->cnd_flame_, idx, x, top - 4), LV_OPA_COVER);
  }
}

}  // namespace weather_fx
}  // namespace esphome

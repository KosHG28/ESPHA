// Новогодняя гирлянда по краю экрана.
//
// Провод чуть провисает между лампочками, лампочки — капельки на патронах,
// смотрят к середине экрана, с бликом и мягким свечением вокруг. Всё —
// готовые маски A8 для каждой из 24 лампочек и 24 отрезков провода (у
// каждой своё направление), цвет задаётся при рисовании. Лампочки
// перемигиваются бегущей волной — каждый шаг гаснет каждая третья, — и
// загораются и гаснут не сразу, а через полутон. Прежде провод был
// сглаженной дугой LVGL во весь круг, и каждый участок у края экрана
// пересчитывал её заново.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

static const float GAR_RF = 221.0f;  // радиус провода
static const uint32_t GAR_PAL[4] = {0xFF3B30, 0xFFD60A, 0x30D158, 0x0A84FF};
static const float BULB_K = 1.3f;  // размер лампочки (рисунок ниже — при 1)
static const float BULB_OFS = 6.5f;  // середина колбы — от провода к центру при размере 1
static const float BULB_GLOW = 11.5f;  // радиус свечения, px
static const int BULB_H = 12;          // полуразмер картинки лампочки
static const float WIRE_SAG = 4.0f;    // провисание провода между лампочками
static const uint32_t STEP_MS = 600, HALF_MS = 130;

// Точка провода у лампочки i из n (в патроне) и направление к середине
// экрана
static void socket_at(int i, int n, float *x, float *y, float *nx, float *ny) {
  const float a = (i + 0.5f) * 6.28318531f / n;
  *nx = -cosf(a);
  *ny = -sinf(a);
  *x = 233.0f - GAR_RF * *nx;
  *y = 233.0f - GAR_RF * *ny;
}

// Лампочка в своих координатах: начало — патрон на проводе, v — к середине
// экрана, u — поперёк. 0 — пусто, 1 — патрон, 2 — колба, 3 — блик
static int bulb_part(float u, float v) {
  if (fabsf(u) < 1.6f && v > -0.6f && v < 2.3f)
    return 1;
  // Колба-капелька: шар у конца, к патрону сужается
  const float t = fx_clamp01((v - 2.0f) / 4.5f);
  const float r = 1.5f + 1.6f * sqrtf(t);
  const float cv = std::min(std::max(v, 2.0f), BULB_OFS);
  const float d = sqrtf(u * u + (v - cv) * (v - cv));
  if (v > 2.0f && d < (v > BULB_OFS ? 3.1f : r)) {
    const float hu = (u + 1.0f) / 0.75f, hv = (v - 5.6f) / 1.5f;
    return hu * hu + hv * hv < 1.0f ? 3 : 2;
  }
  return 0;
}

// ---------------------------------------------------------------------------

void WeatherFx::garland_sprites_() {
  const int n = 2 * BULB_H + 1;
  for (int i = 0; i < NG; i++) {
    float sx, sy, nx, ny;
    socket_at(i, NG, &sx, &sy, &nx, &ny);
    // Лампочка: середина картинки — середина колбы
    FxSprite *m[4] = {&this->gb_glow_[i], &this->gb_cap_[i], &this->gb_body_[i], &this->gb_spec_[i]};
    bool ok = true;
    for (auto *s : m) {
      ok = ok && fx_sprite_init(*s, n, n, false);
      s->ax = s->ay = BULB_H;
    }
    if (!ok)
      return;
    for (int y = 0; y < n; y++) {
      for (int x = 0; x < n; x++) {
        // Свечение — мягкое пятно вокруг колбы
        const float dx = (float) (x - BULB_H), dy = (float) (y - BULB_H);
        const float g = 1.0f - fx_clamp01(sqrtf(dx * dx + dy * dy) / BULB_GLOW);
        this->gb_glow_[i].buf[y * n + x] = (uint8_t) (255.0f * g * g);
        // Колба, патрон и блик — 3×3 подвыборки в координатах лампочки
        int cnt[4] = {0, 0, 0, 0};
        for (int q = 0; q < 9; q++) {
          const float px = dx + ((q % 3) - 1) / 3.0f, py = dy + ((q / 3) - 1) / 3.0f;
          // От середины колбы к патрону — против направления к центру
          const float v = BULB_OFS + (px * nx + py * ny) / BULB_K, u = (-px * ny + py * nx) / BULB_K;
          cnt[bulb_part(u, v)]++;
        }
        this->gb_cap_[i].buf[y * n + x] = (uint8_t) (cnt[1] * 255 / 9);
        this->gb_body_[i].buf[y * n + x] = (uint8_t) ((cnt[2] + cnt[3]) * 255 / 9);
        this->gb_spec_[i].buf[y * n + x] = (uint8_t) (cnt[3] * 255 / 9);
      }
    }
    for (auto *s : m)
      fx_sprite_done(*s);

    // Провод от этой лампочки до следующей: дуга круга, чуть провисшая к
    // середине экрана
    float ex, ey, enx, eny;
    socket_at((i + 1) % NG, NG, &ex, &ey, &enx, &eny);
    const float mnx = (nx + enx) * 0.5f, mny = (ny + eny) * 0.5f;
    // Средняя точка — на окружности провода и ещё провисает внутрь
    const float ml = sqrtf(mnx * mnx + mny * mny);
    const float cx = 233.0f - GAR_RF * mnx / ml + WIRE_SAG * mnx / ml,
                cy = 233.0f - GAR_RF * mny / ml + WIRE_SAG * mny / ml;
    const int x0 = (int) floorf(std::min(std::min(sx, ex), cx)) - 2,
              x1 = (int) ceilf(std::max(std::max(sx, ex), cx)) + 2;
    const int y0 = (int) floorf(std::min(std::min(sy, ey), cy)) - 2,
              y1 = (int) ceilf(std::max(std::max(sy, ey), cy)) + 2;
    FxSprite &w = this->gw_[i];
    if (!fx_sprite_init(w, x1 - x0 + 1, y1 - y0 + 1, false))
      return;
    w.ax = -x0;  // картинка привязана к началу экрана
    w.ay = -y0;
    this->gw_x_[i] = x0;
    this->gw_y_[i] = y0;
    // Квадратичная кривая через середину: 12 отрезков
    float px = sx, py = sy;
    for (int k = 1; k <= 12; k++) {
      const float t = k / 12.0f;
      // Кривая Безье с контрольной точкой так, чтобы пройти через (cx, cy)
      const float kx = 2 * cx - (sx + ex) * 0.5f, ky = 2 * cy - (sy + ey) * 0.5f;
      const float qx = (1 - t) * (1 - t) * sx + 2 * t * (1 - t) * kx + t * t * ex;
      const float qy = (1 - t) * (1 - t) * sy + 2 * t * (1 - t) * ky + t * t * ey;
      const int bx0 = std::max(0, (int) floorf(std::min(px, qx)) - 2 - x0),
                bx1 = std::min(w.dsc.header.w - 1, (int) ceilf(std::max(px, qx)) + 2 - x0);
      const int by0 = std::max(0, (int) floorf(std::min(py, qy)) - 2 - y0),
                by1 = std::min(w.dsc.header.h - 1, (int) ceilf(std::max(py, qy)) + 2 - y0);
      const float dx = qx - px, dy = qy - py, l2 = std::max(1e-3f, dx * dx + dy * dy);
      for (int yy = by0; yy <= by1; yy++) {
        for (int xx = bx0; xx <= bx1; xx++) {
          const float fx = xx + x0 + 0.5f, fy = yy + y0 + 0.5f;
          const float tt = fx_clamp01(((fx - px) * dx + (fy - py) * dy) / l2);
          const float ex2 = px + tt * dx - fx, ey2 = py + tt * dy - fy;
          const int v = (int) (255.0f * fx_clamp01(1.3f - sqrtf(ex2 * ex2 + ey2 * ey2)));
          uint8_t &p = w.buf[yy * w.dsc.header.w + xx];
          if (v > p)
            p = (uint8_t) v;
        }
      }
      px = qx;
      py = qy;
    }
    fx_sprite_done(w);
  }
}

void WeatherFx::garland_apply_() {
  if (this->gar_on_ && !this->gb_body_[0].buf)
    this->garland_sprites_();
  if (!this->gar_on_ && this->gb_body_[0].buf) {
    for (int i = 0; i < NG; i++) {
      fx_sprite_free(this->gb_glow_[i]);
      fx_sprite_free(this->gb_cap_[i]);
      fx_sprite_free(this->gb_body_[i]);
      fx_sprite_free(this->gb_spec_[i]);
      fx_sprite_free(this->gw_[i]);
    }
  }
  for (int i = 0; i < NG; i++) {
    float sx, sy, nx, ny;
    socket_at(i, NG, &sx, &sy, &nx, &ny);
    const int x = (int) lroundf(sx + nx * BULB_OFS * BULB_K), y = (int) lroundf(sy + ny * BULB_OFS * BULB_K);
    this->gx_[i] = x;
    this->gy_[i] = y;
    this->mark_(this->gs_[i], this->gar_on_ && this->gb_body_[i].buf, x - BULB_H, y - BULB_H, 2 * BULB_H + 1,
                2 * BULB_H + 1);
    if (!this->gar_on_)
      this->glev_[i] = 0;
  }
}

void WeatherFx::garland_(uint32_t now) {
  if (!this->gar_on_)
    return;
  // Каждые 0,6 с волна сдвигается: каждая третья лампочка гаснет, а
  // погасшая загорается — через полутон
  if (now - this->gar_ms_ >= STEP_MS) {
    this->gar_ms_ = now;
    this->gar_step_ = (this->gar_step_ + 1) % 3;
  }
  const bool half = now - this->gar_ms_ < HALF_MS;
  for (int i = 0; i < NG; i++) {
    const bool lit = (i + this->gar_step_) % 3 != 0;
    const bool was = (i + this->gar_step_ + 2) % 3 != 0;
    const uint8_t lev = lit == was || !half ? (lit ? 2 : 0) : 1;
    if (lev != this->glev_[i]) {
      this->glev_[i] = lev;
      if (this->gs_[i].on)
        this->invalidate_(this->gs_[i].a);
    }
  }
}

void WeatherFx::garland_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->gar_on_ || !this->gb_body_[0].buf)
    return;
  const bool dim = this->dim_;
  // Провод — только отрезки, которые задевает участок
  const lv_color_t wire = lv_color_hex(dim ? 0x3E7A4C : 0x1E4A2A);
  for (int i = 0; i < NG; i++) {
    const FxSprite &w = this->gw_[i];
    const lv_area_t a = {this->gw_x_[i] + oc.x1, this->gw_y_[i] + oc.y1,
                         this->gw_x_[i] + oc.x1 + (int32_t) w.dsc.header.w - 1,
                         this->gw_y_[i] + oc.y1 + (int32_t) w.dsc.header.h - 1};
    if (fx_hit(a, clip))
      fx_draw(layer, w, a, LV_OPA_COVER, wire);
  }
  const lv_color_t cap = lv_color_hex(dim ? 0x5A6A5E : 0x2C3530);
  for (int i = 0; i < NG; i++) {
    const Spot &s = this->gs_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    const lv_color_t c = lv_color_hex(GAR_PAL[i % 4]);
    const int lev = this->glev_[i];  // 0 погасла, 1 полутон, 2 горит
    if (lev)
      fx_draw(layer, this->gb_glow_[i], a, (lv_opa_t) ((dim ? 170 : 135) * lev / 2), c);
    fx_draw(layer, this->gb_cap_[i], a, LV_OPA_COVER, cap);
    const lv_color_t body = lev == 2   ? lv_color_mix(lv_color_white(), c, 70)
                            : lev == 1 ? c
                                       : lv_color_mix(c, lv_color_black(), dim ? 110 : 70);
    fx_draw(layer, this->gb_body_[i], a, LV_OPA_COVER, body);
    fx_draw(layer, this->gb_spec_[i], a, (lv_opa_t) (lev ? 230 : 90), lv_color_white());
  }
}

}  // namespace weather_fx
}  // namespace esphome

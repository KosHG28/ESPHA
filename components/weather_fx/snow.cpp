// Снег: три слоя глубины и сугроб.
//
// Дальний слой — россыпь мелких тусклых точек: они падают стайками по три
// (стайка — один участок перерисовки) и двигаются через кадр. Средний —
// мягкие круглые хлопья. Ближний — крупные шестилучевые кристаллы с
// веточками, которые медленно поворачиваются; картинки повёрнуты заранее —
// у кристалла шесть одинаковых лучей, так что хватает восьми поворотов в
// пределах 60°. Чем ближе слой, тем быстрее он падает и сильнее качается;
// ветер сносит всех.
//
// Пока идёт снег, по нижнему краю круга растёт сугроб: за 10 минут
// снегопада (за 20 — лёгкого снега) — до 14 px посередине. Снег кончился —
// сугроб за несколько минут тает, в дождь быстрее.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

// Слои: скорость падения, px за 50 мс; размах покачивания и снос ветром
struct SnowLayer {
  float v0, v1, sway, drift;
};
static const SnowLayer SN_LAYERS[3] = {
    {0.50f, 0.70f, 0.30f, 0.6f},  // дальний: 10–14 px/с
    {0.95f, 1.25f, 0.50f, 0.9f},  // средний: 19–25 px/с
    {1.60f, 2.10f, 0.80f, 1.4f},  // ближний: 32–42 px/с
};
// Стайки дальнего слоя: сдвиг точек от первой (первая — ниже всех) и
// яркость каждой
static const int8_t FAR_OFS[4][3][2] = {
    {{0, 0}, {-11, -14}, {8, -22}},
    {{0, 0}, {12, -9}, {-6, -24}},
    {{0, 0}, {-13, -6}, {5, -17}},
    {{0, 0}, {9, -15}, {-12, -26}},
};
static const lv_opa_t FAR_OPA[3] = {255, 215, 180};
// Ближние кристаллы двух размеров: радиус луча
static const float CRY_R[2] = {5.5f, 7.5f};
// Цвета слоёв (дальний, средний, ближний) и сугроба: обычный экран и
// приглушённый — там ярче, иначе при низкой яркости не видно
static const uint32_t SNOW_C[2][3] = {{0x8794A1, 0xB0BCC8, 0xD6E0EA}, {0xC2CBD4, 0xE8EEF4, 0xFFFFFF}};
static const uint32_t COVER_C[2] = {0xA9B6C4, 0xEEF3F8};
// Сугроб: наибольшая высота посередине; где он лежит (по краям круга
// слишком круто — там снег не держится)
static const float COVER_H = 14.0f;
static const int CV_X0 = 43, CV_X1 = 423, CV_Y0 = 364, CV_Y1 = 465;
// Полный сугроб — за столько секунд снегопада силы 16 (слабее — дольше);
// тает за столько секунд, в дождь — быстрее
static const float COVER_GROW_S = 600.0f, COVER_MELT_S = 240.0f, COVER_RAIN_S = 90.0f;

// Мягкая точка: ядро радиуса r и ореол ещё на g px
static void draw_soft_dot(FxSprite &s, float r, float g) {
  const int h = (int) ceilf(r + g), n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float d = sqrtf((float) ((x - h) * (x - h) + (y - h) * (y - h)));
      const float core = fx_clamp01(r + 0.5f - d);
      const float halo = 0.35f * (1.0f - fx_smooth(r - 0.5f, r + g, d));
      s.buf[y * n + x] = (uint8_t) (255.0f * std::max(core, halo) + 0.5f);
    }
  }
  fx_sprite_done(s);
}

// Кристалл: шесть лучей с двумя парами веточек и шестиугольник в середине,
// повёрнут на угол a. Край сглаживается по расстоянию до ближайшего луча
// или веточки — точно, как 3×3 подвыборки, но в двадцать раз быстрее: все
// шестнадцать картинок рисуются за один кадр без заминки
static void draw_crystal(FxSprite &s, float r, float a) {
  struct Seg {
    float x0, y0, dx, dy, l2, hw;
  };
  Seg seg[6 * 5];
  int ns = 0;
  auto add = [&](float x0, float y0, float x1, float y1, float w) {
    seg[ns++] = {x0, y0, x1 - x0, y1 - y0, (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0), w * 0.5f};
  };
  for (int k = 0; k < 6; k++) {
    const float b = a + k * 1.04719755f, c = cosf(b), sn = sinf(b);
    add(0, 0, r * c, r * sn, 1.15f);
    // Веточки под 60° к лучу: ближе к середине длиннее
    static const float AT[2] = {0.42f, 0.70f}, LEN[2] = {0.36f, 0.24f};
    for (int j = 0; j < 2; j++) {
      const float px = r * AT[j] * c, py = r * AT[j] * sn;
      for (int side = -1; side <= 1; side += 2) {
        const float e = b + side * 1.04719755f;
        add(px, py, px + r * LEN[j] * cosf(e), py + r * LEN[j] * sinf(e), 0.9f);
      }
    }
  }
  const int h = (int) ceilf(r) + 2, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float px = (float) (x - h), py = (float) (y - h);
      // Серединка — кружок радиуса 1,6; дальше — ближайший луч или веточка
      float cov = fx_clamp01(1.6f + 0.5f - sqrtf(px * px + py * py));
      for (int i = 0; i < ns && cov < 1.0f; i++) {
        const Seg &g = seg[i];
        const float t = fx_clamp01(((px - g.x0) * g.dx + (py - g.y0) * g.dy) / g.l2);
        const float ex = g.x0 + t * g.dx - px, ey = g.y0 + t * g.dy - py;
        const float d2 = ex * ex + ey * ey;
        if (d2 < (g.hw + 0.5f) * (g.hw + 0.5f))
          cov = std::max(cov, fx_clamp01(g.hw + 0.5f - sqrtf(d2)));
      }
      // Тонкие веточки бледноваты — чуть ярче, как прежде
      s.buf[y * n + x] = (uint8_t) std::min(255.0f, cov * 255.0f * 1.3f);
    }
  }
  fx_sprite_done(s);
}

// Высота полного сугроба над краем круга: посередине выше, к краям сходит
// на нет; поверхность чуть волнистая
static float cover_h(float x) {
  const float t = fabsf(x - FX_CX) / 190.0f;
  if (t >= 1)
    return 0;
  const float base = (1 - t * t) * sqrtf(1 - t * t);
  return COVER_H * base * (0.82f + 0.18f * sinf(x * 0.11f + 0.7f) * cosf(x * 0.037f));
}

static float wrap_snow(float x) {
  if (x < 10)
    return x + 446;
  if (x > 456)
    return x - 446;
  return x;
}

// ---------------------------------------------------------------------------

void WeatherFx::snow_apply_(const Params &p, bool relayout) {
  const int cnt = std::min(std::max(p.count, 0), ND);
  // Снег — сам по себе или половина мокрого снега
  const int c = p.mode == 2 ? cnt : (p.mode == 3 ? cnt / 2 : 0);
  const int pal = p.dim ? 1 : 0;
  for (int k = 0; k < 3; k++)
    this->c_snow_[k] = lv_color_hex(SNOW_C[pal][k]);
  this->c_cover_ = lv_color_hex(COVER_C[pal]);
  // Сугроб растёт в снег, тает без него; «Фон погоды: Выкл» убирает его сразу
  this->cover_rate_ = c ? (p.mode == 3 ? 0.5f : 1.0f) * c / 16.0f / COVER_GROW_S
                        : -1.0f / (p.mode == 1 || p.mode == 4 ? COVER_RAIN_S : COVER_MELT_S);
  if (!p.snow_cover && this->cover_ > 0) {
    this->cover_ = 0;
    this->cover_update_();
  }
  if (c && !this->sn_dot_.buf) {
    draw_soft_dot(this->sn_dot_, 1.2f, 1.2f);
    draw_soft_dot(this->sn_flake_, 2.3f, 1.8f);
    for (int sz = 0; sz < 2; sz++)
      for (int r = 0; r < NSROT; r++)
        draw_crystal(this->sn_cry_[sz][r], CRY_R[sz], r * (1.04719755f / NSROT));
  }
  if (!relayout)
    return;
  for (auto &s : this->sns_)
    this->mark_(s, false, 0, 0, 1, 1);
  // Сколько в каждом слое: стаек дальних — три четверти силы, хлопьев —
  // половина, кристаллов — четверть. Дальние — первыми: рисуются раньше всех
  const int nfar = c * 3 / 4, nmid = c / 2, nnear = c / 4;
  this->nsn_ = std::min(NSN, nfar + nmid + nnear);
  for (int i = 0; i < this->nsn_; i++) {
    this->snk_[i] = (uint8_t) (i < nfar ? 0 : (i < nfar + nmid ? 1 : 2 + i % 2));
    this->snow_respawn_(i, true);
  }
}

void WeatherFx::snow_respawn_(int i, bool anywhere) {
  const SnowLayer &l = SN_LAYERS[std::min<int>(this->snk_[i], 2)];
  this->snv_[i] = l.v0 + (l.v1 - l.v0) * rnd_(0, 101) / 100.0f;
  this->snx_[i] = rnd_(20, 446);
  this->sny_[i] = anywhere ? rnd_(-30, 440) : -20 - rnd_(0, 60);
  this->snph_[i] = rnd_(0, 628) / 100.0f;
  this->snrot_[i] = rnd_(0, 628) / 100.0f;
  // Кристаллы поворачиваются в обе стороны, 8–25° в секунду
  this->snw_[i] = (rnd_(0, 2) ? 1 : -1) * rnd_(7, 22) / 1000.0f;
  this->snacc_[i] = 0;
}

void WeatherFx::snow_hide_() {
  for (auto &s : this->sns_)
    this->mark_(s, false, 0, 0, 1, 1);
  this->nsn_ = 0;
}

void WeatherFx::snow_frame_(float wind) {
  this->snow_tick_++;
  for (int i = 0; i < this->nsn_; i++) {
    const int kind = this->snk_[i], layer = std::min(kind, 2);
    float k = this->k_;
    if (layer == 0) {
      // Дальние — через кадр, половина на чётных, половина на нечётных
      this->snacc_[i] += k;
      if (((this->snow_tick_ + i) & 1) != 0)
        continue;
      k = this->snacc_[i];
      this->snacc_[i] = 0;
    }
    const SnowLayer &l = SN_LAYERS[layer];
    this->sny_[i] += k * this->snv_[i];
    this->snph_[i] += k * (0.045f + layer * 0.012f + (i % 5) * 0.004f);
    if (this->snph_[i] > 6.2831853f)
      this->snph_[i] -= 6.2831853f;
    this->snx_[i] = wrap_snow(this->snx_[i] + k * (cosf(this->snph_[i]) * l.sway + wind * l.drift));
    if (layer == 2)
      this->snrot_[i] += k * this->snw_[i];
    // Упала на край круга или на сугроб — снова сверху
    const float x = this->snx_[i];
    if (this->sny_[i] > fx_bottom(x) - this->cover_ * cover_h(x) + 2.0f) {
      this->snow_respawn_(i, false);
      this->mark_(this->sns_[i], false, 0, 0, 1, 1);
      continue;
    }
    const int px = (int) x, py = (int) this->sny_[i];
    this->snpx_[i] = (int16_t) px;
    this->snpy_[i] = (int16_t) py;
    int x1, y1, w, h;
    if (layer == 0) {
      const auto &o = FAR_OFS[i % 4];
      int ox0 = 0, ox1 = 0, oy0 = 0;
      for (const auto &d : o) {
        ox0 = std::min<int>(ox0, d[0]);
        ox1 = std::max<int>(ox1, d[0]);
        oy0 = std::min<int>(oy0, d[1]);
      }
      const int r = this->sn_dot_.ax;
      x1 = px + ox0 - r;
      y1 = py + oy0 - r;
      w = ox1 - ox0 + 2 * r + 1;
      h = -oy0 + 2 * r + 1;
    } else {
      const FxSprite &s = layer == 1 ? this->sn_flake_ : this->sn_cry_[kind - 2][0];
      x1 = px - s.ax;
      y1 = py - s.ay;
      w = s.dsc.header.w;
      h = s.dsc.header.h;
    }
    this->mark_(this->sns_[i], this->visible_(x1, y1, w, h) && !this->excluded_(x1, y1, w, h), x1, y1, w, h);
  }
}

void WeatherFx::snow_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  for (int i = 0; i < this->nsn_; i++) {
    const Spot &s = this->sns_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    const int x = this->snpx_[i] + oc.x1, y = this->snpy_[i] + oc.y1;
    const int kind = this->snk_[i];
    if (kind == 0) {
      const auto &o = FAR_OFS[i % 4];
      for (int d = 0; d < 3; d++)
        fx_draw(layer, this->sn_dot_, fx_sprite_area(this->sn_dot_, x + o[d][0], y + o[d][1]), FAR_OPA[d],
                this->c_snow_[0]);
    } else if (kind == 1) {
      fx_draw(layer, this->sn_flake_, fx_sprite_area(this->sn_flake_, x, y), LV_OPA_COVER, this->c_snow_[1]);
    } else {
      // Поворот кристалла — один из NSROT в пределах 60°
      float r = fmodf(this->snrot_[i], 1.04719755f);
      if (r < 0)
        r += 1.04719755f;
      const int f = std::min(NSROT - 1, (int) (r / 1.04719755f * NSROT));
      const FxSprite &spr = this->sn_cry_[kind - 2][f];
      fx_draw(layer, spr, fx_sprite_area(spr, x, y), LV_OPA_COVER, this->c_snow_[2]);
    }
  }
}

// ---- Сугроб ----

void WeatherFx::cover_step_(uint32_t now) {
  const float dt = this->cover_ms_ ? (now - this->cover_ms_) / 1000.0f : 0.0f;
  this->cover_ms_ = now;
  if (this->cover_ <= 0 && this->cover_rate_ <= 0)
    return;
  this->cover_ = fx_clamp01(this->cover_ + dt * this->cover_rate_);
  // Перерисовать, когда высота изменилась хотя бы на 0,6 px
  if (fabsf(this->cover_ - this->cover_drawn_) * COVER_H >= 0.6f || (this->cover_ <= 0) != (this->cover_drawn_ <= 0))
    this->cover_update_();
}

void WeatherFx::cover_update_() {
  this->cover_drawn_ = this->cover_;
  const bool on = this->cover_ * COVER_H >= 0.6f;
  if (!on) {
    this->mark_(this->cover_spot_, false, 0, 0, 1, 1);
    fx_sprite_free(this->cover_spr_);
    this->update_root_();
    return;
  }
  const int w = CV_X1 - CV_X0 + 1, h = CV_Y1 - CV_Y0 + 1;
  FxSprite &s = this->cover_spr_;
  if (!fx_sprite_init(s, w, h, false))
    return;
  s.ax = s.ay = 0;
  for (int x = 0; x < w; x++) {
    const float xc = CV_X0 + x + 0.5f;
    const float hh = this->cover_ * cover_h(xc);
    if (hh < 0.3f)
      continue;
    const float yb = fx_bottom(xc), top = yb - hh;
    const int y0 = std::max(CV_Y0, (int) floorf(top)), y1 = std::min(CV_Y1, (int) ceilf(yb) + 1);
    for (int y = y0; y <= y1; y++) {
      // Верхний пиксель — по доле, которую закрывает снег; ниже верхней
      // кромки снег чуть темнее — освещена только поверхность
      const float cov = fx_clamp01(y + 1 - top);
      const float shade = 1.0f - 0.3f * fx_smooth(top + 1.5f, top + 7.0f, y + 0.5f);
      s.buf[(y - CV_Y0) * w + x] = (uint8_t) (255.0f * cov * shade + 0.5f);
    }
  }
  fx_sprite_done(s);
  this->mark_(this->cover_spot_, true, CV_X0, CV_Y0, w, h);
  this->invalidate_(this->cover_spot_.a);
  this->update_root_();
}

void WeatherFx::cover_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->cover_spot_.on || !this->cover_spr_.buf)
    return;
  const Spot &s = this->cover_spot_;
  const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  if (fx_hit(a, clip))
    fx_draw(layer, this->cover_spr_, a, LV_OPA_COVER, this->c_cover_);
}

}  // namespace weather_fx
}  // namespace esphome

// Приметы времён года: лепестки яблонь в мае, тополиный пух в июне, зарницы
// душными летними ночами.
//
// Лепесток — готовая картинка RGB565A8: белый, к краю розовеет, у основания
// — зеленоватый. Каждый нарисован в 12 поворотах и трёх наклонах (целиком,
// вполоборота, почти ребром) — лепесток кувыркается, падая, как лист, но
// легче: медленнее и с большим размахом. Пушинка — маска A8: клочок ваты из
// нескольких мягких комков и тонких изогнутых волосков, полупрозрачный. Пух
// не падает, а плывёт по ветру, то опускаясь, то поднимаясь.
//
// Зарница — далёкая гроза за горизонтом: низ неба беззвучно вспыхивает
// два-три раза подряд, без молнии и грома.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

// Лепестки: повороты, наклоны, цвета; половина длины — дальний и ближний
static const int PT_ROT = 12, PT_FLIP = 3, PT_VAR = 2;
static const float PT_FLIP_K[PT_FLIP] = {1.0f, 0.62f, 0.36f};
static const float PT_R[2] = {6.0f, 8.0f};
// Пух: повороты и два рисунка; радиус — дальний и ближний
static const int FL_ROT = 12, FL_VAR = 2;
static const float FL_R[2] = {6.5f, 11.0f};

static int sea_n(float r) { return 2 * (int) ceilf(r) + 3; }

// Лепесток в своих осях: u — от основания (−1) к кончику (1), v — поперёк.
// Широкий ближе к кончику, кончик округлый, к основанию сужается
static float petal_w(float u) {
  if (u <= -1.0f || u >= 1.0f)
    return 0;
  return 0.74f * sqrtf(1.0f - u * u) * (0.62f + 0.38f * (u + 1.0f) * 0.5f);
}

// Лепесток var (0 — белый с розовым краем, 1 — розовый), поворот rot,
// наклон fl, половина длины r
static void draw_petal(uint8_t *p, int n, float r, int var, int rot, int fl) {
  const float a = rot * (2 * FX_PI / PT_ROT), c = cosf(a), s = sinf(a);
  const float kf = PT_FLIP_K[fl], h = n / 2;
  // Ребром лепесток темнее: свет падает вскользь
  const float shade = 0.72f + 0.28f * kf;
  const uint32_t mid = var ? 0xFCE4EC : 0xFFFFFF, edge = var ? 0xF06292 : 0xF8BBD0;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      // 4×4 точки на пиксель: лепесток мелкий, край должен быть гладким
      int in = 0;
      float eu = 0, ev = 0;
      for (int q = 0; q < 16; q++) {
        const float px = x - h + ((q & 3) - 1.5f) * 0.25f, py = y - h + ((q >> 2) - 1.5f) * 0.25f;
        const float u = (px * c + py * s) / r, v = (-px * s + py * c) / (r * kf);
        const float w = petal_w(u);
        if (fabsf(v) < w) {
          in++;
          eu += u;
          ev += fabsf(v) / w;
        }
      }
      if (!in)
        continue;
      const float cov = in / 16.0f;
      eu /= in;
      ev /= in;
      // К краю и к кончику розовеет, у основания — зеленоватый
      FxMix m;
      m.over(mid, 1);
      m.over(edge, 0.85f * fx_smooth(0.35f, 1.0f, std::max(ev, 0.6f * eu)));
      m.over(0xDCE775, 0.6f * fx_smooth(-0.55f, -0.95f, eu));
      FxMix o;
      o.r = m.r * shade * cov;
      o.g = m.g * shade * cov;
      o.b = m.b * shade * cov;
      o.a = cov;
      fx_put(p, n * n, y * n + x, o, x, y);
    }
  }
}

// Пушинка var радиуса r, повёрнутая на rot: клочок ваты из нескольких
// мягких комков, вокруг — тонкие изогнутые волоски, к концам тают
static void draw_fluff(uint8_t *p, int n, float r, int var, int rot) {
  const float h = n / 2, a0 = rot * (2 * FX_PI / FL_ROT);
  // Рисунок — одинаковый при каждом запуске; поворот — сдвигом всех углов
  FxRand rg(var ? 0x51F0u : 0x2A77u);
  static const int NB = 5, NH = 22;
  float bx[NB], by[NB], br[NB];
  for (int i = 0; i < NB; i++) {
    const float a = a0 + rg.range(0, 2 * FX_PI), d = r * rg.range(0.1f, 0.5f);
    bx[i] = d * cosf(a);
    by[i] = d * sinf(a);
    br[i] = r * rg.range(0.32f, 0.5f);
  }
  float hc[NH], hs[NH], len[NH], bend[NH];
  for (int i = 0; i < NH; i++) {
    const float a = a0 + rg.range(0, 2 * FX_PI);
    hc[i] = cosf(a);
    hs[i] = sinf(a);
    len[i] = r * rg.range(0.55f, 1.0f);
    bend[i] = rg.range(-0.3f, 0.3f);
  }
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = x - h, dy = y - h;
      float v = 0;
      for (int i = 0; i < NB; i++) {
        const float q = 1.0f - (fx_sq(dx - bx[i]) + fx_sq(dy - by[i])) / fx_sq(br[i]);
        if (q > 0)
          v += (1.0f - v) * 0.3f * q * sqrtf(q);
      }
      for (int i = 0; i < NH; i++) {
        const float al = dx * hc[i] + dy * hs[i];
        if (al < 0 || al > len[i])
          continue;
        // Волосок тоньше пикселя и чуть изогнут: яркость — по доле, которую
        // он закрывает
        const float ac = fabsf(dy * hc[i] - dx * hs[i] - bend[i] * al * al / len[i]);
        if (ac < 1.0f)
          v += (1.0f - v) * 0.7f * fx_clamp01(0.75f - ac * 0.75f) * (1.0f - 0.7f * al / len[i]);
      }
      v = std::max(v, 0.4f * fx_clamp01(1.2f - sqrtf(dx * dx + dy * dy)));
      if (v > 0.01f)
        p[y * n + x] = (uint8_t) std::min(255.0f, std::max(0.0f, v * 255.0f + fx_dither(x, y) * 4.0f));
    }
  }
}

// Номер картинки в наборе своего размера
static int petal_idx(int var, int fl, int rot) { return (var * PT_FLIP + fl) * PT_ROT + rot; }
static int fluff_idx(int var, int rot) { return var * FL_ROT + rot; }

void WeatherFx::sea_render_(int sz, int idx) {
  FxSheet &sh = this->sea_sheet_[sz];
  uint8_t *p = fx_sheet_take(sh, idx);
  if (!p)
    return;
  if (this->sea_kind_ == 1)
    draw_petal(p, sh.w, PT_R[sz], idx / (PT_FLIP * PT_ROT), idx % PT_ROT, (idx / PT_ROT) % PT_FLIP);
  else
    draw_fluff(p, sh.w, FL_R[sz], idx / FL_ROT, idx % FL_ROT);
  fx_sheet_crop(sh, idx);
  fx_sheet_done(sh, idx);
}

void WeatherFx::seasons_apply_(const Params &p, bool relayout) {
  const int kind = p.mode == 10 ? 1 : (p.mode == 11 ? 2 : 0);
  const int n = kind ? std::min(std::max(p.count, 0), NSEA) : 0;
  if (kind != this->sea_kind_) {
    // Другие картинки — набор заново
    for (auto &s : this->sea_sheet_)
      fx_sheet_free(s);
    this->sea_kind_ = kind;
    relayout = true;
    if (kind) {
      for (int sz = 0; sz < 2; sz++) {
        const int w = sea_n(kind == 1 ? PT_R[sz] : FL_R[sz]);
        const int cnt = kind == 1 ? PT_VAR * PT_FLIP * PT_ROT : FL_VAR * FL_ROT;
        if (!fx_sheet_init(this->sea_sheet_[sz], cnt, w, w, kind == 1)) {
          for (auto &s : this->sea_sheet_)
            fx_sheet_free(s);
          this->sea_kind_ = 0;
          break;
        }
        this->sea_sheet_[sz].ax = this->sea_sheet_[sz].ay = w / 2;
      }
    }
  }
  if (!relayout && n == this->nsea_)
    return;
  for (auto &s : this->sea_spot_)
    this->mark_(s, false, 0, 0, 1, 1);
  this->nsea_ = this->sea_kind_ ? n : 0;
  for (int i = 0; i < this->nsea_; i++)
    this->sea_respawn_(i, true);
}

void WeatherFx::sea_respawn_(int i, bool anywhere) {
  const bool big = i % 2;
  this->sea_x_[i] = rnd_(20, 446);
  this->sea_y_[i] = anywhere ? rnd_(-20, 420) : -16 - rnd_(0, 50);
  this->sea_ph_[i] = rnd_(0, 628) / 100.0f;
  this->sea_rot_[i] = rnd_(0, 628) / 100.0f;
  this->sea_flip_[i] = rnd_(0, 628) / 100.0f;
  this->sea_rest_[i] = 0;
  this->sea_opa_[i] = LV_OPA_COVER;
  if (this->sea_kind_ == 1) {
    // Лепесток падает медленнее листа, кружится и кувыркается
    this->sea_v_[i] = big ? 0.55f + (i % 3) * 0.12f : 0.38f + (i % 3) * 0.08f;
    this->sea_spin_[i] = (rnd_(0, 2) ? 1 : -1) * rnd_(30, 90) / 1000.0f;
    this->sea_flipw_[i] = rnd_(40, 90) / 1000.0f;
  } else {
    // Пушинка медленно вертится и плывёт
    this->sea_v_[i] = big ? 0.22f + (i % 3) * 0.05f : 0.15f + (i % 3) * 0.04f;
    this->sea_spin_[i] = (rnd_(0, 2) ? 1 : -1) * rnd_(4, 14) / 1000.0f;
    this->sea_flipw_[i] = rnd_(25, 45) / 1000.0f;
  }
}

void WeatherFx::seasons_frame_(float wind) {
  const float k = this->k_;
  for (int i = 0; i < this->nsea_; i++) {
    const bool big = i % 2;
    const int sz = big ? 1 : 0;
    Spot &sp = this->sea_spot_[i];
    if (this->sea_rest_[i] > 0) {
      // Лепесток лежит на краю: подождать и растаять ступенями, как лист
      this->sea_rest_[i] -= this->dt_ms_;
      if (this->sea_rest_[i] > 800)
        continue;
      const int opa = std::max(0, (int) (255 * this->sea_rest_[i] / 800.0f)) & ~31;
      if (opa != this->sea_opa_[i]) {
        this->sea_opa_[i] = (lv_opa_t) opa;
        if (sp.on)
          this->invalidate_(sp.a);
      }
      if (this->sea_rest_[i] <= 0) {
        this->mark_(sp, false, 0, 0, 1, 1);
        this->sea_respawn_(i, false);
      }
      continue;
    }
    float x = this->sea_x_[i], y = this->sea_y_[i];
    int idx;
    if (this->sea_kind_ == 1) {
      // Лепесток: качается с большим размахом, в крайних точках замедляясь
      this->sea_ph_[i] += k * ((big ? 0.11f : 0.09f) + i * 0.004f);
      if (this->sea_ph_[i] > 2 * FX_PI)
        this->sea_ph_[i] -= 2 * FX_PI;
      const float sw = sinf(this->sea_ph_[i]);
      y += k * this->sea_v_[i] * (0.55f + 0.45f * fabsf(sw));
      x += k * (cosf(this->sea_ph_[i]) * (big ? 1.7f : 1.1f) + wind * (big ? 2.4f : 1.7f));
      this->sea_rot_[i] += k * this->sea_spin_[i];
      this->sea_flip_[i] += k * this->sea_flipw_[i];
      if (y > fx_bottom(x) - 3.0f - this->cover_ * 10.0f) {
        this->sea_rest_[i] = rnd_(2000, 4000);
        this->sea_spin_[i] = 0;
        this->sea_flipw_[i] = 0;
      }
      float ang = fmodf(this->sea_rot_[i] + 0.5f * sw, 2 * FX_PI);
      if (ang < 0)
        ang += 2 * FX_PI;
      const int rot = (int) (ang / (2 * FX_PI) * PT_ROT + 0.5f) % PT_ROT;
      const float cf = cosf(this->sea_flip_[i]), af = fabsf(cf);
      const int fl = af > 0.8f ? 0 : (af > 0.45f ? 1 : 2);
      this->sea_back_[i] = cf < 0;
      idx = petal_idx((i / 2) % PT_VAR, fl, rot);
    } else {
      // Пушинка: плывёт по ветру, петляет и то опускается, то поднимается
      this->sea_ph_[i] += k * (0.035f + i * 0.002f);
      this->sea_flip_[i] += k * this->sea_flipw_[i];
      x += k * (wind * (big ? 2.2f : 1.5f) + 0.5f * sinf(this->sea_ph_[i]));
      y += k * (this->sea_v_[i] + 0.45f * sinf(this->sea_flip_[i]));
      this->sea_rot_[i] += k * this->sea_spin_[i];
      if (y < -20.0f)
        y = -20.0f;
      if (y > fx_bottom(x) + 10.0f) {
        // Уплыла за нижний край — новая сверху
        this->mark_(sp, false, 0, 0, 1, 1);
        this->sea_respawn_(i, false);
        continue;
      }
      float ang = fmodf(this->sea_rot_[i], 2 * FX_PI);
      if (ang < 0)
        ang += 2 * FX_PI;
      idx = fluff_idx((i / 2) % FL_VAR, (int) (ang / (2 * FX_PI) * FL_ROT + 0.5f) % FL_ROT);
    }
    if (x < 10)
      x += 446;
    else if (x > 456)
      x -= 446;
    this->sea_x_[i] = x;
    this->sea_y_[i] = y;
    FxSheet &sh = this->sea_sheet_[sz];
    if (!fx_sheet_ready(sh, idx)) {
      // Новых картинок — не больше пары за кадр; пока нужный поворот не
      // нарисован — ближайший готовый того же наклона
      const int nr = this->sea_kind_ == 1 ? PT_ROT : FL_ROT, base = idx - idx % nr, rot = idx % nr;
      int near = -1;
      if (this->render_budget_ <= 0)
        for (int d = 1; d <= nr / 2 && near < 0; d++)
          for (int sgn = -1; sgn <= 1 && near < 0; sgn += 2)
            if (fx_sheet_ready(sh, base + (rot + sgn * d + nr) % nr))
              near = base + (rot + sgn * d + nr) % nr;
      if (near >= 0) {
        idx = near;
      } else {
        this->render_budget_--;
        this->sea_render_(sz, idx);
      }
    }
    if (!fx_sheet_ready(sh, idx)) {
      this->mark_(sp, false, 0, 0, 1, 1);
      continue;
    }
    const bool changed = idx != this->sea_idx_[i];
    this->sea_idx_[i] = (uint16_t) idx;
    const lv_area_t a = fx_sheet_area(sh, idx, (int) x, (int) y);
    const int w = a.x2 - a.x1 + 1, hh = a.y2 - a.y1 + 1;
    const bool same = sp.on && a.x1 == sp.a.x1 && a.y1 == sp.a.y1 && a.x2 == sp.a.x2 && a.y2 == sp.a.y2;
    if (same && changed)
      this->invalidate_(sp.a);
    this->mark_(sp, this->visible_(a.x1, a.y1, w, hh) && !this->excluded_(a.x1, a.y1, w, hh), a.x1, a.y1, w, hh);
  }
}

void WeatherFx::seasons_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->nsea_)
    return;
  // Пух — белый (на приглушённом экране — ярче), лепестки — своих цветов;
  // изнанка лепестка темнее
  const lv_color_t fc = lv_color_hex(this->dim_ ? 0xFFFFFF : 0xE8EEF2);
  for (int i = 0; i < this->nsea_; i++) {
    const Spot &s = this->sea_spot_[i];
    if (!s.on || !this->sea_opa_[i])
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    lv_opa_t o = this->sea_opa_[i];
    if (this->sea_kind_ == 1 && this->sea_back_[i])
      o = (lv_opa_t) (o * 200 / 255);
    fx_sheet_draw(layer, this->sea_sheet_[i % 2], this->sea_idx_[i], a, o, fc);
  }
}

// ---- Зарницы ----

// Сияние — полуэллипс, светлее всего у нижней середины: оттуда, из-за
// горизонта, и светит далёкая гроза. Три яркости — готовыми масками с
// подмешанным шумом: на RGB565 ослабленная при выводе маска легла бы
// кольцами с зеленцой. Вспышка мерцает ступенями, как настоящая
static const int HL_W = 240, HL_H = 84, HL_LV = 3;
static const float HL_LEVEL[HL_LV] = {1.0f, 0.5f, 0.22f};
// Вспышка гаснет за столько мс
static const float HL_TAU = 95.0f;

static void draw_hl(FxSprite &s, float k) {
  if (!fx_sprite_init(s, HL_W, HL_H, false))
    return;
  s.ax = HL_W / 2;
  s.ay = HL_H - 1;
  for (int y = 0; y < HL_H; y++) {
    for (int x = 0; x < HL_W; x++) {
      const float dx = (x - HL_W / 2 + 0.5f) / (HL_W / 2), dy = (HL_H - 1 - y) / (float) HL_H;
      const float q = 1.0f - dx * dx - dy * dy;
      if (q <= 0)
        continue;
      const float v = 160.0f * k * q * sqrtf(q) + fx_dither(x, y) * 8.0f;
      if (v > 0.5f)
        s.buf[y * HL_W + x] = (uint8_t) std::min(255.0f, v);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::hl_apply_(int mode) {
  this->hl_mode_ = mode;
  this->next_hl_ = 0;
  if (!mode) {
    this->mark_(this->hl_spot_, false, 0, 0, 1, 1);
    this->hl_n_ = 0;
    for (auto &s : this->hl_spr_)
      fx_sprite_free(s);
  }
}

void WeatherFx::hl_(uint32_t now) {
  if (!this->hl_mode_)
    return;
  if (!this->hl_n_) {
    if (this->next_hl_ == 0)
      this->next_hl_ = now + (this->hl_mode_ == 2 ? 1500 : rnd_(3000, 15000));
    if ((int32_t) (now - this->next_hl_) < 0)
      return;
    if (!this->hl_spr_[HL_LV - 1].buf) {
      for (int i = 0; i < HL_LV; i++)
        draw_hl(this->hl_spr_[i], HL_LEVEL[i]);
      if (!this->hl_spr_[HL_LV - 1].buf) {
        this->next_hl_ = now + 60000;
        return;
      }
    }
    // Новая зарница: где за горизонтом и две-три вспышки подряд
    this->hl_x_ = rnd_(120, 347);
    this->hl_n_ = rnd_(2, 4);
    this->hl_t0_ = now;
    uint16_t t = 0;
    for (int j = 0; j < this->hl_n_; j++) {
      this->hl_dt_[j] = t;
      this->hl_pk_[j] = (uint8_t) rnd_(150, 256);
      t += (uint16_t) rnd_(70, 260);
    }
  }
  // Яркость — сумма гаснущих вспышек, ступенью из трёх
  const float t = now - this->hl_t0_;
  float v = 0;
  for (int j = 0; j < this->hl_n_; j++)
    if (t >= this->hl_dt_[j])
      v += this->hl_pk_[j] / 255.0f * expf(-(t - this->hl_dt_[j]) / HL_TAU);
  const int lv = v >= 0.72f ? 0 : (v >= 0.33f ? 1 : (v >= 0.1f ? 2 : -1));
  if (t > this->hl_dt_[this->hl_n_ - 1] && lv < 0) {
    // Отсверкало — следующая через несколько секунд
    this->mark_(this->hl_spot_, false, 0, 0, 1, 1);
    this->hl_n_ = 0;
    this->next_hl_ = now + (this->hl_mode_ == 2 ? rnd_(2500, 9000) : rnd_(8000, 30000));
    return;
  }
  const int bx = this->hl_x_ - HL_W / 2, by = (int) fx_bottom(this->hl_x_) + 6 - HL_H;
  if (this->hl_spot_.on && lv != this->hl_lv_)
    this->invalidate_(this->hl_spot_.a);
  this->hl_lv_ = (int8_t) lv;
  this->mark_(this->hl_spot_, lv >= 0, bx, by, HL_W, HL_H);
}

void WeatherFx::hl_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const Spot &s = this->hl_spot_;
  if (!s.on || this->hl_lv_ < 0)
    return;
  const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  if (fx_hit(a, clip))
    fx_draw(layer, this->hl_spr_[this->hl_lv_], a, LV_OPA_COVER, lv_color_hex(this->dim_ ? 0xF0F2FF : 0xE6E8FF));
}

}  // namespace weather_fx
}  // namespace esphome

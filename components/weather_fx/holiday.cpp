// Праздничное: Вифлеемская звезда на Рождество, ракета с дымом в День
// космонавтики, пасхальные яйца, «Матрица» в День программиста.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

// ---- Вифлеемская звезда ----

// Над часами слева от середины, мимо солнца. Картинка: привязка —
// сердцевина, длинный луч — вниз
static const int BSTAR_X = 190, BSTAR_Y = 66;
static const int BS_W = 55, BS_H = 66, BS_AX = 27, BS_AY = 27;
static const int BS_N = 8;  // кадров «дыхания» лучей

// Главные лучи (вверх, в стороны и длинный вниз) — длиной tw от полной,
// косые — наоборот, длиннее. Лучи сужаются и тускнеют к концу, вдоль
// главных — тонкое сияние; тёплый ореол и белая сердцевина. На
// приглушённом экране — ярче и белее
static void draw_bstar(uint8_t *p, float tw, bool dim) {
  float ux[8], uy[8], len[8], wid[8];
  for (int k = 0; k < 8; k++) {
    const float a = k * (FX_PI / 4) - FX_PI / 2;
    ux[k] = cosf(a);
    uy[k] = sinf(a);
    len[k] = (k == 4 ? 34.0f : (k % 2 ? 13.0f : 24.0f)) * (k % 2 ? 2.0f - tw : tw);
    wid[k] = k % 2 ? 1.3f : 2.2f;
  }
  const uint32_t ray_c = dim ? 0xFFFFFF : 0xFFF3C4;
  for (int y = 0; y < BS_H; y++) {
    for (int x = 0; x < BS_W; x++) {
      const float px = (float) (x - BS_AX), py = (float) (y - BS_AY);
      const float d = sqrtf(px * px + py * py);
      FxMix m;
      const float g = fx_clamp01(1.0f - d / 27.0f);
      m.over(0xFFE9A8, (dim ? 0.36f : 0.28f) * g * sqrtf(g));
      const float in = fx_clamp01(1.0f - d / 13.0f);
      m.over(0xFFF3C4, (dim ? 0.5f : 0.4f) * in * sqrtf(in));
      float ray = 0, halo = 0;
      for (int k = 0; k < 8; k++) {
        const float s = px * ux[k] + py * uy[k];
        if (s < -1.0f || s > len[k] + 1.0f)
          continue;
        const float q = fabsf(py * ux[k] - px * uy[k]);
        const float f = fx_clamp01(s / len[k]);
        if (k % 2 == 0 && q < 6.0f)
          halo = std::max(halo, 0.3f * expf(-q * q / 6.0f) * (1.0f - f));
        const float w = wid[k] * (1.0f - f) + 0.2f;
        ray = std::max(ray, fx_clamp01(w + 0.5f - q) * (1.0f - 0.5f * f * f) * fx_clamp01(len[k] + 0.5f - s));
      }
      m.over(ray_c, halo);
      m.over(ray_c, 0.95f * ray);
      m.over(0xFFFFFF, fx_clamp01(4.0f - d));
      fx_put(p, BS_W * BS_H, y * BS_W + x, m, x, y);
    }
  }
}

// Кадр по фазе мерцания: длина главных лучей 0,7…1
static int bstar_frame(float ph) {
  const float tw = 0.85f + 0.15f * sinf(ph);
  return std::min(BS_N - 1, std::max(0, (int) lroundf((tw - 0.7f) / 0.3f * (BS_N - 1))));
}

void WeatherFx::bstar_apply_(bool on) {
  this->bstar_on_ = on;
  this->mark_(this->bstar_spot_, on, BSTAR_X - BS_AX, BSTAR_Y - BS_AY, BS_W, BS_H);
  if (!on) {
    fx_sheet_free(this->bstar_sheet_);
    return;
  }
  if (!this->bstar_sheet_.buf) {
    if (!fx_sheet_init(this->bstar_sheet_, BS_N, BS_W, BS_H, true))
      return;
    this->bstar_sheet_.ax = BS_AX;
    this->bstar_sheet_.ay = BS_AY;
  } else if (this->bstar_dim_ != this->dim_) {
    fx_sheet_reset(this->bstar_sheet_);
  }
  this->bstar_dim_ = this->dim_;
  this->bstar_idx_ = bstar_frame(this->bstar_ph_);
  this->bstar_render_(this->bstar_idx_);
}

// Нарисовать кадр звезды i, если его ещё нет
void WeatherFx::bstar_render_(int i) {
  if (uint8_t *p = fx_sheet_take(this->bstar_sheet_, i)) {
    draw_bstar(p, 0.7f + 0.3f * i / (BS_N - 1), this->dim_);
    fx_sheet_done(this->bstar_sheet_, i);
  }
}

void WeatherFx::bstar_(uint32_t now) {
  // Лучи чуть вытягиваются и укорачиваются, 8 раз в секунду; перерисовать —
  // только если сменился кадр
  if (!this->bstar_on_ || now - this->bstar_ms_ < 120)
    return;
  this->bstar_ms_ = now;
  this->bstar_ph_ = fmodf(this->bstar_ph_ + 0.35f, 2 * FX_PI);
  const int i = bstar_frame(this->bstar_ph_);
  if (i == this->bstar_idx_)
    return;
  this->bstar_render_(i);
  this->bstar_idx_ = i;
  if (this->bstar_spot_.on)
    this->invalidate_(this->bstar_spot_.a);
}

void WeatherFx::bstar_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->bstar_spot_.on || !fx_sheet_ready(this->bstar_sheet_, this->bstar_idx_))
    return;
  const lv_area_t a = fx_sheet_area(this->bstar_sheet_, this->bstar_idx_, BSTAR_X + oc.x1, BSTAR_Y + oc.y1);
  if (fx_hit(a, clip))
    fx_sheet_draw(layer, this->bstar_sheet_, this->bstar_idx_, a, LV_OPA_COVER);
}

// ---- Ракета ----

// Летит 4,5 с снизу слева вверх направо, над строкой погоды
static const uint32_t RK_MS = 4500;
static const float PUFF_MS = 900.0f;  // клуб дыма тает
// Картинки корпуса и пламени (привязка — середина ракеты) — точно под
// повёрнутые очертания, на каждый курс через 2°
static const float RK_BIN = 2.0f * FX_PI / 180.0f;
static const float RK_FLAME[3] = {9.0f, 12.5f, 16.0f};

// Путь — плавная дуга от левого края над строкой погоды к верху справа: где
// ракета на доле пути u и её курс
static void rk_path(float u, float *x, float *y, float *a) {
  *x = -40.0f + 546.0f * u;
  *y = 205.0f - 185.0f * u - 45.0f * sinf(FX_PI * u);
  *a = atan2f(-185.0f - 45.0f * FX_PI * cosf(FX_PI * u), 546.0f);
}

// Номер картинок курса a (из nb): 0 — горизонтально, номер растёт с подъёмом
static int rk_bin_of(float a, int nb) { return std::min(nb - 1, std::max(0, (int) lroundf(-a / RK_BIN))); }

// Точка (x, y) внутри треугольника?
static bool in_tri(float x, float y, float ax, float ay, float bx, float by, float cx, float cy) {
  const float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
  const float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
  const float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
  return !(((d1 < 0) || (d2 < 0) || (d3 < 0)) && ((d1 > 0) || (d2 > 0) || (d3 > 0)));
}

// Ракета в своих осях: x — вперёд по курсу, y — вниз от оси. Красные
// стабилизаторы (нижний в тени), сопло, белый корпус-цилиндр со светом
// сверху и бликом, красные нос и полоса, иллюминатор с ободком и бликом
static FxMix rocket_at(float lx, float ly) {
  FxMix m;
  if (lx < -17.5f || lx > 19.5f || fabsf(ly) > 11.5f)
    return m;
  if (fabsf(ly) > 4.0f && lx < -3.5f)
    m.over(ly > 0 ? 0xB71C1C : 0xE53935, in_tri(lx, fabsf(ly), -12, 4.5f, -4, 4.5f, -17, 11) ? 1.0f : 0.0f);
  if (lx > -14.6f && lx < -11.5f && fabsf(ly) < 2.0f + (lx + 14.6f) * 0.35f)
    m.over(0x455A64, 1.0f);
  const float hw = lx < 9.0f ? 5.0f : 5.0f * sqrtf(fx_clamp01(1.0f - fx_sq((lx - 9.0f) / 10.5f)));
  if (lx > -12.3f && lx < 19.5f && fabsf(ly) < hw) {
    const bool red = lx >= 9.0f || (lx > -8.5f && lx < -6.5f);
    const float u = ly / 5.0f;  // −1 — верх, 1 — низ
    const float k =
        0.8f + 0.3f * (1.0f - fx_sq(u + 0.35f)) + 0.25f * fx_sq(fx_clamp01(1.0f - fx_sq((u + 0.5f) / 0.35f)));
    const uint32_t c = red ? 0xE53935 : 0xECEFF1;
    m.over(((uint32_t) std::min(255.0f, ((c >> 16) & 0xFF) * k) << 16) |
               ((uint32_t) std::min(255.0f, ((c >> 8) & 0xFF) * k) << 8) | (uint32_t) std::min(255.0f, (c & 0xFF) * k),
           1.0f);
    const float dw = sqrtf(fx_sq(lx - 2.0f) + ly * ly);
    if (dw < 3.4f)
      m.over(dw < 2.6f ? (ly < -0.5f ? 0x81D4FA : 0x29B6F6) : 0x90A4AE, 1.0f);
    if (fx_sq(lx - 1.2f) + fx_sq(ly + 1.0f) < 0.7f)
      m.over(0xFFFFFF, 1.0f);
  }
  return m;
}

// Пламя длиной fl позади сопла: ядро почти белое, к краю и концу — жёлтое,
// оранжевое, красноватое; вокруг — тёплое сияние. Край сглаживается
// расстоянием до контура — хватает одной точки на пиксель
static FxMix flame_at(float lx, float ly, float fl) {
  FxMix m;
  const float s = -14.0f - lx;
  const float sc = std::min(std::max(s, 0.0f), fl);
  const float gd = fx_sq(s - sc) + ly * ly;
  if (gd < 30.0f)
    m.over(0xFF9A3C, 0.4f * fx_sq(1.0f - gd / 30.0f) * (1.0f - 0.7f * sc / fl));
  if (s > -0.5f && s < fl) {
    const float t = fx_clamp01(s / fl);
    const float hw = 2.9f * sqrtf(1.0f - t) + 0.2f;
    const float d = fabsf(ly);
    const float cov = fx_clamp01(hw + 0.5f - d);
    if (cov > 0) {
      const float heat = (1.0f - fx_sq(std::min(d / hw, 1.0f))) * (1.0f - t);
      uint32_t c;
      if (heat > 0.55f)
        c = 0xFFF8E1;
      else if (heat > 0.3f)
        c = 0xFFEB3B;
      else if (heat > 0.12f)
        c = 0xFF9800;
      else
        c = 0xF4511E;
      m.over(c, cov * (1.0f - 0.55f * t));
    }
  }
  return m;
}

// Клуб дыма радиусом r: плотный в середине, мягкий по краю, чуть
// неровный — к нему прилип клубок поменьше
static void draw_puff(FxSprite &s, float r) {
  const int h = (int) ceilf(r) + 1, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = (x - h) / r, dy = (y - h) / r;
      const float ex = (dx - 0.38f) / 0.62f, ey = (dy + 0.3f) / 0.62f;
      const float v = std::max(1.0f - dx * dx - dy * dy, 0.85f * (1.0f - ex * ex - ey * ey));
      if (v > 0)
        s.buf[y * n + x] = (uint8_t) std::min(255.0f, std::max(0.0f, 255.0f * sqrtf(v) + fx_dither(x, y) * 4.0f));
    }
  }
  fx_sprite_done(s);
}

// Прямоугольник пикселей вокруг привязки, в который попадает повёрнутый
// (cos c, sin s) прямоугольник [x0, x1]×[y0, y1] в осях ракеты
static void rot_box(float x0, float x1, float y0, float y1, float c, float s, int *bx, int *by, int *bw, int *bh) {
  float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
  for (int k = 0; k < 4; k++) {
    const float x = k & 1 ? x1 : x0, y = k & 2 ? y1 : y0;
    const float X = x * c - y * s, Y = x * s + y * c;
    mnx = std::min(mnx, X);
    mxx = std::max(mxx, X);
    mny = std::min(mny, Y);
    mxy = std::max(mxy, Y);
  }
  *bx = (int) floorf(mnx) - 1;
  *by = (int) floorf(mny) - 1;
  *bw = (int) ceilf(mxx) + 1 - *bx + 1;
  *bh = (int) ceilf(mxy) + 1 - *by + 1;
}

// Корпус и три длины пламени под курс −2°·bin
void WeatherFx::rocket_render_(int bin) {
  const float a = -bin * RK_BIN, c = cosf(a), s = sinf(a);
  int bx, by, bw, bh;
  rot_box(-17.5f, 19.5f, -11.5f, 11.5f, c, s, &bx, &by, &bw, &bh);
  FxSprite &b = this->rk_body_[bin];
  if (fx_sprite_init(b, bw, bh, true)) {
    b.ax = -bx;
    b.ay = -by;
    for (int y = 0; y < bh; y++) {
      for (int x = 0; x < bw; x++) {
        FxMix px;
        for (int q = 0; q < 4; q++) {
          const float sx = x + bx + (q & 1 ? 0.25f : -0.25f), sy = y + by + (q & 2 ? 0.25f : -0.25f);
          px.add(rocket_at(sx * c + sy * s, -sx * s + sy * c), 0.25f);
        }
        fx_put(b.buf, bw * bh, y * bw + x, px, x, y);
      }
    }
    fx_sprite_done(b);
  }
  rot_box(-14.0f - RK_FLAME[2] - 3.5f, -10.5f, -6.0f, 6.0f, c, s, &bx, &by, &bw, &bh);
  for (int k = 0; k < 3; k++) {
    FxSprite &f = this->rk_fl_[bin][k];
    if (!fx_sprite_init(f, bw, bh, true))
      continue;
    f.ax = -bx;
    f.ay = -by;
    for (int y = 0; y < bh; y++) {
      for (int x = 0; x < bw; x++) {
        const float sx = (float) (x + bx), sy = (float) (y + by);
        fx_put(f.buf, bw * bh, y * bw + x, flame_at(sx * c + sy * s, -sx * s + sy * c, RK_FLAME[k]), x, y);
      }
    }
    fx_sprite_done(f);
  }
}

void WeatherFx::rocket_apply_(bool on) {
  if (on)
    return;
  // Праздник кончился — убрать ракету и её картинки
  if (this->rk_on_)
    this->rocket_stop_();
  this->rocket_free_();
  this->next_rk_ = 0;
}

void WeatherFx::rocket_stop_() {
  this->mark_(this->rk_spot_, false, 0, 0, 1, 1);
  this->rk_on_ = false;
}

void WeatherFx::rocket_free_() {
  for (int i = 0; i < RK_NB; i++) {
    fx_sprite_free(this->rk_body_[i]);
    for (auto &f : this->rk_fl_[i])
      fx_sprite_free(f);
  }
  for (auto &p : this->rk_puff_)
    fx_sprite_free(p);
  this->rk_pre_ = 0;
}

void WeatherFx::rocket_(uint32_t now) {
  if (this->hol_ != HOL_COSMOS)
    return;
  if (!this->rk_on_) {
    // Пока ракета ждёт взлёта — заранее нарисовать её на всех курсах пути,
    // по курсу за кадр
    static const int PRE = 48;
    while (this->rk_pre_ <= PRE && this->render_budget_ > 0) {
      float px, py, pa;
      rk_path(this->rk_pre_++ / (float) PRE, &px, &py, &pa);
      const int b = rk_bin_of(pa, RK_NB);
      if (!this->rk_body_[b].buf) {
        this->rocket_render_(b);
        this->render_budget_ = 0;
      }
    }
    if (this->next_rk_ == 0)
      this->next_rk_ = now + 2000;
    if ((int32_t) (now - this->next_rk_) < 0)
      return;
    this->rk_on_ = true;
    this->rk_t0_ = now;
    this->rk_puff_ms_ = now;
    for (auto &a : this->puff_age_)
      a = PUFF_MS;
    for (int i = 0; i < NPUFF_R; i++)
      if (!this->rk_puff_[i].buf)
        draw_puff(this->rk_puff_[i], 4.0f + i);
  }
  const uint32_t t = now - this->rk_t0_;
  // Дым: клубы остаются позади, расплываются и тают
  for (auto &a : this->puff_age_)
    a += this->dt_ms_;
  if (t >= RK_MS) {
    bool smoke = false;
    for (float a : this->puff_age_)
      smoke |= a < PUFF_MS;
    if (!smoke) {
      this->rocket_stop_();
      this->next_rk_ = now + rnd_(20000, 45000);
      return;
    }
  }
  float x, y, a;
  rk_path(std::min(t / (float) RK_MS, 1.0f), &x, &y, &a);
  if (t < RK_MS) {
    this->rk_x_ = x;
    this->rk_y_ = y;
    this->rk_a_ = a;
    this->rk_flame_ = 8.0f + rnd_(0, 9);
    // Картинки курса ещё нет (не успели заранее) — нарисовать сейчас
    this->rk_bin_ = rk_bin_of(a, RK_NB);
    if (!this->rk_body_[this->rk_bin_].buf)
      this->rocket_render_(this->rk_bin_);
    if (now - this->rk_puff_ms_ > 110) {
      this->rk_puff_ms_ = now;
      int j = 0;
      for (int i = 1; i < NPUFF; i++)
        if (this->puff_age_[i] > this->puff_age_[j])
          j = i;
      this->puff_x_[j] = x - 26.0f * cosf(this->rk_a_);
      this->puff_y_[j] = y - 26.0f * sinf(this->rk_a_);
      this->puff_age_[j] = 0;
    }
  } else {
    this->rk_x_ = -1000;  // ракета улетела, остаётся тающий дым
  }
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  const FxSprite &body = this->rk_body_[this->rk_bin_];
  if (t < RK_MS && body.buf) {
    // Корпус и самое длинное пламя
    const int rx = (int) lroundf(this->rk_x_), ry = (int) lroundf(this->rk_y_);
    const lv_area_t b = fx_sprite_area(body, rx, ry), f = fx_sprite_area(this->rk_fl_[this->rk_bin_][2], rx, ry);
    x0 = std::min(b.x1, f.x1);
    y0 = std::min(b.y1, f.y1);
    x1 = std::max(b.x2, f.x2);
    y1 = std::max(b.y2, f.y2);
  }
  for (int j = 0; j < NPUFF; j++) {
    if (this->puff_age_[j] >= PUFF_MS)
      continue;
    x0 = std::min(x0, this->puff_x_[j] - 13);
    y0 = std::min(y0, this->puff_y_[j] - 13);
    x1 = std::max(x1, this->puff_x_[j] + 13);
    y1 = std::max(y1, this->puff_y_[j] + 13);
  }
  if (x0 > x1) {
    this->mark_(this->rk_spot_, false, 0, 0, 1, 1);
    return;
  }
  // Пламя мигает, дым тает — перерисовать, даже если рамка та же
  if (this->rk_spot_.on)
    this->invalidate_(this->rk_spot_.a);
  this->mark_(this->rk_spot_, true, (int) x0, (int) y0, (int) (x1 - x0) + 1, (int) (y1 - y0) + 1);
}

void WeatherFx::rocket_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const Spot &s = this->rk_spot_;
  if (!s.on)
    return;
  const lv_area_t box = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  if (!fx_hit(box, clip))
    return;
  // Дым — от старых клубов к свежим: свежие тёплые от пламени, потом серые,
  // расплываются и тают
  int ord[NPUFF];
  for (int j = 0; j < NPUFF; j++) {
    int k = j;
    while (k > 0 && this->puff_age_[ord[k - 1]] < this->puff_age_[j]) {
      ord[k] = ord[k - 1];
      k--;
    }
    ord[k] = j;
  }
  for (int j : ord) {
    const float age = this->puff_age_[j];
    if (age >= PUFF_MS)
      continue;
    const float f = age / PUFF_MS;
    const FxSprite &p = this->rk_puff_[std::min(NPUFF_R - 1, (int) lroundf(f * (NPUFF_R - 1)))];
    const lv_color_t c =
        lv_color_mix(lv_color_hex(0xA0A8B0), lv_color_hex(0xFFD6A0), (uint8_t) (255 * fx_clamp01(age / 400.0f)));
    const lv_opa_t o = (lv_opa_t) ((this->dim_ ? 190.0f : 150.0f) * powf(1.0f - f, 1.2f));
    fx_draw(layer, p,
            fx_sprite_area(p, (int) lroundf(this->puff_x_[j]) + oc.x1, (int) lroundf(this->puff_y_[j]) + oc.y1), o, c);
  }
  const FxSprite &body = this->rk_body_[this->rk_bin_];
  if (this->rk_x_ > -500 && body.buf) {
    const int x = (int) lroundf(this->rk_x_) + oc.x1, y = (int) lroundf(this->rk_y_) + oc.y1;
    const int k = this->rk_flame_ < 10.7f ? 0 : (this->rk_flame_ < 14.2f ? 1 : 2);
    const FxSprite &fl = this->rk_fl_[this->rk_bin_][k];
    fx_draw(layer, fl, fx_sprite_area(fl, x, y), LV_OPA_COVER);
    fx_draw(layer, body, fx_sprite_area(body, x, y), LV_OPA_COVER);
  }
}

// ---- Пасхальные яйца ----

// Крашеное яйцо: длина 2·EG_A, ширина 2·EG_B, тупой конец шире (EG_K).
// Картинки — повороты через 15°: узор поворачивается вместе с яйцом, а свет
// всегда слева сверху — яйцо кувыркается, а не просто едет
static const float EG_A = 13.0f, EG_B = 9.3f, EG_K = 0.12f;
static const int EG_N = 24, EG_W = 29, EG_C = 14;
// Яйца катятся по нижнему краю круга — «земля» чуть внутри края — от −62°
// до +62° от самого низа. Скорость по дуге (px/мс), промежуток между
// яйцами, средний радиус яйца (для вращения) и радиус пути середины
static const float EG_GROUND = 228.0f, EG_ARC = 1.08f, EG_V = 0.075f, EG_GAP = 40.0f, EG_ROLL = 11.2f;
static const float EG_PATH_R = 217.0f, EG_LEN = 2.0f * EG_ARC * EG_PATH_R;
// Свет — слева сверху и спереди; блик — посередине между светом и взглядом
static const float EG_L[3] = {-0.48f, -0.62f, 0.62f};
static const float EG_H[3] = {-0.267f, -0.344f, 0.900f};

// Внутри яйца: (u/a)² + (v/(b·(1 + k·u/a)))² ≤ 1
static inline float egg_f(float u, float v) {
  const float t = u / EG_A, w = EG_B * (1.0f + EG_K * t);
  return t * t + v * v / (w * w);
}

// Цвет яйца kind в точке поверхности: U — вдоль яйца (−1 острый конец …
// 1 тупой), f — угол вокруг длинной оси (0 — к нам), rho — толщина яйца
// здесь, px. Края узора мягкие, в пиксель
static void egg_color(int kind, float U, float f, float rho, FxMix &m) {
  const float pu = 1.0f / EG_A;
  auto band = [&](float lo, float hi) { return fx_clamp01((U - lo) / pu + 0.5f) * fx_clamp01((hi - U) / pu + 0.5f); };
  // Кружок радиуса r px на поверхности вокруг (u0, f0)
  auto dot = [&](float u0, float f0, float r) {
    const float du = (U - u0) * EG_A, df = (f - f0) * rho;
    return fx_clamp01(r + 0.5f - sqrtf(du * du + df * df));
  };
  // Ближайший к f угол сетки с шагом st и сдвигом of
  auto grid = [&](float st, float of) { return of + st * floorf((f - of) / st + 0.5f); };
  if (kind == 0) {
    // Красное: светлый пояс с зубчатыми краями, в нём красный горошек;
    // золотые ободки ближе к концам
    m.over(0xD32F2F, 1);
    const float q = fabsf(fmodf(f * 3.0f / FX_PI + 100.0f, 1.0f) * 2.0f - 1.0f);
    m.over(0xFFF3DC, band(-0.27f + 0.11f * q, 0.27f - 0.11f * q));
    m.over(0xD32F2F, dot(0, grid(FX_PI / 3, FX_PI / 6), 1.1f));
    m.over(0xFFC107, band(-0.62f, -0.52f) + band(0.52f, 0.62f));
  } else if (kind == 1) {
    // Синее в горошек: ряды белых и жёлтых точек вразбежку
    m.over(0x1E88E5, 1);
    static const float ROWS[4] = {-0.6f, -0.2f, 0.2f, 0.6f};
    for (int j = 0; j < 4; j++)
      m.over(j % 2 ? 0xFFEB3B : 0xFFFFFF, dot(ROWS[j], grid(0.8f, j % 2 ? 0.4f : 0.0f), 1.6f));
  } else {
    // Жёлтое в полоску: зелёная и малиновая, между ними — белая волна
    m.over(0xFFC107, 1);
    m.over(0x2E7D32, band(-0.52f, -0.34f));
    m.over(0xD81B60, band(0.34f, 0.52f));
    const float wv = 0.12f * sinf(f * 4.0f);
    m.over(0xFFFFFF, band(wv - 0.06f, wv + 0.06f));
  }
}

// Поворот i яйца kind: объём — освещением, край сглажен по 16 точкам
// пикселя (только у края — внутри и снаружи хватает середины)
static void draw_egg(uint8_t *p, int kind, int i) {
  const float th = 2 * FX_PI * i / EG_N, cs = cosf(th), sn = sinf(th);
  const int n = EG_W * EG_W;
  for (int y = 0; y < EG_W; y++) {
    for (int x = 0; x < EG_W; x++) {
      // В осях яйца: u — вдоль (к тупому концу), v — поперёк
      const float X = (float) (x - EG_C), Y = (float) (y - EG_C);
      float u = X * cs + Y * sn, v = -X * sn + Y * cs;
      const float t0 = u / EG_A, w0 = EG_B * (1.0f + EG_K * t0);
      const float F = egg_f(u, v);
      // До края, px: по наклону F
      const float gu = 2 * t0 / EG_A - 2 * v * v * EG_K / (EG_A * w0 * w0 * (1.0f + EG_K * t0));
      const float gv = 2 * v / (w0 * w0);
      const float e = (1.0f - F) / std::max(1e-4f, sqrtf(gu * gu + gv * gv));
      float cov = 1;
      if (e < -1.0f)
        continue;
      if (e < 1.0f) {
        int in = 0;
        for (int sy = 0; sy < 4; sy++) {
          for (int sx = 0; sx < 4; sx++) {
            const float ox = (sx - 1.5f) * 0.25f, oy = (sy - 1.5f) * 0.25f;
            in += egg_f(u + ox * cs + oy * sn, v - ox * sn + oy * cs) <= 1.0f;
          }
        }
        if (!in)
          continue;
        cov = in / 16.0f;
      }
      // Точка поверхности: у самого края — чуть внутрь
      if (F > 0.96f) {
        const float k = sqrtf(0.96f / F);
        u *= k;
        v *= k;
      }
      const float t = u / EG_A, q = 1.0f + EG_K * t;
      const float rho = EG_B * q * sqrtf(std::max(0.0f, 1.0f - t * t));
      const float z = sqrtf(std::max(0.0f, rho * rho - v * v));
      // Нормаль тела вращения: (−ρ·ρ′, v, z)
      const float rr = EG_B * EG_B / EG_A * q * (EG_K * (1.0f - t * t) - t * q);
      float nu = -rr, nv = v, nz = z;
      const float nl = sqrtf(nu * nu + nv * nv + nz * nz);
      if (nl > 1e-4f) {
        nu /= nl;
        nv /= nl;
        nz /= nl;
      }
      const float nx = nu * cs - nv * sn, ny = nu * sn + nv * cs;
      const float dif = std::max(0.0f, nx * EG_L[0] + ny * EG_L[1] + nz * EG_L[2]);
      // Блик — степень 32 (возведением в квадрат: powf на плате медленный)
      float sp = std::max(0.0f, nx * EG_H[0] + ny * EG_H[1] + nz * EG_H[2]);
      for (int j = 0; j < 5; j++)
        sp *= sp;
      FxMix c;
      egg_color(kind, t, fx_atan2(v, z), std::max(rho, 1.0f), c);
      const float sh = (0.3f + 0.8f * dif) * (0.82f + 0.18f * nz);
      FxMix m;
      m.r = std::min(255.0f, c.r * sh + 150.0f * sp) * cov;
      m.g = std::min(255.0f, c.g * sh + 150.0f * sp) * cov;
      m.b = std::min(255.0f, c.b * sh + 150.0f * sp) * cov;
      m.a = cov;
      fx_put(p, n, y * EG_W + x, m, x, y);
    }
  }
}

// Нарисовать поворот i яйца kind, если ещё не нарисован
static bool egg_render(FxSheet &s, int kind, int i) {
  uint8_t *p = fx_sheet_take(s, i);
  if (!p)
    return false;
  draw_egg(p, kind, i);
  fx_sheet_crop(s, i);
  fx_sheet_done(s, i);
  return true;
}

void WeatherFx::eggs_apply_(bool on) {
  if (on && !this->egg_sheet_[0].buf) {
    // Картинки — на весь праздник: яйца катятся снова и снова
    for (auto &s : this->egg_sheet_) {
      if (!fx_sheet_init(s, EG_N, EG_W, EG_W, true)) {
        for (auto &f : this->egg_sheet_)
          fx_sheet_free(f);
        return;
      }
      s.ax = s.ay = EG_C;
    }
    this->egg_pre_ = 0;
  }
  if (!on) {
    for (auto &e : this->egg_spot_)
      this->mark_(e, false, 0, 0, 1, 1);
    for (auto &s : this->egg_sheet_)
      fx_sheet_free(s);
    this->egg_on_ = false;
    this->next_egg_ = 0;
  }
}

void WeatherFx::eggs_(uint32_t now) {
  if (this->hol_ != HOL_EASTER || !this->egg_sheet_[NEGG - 1].buf)
    return;
  if (!this->egg_on_) {
    // Пока яйца не покатились — заранее нарисовать все повороты, по паре
    // картинок за кадр
    while (this->egg_pre_ < NEGG * EG_N && this->render_budget_ > 0) {
      const int k = this->egg_pre_++;
      if (egg_render(this->egg_sheet_[k / EG_N], k / EG_N, k % EG_N))
        this->render_budget_--;
    }
    if (this->next_egg_ == 0)
      this->next_egg_ = now + 1500;
    if ((int32_t) (now - this->next_egg_) < 0)
      return;
    this->egg_on_ = true;
    this->egg_t0_ = now;
    this->egg_dir_ = rnd_(0, 2) ? 1 : -1;
    for (auto &th : this->egg_th_)
      th = rnd_(0, 628) / 100.0f;
  }
  // Три яйца одно за другим катятся по краю круга, кувыркаясь: стоит на
  // конце — середина выше, лежит на боку — ниже
  const float t = now - this->egg_t0_, dir = this->egg_dir_;
  bool any = false;
  for (int i = 0; i < NEGG; i++) {
    const float s = EG_V * t - i * EG_GAP;
    any |= s < EG_LEN;
    Spot &sp = this->egg_spot_[i];
    if (s < 0 || s >= EG_LEN) {
      this->mark_(sp, false, 0, 0, 1, 1);
      continue;
    }
    FxSheet &sh = this->egg_sheet_[i];
    const float th = this->egg_th_[i] + dir * s / EG_ROLL;
    int k = ((int) lroundf(th / (2 * FX_PI) * EG_N) % EG_N + EG_N) % EG_N;
    if (!fx_sheet_ready(sh, k)) {
      // Ещё не нарисован: по очереди с остальными картинками, а пока —
      // ближайший готовый поворот (если готовых нет — сразу)
      int near = -1;
      for (int d = 1; d <= EG_N / 2 && near < 0; d++) {
        if (fx_sheet_ready(sh, (k + d) % EG_N))
          near = (k + d) % EG_N;
        else if (fx_sheet_ready(sh, (k + EG_N - d) % EG_N))
          near = (k + EG_N - d) % EG_N;
      }
      if (this->render_budget_ > 0 || near < 0) {
        if (egg_render(sh, i, k))
          this->render_budget_--;
      } else {
        k = near;
      }
    }
    const float al = dir * (s / EG_PATH_R - EG_ARC), sa = sinf(al), ca = cosf(al);
    // Опора: насколько яйцо в этом повороте выступает к земле
    const float thk = 2 * FX_PI * k / EG_N, c = cosf(thk) * sa + sinf(thk) * ca;
    const float r = EG_GROUND - sqrtf(EG_A * EG_A * c * c + EG_B * EG_B * (1.0f - c * c));
    const int cx = (int) lroundf(FX_CX + r * sa), cy = (int) lroundf(FX_CY + r * ca);
    // Появляется и исчезает плавно
    const uint8_t o = (uint8_t) (255.0f * std::min(1.0f, std::min(s, EG_LEN - s) / 24.0f));
    const lv_area_t a = fx_sheet_area(sh, k, cx, cy);
    const bool same = sp.on && a.x1 == sp.a.x1 && a.y1 == sp.a.y1 && a.x2 == sp.a.x2 && a.y2 == sp.a.y2;
    if (same && (k != this->egg_f_[i] || o != this->egg_o_[i]))
      this->invalidate_(sp.a);
    this->egg_f_[i] = (uint8_t) k;
    this->egg_o_[i] = o;
    this->mark_(sp, true, a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1);
  }
  if (!any) {
    this->egg_on_ = false;
    this->next_egg_ = now + rnd_(15000, 30000);
  }
}

void WeatherFx::eggs_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  for (int i = 0; i < NEGG; i++) {
    const Spot &s = this->egg_spot_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (fx_hit(a, clip))
      fx_sheet_draw(layer, this->egg_sheet_[i], this->egg_f_[i], a, this->egg_o_[i]);
  }
}

// ---- «Матрица» ----

// Шаг столбцов и высота знака
static const int MX_X0 = 45, MX_DX = 47, MX_STEP = 18;

void WeatherFx::matrix_() {
  if (!this->mx_on_)
    return;
  const float k = this->k_;
  for (int c = 0; c < NMX; c++) {
    this->mx_y_[c] += k * this->mx_v_[c];
    const float top = this->mx_y_[c] - (MX_LEN - 1) * MX_STEP;
    if (top > 480) {
      this->mx_y_[c] = -rnd_(0, 200);
      this->mx_v_[c] = rnd_(40, 80) / 10.0f;
    }
    // Знаки в столбце то и дело меняются
    if (rnd_(0, 3) == 0)
      this->mx_ch_[c][rnd_(0, MX_LEN)] ^= 1;
    const int x = MX_X0 + c * MX_DX;
    const int y0 = (int) top + this->mx_oy_, y1 = (int) this->mx_y_[c] + 6;
    if (this->mx_spot_[c].on)
      this->invalidate_(this->mx_spot_[c].a);
    this->mark_(this->mx_spot_[c], y1 > 0 && y0 < 466, x - 10, y0, 21, y1 - y0);
  }
}

void WeatherFx::matrix_apply_(bool on) {
  if (on && !this->mx_on_) {
    for (int c = 0; c < NMX; c++) {
      this->mx_y_[c] = -rnd_(0, 300);
      this->mx_v_[c] = rnd_(40, 80) / 10.0f;
      for (auto &ch : this->mx_ch_[c])
        ch = rnd_(0, 2);
    }
  }
  if (!on)
    for (auto &m : this->mx_spot_)
      this->mark_(m, false, 0, 0, 1, 1);
  this->mx_on_ = on;
}

void WeatherFx::matrix_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  lv_area_t abs;
  // Экранный прямоугольник частицы; false — если не попадает в участок
  auto place = [&](const Spot &s) {
    if (!s.on)
      return false;
    abs = {(int32_t) (s.a.x1 + oc.x1), (int32_t) (s.a.y1 + oc.y1), (int32_t) (s.a.x2 + oc.x1),
           (int32_t) (s.a.y2 + oc.y1)};
    return fx_hit(abs, clip);
  };

  // «Матрица»: столбцы нулей и единиц, яркая голова и гаснущий хвост
  if (this->mx_on_) {
    lv_draw_letter_dsc_t mx;
    lv_draw_letter_dsc_init(&mx);
    mx.font = this->font_cap_;
    for (int c = 0; c < NMX; c++) {
      if (!place(this->mx_spot_[c]))
        continue;
      const int x = MX_X0 + c * MX_DX + oc.x1;
      for (int j = 0; j < MX_LEN; j++) {
        const int y = (int) this->mx_y_[c] - j * MX_STEP + oc.y1;
        mx.color = lv_color_hex(j == 0 ? 0xD8FFD8 : 0x22DD55);
        mx.opa = (lv_opa_t) (j == 0 ? 255 : 230 - j * 26);
        mx.unicode = '0' + this->mx_ch_[c][j];
        lv_point_t pt = {x, y};
        lv_draw_letter(layer, &mx, &pt);
      }
    }
  }
}

}  // namespace weather_fx
}  // namespace esphome

// Дождь и град на странице часов.
//
// Струи в три слоя глубины: дальние тонкие, короткие, тусклые и медленные,
// ближние — длинные, яркие и быстрые. Все струи наклонены по ветру и вместе
// качаются в порывах. Ближние и средние разбиваются о нижний край круга
// брызгами; градины отскакивают от него.
//
// Всё рисуется готовыми картинками, нарисованными при запуске: струя — маска
// A8 (цвет и прозрачность задаются при рисовании, так что вспышка молнии
// ничего не стоит). Картинка дешевле сглаженной линии LVGL и рисует то, что
// примитивами не нарисовать. За кругом экрана ничего не рисуется — там
// пиксели не видны.
#include "fx_draw.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace weather_fx {

// Слои глубины падающего дождя: скорость, px за 50 мс; длина струи (два
// варианта); толщина; непрозрачность
struct RainLayer {
  float v0, v1;
  int len[2];
  float thick;
  lv_opa_t opa;
};
static const RainLayer LAYERS[3] = {
    {13.0f, 16.0f, {15, 19}, 1.5f, 190},  // дальний: 260–320 px/с
    {19.0f, 23.0f, {24, 30}, 2.0f, 230},  // средний: 380–460 px/с
    {26.0f, 31.0f, {38, 46}, 2.8f, 255},  // ближний: 520–620 px/с
};
// Наклон струй при полном ветре (тангенс угла от вертикали): ~30°
static const float MAX_TAN = 0.58f;
// Картинки струй перерисовываются, когда угол меняется на 1,5°
static const float ANG_STEP = 0.026f;
// Цвета струй по слоям (дальний, средний, ближний) и брызг: обычные и для
// приглушённого экрана — там ярче, иначе при низкой яркости не видно
static const uint32_t STREAK_C[2][3] = {{0x5687B0, 0x7DB2DD, 0xB4DDFA}, {0x86B5DC, 0xB2DAF6, 0xE6F5FF}};
static const uint32_t SPLASH_C[2] = {0x9FD0F2, 0xE2F4FF};
static const uint32_t FLASH_C = 0xEAF6FF;  // струи во вспышке молнии

// Струи падают «пучками»: в дальнем слое по четыре струи, в среднем по три,
// ближние — по одной. Струи одного слоя падают с одной скоростью, так что
// пучок выглядит естественно, а перерисовывается одним участком — это
// дешевле отдельных струй. Рисуется пучок отдельными узкими картинками
// струй: одна общая картинка была бы почти пустой, а смешивание пикселей
// идёт по всей картинке, включая прозрачные. kind: 0–2 — рисунки пучков
// дальнего слоя, 3–5 — среднего, 6–7 — ближние струи двух длин
static const uint8_t KIND_LAYER[8] = {0, 0, 0, 1, 1, 1, 2, 2};

// Пучок: сколько струй, сдвиг головы каждой от головы первой (первая ниже
// всех), длина (0 — короткая картинка слоя, 1 — длинная) и яркость
struct Cluster {
  uint8_t n;
  int8_t ox[4], oy[4];
  uint8_t var[4];
  uint8_t k[4];
};

static const Cluster &cluster(int kind) {
  static Cluster tab[8];
  static bool ready = false;
  if (!ready) {
    ready = true;
    for (int kd = 0; kd < 8; kd++) {
      Cluster &c = tab[kd];
      const int layer = KIND_LAYER[kd];
      if (layer == 2) {
        c.n = 1;
        c.ox[0] = c.oy[0] = 0;
        c.var[0] = (uint8_t) (kd - 6);
        c.k[0] = 255;
        continue;
      }
      // У каждого рисунка свой постоянный разброс — пучки не похожи друг на друга
      uint32_t seed = 2654435761u * (uint32_t) (kd + 1);
      auto r = [&seed](int lo, int hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (int) ((seed >> 8) % (uint32_t) (hi - lo + 1));
      };
      c.n = layer == 0 ? 4 : 3;
      c.ox[0] = c.oy[0] = 0;
      c.var[0] = (uint8_t) r(0, 1);
      c.k[0] = 255;
      int side = r(0, 1) ? 1 : -1;
      for (int m = 1; m < c.n; m++) {
        // Через одну — по разные стороны от первой, дальше и выше
        const int far = (m + 1) / 2;
        c.ox[m] = (int8_t) (side * (far == 1 ? r(layer == 0 ? 6 : 9, layer == 0 ? 13 : 17)
                                             : r(layer == 0 ? 15 : 20, layer == 0 ? 24 : 30)));
        c.oy[m] = (int8_t) -r(8 + (far - 1) * 14, (layer == 0 ? 30 : 34) + (far - 1) * 14);
        c.var[m] = (uint8_t) r(0, 1);
        c.k[m] = (uint8_t) r(165, 255);
        side = -side;
      }
    }
  }
  return tab[kind];
}

// Струя: сглаженный отрезок от хвоста (почти прозрачного) к голове (яркой),
// к голове чуть толще — там сама капля. Наклон tan_a — на сколько px струя
// сдвигается вбок за 1 px вниз. Маска A8, точка привязки — голова
static void draw_streak(FxSprite &s, float len, float thick, float tan_a) {
  const float c = 1.0f / sqrtf(1.0f + tan_a * tan_a), sn = tan_a * c;
  const float dx = len * sn, dy = len * c;
  const float pad = thick * 0.5f + 1.5f;
  const int w = (int) ceilf(fabsf(dx) + 2 * pad), h = (int) ceilf(dy + 2 * pad);
  if (!fx_sprite_init(s, w, h, false))
    return;
  const float hx = dx >= 0 ? w - pad : pad, hy = h - pad;
  const float tx = hx - dx, ty = hy - dy;
  s.ax = (int) hx;
  s.ay = (int) hy;
  const float l2 = len * len;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      // Далеко от струи — пусто, подвыборки не нужны
      {
        const float px = x + 0.5f, py = y + 0.5f;
        const float t = fx_clamp01(((px - tx) * dx + (py - ty) * dy) / l2);
        const float ex = tx + t * dx - px, ey = ty + t * dy - py;
        const float reach = thick * 0.5f + 0.75f;
        if (ex * ex + ey * ey > reach * reach)
          continue;
      }
      float acc = 0;
      for (int sy = 0; sy < 3; sy++) {
        for (int sx = 0; sx < 3; sx++) {
          const float px = x + (sx + 0.5f) / 3.0f, py = y + (sy + 0.5f) / 3.0f;
          const float t = fx_clamp01(((px - tx) * dx + (py - ty) * dy) / l2);
          const float ex = tx + t * dx - px, ey = ty + t * dy - py;
          const float half = thick * 0.5f * (0.55f + 0.45f * t * t);
          if (ex * ex + ey * ey > half * half)
            continue;
          // Вдоль струи: хвост мягко тает, у головы ярче всего
          acc += 0.12f + 0.88f * t * sqrtf(t);
        }
      }
      s.buf[y * w + x] = (uint8_t) std::min(255.0f, acc / 9.0f * 255.0f + 0.5f);
    }
  }
  fx_sprite_done(s);
}

// Капелька брызг: мягкая точка 3×3; вспышка удара — сплюснутый овал 9×3
static void draw_dot(FxSprite &s, int w, int h) {
  if (!fx_sprite_init(s, w, h, false))
    return;
  s.ax = w / 2;
  s.ay = h / 2;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const float u = (x + 0.5f - w * 0.5f) / (w * 0.5f), v = (y + 0.5f - h * 0.5f) / (h * 0.5f);
      const float k = fx_clamp01(1.15f - sqrtf(u * u + v * v));
      s.buf[y * w + x] = (uint8_t) (255.0f * std::min(1.0f, k * 1.6f));
    }
  }
  fx_sprite_done(s);
}

// ---------------------------------------------------------------------------

bool WeatherFx::visible_(int x, int y, int w, int h) const {
  // Ближайшая к центру экрана точка прямоугольника — внутри круга?
  const float nx = std::min(std::max(FX_CX, (float) x), (float) (x + w - 1));
  const float ny = std::min(std::max(FX_CY, (float) y), (float) (y + h - 1));
  return (nx - FX_CX) * (nx - FX_CX) + (ny - FX_CY) * (ny - FX_CY) < FX_R * FX_R;
}

void WeatherFx::streak_sprites_(int q) {
  this->streak_q_ = q;
  const float t = tanf(q * ANG_STEP);
  for (int k = 0; k < NSTV; k++)
    draw_streak(this->streak_[k], LAYERS[k / 2].len[k % 2], LAYERS[k / 2].thick, t);
}

// Градина: белёсый ледяной шарик с бликом
void WeatherFx::hail_sprite_() {
  if (this->hail_spr_.buf)
    return;
  const int d = 8;
  FxSprite &s = this->hail_spr_;
  if (!fx_sprite_init(s, d, d, true))
    return;
  s.ax = d / 2;
  s.ay = d / 2;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + d * d * 2;
  for (int y = 0; y < d; y++) {
    for (int x = 0; x < d; x++) {
      const float u = (x + 0.5f - d * 0.5f) / (d * 0.5f), v = (y + 0.5f - d * 0.5f) / (d * 0.5f);
      const float rr = sqrtf(u * u + v * v);
      const float cov = fx_clamp01((1.0f - rr) * d * 0.5f + 0.5f);
      const float spec = expf(-((u + 0.35f) * (u + 0.35f) + (v + 0.35f) * (v + 0.35f)) / 0.08f);
      const float shade = 0.72f + 0.28f * (1.0f - rr);
      const int base_r = (int) (200 * shade), base_g = (int) (222 * shade), base_b = (int) (238 * shade);
      const lv_color_t c =
          lv_color_make((uint8_t) (base_r + (255 - base_r) * spec), (uint8_t) (base_g + (255 - base_g) * spec),
                        (uint8_t) (base_b + (255 - base_b) * spec));
      rgb[y * d + x] = lv_color_to_u16(c);
      alpha[y * d + x] = (uint8_t) (cov * 245);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::rain_hide_() {
  for (auto &s : this->rs_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (int j = 0; j < NSPL; j++) {
    this->mark_(this->sps_[j], false, 0, 0, 1, 1);
    this->splife_[j] = 0;
  }
}

void WeatherFx::rain_apply_(const Params &p, bool relayout) {
  const int cnt = std::min(std::max(p.count, 0), ND);
  this->hail_ = p.mode == 4;
  // В мокром снеге дождь вдвое слабее
  this->nd_ = (p.mode == 1 || p.mode == 4) ? cnt : (p.mode == 3 ? cnt / 2 : 0);
  const int pal = p.dim ? 1 : 0;
  for (int i = 0; i < 3; i++)
    this->c_streak_[i] = lv_color_hex(STREAK_C[pal][i]);
  this->c_splash_ = lv_color_hex(SPLASH_C[pal]);
  // Струй (пучков) и градин — по силе дождя: это около двух струй на единицу
  // силы
  const int nrs = std::min(NRS, this->nd_);
  if (!this->dot_.buf) {
    draw_dot(this->dot_, 3, 3);
    draw_dot(this->flash_, 9, 3);
  }
  if (this->hail_)
    this->hail_sprite_();
  if (nrs && !this->streak_[0].buf)
    this->streak_sprites_(0);
  // Слой погоды виден, только если на нём что-то есть — теперь и дождь
  this->update_root_();
  if (!relayout)
    return;

  // Новая погода — всё с чистого листа: струи сразу по всему экрану
  this->rain_hide_();
  this->nrs_ = nrs;
  for (int i = 0; i < nrs; i++) {
    // Слои вперемешку: из каждых 10 — 4 пучка дальних, 3 пучка средних и
    // 3 ближние струи; рисунки пучков чередуются
    const int m = i % 10, v = i + i / 10;
    this->rkind_[i] = (uint8_t) (this->hail_ ? 6 : (m < 4 ? v % 3 : (m < 7 ? 3 + v % 3 : 6 + v % 2)));
    this->respawn_streak_(i, this->tan_a_, true);
  }
}

void WeatherFx::respawn_streak_(int i, float tan_a, bool anywhere) {
  const RainLayer &l = LAYERS[KIND_LAYER[this->rkind_[i]]];
  this->rv_[i] = l.v0 + (l.v1 - l.v0) * rnd_(0, 101) / 100.0f;
  this->rland_[i] = false;
  if (this->hail_)
    this->rv_[i] *= 0.85f;
  // Новая струя — над экраном (или где угодно — при смене погоды). Чтобы
  // снос ветром не уносил струи за край, они начинаются против ветра
  const float y = anywhere ? rnd_(-60, 440) : -rnd_(0, 60);
  this->ry_[i] = y;
  this->rx_[i] = rnd_(10, 456) - tan_a * (FX_CY - y) * 0.9f;
}

void WeatherFx::streaks_(float wind) {
  // Наклон — общий для всех струй: по ветру, в порывах качается
  const float want = wind * MAX_TAN;
  const int q = (int) lroundf(atanf(want) / ANG_STEP);
  if (q != this->streak_q_ && !this->hail_)
    this->streak_sprites_(q);
  this->tan_a_ = this->hail_ ? want : tanf(this->streak_q_ * ANG_STEP);
  // Дальние пучки — тусклые и мелкие — двигаются через кадр, вдвое дальше:
  // глазу незаметно, а участков к перерисовке меньше. Половина — на чётных
  // кадрах, половина — на нечётных, чтобы кадры были ровными
  this->rain_tick_ ^= 1;
  for (int i = 0; i < this->nrs_; i++) {
    const int kind = this->rkind_[i];
    float k = this->k_;
    if (!this->hail_ && KIND_LAYER[kind] == 0) {
      this->rfar_k_[i] += k;
      if ((i & 1) != this->rain_tick_)
        continue;
      k = this->rfar_k_[i];
      this->rfar_k_[i] = 0;
    }
    const float v = this->rv_[i];
    this->ry_[i] += k * v;
    this->rx_[i] += k * v * this->tan_a_;
    // Нижний край круга. Голова коснулась — брызги (у дальних их не видно),
    // градина отскакивает. Струя уходит в землю: ниже края её не видно, и
    // новая начинается, когда за край ушёл и хвост
    const float g = fx_bottom(this->rx_[i]) - 6.0f;
    this->rgy_[i] = g;
    if (this->ry_[i] > g && !this->rland_[i]) {
      this->rland_[i] = true;
      if (std::fabs(this->rx_[i] - FX_CX) < 205.0f)
        this->splash_(this->rx_[i], g, this->hail_ ? 3 : KIND_LAYER[kind], wind);
    }
    if (this->rland_[i] || this->rx_[i] < -50 || this->rx_[i] > 516) {
      // Верх пучка (самый высокий хвост) ушёл за край — струя кончилась
      const FxSprite *spr[4];
      lv_area_t r[4];
      lv_opa_t o[4];
      const int n = this->streak_parts_(i, spr, r, o);
      int top = 10000;
      for (int m = 0; m < n; m++)
        top = std::min(top, (int) r[m].y1);
      if (this->hail_ || top > g || this->rx_[i] < -50 || this->rx_[i] > 516)
        this->respawn_streak_(i, this->tan_a_, false);
    }
    // Участок к перерисовке — общий на весь пучок
    const FxSprite *spr[4];
    lv_area_t r[4];
    lv_opa_t o[4];
    const int n = this->streak_parts_(i, spr, r, o);
    lv_area_t u = r[0];
    for (int m = 1; m < n; m++)
      lv_area_join(&u, &u, &r[m]);
    const int w = lv_area_get_width(&u), h = lv_area_get_height(&u);
    this->mark_(this->rs_[i], spr[0]->buf && this->visible_(u.x1, u.y1, w, h) && !this->excluded_(u.x1, u.y1, w, h),
                u.x1, u.y1, w, h);
  }
}

int WeatherFx::streak_parts_(int i, const FxSprite **spr, lv_area_t *rect, lv_opa_t *opa) const {
  const int x0 = (int) this->rx_[i], y0 = (int) this->ry_[i];
  if (this->hail_) {
    spr[0] = &this->hail_spr_;
    rect[0] = fx_sprite_area(this->hail_spr_, x0, y0);
    opa[0] = LV_OPA_COVER;
    return 1;
  }
  const int kind = this->rkind_[i], layer = KIND_LAYER[kind];
  const Cluster &c = cluster(kind);
  for (int m = 0; m < c.n; m++) {
    const FxSprite &s = this->streak_[layer * 2 + c.var[m]];
    spr[m] = &s;
    rect[m] = fx_sprite_area(s, x0 + c.ox[m], y0 + c.oy[m]);
    opa[m] = (lv_opa_t) (LAYERS[layer].opa * c.k[m] / 255);
  }
  return c.n;
}

// kind: 0 — дальняя капля (брызг не видно), 1 — средняя, 2 — ближняя,
// 3 — градина (отскакивает)
void WeatherFx::splash_(float x, float y, int kind, float wind) {
  if (kind == 0)
    return;
  auto slot = [this]() {
    for (int j = 0; j < NSPL; j++)
      if (this->splife_[j] <= 0)
        return j;
    return -1;
  };
  auto add = [&](int sk, float vx, float vy, float life) {
    const int j = slot();
    if (j < 0)
      return;
    this->spk_[j] = (uint8_t) sk;
    this->spx_[j] = x;
    this->spy_[j] = y - (sk == 1 ? 0 : 2);
    this->spvx_[j] = vx;
    this->spvy_[j] = vy;
    this->splife_[j] = life;
    this->spmax_[j] = life;
  };
  if (kind == 3) {
    add(2, rnd_(-14, 15) / 10.0f + wind, -rnd_(20, 33) / 10.0f, rnd_(550, 750));
    return;
  }
  if (kind == 1 && rnd_(0, 100) < 45)
    return;
  // Вспышка удара и капельки веером вверх и в стороны
  if (kind == 2)
    add(1, 0, 0, 110);
  // Ближняя капля — корона из 3–5 капелек, взлетают на 10–20 px; средняя —
  // одна-две пониже
  const int n = kind == 2 ? rnd_(3, 6) : rnd_(1, 3);
  for (int m = 0; m < n; m++) {
    const float side = (m % 2 ? 1.0f : -1.0f) * (kind == 2 ? rnd_(8, 30) : rnd_(5, 18)) / 10.0f;
    const float up = (kind == 2 ? rnd_(38, 60) : rnd_(22, 36)) / 10.0f;
    add(0, side + wind * 0.6f, -up, kind == 2 ? rnd_(380, 620) : rnd_(260, 420));
  }
}

void WeatherFx::splashes_() {
  const float k = this->k_;
  for (int j = 0; j < NSPL; j++) {
    if (this->splife_[j] <= 0) {
      this->mark_(this->sps_[j], false, 0, 0, 1, 1);
      continue;
    }
    this->splife_[j] -= this->dt_ms_;
    this->spx_[j] += k * this->spvx_[j];
    this->spy_[j] += k * this->spvy_[j];
    this->spvy_[j] += k * 0.85f;
    // Градина отскакивает ещё раз, слабее
    if (this->spk_[j] == 2 && this->spvy_[j] > 0 && this->spy_[j] > fx_bottom(this->spx_[j]) - 6.0f) {
      this->spy_[j] = fx_bottom(this->spx_[j]) - 6.0f;
      this->spvy_[j] *= -0.35f;
    }
    // Гаснут к концу жизни; вспышка — сразу
    const float f = fx_clamp01(this->splife_[j] / this->spmax_[j]);
    const lv_opa_t opa = (lv_opa_t) (255.0f * (this->spk_[j] == 1 ? f : std::min(1.0f, f * 2.2f)));
    const FxSprite &sp = this->spk_[j] == 2 ? this->hail_spr_ : (this->spk_[j] == 1 ? this->flash_ : this->dot_);
    const lv_area_t a = fx_sprite_area(sp, (int) this->spx_[j], (int) this->spy_[j]);
    const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    const bool on =
        this->splife_[j] > 0 && sp.buf && this->visible_(a.x1, a.y1, w, h) && !this->excluded_(a.x1, a.y1, w, h);
    Spot &s = this->sps_[j];
    // Стоит на месте, но гаснет — перерисовать и так
    if (on && s.on && s.a.x1 == a.x1 && s.a.y1 == a.y1 && opa != this->spopa_[j])
      this->invalidate_(s.a);
    this->spopa_[j] = opa;
    this->mark_(s, on, a.x1, a.y1, w, h);
  }
}

void WeatherFx::rain_frame_(float wind) {
  if (this->nrs_)
    this->streaks_(wind);
  this->splashes_();
}

void WeatherFx::rain_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  lv_area_t a;
  auto place = [&](const Spot &s) {
    if (!s.on)
      return false;
    a = {(int32_t) (s.a.x1 + oc.x1), (int32_t) (s.a.y1 + oc.y1), (int32_t) (s.a.x2 + oc.x1),
         (int32_t) (s.a.y2 + oc.y1)};
    return fx_hit(a, clip);
  };
  // Струи — цветом своего слоя; во вспышке молнии все белые. Ниже края
  // круга струя ушла в землю — там её не рисовать
  const bool flash = this->bolt_on_;
  const lv_area_t keep = layer->_clip_area;
  for (int i = 0; i < this->nrs_; i++) {
    if (!place(this->rs_[i]))
      continue;
    const FxSprite *spr[4];
    lv_area_t r[4];
    lv_opa_t o[4];
    const int n = this->streak_parts_(i, spr, r, o);
    const lv_color_t c = flash ? lv_color_hex(FLASH_C) : this->c_streak_[KIND_LAYER[this->rkind_[i]]];
    const int32_t gy = (int32_t) this->rgy_[i] + oc.y1;
    if (gy < layer->_clip_area.y2)
      layer->_clip_area.y2 = gy;
    for (int m = 0; m < n && layer->_clip_area.y2 >= layer->_clip_area.y1; m++) {
      a = {r[m].x1 + oc.x1, r[m].y1 + oc.y1, r[m].x2 + oc.x1, r[m].y2 + oc.y1};
      if (fx_hit(a, layer->_clip_area))
        fx_draw(layer, *spr[m], a, flash ? (lv_opa_t) LV_OPA_COVER : o[m], c);
    }
    layer->_clip_area = keep;
  }
  // Брызги и отскоки
  for (int j = 0; j < NSPL; j++) {
    if (!place(this->sps_[j]))
      continue;
    const uint8_t sk = this->spk_[j];
    fx_draw(layer, sk == 2 ? this->hail_spr_ : (sk == 1 ? this->flash_ : this->dot_), a, this->spopa_[j],
            this->c_splash_);
  }
}

}  // namespace weather_fx
}  // namespace esphome

// Праздничное и редкое: Вифлеемская звезда, метеор, воздушный змей, ракета
// с дымом, марево в жару и сосульки в мороз.
//
// Всё — готовыми картинками, как остальной погодный фон. Прежде это были
// треугольники, отрезки и круги LVGL, которые заново считались на каждом
// участке перерисовки: марево из 36 сглаженных отрезков обходилось дороже
// ливня, змей и ракета — вдвое дороже звёздного неба.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

static const float PI_X = 3.14159265f;

namespace {

// Точка картинки RGB565A8, собираемая слоями снизу вверх: цвет, умноженный
// на непрозрачность, и сама непрозрачность
struct Layered {
  float r{0}, g{0}, b{0}, a{0};
  // Слой цвета c с непрозрачностью k поверх
  void over(uint32_t c, float k) {
    if (k <= 0)
      return;
    k = std::min(k, 1.0f);
    this->r = ((c >> 16) & 0xFF) * k + this->r * (1 - k);
    this->g = ((c >> 8) & 0xFF) * k + this->g * (1 - k);
    this->b = (c & 0xFF) * k + this->b * (1 - k);
    this->a = k + this->a * (1 - k);
  }
  // Сложить с долей w — для сглаживания по нескольким точкам пикселя
  void add(const Layered &o, float w) {
    this->r += o.r * w;
    this->g += o.g * w;
    this->b += o.b * w;
    this->a += o.a * w;
  }
};

}  // namespace

// Записать точку i картинки RGB565A8 из n точек
static void put_px(uint8_t *p, int n, int i, const Layered &m, int x, int y) {
  if (m.a <= 0.004f)
    return;
  reinterpret_cast<uint16_t *>(p)[i] =
      lv_color_to_u16(lv_color_make((uint8_t) std::min(255.0f, m.r / m.a), (uint8_t) std::min(255.0f, m.g / m.a),
                                    (uint8_t) std::min(255.0f, m.b / m.a)));
  p[n * 2 + i] = (uint8_t) std::min(255.0f, std::max(0.0f, m.a * 255.0f + fx_dither(x, y) * 3.0f));
}

static inline float sq(float v) { return v * v; }

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
    const float a = k * (PI_X / 4) - PI_X / 2;
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
      Layered m;
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
      put_px(p, BS_W * BS_H, y * BS_W + x, m, x, y);
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
  this->bstar_ph_ = fmodf(this->bstar_ph_ + 0.35f, 2 * PI_X);
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

// ---- Метеор ----

static const float MET_TAIL = 60.0f;  // длина хвоста, px
static const int MET_M = 5;           // поле вокруг головы и конца хвоста

// След под курс метеора, рисуется при вылете: тонкое ядро сужается и
// гаснет к концу хвоста, вокруг — голубоватое сияние, голова — яркая точка
// с ореолом. Яркость и оттенок у каждого метеора свои: чаще голубовато-
// белый, изредка зеленоватый или золотистый
void WeatherFx::meteor_sprite_() {
  const float sp = sqrtf(this->met_vx_ * this->met_vx_ + this->met_vy_ * this->met_vy_);
  const float ux = this->met_vx_ / sp, uy = this->met_vy_ / sp;
  const float ex = -ux * MET_TAIL, ey = -uy * MET_TAIL;
  const int x0 = (int) floorf(std::min(0.0f, ex)) - MET_M, x1 = (int) ceilf(std::max(0.0f, ex)) + MET_M;
  const int y0 = (int) floorf(std::min(0.0f, ey)) - MET_M, y1 = (int) ceilf(std::max(0.0f, ey)) + MET_M;
  const int w = x1 - x0 + 1, h = y1 - y0 + 1;
  FxSprite &s = this->met_spr_;
  if (!fx_sprite_init(s, w, h, true))
    return;
  s.ax = -x0;
  s.ay = -y0;
  static const uint32_t TINT[3] = {0xDCE8FF, 0xD6FFE2, 0xFFEFD6};
  const int v = rnd_(0, 6);
  const uint32_t tint = TINT[v < 4 ? 0 : v - 3];
  const float br = rnd_(85, 101) / 100.0f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const float px = (float) (x + x0), py = (float) (y + y0);
      const float d = sqrtf(px * px + py * py);
      // sb — насколько позади головы, q — как далеко от оси хвоста
      const float sb = -(px * ux + py * uy);
      const float q = sb > 0 ? fabsf(px * uy - py * ux) : d;
      Layered m;
      if (sb < MET_TAIL) {
        const float f = fx_clamp01(sb / MET_TAIL), fade = (1.0f - f) * sqrtf(1.0f - f);
        if (q < 6.0f)
          m.over(0x9DB8FF, 0.4f * expf(-q * q / 5.0f) * sq(1.0f - f));
        m.over(tint, fx_clamp01(1.3f * (1.0f - f) + 0.8f - q) * fade);
      }
      m.over(0xE8F0FF, 0.7f * sq(fx_clamp01(1.0f - d / 6.0f)));
      m.over(0xFFFFFF, fx_clamp01(2.5f - d));
      m.r *= br;
      m.g *= br;
      m.b *= br;
      m.a *= br;
      put_px(s.buf, w * h, y * w + x, m, x, y);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::meteor_(uint32_t now) {
  if (!this->stars_on_) {
    this->next_met_ = 0;
    return;
  }
  if (!this->met_on_) {
    // В ночи звездопадов метеоры летят каждые несколько секунд
    const bool shower = this->hol_ == HOL_METEORS;
    if (this->next_met_ == 0)
      this->next_met_ = now + (shower ? rnd_(1500, 4000) : rnd_(15000, 45000));
    if ((int32_t) (now - this->next_met_) < 0)
      return;
    // Пролетает по верхней части неба наискосок вниз, влево или вправо
    const bool right = rnd_(0, 2);
    const float a = rnd_(12, 33) * (PI_X / 180.0f);
    const float sp = rnd_(32, 43) / 100.0f;  // px/мс
    this->met_vx_ = (right ? sp : -sp) * cosf(a);
    this->met_vy_ = sp * sinf(a);
    this->met_x0_ = right ? rnd_(90, 250) : rnd_(216, 376);
    this->met_y0_ = rnd_(30, 71);
    // Не ниже 150 px — к цифрам времени не подлетает
    this->met_len_ = std::min((uint32_t) rnd_(450, 651), (uint32_t) ((150.0f - this->met_y0_) / this->met_vy_));
    this->met_t0_ = now;
    this->met_on_ = true;
    this->meteor_sprite_();
  }
  const uint32_t t = now - this->met_t0_;
  if (t >= this->met_len_) {
    this->mark_(this->met_spot_, false, 0, 0, 1, 1);
    this->met_on_ = false;
    this->next_met_ = now + (this->hol_ == HOL_METEORS ? rnd_(2000, 6000) : rnd_(30000, 90000));
    return;
  }
  const float sp = sqrtf(this->met_vx_ * this->met_vx_ + this->met_vy_ * this->met_vy_);
  const float tail = std::min(MET_TAIL, sp * t);
  this->met_hx_ = this->met_x0_ + this->met_vx_ * t;
  this->met_hy_ = this->met_y0_ + this->met_vy_ * t;
  this->met_tx_ = this->met_hx_ - this->met_vx_ / sp * tail;
  this->met_ty_ = this->met_hy_ - this->met_vy_ / sp * tail;
  // Вспыхивает за 80 мс и гаснет за последние 150 мс
  float o = 1.0f;
  if (t < 80)
    o = t / 80.0f;
  if (t + 150 > this->met_len_)
    o = std::min(o, (this->met_len_ - t) / 150.0f);
  this->met_opa_ = (lv_opa_t) (255 * o);
  // Рамка — от конца видимого хвоста до головы: в ней же след и рисуется,
  // поэтому сначала хвост вытягивается от места вспышки
  const int x = (int) floorf(std::min(this->met_hx_, this->met_tx_)) - MET_M;
  const int y = (int) floorf(std::min(this->met_hy_, this->met_ty_)) - MET_M;
  const int w = (int) ceilf(std::max(this->met_hx_, this->met_tx_)) + MET_M - x + 1;
  const int h = (int) ceilf(std::max(this->met_hy_, this->met_ty_)) + MET_M - y + 1;
  // Меняется яркость — перерисовать, даже если рамка та же
  if (this->met_spot_.on)
    this->invalidate_(this->met_spot_.a);
  this->mark_(this->met_spot_, true, x, y, w, h);
}

void WeatherFx::meteor_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const Spot &s = this->met_spot_;
  if (!s.on || !this->met_spr_.buf)
    return;
  const lv_area_t box = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  lv_area_t c;
  if (!lv_area_intersect(&c, &clip, &box))
    return;
  // Хвост не длиннее пройденного пути: только в рамке метеора
  const lv_area_t keep = layer->_clip_area;
  layer->_clip_area = c;
  const int x = (int) lroundf(this->met_hx_) + oc.x1, y = (int) lroundf(this->met_hy_) + oc.y1;
  fx_draw(layer, this->met_spr_, fx_sprite_area(this->met_spr_, x, y), this->met_opa_);
  layer->_clip_area = keep;
}

// ---- Воздушный змей ----

// Ромб: от середины вверх 18 px, вниз 24, в стороны по 14. Картинка
// одного наклона, привязка — середина ромба
static const int KB_W = 33, KB_H = 47, KB_AX = 16, KB_AY = 20;
static const int KB_N = 9;  // наклонов: −12°…+12° через 3°
static const float KB_STEP = 3.0f * PI_X / 180.0f;
// Хвост с бантиками — кадры по фазе волны, привязка — нижний угол ромба.
// Нарисован под змея, летящего вправо (хвост тянется влево); летит влево —
// кадры отражены
static const int KT_N = 16;
static const int KT_W = 62, KT_H = 52, KT_AX = 55, KT_AY = 5;
static const uint32_t KITE_C[4] = {0xFF5252, 0xFFD740, 0x40C4FF, 0x69F0AE};

// Ромб из четырёх цветных клиньев с наклоном th: ткань светлее к середине
// и к свету слева сверху, по краю — тёмная кромка, крестовина из тонких
// реек. Четыре точки на пиксель — края гладкие
static void draw_kite(uint8_t *p, float th) {
  const float c = cosf(th), s = sinf(th);
  for (int y = 0; y < KB_H; y++) {
    for (int x = 0; x < KB_W; x++) {
      Layered px;
      for (int q = 0; q < 4; q++) {
        const float sx = x - KB_AX + (q & 1 ? 0.25f : -0.25f), sy = y - KB_AY + (q & 2 ? 0.25f : -0.25f);
        // В осях змея
        const float lx = sx * c + sy * s, ly = -sx * s + sy * c;
        const float hh = ly < 0 ? 18.0f : 24.0f;
        const float e = fabsf(lx) / 14.0f + fabsf(ly) / hh;
        if (e >= 1.0f)
          continue;
        const float edge = (1.0f - e) / sqrtf(1.0f / 196.0f + 1.0f / (hh * hh));
        const uint32_t col = KITE_C[(lx < 0 ? 0 : 1) + (ly < 0 ? 0 : 2)];
        float k = 0.93f + 0.15f * (1.0f - e) + 0.07f * (-lx / 14.0f - ly / hh);
        if (edge < 1.3f)
          k *= 0.78f;
        uint32_t out = ((uint32_t) std::min(255.0f, ((col >> 16) & 0xFF) * k) << 16) |
                       ((uint32_t) std::min(255.0f, ((col >> 8) & 0xFF) * k) << 8) |
                       (uint32_t) std::min(255.0f, (col & 0xFF) * k);
        if (fabsf(lx) < 0.7f || fabsf(ly) < 0.7f)
          out = 0x5D4037;
        Layered one;
        one.over(out, 1.0f);
        px.add(one, 0.25f);
      }
      put_px(p, KB_W * KB_H, y * KB_W + x, px, x, y);
    }
  }
}

// Хвост: нитка через n точек (x, y) и бантики на 3-й, 6-й и последней —
// поперёк нитки, два крылышка и тёмный узелок. Края сглаживаются
// расстоянием до контура
static void draw_tail(uint8_t *p, const float *x, const float *y, int n) {
  const int w = KT_W, h = KT_H;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(p);
  uint8_t *al = p + w * h * 2;
  const uint16_t thread = lv_color_to_u16(lv_color_hex(0xCFD8DC));
  for (int j = 1; j < n; j++) {
    const float x0 = x[j - 1], y0 = y[j - 1], dx = x[j] - x0, dy = y[j] - y0;
    const float l2 = dx * dx + dy * dy + 1e-6f;
    const int bx0 = std::max(0, (int) floorf(std::min(x0, x0 + dx) - 2));
    const int bx1 = std::min(w - 1, (int) ceilf(std::max(x0, x0 + dx) + 2));
    const int by0 = std::max(0, (int) floorf(std::min(y0, y0 + dy) - 2));
    const int by1 = std::min(h - 1, (int) ceilf(std::max(y0, y0 + dy) + 2));
    for (int py = by0; py <= by1; py++) {
      for (int px = bx0; px <= bx1; px++) {
        const float qx = px - x0, qy = py - y0;
        const float t = fx_clamp01((qx * dx + qy * dy) / l2);
        const float ex = qx - t * dx, ey = qy - t * dy;
        const uint8_t v = (uint8_t) (230.0f * fx_clamp01(1.05f - sqrtf(ex * ex + ey * ey)));
        const int i = py * w + px;
        if (v > al[i]) {
          al[i] = v;
          rgb[i] = thread;
        }
      }
    }
  }
  // Бантик: узелок темнее, крылышки к концам светлее
  static const int BOW[3] = {3, 6, -1};
  for (int bi : BOW) {
    const int j = bi < 0 ? n - 1 : bi;
    const float br = 255, bg = bi == 6 ? 215 : 82, bb = bi == 6 ? 64 : 82;
    float sx = x[j] - x[j - 1], sy = y[j] - y[j - 1];
    const float l = sqrtf(sx * sx + sy * sy) + 1e-6f;
    sx /= l;
    sy /= l;
    for (int py = std::max(0, (int) y[j] - 6); py <= std::min(h - 1, (int) y[j] + 6); py++) {
      for (int px = std::max(0, (int) x[j] - 6); px <= std::min(w - 1, (int) x[j] + 6); px++) {
        // u — вдоль крылышек (поперёк нитки), v — вдоль нитки
        const float qx = px - x[j], qy = py - y[j];
        const float u = fabsf(-qx * sy + qy * sx), v = fabsf(qx * sx + qy * sy);
        const float knot = fx_clamp01(1.7f - sqrtf(u * u + v * v));
        const float wing = fx_clamp01(0.5f + std::min(4.4f - u, (0.45f + 0.6f * u - v) * 0.857f));
        const float cov = std::max(knot, wing);
        if (cov <= 0)
          continue;
        const float k = knot >= wing ? 0.72f : 0.8f + 0.2f * u / 4.4f;
        // Поверх нитки: её цвет известен — смешать без разбора RGB565
        const int i = py * w + px;
        Layered m;
        m.over(0xCFD8DC, al[i] / 255.0f);
        m.r = br * k * cov + m.r * (1 - cov);
        m.g = bg * k * cov + m.g * (1 - cov);
        m.b = bb * k * cov + m.b * (1 - cov);
        m.a = cov + m.a * (1 - cov);
        al[i] = 0;
        put_px(p, w * h, i, m, px, py);
      }
    }
  }
}

// Кадр хвоста i: волна бежит от угла ромба к концу и треплет хвост вверх-
// вниз и чуть вбок; у самого угла он почти не качается
static void draw_tail_frame(uint8_t *p, int i, float dir) {
  const float ph = 2 * PI_X * i / KT_N;
  const float ax = dir > 0 ? KT_AX : KT_W - 1 - KT_AX;
  float x[9], y[9];
  for (int j = 0; j < 9; j++) {
    const float amp = std::min(1.0f, j / 3.0f);
    x[j] = ax + dir * (-j * 6.0f + 1.5f * sinf(ph + j * 0.8f + 2.0f) * amp);
    y[j] = KT_AY + j * 4.5f + 5.0f * sinf(ph + j * 0.9f) * amp;
  }
  draw_tail(p, x, y, 9);
}

void WeatherFx::kite_stop_() {
  this->mark_(this->kite_spot_, false, 0, 0, 1, 1);
  this->kite_on_ = false;
  fx_sheet_free(this->kite_sheet_);
  fx_sheet_free(this->kite_tail_);
}

// Кадр набора i, если ещё не нарисован и есть очередь; иначе — прежний
// кадр prev (−1 — ещё ни одного: тогда нарисовать сразу)
template <typename F>
static int kite_pick(FxSheet &s, int i, int prev, int &budget, F draw) {
  if (!fx_sheet_ready(s, i) && (budget > 0 || prev < 0)) {
    if (uint8_t *p = fx_sheet_take(s, i)) {
      draw(p);
      fx_sheet_done(s, i);
      budget--;
    }
  }
  return fx_sheet_ready(s, i) ? i : prev;
}

void WeatherFx::kite_(uint32_t now, float wind) {
  if (!this->kite_mode_)
    return;
  if (!this->kite_on_) {
    if (this->next_kite_ == 0)
      this->next_kite_ = now + (this->kite_mode_ == 2 ? 1000 : rnd_(20000, 60000));
    if ((int32_t) (now - this->next_kite_) < 0)
      return;
    // Летит по ветру, а без ветра — в случайную сторону
    this->kite_dir_ = wind > 0.05f ? 1.0f : (wind < -0.05f ? -1.0f : (rnd_(0, 2) ? 1.0f : -1.0f));
    this->kite_speed_ = (25.0f + std::fabs(wind) * 25.0f + rnd_(0, 10)) / 1000.0f;  // px/мс
    this->kite_x_ = this->kite_dir_ > 0 ? -40.0f : 506.0f;
    this->kite_t0_ = now;
    if (!fx_sheet_init(this->kite_sheet_, KB_N, KB_W, KB_H, true) ||
        !fx_sheet_init(this->kite_tail_, KT_N, KT_W, KT_H, true)) {
      this->kite_stop_();
      this->next_kite_ = now + 60000;
      return;
    }
    this->kite_sheet_.ax = KB_AX;
    this->kite_sheet_.ay = KB_AY;
    this->kite_tail_.ax = this->kite_dir_ > 0 ? KT_AX : KT_W - 1 - KT_AX;
    this->kite_tail_.ay = KT_AY;
    this->kite_frame_ = this->kite_tframe_ = -1;
    this->kite_on_ = true;
  }
  const float t = now - this->kite_t0_;
  this->kite_x_ = (this->kite_dir_ > 0 ? -40.0f : 506.0f) + this->kite_dir_ * this->kite_speed_ * t;
  this->kite_y_ = 72.0f + 16.0f * sinf(t * 0.0013f);
  if (this->kite_x_ < -60.0f || this->kite_x_ > 526.0f) {
    this->kite_stop_();
    this->next_kite_ = now + (this->kite_mode_ == 2 ? rnd_(4000, 8000) : rnd_(180000, 480000));
    return;
  }
  // Змей покачивается: наклон бродит в пределах ±12°, хвост треплется. Ещё
  // не нарисованный кадр — по очереди с остальными картинками, а пока —
  // прежний
  const float th = 0.15f * sinf(t * 0.0021f) + 0.06f * sinf(t * 0.0047f + 1.3f);
  const int fi = std::min(KB_N - 1, std::max(0, (int) lroundf(th / KB_STEP) + KB_N / 2));
  this->kite_frame_ = kite_pick(this->kite_sheet_, fi, this->kite_frame_, this->render_budget_,
                                [fi](uint8_t *p) { draw_kite(p, (fi - KB_N / 2) * KB_STEP); });
  const int ti = (int) (fmodf(t * 0.009f, 2 * PI_X) / (2 * PI_X) * KT_N) % KT_N;
  const float dir = this->kite_dir_;
  this->kite_tframe_ =
      kite_pick(this->kite_tail_, ti, this->kite_tframe_, this->render_budget_, [this, ti, dir](uint8_t *p) {
        draw_tail_frame(p, ti, dir);
        fx_sheet_crop(this->kite_tail_, ti);
      });
  const int cx = (int) lroundf(this->kite_x_), cy = (int) lroundf(this->kite_y_);
  this->kite_cx_ = cx;
  this->kite_cy_ = cy;
  // Хвост привязан к нижнему углу ромба
  const float tq = (this->kite_frame_ - KB_N / 2) * KB_STEP;
  this->kite_tx_ = (int) lroundf(cx - 24.0f * sinf(tq));
  this->kite_ty_ = (int) lroundf(cy + 24.0f * cosf(tq));
  // Рамка: ромб и хвост с бантиками
  const lv_area_t tb = fx_sheet_area(this->kite_tail_, this->kite_tframe_, this->kite_tx_, this->kite_ty_);
  const int x1 = std::min(cx - KB_AX, (int) tb.x1), y1 = cy - KB_AY;
  const int x2 = std::max(cx - KB_AX + KB_W - 1, (int) tb.x2), y2 = std::max(cy - KB_AY + KB_H - 1, (int) tb.y2);
  // Хвост треплется и при той же рамке — перерисовать в любом случае
  if (this->kite_spot_.on)
    this->invalidate_(this->kite_spot_.a);
  this->mark_(this->kite_spot_, true, x1, y1, x2 - x1 + 1, y2 - y1 + 1);
}

void WeatherFx::kite_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const Spot &s = this->kite_spot_;
  if (!s.on)
    return;
  const lv_area_t box = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  if (!fx_hit(box, clip))
    return;
  // Хвост — из-под ромба
  if (fx_sheet_ready(this->kite_tail_, this->kite_tframe_))
    fx_sheet_draw(layer, this->kite_tail_, this->kite_tframe_,
                  fx_sheet_area(this->kite_tail_, this->kite_tframe_, this->kite_tx_ + oc.x1, this->kite_ty_ + oc.y1),
                  LV_OPA_COVER);
  if (fx_sheet_ready(this->kite_sheet_, this->kite_frame_))
    fx_sheet_draw(layer, this->kite_sheet_, this->kite_frame_,
                  fx_sheet_area(this->kite_sheet_, this->kite_frame_, this->kite_cx_ + oc.x1, this->kite_cy_ + oc.y1),
                  LV_OPA_COVER);
}

// ---- Ракета ----

// Летит 4,5 с снизу слева вверх направо, над строкой погоды
static const uint32_t RK_MS = 4500;
static const float PUFF_MS = 900.0f;  // клуб дыма тает
// Картинки корпуса и пламени (привязка — середина ракеты) — точно под
// повёрнутые очертания; перерисовываются, когда курс сменится на 2°
static const float RK_BIN = 2.0f * PI_X / 180.0f;
static const float RK_FLAME[3] = {9.0f, 12.5f, 16.0f};

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
static Layered rocket_at(float lx, float ly) {
  Layered m;
  if (lx < -17.5f || lx > 19.5f || fabsf(ly) > 11.5f)
    return m;
  if (fabsf(ly) > 4.0f && lx < -3.5f)
    m.over(ly > 0 ? 0xB71C1C : 0xE53935, in_tri(lx, fabsf(ly), -12, 4.5f, -4, 4.5f, -17, 11) ? 1.0f : 0.0f);
  if (lx > -14.6f && lx < -11.5f && fabsf(ly) < 2.0f + (lx + 14.6f) * 0.35f)
    m.over(0x455A64, 1.0f);
  const float hw = lx < 9.0f ? 5.0f : 5.0f * sqrtf(fx_clamp01(1.0f - sq((lx - 9.0f) / 10.5f)));
  if (lx > -12.3f && lx < 19.5f && fabsf(ly) < hw) {
    const bool red = lx >= 9.0f || (lx > -8.5f && lx < -6.5f);
    const float u = ly / 5.0f;  // −1 — верх, 1 — низ
    const float k = 0.8f + 0.3f * (1.0f - sq(u + 0.35f)) + 0.25f * sq(fx_clamp01(1.0f - sq((u + 0.5f) / 0.35f)));
    const uint32_t c = red ? 0xE53935 : 0xECEFF1;
    m.over(((uint32_t) std::min(255.0f, ((c >> 16) & 0xFF) * k) << 16) |
               ((uint32_t) std::min(255.0f, ((c >> 8) & 0xFF) * k) << 8) | (uint32_t) std::min(255.0f, (c & 0xFF) * k),
           1.0f);
    const float dw = sqrtf(sq(lx - 2.0f) + ly * ly);
    if (dw < 3.4f)
      m.over(dw < 2.6f ? (ly < -0.5f ? 0x81D4FA : 0x29B6F6) : 0x90A4AE, 1.0f);
    if (sq(lx - 1.2f) + sq(ly + 1.0f) < 0.7f)
      m.over(0xFFFFFF, 1.0f);
  }
  return m;
}

// Пламя длиной fl позади сопла: ядро почти белое, к краю и концу — жёлтое,
// оранжевое, красноватое; вокруг — тёплое сияние. Край сглаживается
// расстоянием до контура — хватает одной точки на пиксель
static Layered flame_at(float lx, float ly, float fl) {
  Layered m;
  const float s = -14.0f - lx;
  const float sc = std::min(std::max(s, 0.0f), fl);
  const float gd = sq(s - sc) + ly * ly;
  if (gd < 30.0f)
    m.over(0xFF9A3C, 0.4f * sq(1.0f - gd / 30.0f) * (1.0f - 0.7f * sc / fl));
  if (s > -0.5f && s < fl) {
    const float t = fx_clamp01(s / fl);
    const float hw = 2.9f * sqrtf(1.0f - t) + 0.2f;
    const float d = fabsf(ly);
    const float cov = fx_clamp01(hw + 0.5f - d);
    if (cov > 0) {
      const float heat = (1.0f - sq(std::min(d / hw, 1.0f))) * (1.0f - t);
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

// Корпус и три длины пламени под курс bin·2°
void WeatherFx::rocket_render_(int bin) {
  const float a = bin * RK_BIN, c = cosf(a), s = sinf(a);
  int bx, by, bw, bh;
  rot_box(-17.5f, 19.5f, -11.5f, 11.5f, c, s, &bx, &by, &bw, &bh);
  FxSprite &b = this->rk_body_;
  if (fx_sprite_init(b, bw, bh, true)) {
    b.ax = -bx;
    b.ay = -by;
    for (int y = 0; y < bh; y++) {
      for (int x = 0; x < bw; x++) {
        Layered px;
        for (int q = 0; q < 4; q++) {
          const float sx = x + bx + (q & 1 ? 0.25f : -0.25f), sy = y + by + (q & 2 ? 0.25f : -0.25f);
          px.add(rocket_at(sx * c + sy * s, -sx * s + sy * c), 0.25f);
        }
        put_px(b.buf, bw * bh, y * bw + x, px, x, y);
      }
    }
    fx_sprite_done(b);
  }
  rot_box(-14.0f - RK_FLAME[2] - 3.5f, -10.5f, -6.0f, 6.0f, c, s, &bx, &by, &bw, &bh);
  for (int k = 0; k < 3; k++) {
    FxSprite &f = this->rk_fl_[k];
    if (!fx_sprite_init(f, bw, bh, true))
      continue;
    f.ax = -bx;
    f.ay = -by;
    for (int y = 0; y < bh; y++) {
      for (int x = 0; x < bw; x++) {
        const float sx = (float) (x + bx), sy = (float) (y + by);
        put_px(f.buf, bw * bh, y * bw + x, flame_at(sx * c + sy * s, -sx * s + sy * c, RK_FLAME[k]), x, y);
      }
    }
    fx_sprite_done(f);
  }
}

void WeatherFx::rocket_apply_(bool on) {
  if (on)
    return;
  // Праздник кончился — убрать ракету
  if (this->rk_on_)
    this->rocket_stop_();
  this->next_rk_ = 0;
}

void WeatherFx::rocket_stop_() {
  this->mark_(this->rk_spot_, false, 0, 0, 1, 1);
  this->rk_on_ = false;
  fx_sprite_free(this->rk_body_);
  for (auto &f : this->rk_fl_)
    fx_sprite_free(f);
  for (auto &p : this->rk_puff_)
    fx_sprite_free(p);
}

void WeatherFx::rocket_(uint32_t now) {
  if (this->hol_ != HOL_COSMOS)
    return;
  if (!this->rk_on_) {
    if (this->next_rk_ == 0)
      this->next_rk_ = now + 2000;
    if ((int32_t) (now - this->next_rk_) < 0)
      return;
    this->rk_on_ = true;
    this->rk_t0_ = now;
    this->rk_puff_ms_ = now;
    this->rk_bin_ = 1000;
    for (auto &a : this->puff_age_)
      a = PUFF_MS;
    for (int i = 0; i < NPUFF_R; i++)
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
  // Путь — плавная дуга от левого края над строкой погоды к верху справа
  const float u = std::min(t / (float) RK_MS, 1.0f);
  const float x = -40.0f + 546.0f * u;
  const float y = 205.0f - 185.0f * u - 45.0f * sinf(PI_X * u);
  const float dx = 546.0f, dy = -185.0f - 45.0f * PI_X * cosf(PI_X * u);
  if (t < RK_MS) {
    this->rk_x_ = x;
    this->rk_y_ = y;
    this->rk_a_ = atan2f(dy, dx);
    this->rk_flame_ = 8.0f + rnd_(0, 9);
    const int bin = (int) lroundf(this->rk_a_ / RK_BIN);
    if (bin != this->rk_bin_) {
      this->rocket_render_(bin);
      this->rk_bin_ = bin;
    }
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
  if (t < RK_MS && this->rk_body_.buf) {
    // Корпус и самое длинное пламя
    const int rx = (int) lroundf(this->rk_x_), ry = (int) lroundf(this->rk_y_);
    const lv_area_t b = fx_sprite_area(this->rk_body_, rx, ry), f = fx_sprite_area(this->rk_fl_[2], rx, ry);
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
  if (this->rk_x_ > -500 && this->rk_body_.buf) {
    const int x = (int) lroundf(this->rk_x_) + oc.x1, y = (int) lroundf(this->rk_y_) + oc.y1;
    const int k = this->rk_flame_ < 10.7f ? 0 : (this->rk_flame_ < 14.2f ? 1 : 2);
    fx_draw(layer, this->rk_fl_[k], fx_sprite_area(this->rk_fl_[k], x, y), LV_OPA_COVER);
    fx_draw(layer, this->rk_body_, fx_sprite_area(this->rk_body_, x, y), LV_OPA_COVER);
  }
}

// ---- Марево ----

// Струйка: волна на 26 px, толще посередине, концы тают. Кадры — по фазе
// волны, привязка — середина
static const int HT_W = 33, HT_H = 11, HT_AX = 16, HT_AY = 5;
static const int HT_N = 24;

static void draw_heat(uint8_t *p, float ph) {
  for (int y = 0; y < HT_H; y++) {
    for (int x = 0; x < HT_W; x++) {
      const float u = (x - 3.0f) / 26.4f;
      if (u < -0.08f || u > 1.08f)
        continue;
      const float ang = ph + u * 5.4f;
      const float yc = HT_AY + 2.0f * sinf(ang), slope = 2.0f * 5.4f / 26.4f * cosf(ang);
      const float d = fabsf(y - yc) / sqrtf(1.0f + slope * slope);
      const float w = 0.55f + 0.8f * sinf(PI_X * fx_clamp01(u));
      const float a = std::max(fx_clamp01(w + 0.5f - d), 0.25f * expf(-d * d / 2.0f)) * fx_smooth(-0.08f, 0.15f, u) *
                      (1.0f - fx_smooth(0.85f, 1.08f, u));
      p[y * HT_W + x] = (uint8_t) std::min(255.0f, std::max(0.0f, a * 255.0f + fx_dither(x, y) * 3.0f));
    }
  }
}

void WeatherFx::heat_apply_(bool on) {
  this->heat_on_ = on;
  for (int i = 0; i < NHEAT; i++) {
    this->hage_[i] = 1.0f - i / (float) NHEAT;  // струйки вразнобой
    this->hx_[i] = rnd_(140, 326);
    this->hy_[i] = 440;
    if (!on)
      this->mark_(this->heat_spot_[i], false, 0, 0, 1, 1);
  }
  if (!on) {
    fx_sheet_free(this->heat_sheet_);
    return;
  }
  if (!this->heat_sheet_.buf && fx_sheet_init(this->heat_sheet_, HT_N, HT_W, HT_H, false)) {
    this->heat_sheet_.ax = HT_AX;
    this->heat_sheet_.ay = HT_AY;
    for (int i = 0; i < HT_N; i++) {
      draw_heat(fx_sheet_take(this->heat_sheet_, i), 2 * PI_X * i / HT_N);
      fx_sheet_done(this->heat_sheet_, i);
    }
  }
}

void WeatherFx::heat_() {
  if (!this->heat_on_)
    return;
  // Струйки поднимаются на ~60 px за 6 с, колышутся и тают; на смену —
  // новые у самого края
  for (int i = 0; i < NHEAT; i++) {
    this->hage_[i] += this->dt_ms_ / 6000.0f;
    if (this->hage_[i] >= 1.0f) {
      this->hage_[i] = 0;
      this->hx_[i] = rnd_(140, 326);
      this->hy_[i] = 440;
    }
    this->hy_[i] -= this->k_ * 0.5f;
    this->hph_[i] = fmodf(this->hph_[i] + this->k_ * 0.35f, 2 * PI_X);
    const int x = (int) lroundf(this->hx_[i]) - HT_AX, y = (int) lroundf(this->hy_[i]) - HT_AY;
    if (this->heat_spot_[i].on)
      this->invalidate_(this->heat_spot_[i].a);
    this->mark_(this->heat_spot_[i], true, x, y, HT_W, HT_H);
  }
}

void WeatherFx::heat_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->heat_on_ || !this->heat_sheet_.buf)
    return;
  const lv_color_t c = lv_color_hex(0xFFC48A);
  for (int i = 0; i < NHEAT; i++) {
    const Spot &s = this->heat_spot_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    const float age = this->hage_[i];
    const float vis = std::min(age * 4.0f, 1.0f) * (1.0f - age);
    const int f = (int) (this->hph_[i] / (2 * PI_X) * HT_N) % HT_N;
    fx_sheet_draw(layer, this->heat_sheet_, f, a, (lv_opa_t) ((this->dim_ ? 170.0f : 135.0f) * vis), c);
  }
}

// ---- Сосульки ----

// Полочка под цифрами часов и минут и сосульки на ней — x, длина и ширина
// у основания. Не ниже строки «Ясно»
static const int ICE_Y = 265;
static const int ICE_SHELF[2][2] = {{74, 218}, {248, 392}};
static const int16_t ICICLES[][3] = {
    {82, 9, 5},   {97, 14, 6},  {113, 7, 4},  {128, 12, 6}, {146, 6, 4},  {161, 13, 5},
    {179, 9, 5},  {196, 11, 6}, {211, 6, 4},  {256, 8, 4},  {271, 13, 6}, {289, 7, 5},
    {305, 12, 5}, {323, 6, 4},  {339, 14, 6}, {357, 8, 5},  {372, 11, 5}, {386, 6, 4},
};
static const int ICE_X0 = 72, ICE_W = 323, ICE_H = 21;

// Ледяная корка с неровным низом и светлым верхом; сосулька i (−1 — нет её
// в этом столбце) сужается к округлому кончику, прозрачнее к концу, слева —
// блик; на каждой третьей висит капля
static Layered ice_at(float X, float Y, uint32_t ice, int i) {
  Layered m;
  for (const auto &sh : ICE_SHELF) {
    if (Y < 0 || Y >= 3.5f || X < sh[0] - 2 || X > sh[1] + 2)
      continue;
    const float th = (2.6f + 0.6f * sinf(X * 0.41f) * sinf(X * 0.13f + 1.0f)) *
                     fx_smooth(sh[0] - 2.0f, sh[0] + 1.5f, X) * (1.0f - fx_smooth(sh[1] - 1.5f, sh[1] + 2.0f, X));
    if (Y >= 0 && Y < th)
      m.over(Y < 1.0f ? lv_color_to_u32(lv_color_mix(lv_color_white(), lv_color_hex(ice), 150)) & 0xFFFFFF : ice,
             0.88f);
  }
  if (i >= 0) {
    const float ix = ICICLES[i][0], len = ICICLES[i][1], wb = ICICLES[i][2] / 2.0f;
    const float v = (Y - 1.5f) / len;
    if (v >= 0 && v < 1) {
      const float hw = wb * powf(1.0f - v, 0.9f) + 0.15f;
      const float dx = X - ix;
      if (fabsf(dx) < hw) {
        m.over(ice, 0.82f - 0.3f * v);
        if (dx < -0.2f * hw && dx > -0.65f * hw && v < 0.8f)
          m.over(0xFFFFFF, 0.75f * (1.0f - v));
      }
    }
    if (i % 3 == 1 && sq(X - ix) + sq(Y - 1.5f - len - 0.9f) < 1.1f)
      m.over(lv_color_to_u32(lv_color_mix(lv_color_white(), lv_color_hex(ice), 128)) & 0xFFFFFF, 0.9f);
  }
  return m;
}

void WeatherFx::icicles_apply_() {
  // Только при часах на экране — под цифрами; при новой яркости — заново
  const bool on = this->frost_on_ && !this->full_;
  if (on && this->ice_spr_.buf && this->ice_dim_ == this->dim_)
    return;
  const lv_area_t a = {ICE_X0, ICE_Y, ICE_X0 + ICE_W - 1, ICE_Y + ICE_H - 1};
  if (!on) {
    if (this->ice_spr_.buf)
      this->invalidate_(a);
    fx_sprite_free(this->ice_spr_);
    return;
  }
  FxSprite &s = this->ice_spr_;
  if (!fx_sprite_init(s, ICE_W, ICE_H, true))
    return;
  s.ax = s.ay = 0;
  this->ice_dim_ = this->dim_;
  const uint32_t ice = this->dim_ ? 0xDDF0FF : 0xA8CCEA;
  // Какая сосулька в каком столбце: они далеко друг от друга, в столбце —
  // не больше одной
  int8_t col[ICE_W];
  memset(col, -1, sizeof(col));
  for (size_t i = 0; i < sizeof(ICICLES) / sizeof(ICICLES[0]); i++) {
    const int ix = ICICLES[i][0] - ICE_X0, r = ICICLES[i][2] / 2 + 2;
    for (int x = std::max(0, ix - r); x <= std::min(ICE_W - 1, ix + r); x++)
      col[x] = (int8_t) i;
  }
  for (int y = 0; y < ICE_H; y++) {
    for (int x = 0; x < ICE_W; x++) {
      Layered px;
      for (int q = 0; q < 4; q++)
        px.add(ice_at(ICE_X0 + x + (q & 1 ? 0.25f : -0.25f), y + (q & 2 ? 0.25f : -0.25f), ice, col[x]), 0.25f);
      put_px(s.buf, ICE_W * ICE_H, y * ICE_W + x, px, x, y);
    }
  }
  fx_sprite_done(s);
  this->invalidate_(a);
}

void WeatherFx::icicles_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->ice_spr_.buf || !this->frost_on_ || this->full_)
    return;
  const lv_area_t a = fx_sprite_area(this->ice_spr_, ICE_X0 + oc.x1, ICE_Y + oc.y1);
  if (fx_hit(a, clip))
    fx_draw(layer, this->ice_spr_, a, LV_OPA_COVER);
}

}  // namespace weather_fx
}  // namespace esphome

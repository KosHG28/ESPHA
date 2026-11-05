// Редкое на небе и по краю: метеор, воздушный змей, марево в жару, зарево
// на восходе и закате, светлячки летними ночами, самолёт ночью.
//
// Всё — готовыми картинками, как остальной погодный фон. Прежде это были
// треугольники, отрезки, круги и градиенты LVGL, которые заново считались на
// каждом участке перерисовки: марево из 36 сглаженных отрезков обходилось
// дороже ливня, змей — вдвое дороже звёздного неба, а градиент зарева в
// 16-битном цвете ложился заметными ступенями.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

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
      FxMix m;
      if (sb < MET_TAIL) {
        const float f = fx_clamp01(sb / MET_TAIL), fade = (1.0f - f) * sqrtf(1.0f - f);
        if (q < 6.0f)
          m.over(0x9DB8FF, 0.4f * expf(-q * q / 5.0f) * fx_sq(1.0f - f));
        m.over(tint, fx_clamp01(1.3f * (1.0f - f) + 0.8f - q) * fade);
      }
      m.over(0xE8F0FF, 0.7f * fx_sq(fx_clamp01(1.0f - d / 6.0f)));
      m.over(0xFFFFFF, fx_clamp01(2.5f - d));
      m.r *= br;
      m.g *= br;
      m.b *= br;
      m.a *= br;
      fx_put(s.buf, w * h, y * w + x, m, x, y);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::meteor_(uint32_t now) {
  // В светлые сумерки метеоров не видно
  if (!this->stars_on_ || this->tw_q_[2] < 8) {
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
    const float a = rnd_(12, 33) * (FX_PI / 180.0f);
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
static const float KB_STEP = 3.0f * FX_PI / 180.0f;
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
      FxMix px;
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
        FxMix one;
        one.over(out, 1.0f);
        px.add(one, 0.25f);
      }
      fx_put(p, KB_W * KB_H, y * KB_W + x, px, x, y);
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
        FxMix m;
        m.over(0xCFD8DC, al[i] / 255.0f);
        m.r = br * k * cov + m.r * (1 - cov);
        m.g = bg * k * cov + m.g * (1 - cov);
        m.b = bb * k * cov + m.b * (1 - cov);
        m.a = cov + m.a * (1 - cov);
        al[i] = 0;
        fx_put(p, w * h, i, m, px, py);
      }
    }
  }
}

// Кадр хвоста i: волна бежит от угла ромба к концу и треплет хвост вверх-
// вниз и чуть вбок; у самого угла он почти не качается
static void draw_tail_frame(uint8_t *p, int i, float dir) {
  const float ph = 2 * FX_PI * i / KT_N;
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
  const int ti = (int) (fmodf(t * 0.009f, 2 * FX_PI) / (2 * FX_PI) * KT_N) % KT_N;
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
      const float w = 0.55f + 0.8f * sinf(FX_PI * fx_clamp01(u));
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
      draw_heat(fx_sheet_take(this->heat_sheet_, i), 2 * FX_PI * i / HT_N);
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
    this->hph_[i] = fmodf(this->hph_[i] + this->k_ * 0.35f, 2 * FX_PI);
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
    const int f = (int) (this->hph_[i] / (2 * FX_PI) * HT_N) % HT_N;
    fx_sheet_draw(layer, this->heat_sheet_, f, a, (lv_opa_t) ((this->dim_ ? 170.0f : 135.0f) * vis), c);
  }
}

// ---- Зарево ----

// Полоса зарева — от этой строки до низа круга. Она одинакова по всей
// ширине, поэтому картинка — узкая полоска, которую LVGL повторяет вдоль
// строки (ширина кратна 4 — так узор сглаживания не рвётся на стыках)
static const int GL_Y0 = 250, GL_H = 466 - GL_Y0, GL_W = 64;
// Цвета: у самого горизонта и выше. Утром — золотистое и персиковое, вечером
// — оранжевое и алое. Без синевы: прежнее розовое утро на этом экране
// выглядело лиловым
static const uint32_t GLOW_LOW[2] = {0xFF7A33, 0xFFC27A}, GLOW_HIGH[2] = {0xFF4E45, 0xFF946B};

// Перерисовать полоску под нынешние силу зарева, время суток и яркость экрана.
// Сила зашита в прозрачность самой картинки, а не задаётся при выводе: тогда
// упорядоченный шум сглаживания попадает ровно на ступени смешивания LVGL
// (32 уровня прозрачности), и переход выходит плавным, без полос
void WeatherFx::glow_apply_() {
  if (this->horizon_ <= 0.01f) {
    fx_sprite_free(this->glow_spr_);
    return;
  }
  if (this->glow_spr_.buf && this->glow_k_ == this->horizon_ && this->glow_dawn_ == this->dawn_ &&
      this->glow_dim_ == this->dim_)
    return;
  FxSprite &s = this->glow_spr_;
  if (!fx_sprite_init(s, GL_W, GL_H, true))
    return;
  s.ax = s.ay = 0;
  this->glow_k_ = this->horizon_;
  this->glow_dawn_ = this->dawn_;
  this->glow_dim_ = this->dim_;
  const int tod = this->dawn_ ? 1 : 0;
  const float amax = this->horizon_ * (this->dim_ ? 150.0f : 110.0f);
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + GL_W * GL_H * 2;
  const uint32_t lo = GLOW_LOW[tod], hi = GLOW_HIGH[tod];
  for (int y = 0; y < GL_H; y++) {
    // 0 — верх полосы, 1 — нижний край: свет плавно набирается к горизонту,
    // а цвет переходит от верхнего к цвету у горизонта
    const float t = (y + 0.5f) / GL_H;
    const float a = amax * powf(t, 1.4f), m = powf(t, 1.2f);
    const float r = ((hi >> 16) & 0xFF) + ((float) ((lo >> 16) & 0xFF) - ((hi >> 16) & 0xFF)) * m;
    const float g = ((hi >> 8) & 0xFF) + ((float) ((lo >> 8) & 0xFF) - ((hi >> 8) & 0xFF)) * m;
    const float b = (hi & 0xFF) + ((float) (lo & 0xFF) - (hi & 0xFF)) * m;
    for (int x = 0; x < GL_W; x++) {
      // И цвет, и прозрачность — с упорядоченным шумом на шаг своей
      // ступени: 5 и 6 бит цвета, 32 уровня смешивания
      const float d = fx_dither(x, y);
      const int r5 = std::min(31, std::max(0, (int) ((r + 4.0f + d * 8.0f) / 8.0f)));
      const int g6 = std::min(63, std::max(0, (int) ((g + 2.0f + d * 4.0f) / 4.0f)));
      const int b5 = std::min(31, std::max(0, (int) ((b + 4.0f + d * 8.0f) / 8.0f)));
      rgb[y * GL_W + x] = (uint16_t) ((r5 << 11) | (g6 << 5) | b5);
      alpha[y * GL_W + x] = (uint8_t) std::min(255.0f, std::max(0.0f, a + d * 8.0f + 0.5f));
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::glow_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (this->horizon_ <= 0.01f || !this->glow_spr_.buf)
    return;
  const lv_area_t band = {oc.x1, oc.y1 + GL_Y0, oc.x1 + 465, oc.y1 + GL_Y0 + GL_H - 1};
  lv_area_t c;
  if (!lv_area_intersect(&c, &band, &clip))
    return;
  // Полоска повторяется по ширине участка — с ближайшего к нему своего начала
  const int32_t x0 = oc.x1 + (c.x1 - oc.x1) / GL_W * GL_W;
  lv_draw_image_dsc_t im;
  lv_draw_image_dsc_init(&im);
  im.src = &this->glow_spr_.dsc;
  im.tile = 1;
  im.image_area = {x0, band.y1, x0 + GL_W - 1, band.y2};
  const lv_area_t a = {x0, band.y1, c.x2, band.y2};
  lv_draw_image(layer, &im, &a);
}

// ---- Светлячки ----

// Светлячок — тёплая жёлто-зелёная точка в мягком ореоле. Три картинки: чем
// ярче вспышка, тем шире ореол — светлячок разгорается и гаснет
static const int FF_N = 3, FF_W = 17, FF_A = 8;
static const float FF_HALO[FF_N] = {5.5f, 7.0f, 8.5f};

static void draw_firefly(FxSprite &s, float halo) {
  if (!fx_sprite_init(s, FF_W, FF_W, true))
    return;
  s.ax = s.ay = FF_A;
  for (int y = 0; y < FF_W; y++) {
    for (int x = 0; x < FF_W; x++) {
      const float d = sqrtf((float) ((x - FF_A) * (x - FF_A) + (y - FF_A) * (y - FF_A)));
      FxMix m;
      const float o = fx_clamp01(1.0f - d / halo);
      m.over(0xB0FF3C, 0.75f * o * sqrtf(o));
      const float g = fx_clamp01(1.0f - d / 3.4f);
      m.over(0xE4FF66, 0.9f * g);
      m.over(0xFAFFC8, fx_clamp01(2.3f - d));
      fx_put(s.buf, FF_W * FF_W, y * FF_W + x, m, x, y);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::fireflies_apply_(bool on) {
  if (on && !this->ff_on_) {
    // Светлячки — над травой внизу круга, у каждого своё место и ритм
    for (int i = 0; i < NFF; i++) {
      int x, y;
      do {
        x = rnd_(60, 406);
        y = rnd_(290, 420);
      } while ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 205 * 205);
      this->ffx_[i] = x;
      this->ffy_[i] = y;
      for (auto &ph : this->ffph_[i])
        ph = rnd_(0, 628) / 100.0f;
    }
    for (int i = 0; i < FF_N; i++)
      draw_firefly(this->ff_spr_[i], FF_HALO[i]);
  }
  if (!on) {
    for (auto &f : this->ff_spot_)
      this->mark_(f, false, 0, 0, 1, 1);
    for (auto &s : this->ff_spr_)
      fx_sprite_free(s);
  }
  this->ff_on_ = on;
}

void WeatherFx::fireflies_(uint32_t now) {
  // Светлячки кружат у своего места и мерцают — раз в ~66 мс
  if (!this->ff_on_ || now - this->ff_ms_ < 60)
    return;
  this->ff_ms_ = now;
  const float t = now / 1000.0f;
  for (int i = 0; i < NFF; i++) {
    const float *ph = this->ffph_[i];
    const int x = (int) (this->ffx_[i] + 18.0f * sinf(t * (0.35f + i * 0.03f) + ph[0]));
    const int y = (int) (this->ffy_[i] + 10.0f * sinf(t * (0.5f + i * 0.02f) + ph[1]));
    // Вспыхивает на пару секунд и надолго гаснет
    const float b = sinf(t * (0.9f + i * 0.07f) + ph[2]);
    const float k = b > 0.3f ? (b - 0.3f) / 0.7f : 0.0f;
    const bool was = this->ff_spot_[i].on;
    if (was && k != this->ffk_[i])
      this->invalidate_(this->ff_spot_[i].a);
    this->ffk_[i] = k;
    this->ffpx_[i] = x;
    this->ffpy_[i] = y;
    this->mark_(this->ff_spot_[i], k > 0.02f, x - FF_A, y - FF_A, FF_W, FF_W);
  }
}

void WeatherFx::fireflies_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->ff_on_)
    return;
  for (int i = 0; i < NFF; i++) {
    const Spot &s = this->ff_spot_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    const float k = this->ffk_[i];
    const FxSprite &spr = this->ff_spr_[k < 0.35f ? 0 : (k < 0.7f ? 1 : 2)];
    fx_draw(layer, spr, a, (lv_opa_t) (255.0f * std::min(1.0f, k * 1.15f)));
  }
}

// ---- Самолёт ----

// Скорость, px/мс
static const float AIR_SPEED = 0.022f;
// Огни: проблесковый маяк — белая точка в ореоле, бортовые — маленькие
// точки с ореолом (красная слева, зелёная справа)
static const int AIR_SW = 13, AIR_NW = 7;

static void draw_light(FxSprite &s, int n, float core, float halo, float k) {
  if (!fx_sprite_init(s, n, n, false))
    return;
  const int h = n / 2;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float d = sqrtf((float) ((x - h) * (x - h) + (y - h) * (y - h)));
      const float v = std::max(fx_clamp01(core + 0.5f - d), k * fx_sq(fx_clamp01(1.0f - d / halo)));
      s.buf[y * n + x] = (uint8_t) std::min(255.0f, std::max(0.0f, v * 255.0f + fx_dither(x, y) * 4.0f));
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::plane_apply_(int mode) {
  this->plane_mode_ = mode;
  this->next_air_ = 0;
  if (!mode) {
    if (this->air_on_) {
      this->mark_(this->air_spot_, false, 0, 0, 1, 1);
      this->air_on_ = false;
    }
    fx_sprite_free(this->air_strobe_);
    fx_sprite_free(this->air_nav_);
  }
}

void WeatherFx::plane_(uint32_t now) {
  if (!this->plane_mode_)
    return;
  if (!this->air_on_) {
    if (this->next_air_ == 0)
      this->next_air_ = now + (this->plane_mode_ == 2 ? 1500 : rnd_(120000, 480000));
    if ((int32_t) (now - this->next_air_) < 0)
      return;
    this->air_on_ = true;
    this->air_t0_ = now;
    this->air_v_ = rnd_(0, 2) ? AIR_SPEED : -AIR_SPEED;
    this->air_y_ = rnd_(45, 140);
    if (!this->air_strobe_.buf) {
      draw_light(this->air_strobe_, AIR_SW, 2.2f, 6.0f, 0.75f);
      draw_light(this->air_nav_, AIR_NW, 1.5f, 3.4f, 0.5f);
    }
  }
  const float t = now - this->air_t0_;
  this->air_x_ = (this->air_v_ > 0 ? -20.0f : 486.0f) + this->air_v_ * t;
  const float y = this->air_y_;
  if (this->air_x_ < -30.0f || this->air_x_ > 496.0f) {
    this->mark_(this->air_spot_, false, 0, 0, 1, 1);
    this->air_on_ = false;
    this->next_air_ = now + (this->plane_mode_ == 2 ? rnd_(3000, 6000) : rnd_(240000, 600000));
    return;
  }
  const int x = (int) this->air_x_, yy = (int) y;
  if (this->air_spot_.on)
    this->invalidate_(this->air_spot_.a);  // мигает — перерисовать
  this->mark_(this->air_spot_, true, x - 10, yy - 6, 21, 13);
}

void WeatherFx::plane_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const Spot &s = this->air_spot_;
  if (!s.on || !this->air_strobe_.buf)
    return;
  const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  if (!fx_hit(a, clip))
    return;
  // Красный и зелёный огни на крыльях, между ними — белая проблесковая
  // вспышка (двойная, раз в 1,2 с)
  const int x = (int) this->air_x_ + oc.x1, y = (int) this->air_y_ + oc.y1;
  const uint32_t t = (millis() - this->air_t0_) % 1200;
  if (t < 70 || (t > 160 && t < 230))
    fx_draw(layer, this->air_strobe_, fx_sprite_area(this->air_strobe_, x, y), this->dim_ ? LV_OPA_COVER : 225);
  fx_draw(layer, this->air_nav_, fx_sprite_area(this->air_nav_, x - 7, y + 1), LV_OPA_COVER, lv_color_hex(0xFF3B30));
  fx_draw(layer, this->air_nav_, fx_sprite_area(this->air_nav_, x + 7, y + 1), LV_OPA_COVER, lv_color_hex(0x30D158));
}

}  // namespace weather_fx
}  // namespace esphome

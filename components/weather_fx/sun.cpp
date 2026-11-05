// Солнце ясным днём: диск с ореолом, венец коротких лучей и — когда часы
// убраны — длинные лучи по всему небу.
//
// Всё рисуется готовыми картинками. Длинные лучи и мягкое сияние неба вокруг
// солнца неподвижны: это одна маска во весь экран, нарисованная под место
// солнца. Венец — маска размером с солнце: на каждом шаге (6 раз в секунду)
// она рисуется заново — лучи медленно поворачиваются и дрожат, как пламя, — и
// перерисовывается только квадрат вокруг солнца. Прежде длинные лучи
// поворачивались сами, и экран целиком перерисовывался четыре раза в секунду.
//
// Солнце идёт по небу (sky.cpp): восходит слева, к полудню поднимается — летом
// выше, зимой ниже, — заходит справа. У горизонта оно тусклое и краснеет.
// Когда солнце уходит от места, под которое нарисована маска длинных лучей,
// на несколько пикселей, она рисуется заново — тоже по частям.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

static const float PI2 = 6.28318531f;

// Солнце, когда его места на небе не узнать (нет времени или места, или
// проверка «Ясный день» ночью), — справа в «шапке» круга, мимо значка двери
// посередине
static const int SUN_X = 300, SUN_Y = 74;
// Без часов (небо на весь экран) — крупнее, справа вверху; от солнца по
// всему небу расходятся длинные лучи
static const int SUN_FX = 318, SUN_FY = 130;
static const float SUN_FK = 1.3f;
// При масштабе 1: радиус диска и ореола вокруг него, откуда начинаются лучи
// венца
static const float DISC_R = 17.0f, HALO_R = 36.0f, COR_R0 = 21.0f;
// Лучи венца — через один длинные и короткие: длина и полуширина у основания
static const int NCOR = 12;
static const float COR_LEN[2] = {17.0f, 11.0f}, COR_W[2] = {2.6f, 1.8f};
// Каждый луч венца дрожит со своей частотой — целым числом колебаний за
// минуту, чтобы через минуту всё сходилось без скачка
static const uint8_t COR_FLICK[NCOR] = {23, 31, 19, 37, 29, 17, 41, 25, 33, 21, 27, 35};
static const float COR_AMP = 0.12f;  // насколько луч удлиняется и укорачивается
static const uint32_t SUN_STEP_MS = 160;
// Маска длинных лучей рисуется по столько строк за кадр
static const int SKY_ROWS = 59;
// Длинные лучи по небу — через 30°: угловая полуширина (рад), длина (px) и
// яркость. Широкие и длинные чередуются с узкими и короткими
static const int NBEAM = 12;
static const float BEAM_A0 = 0.26f, BEAM_STEP = PI2 / NBEAM;
struct Beam {
  float w, len, k;
};
static const Beam BEAMS[NBEAM] = {
    {0.080f, 380, 1.00f}, {0.050f, 260, 0.70f}, {0.085f, 400, 0.95f}, {0.045f, 250, 0.65f},
    {0.080f, 420, 1.00f}, {0.050f, 280, 0.75f}, {0.085f, 380, 0.90f}, {0.045f, 240, 0.65f},
    {0.080f, 360, 0.95f}, {0.050f, 260, 0.70f}, {0.085f, 380, 1.00f}, {0.045f, 250, 0.65f},
};
// Сияние неба вокруг солнца и лучи: наибольшая непрозрачность маски,
// радиус сияния
static const float GLOW_A = 84.0f, GLOW_R = 58.0f, BEAM_A = 72.0f;
// Цвета: лучи и сияние неба — тёплый оранжевый (у тусклого жёлтого на
// RGB565 проступает зелень), венец — золотой, диск — светлый в середине и
// золотой к краю, ореол вокруг диска — оранжевый
static const uint32_t SKY_C = 0xFF8A2A;
static const uint32_t COR_C[2] = {0xFFB300, 0xFFCA28};
// Они же у самого горизонта: солнце там красное
static const uint32_t SKY_LOW_C = 0xFF5230, COR_LOW_C = 0xFF6420;
// Маска длинных лучей рисуется заново, когда солнце ушло от места, под
// которое она нарисована, на столько пикселей
static const int SKY_MOVE = 4;

static lv_color_t mix_hex(uint32_t to, uint32_t from, float k) {
  return lv_color_mix(lv_color_hex(to), lv_color_hex(from), (uint8_t) lroundf(255.0f * fx_clamp01(k)));
}

// Полуразмер квадрата, где рисуется солнце, при масштабе k: венец с самым
// длинным лучом и запасом на сглаживание
static int sun_half(float k) { return (int) ceilf((COR_R0 + COR_LEN[0] * (1 + COR_AMP)) * k + 1.5f) + 1; }

// Диск с ореолом (RGB565A8). Ореол тает к краю, диск чуть темнеет и
// краснеет к краю — как настоящее солнце. red — насколько солнце покраснело
// у горизонта, 0..1
static void draw_core(FxSprite &s, float k, float red) {
  const float rd = DISC_R * k, rh = HALO_R * k;
  const int h = (int) ceilf(rh) + 1, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, true))
    return;
  s.ax = s.ay = h;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + n * n * 2;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = x - h, dy = y - h, d = sqrtf(dx * dx + dy * dy);
      const float cov = fx_clamp01(rd + 0.5f - d);
      const float fall = 1.0f - fx_smooth(rd - 1.0f, rh, d);
      const float ah = std::max(0.0f, 150.0f * fall * fall + fx_dither(x, y) * 8.0f);
      const float a = cov * 255.0f + (1.0f - cov) * ah;
      if (a < 0.5f)
        continue;
      // Диск: от бледно-жёлтой середины к золотому краю; у горизонта — от
      // персиковой к красно-оранжевому
      const float t = fx_clamp01(d / rd);
      const float t2 = t * t, q = red;
      const float dr = 255 - 15 * q * t2, dg = (238 - 60 * t2) * (1 - q) + (176 - 80 * t2) * q,
                  db = (160 - 150 * t2) * (1 - q) + (92 - 56 * t2) * q;
      // Ореол — оранжевый, у горизонта — красноватый
      const float hr = 255, hg = 150 - 60 * q, hb = 20 + 20 * q;
      const float wd = cov * 255.0f / a, wh = 1.0f - wd;
      rgb[y * n + x] = lv_color_to_u16(
          lv_color_make((uint8_t) (dr * wd + hr * wh), (uint8_t) (dg * wd + hg * wh), (uint8_t) (db * wd + hb * wh)));
      alpha[y * n + x] = (uint8_t) std::min(255.0f, a + 0.5f);
    }
  }
  fx_sprite_done(s);
}

// Длинные лучи и сияние неба вокруг солнца в (sx, sy) — маска во весь
// экран, строки [y0, y1): целиком она считается около десятой доли секунды,
// поэтому рисуется частями в нескольких кадрах. Считается только внутри
// круга экрана. init — начать с чистой маски (иначе строки рисуются поверх
// прежних: солнце сдвинулось, а прежние лучи ещё на экране)
static void draw_sky(FxSprite &s, int y0, int y1, int sx, int sy, bool init) {
  if (init && y0 == 0 && !fx_sprite_init(s, 466, 466, false))
    return;
  if (!s.buf)
    return;
  s.ax = s.ay = 0;
  const float r0 = COR_R0 * SUN_FK + 10.0f;
  for (int y = y0; y < y1; y++) {
    const float cy = y + 0.5f - FX_CY;
    const float half = sqrtf(std::max(0.0f, FX_R * FX_R - cy * cy));
    const int x0 = std::max(0, (int) (FX_CX - half)), x1 = std::min(465, (int) (FX_CX + half));
    uint8_t *row = s.buf + y * 466;
    for (int x = x0; x <= x1; x++) {
      const float dx = (float) (x - sx), dy = (float) (y - sy);
      const float r2 = dx * dx + dy * dy, r = sqrtf(r2);
      // Сияние: мягко гаснет, но долго — небо светлее во всю «шапку»
      const float q = 1.0f / (1.0f + r2 / (GLOW_R * GLOW_R));
      float v = GLOW_A * q * sqrtf(q);
      // Ближайший луч и насколько точка в стороне от его оси
      float u = (fx_atan2(dy, dx) - BEAM_A0) / BEAM_STEP;
      int i = (int) floorf(u + 0.5f);
      const float off = (u - i) * BEAM_STEP;
      i = ((i % NBEAM) + NBEAM) % NBEAM;
      const Beam &b = BEAMS[i];
      if (fabsf(off) < b.w && r < b.len) {
        const float c = off / b.w, prof = (1 - c * c) * (1 - c * c);
        const float f = 1.0f - r / b.len;
        v += BEAM_A * b.k * prof * fx_smooth(r0, r0 + 50.0f, r) * f * sqrtf(f);
      }
      v += fx_dither(x, y) * 8.0f;
      row[x] = (uint8_t) std::min(255.0f, std::max(0.0f, v + 0.5f));
    }
  }
  if (y1 >= 466)
    fx_sprite_done(s);
}

// Венец: двенадцать заострённых лучей, сглаженных по краю, к концу тают.
// Длина каждого — со своим дрожанием в момент t (секунды, 0..60)
static void draw_corona(FxSprite &s, float k, float ang, float t) {
  const int h = sun_half(k), n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  const float r0 = COR_R0 * k;
  for (int i = 0; i < NCOR; i++) {
    const int kind = i % 2;
    const float a = ang + i * (PI2 / NCOR), ca = cosf(a), sa = sinf(a);
    const float len = COR_LEN[kind] * k * (1.0f + COR_AMP * sinf(PI2 * COR_FLICK[i] * t / 60.0f + i * 2.4f));
    const float w0 = COR_W[kind] * k;
    // Рамка луча: от основания до кончика, с запасом на ширину
    const float bx0 = h + r0 * ca, by0 = h + r0 * sa, bx1 = h + (r0 + len) * ca, by1 = h + (r0 + len) * sa;
    const int x0 = std::max(0, (int) floorf(std::min(bx0, bx1) - w0 - 2)),
              x1 = std::min(n - 1, (int) ceilf(std::max(bx0, bx1) + w0 + 2));
    const int y0 = std::max(0, (int) floorf(std::min(by0, by1) - w0 - 2)),
              y1 = std::min(n - 1, (int) ceilf(std::max(by0, by1) + w0 + 2));
    for (int y = y0; y <= y1; y++) {
      for (int x = x0; x <= x1; x++) {
        const float px = (float) (x - h), py = (float) (y - h);
        const float across = fabsf(py * ca - px * sa);
        if (across > w0 + 1.0f)
          continue;
        const float along = px * ca + py * sa, tt = (along - r0) / len;
        if (tt < -0.15f || tt > 1.05f)
          continue;
        // Основание шире, к кончику — острие; основание мягко проявляется
        const float hw = w0 * (1.0f - fx_clamp01(tt)) + 0.35f;
        const float cov = fx_clamp01(hw - across + 0.5f);
        const float inten = (1.0f - 0.55f * fx_clamp01(tt) * fx_clamp01(tt)) * fx_smooth(-0.15f, 0.1f, tt);
        const int v = (int) (255.0f * cov * inten + 0.5f);
        uint8_t &p = s.buf[y * n + x];
        if (v > p)
          p = (uint8_t) std::min(v, 255);
      }
    }
  }
  fx_sprite_done(s);
}

// ---------------------------------------------------------------------------

// Где солнце сейчас (середина, относительно слоя), насколько видно (0..1) и
// как покраснело (0…16): по небу дома; если его места не узнать или оно под
// горизонтом (проверка «Ясный день» ночью) — на прежнем месте справа
void WeatherFx::sun_target_(int *x, int *y, float *fade, int *red) const {
  float fx, fy;
  if (this->eph_ok_ && this->dome_(this->sun_path_, false, &fx, &fy)) {
    const float alt = (float) this->sun_path_.alt;
    *x = (int) lroundf(fx);
    *y = (int) lroundf(fy);
    // Встаёт и садится солнце не вдруг: у горизонта оно тусклое и красное
    *fade = fx_smooth(-0.833f, 1.5f, alt);
    *red = (int) lroundf(16.0f * (1.0f - fx_smooth(-0.833f, 10.0f, alt)));
    return;
  }
  *x = this->full_ ? SUN_FX : SUN_X;
  *y = this->full_ ? SUN_FY : SUN_Y;
  *fade = 1.0f;
  *red = 0;
}

// Где солнце и какого размера: в «шапке» или крупнее, когда часы убраны
void WeatherFx::sun_place_(int *x, int *y, float *k) const {
  *x = this->sun_x_;
  *y = this->sun_y_;
  *k = this->full_ ? SUN_FK : 1.0f;
}

void WeatherFx::sun_apply_() {
  const bool on = this->sun_on_;
  const float k = this->full_ ? SUN_FK : 1.0f;
  if (!on) {
    // Солнце зашло — память картинок вернуть
    fx_sprite_free(this->sun_core_);
    fx_sprite_free(this->sun_cor_);
    this->sun_k_ = 0;
    this->sun_core_red_ = -1;
  } else {
    this->sun_target_(&this->sun_x_, &this->sun_y_, &this->sun_fade_, &this->sun_red_);
    if (k != this->sun_k_ || !this->sun_core_.buf || this->sun_core_red_ != this->sun_red_) {
      this->sun_k_ = k;
      this->sun_core_red_ = this->sun_red_;
      draw_core(this->sun_core_, k, this->sun_red_ / 16.0f);
      draw_corona(this->sun_cor_, k, this->sun_ang_, this->sun_t_);
    }
  }
  const int h = sun_half(k);
  this->mark_(this->sun_spot_, on && this->sun_core_.buf, this->sun_x_ - h, this->sun_y_ - h, 2 * h + 1, 2 * h + 1);
  // Длинные лучи — только когда часы убраны: иначе они легли бы на цифры.
  // Маска рисуется по частям в нескольких кадрах (sun_sky_step_) и остаётся,
  // пока светит солнце: часы уходят и возвращаются часто. Если за это время
  // солнце ушло далеко, она рисуется заново, а пока лучей не видно
  const bool sky = on && this->full_;
  if (!on) {
    fx_sprite_free(this->sun_sky_);
    this->sun_sky_rows_ = 0;
  } else if (sky && this->sun_sky_rows_ >= 466 &&
             (std::abs(this->sun_x_ - this->sun_sky_x_) >= SKY_MOVE ||
              std::abs(this->sun_y_ - this->sun_sky_y_) >= SKY_MOVE)) {
    this->sun_sky_rows_ = 0;
  }
  if (sky && this->sun_sky_rows_ == 0) {
    this->sun_sky_x_ = this->sun_x_;
    this->sun_sky_y_ = this->sun_y_;
    this->sun_sky_redo_ = 466;
  }
  this->mark_(this->sun_sky_spot_, sky && this->sun_sky_rows_ >= 466, 0, 0, 466, 466);
  // Прозрачность и краснота длинных лучей — сразу под нынешнее солнце
  if (on)
    this->sun_move_();
  else
    this->sun_sky_q_ = -1;
}

// Очередная часть маски длинных лучей — вместо прочих новых картинок кадра.
// Готова — лучи появляются. Солнце сдвинулось — маска рисуется заново
// поверх прежней, тоже по частям, а потом лучи перерисовываются разом
void WeatherFx::sun_sky_step_() {
  if (!this->sun_on_ || !this->full_ || this->render_budget_ <= 0)
    return;
  if (this->sun_sky_rows_ < 466) {
    this->render_budget_ = 0;
    const int y0 = this->sun_sky_rows_, y1 = std::min(466, y0 + SKY_ROWS);
    draw_sky(this->sun_sky_, y0, y1, this->sun_sky_x_, this->sun_sky_y_, true);
    this->sun_sky_rows_ = this->sun_sky_.buf ? y1 : 0;
    if (this->sun_sky_rows_ >= 466)
      this->mark_(this->sun_sky_spot_, true, 0, 0, 466, 466);
  } else if (this->sun_sky_redo_ < 466) {
    this->render_budget_ = 0;
    const int y0 = this->sun_sky_redo_, y1 = std::min(466, y0 + SKY_ROWS);
    draw_sky(this->sun_sky_, y0, y1, this->sun_sky_x_, this->sun_sky_y_, false);
    this->sun_sky_redo_ = y1;
    if (y1 >= 466) {
      fx_sprite_done(this->sun_sky_);
      this->invalidate_(this->sun_sky_spot_.a);
    }
  }
}

// Солнце идёт по небу: сдвинулось — перенести, покраснело — перерисовать
// диск, ушло от маски длинных лучей — нарисовать её заново. Прозрачность и
// цвет лучей меняются ступенями и с перерисовкой всего неба разом: иначе
// участки, которые перерисовываются чаще, выделялись бы на нём
void WeatherFx::sun_move_() {
  if (!this->sun_on_ || !this->sun_core_.buf)
    return;
  int x, y, red;
  float fade;
  this->sun_target_(&x, &y, &fade, &red);
  this->sun_fade_ = fade;
  this->sun_red_ = red;
  if (red != this->sun_core_red_) {
    this->sun_core_red_ = red;
    draw_core(this->sun_core_, this->sun_k_, red / 16.0f);
    this->invalidate_(this->sun_spot_.a);
  }
  if (x != this->sun_x_ || y != this->sun_y_) {
    this->sun_x_ = x;
    this->sun_y_ = y;
    const int h = sun_half(this->sun_k_);
    this->mark_(this->sun_spot_, true, x - h, y - h, 2 * h + 1, 2 * h + 1);
  }
  if (this->full_ && this->sun_sky_rows_ >= 466 && this->sun_sky_redo_ >= 466 &&
      (std::abs(x - this->sun_sky_x_) >= SKY_MOVE || std::abs(y - this->sun_sky_y_) >= SKY_MOVE)) {
    this->sun_sky_x_ = x;
    this->sun_sky_y_ = y;
    this->sun_sky_redo_ = 0;
  }
  const int q = red * 17 + (int) lroundf(fade * 16.0f);
  if (q != this->sun_sky_q_) {
    if (this->sun_sky_spot_.on && this->sun_sky_q_ >= 0)
      this->invalidate_(this->sun_sky_spot_.a);
    this->sun_sky_q_ = q;
  }
}

void WeatherFx::sun_(uint32_t now) {
  this->sun_move_();
  this->sun_sky_step_();
  if (!this->sun_on_ || !this->sun_spot_.on || now - this->sun_ms_ < SUN_STEP_MS)
    return;
  const uint32_t dt = this->sun_ms_ ? std::min<uint32_t>(now - this->sun_ms_, 1000) : SUN_STEP_MS;
  this->sun_ms_ = now;
  // Венец — оборот за две минуты
  this->sun_ang_ += dt * (PI2 / 120000.0f);
  if (this->sun_ang_ > PI2)
    this->sun_ang_ -= PI2;
  this->sun_t_ += dt / 1000.0f;
  if (this->sun_t_ >= 60.0f)
    this->sun_t_ -= 60.0f;
  draw_corona(this->sun_cor_, this->sun_k_, this->sun_ang_, this->sun_t_);
  this->invalidate_(this->sun_spot_.a);
}

void WeatherFx::sun_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->sun_on_)
    return;
  const bool dim = this->dim_;
  const float red = this->sun_red_ / 16.0f, fade = this->sun_fade_;
  if (this->sun_sky_spot_.on && this->sun_sky_q_ >= 0) {
    // Лучи — ступенями прозрачности и красноты (sun_move_)
    const float sq = (this->sun_sky_q_ % 17) / 16.0f, sr = (this->sun_sky_q_ / 17) / 16.0f;
    const lv_area_t a = {oc.x1, oc.y1, oc.x1 + 465, oc.y1 + 465};
    fx_draw(layer, this->sun_sky_, a, (lv_opa_t) ((dim ? 255.0f : 200.0f) * sq), mix_hex(SKY_LOW_C, SKY_C, sr));
  }
  if (!this->sun_spot_.on)
    return;
  const lv_area_t box = {this->sun_spot_.a.x1 + oc.x1, this->sun_spot_.a.y1 + oc.y1, this->sun_spot_.a.x2 + oc.x1,
                         this->sun_spot_.a.y2 + oc.y1};
  if (!fx_hit(box, clip))
    return;
  const int cx = this->sun_x_ + oc.x1, cy = this->sun_y_ + oc.y1;
  // Венец чуть «дышит» — яркость плавает с периодом 5 с
  const float br = 0.9f + 0.1f * sinf(PI2 * this->sun_t_ / 5.0f);
  fx_draw(layer, this->sun_cor_, fx_sprite_area(this->sun_cor_, cx, cy), (lv_opa_t) (255 * br * fade),
          mix_hex(COR_LOW_C, COR_C[dim ? 1 : 0], red));
  fx_draw(layer, this->sun_core_, fx_sprite_area(this->sun_core_, cx, cy), (lv_opa_t) (255 * fade));
  if (this->hol_ != HOL_MASLENITSA)
    return;
  // Масленица: солнце — румяный блин с весёлым лицом
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  auto disc = [&](int x, int y, int r, uint32_t c, lv_opa_t o) {
    lv_area_t a = {x - r, y - r, x + r, y + r};
    fill.radius = LV_RADIUS_CIRCLE;
    fill.color = lv_color_hex(c);
    fill.opa = (lv_opa_t) (o * fade);
    lv_draw_fill(layer, &fill, &a);
  };
  disc(cx, cy, (int) (DISC_R * this->sun_k_), 0xF2B84B, LV_OPA_COVER);
  static const int8_t SPOTS[5][3] = {{-9, -8, 2}, {10, -6, 2}, {-11, 6, 1}, {9, 9, 2}, {1, -12, 1}};
  for (const auto &sp : SPOTS)
    disc(cx + sp[0], cy + sp[1], sp[2], 0xC98A2E, 200);
  fill.radius = 1;
  fill.opa = (lv_opa_t) (255 * fade);
  fill.color = lv_color_hex(0x5A3A12);
  lv_area_t eye_l = {cx - 7, cy - 5, cx - 5, cy - 3}, eye_r = {cx + 5, cy - 5, cx + 7, cy - 3};
  lv_draw_fill(layer, &fill, &eye_l);
  lv_draw_fill(layer, &fill, &eye_r);
  lv_draw_arc_dsc_t smile;
  lv_draw_arc_dsc_init(&smile);
  smile.center = {cx, cy};
  smile.radius = 9;
  smile.width = 2;
  smile.start_angle = 30;
  smile.end_angle = 150;
  smile.color = lv_color_hex(0x5A3A12);
  smile.opa = (lv_opa_t) (255 * fade);
  lv_draw_arc(layer, &smile);
}

}  // namespace weather_fx
}  // namespace esphome

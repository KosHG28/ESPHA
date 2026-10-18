// Солнце ясным днём: диск с ореолом, венец коротких лучей и — когда часы
// убраны — длинные лучи по всему небу.
//
// Всё рисуется готовыми картинками. Длинные лучи и мягкое сияние неба вокруг
// солнца неподвижны: это одна маска во весь экран, нарисованная, когда
// солнце появилось. Венец — маска размером с солнце: на каждом шаге (6 раз
// в секунду) она рисуется заново — лучи медленно поворачиваются и дрожат,
// как пламя, — и перерисовывается только квадрат вокруг солнца. Прежде
// длинные лучи поворачивались сами, и экран целиком перерисовывался четыре
// раза в секунду.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

static const float PI2 = 6.28318531f;

// Солнце — справа в «шапке» круга, мимо значка двери посередине
static const int SUN_X = 300, SUN_Y = 74;
// Без часов (небо на весь экран) — крупнее, справа вверху, и от него по
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

// Полуразмер квадрата, где рисуется солнце, при масштабе k: венец с самым
// длинным лучом и запасом на сглаживание
static int sun_half(float k) { return (int) ceilf((COR_R0 + COR_LEN[0] * (1 + COR_AMP)) * k + 1.5f) + 1; }

// Диск с ореолом (RGB565A8). Ореол тает к краю, диск чуть темнеет и
// краснеет к краю — как настоящее солнце
static void draw_core(FxSprite &s, float k) {
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
      // Диск: от бледно-жёлтой середины к золотому краю
      const float t = fx_clamp01(d / rd);
      const float t2 = t * t;
      const float dr = 255, dg = 238 - 60 * t2, db = 160 - 150 * t2;
      // Ореол — оранжевый
      const float hr = 255, hg = 150, hb = 20;
      const float wd = cov * 255.0f / a, wh = 1.0f - wd;
      rgb[y * n + x] = lv_color_to_u16(
          lv_color_make((uint8_t) (dr * wd + hr * wh), (uint8_t) (dg * wd + hg * wh), (uint8_t) (db * wd + hb * wh)));
      alpha[y * n + x] = (uint8_t) std::min(255.0f, a + 0.5f);
    }
  }
  fx_sprite_done(s);
}

// Длинные лучи и сияние неба вокруг солнца — маска во весь экран. Считается
// только внутри круга экрана
static void draw_sky(FxSprite &s) {
  if (!fx_sprite_init(s, 466, 466, false))
    return;
  s.ax = s.ay = 0;
  const float r0 = COR_R0 * SUN_FK + 10.0f;
  for (int y = 0; y < 466; y++) {
    const float cy = y + 0.5f - FX_CY;
    const float half = sqrtf(std::max(0.0f, FX_R * FX_R - cy * cy));
    const int x0 = std::max(0, (int) (FX_CX - half)), x1 = std::min(465, (int) (FX_CX + half));
    uint8_t *row = s.buf + y * 466;
    for (int x = x0; x <= x1; x++) {
      const float dx = x - SUN_FX, dy = y - SUN_FY;
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

void WeatherFx::sun_apply_() {
  const bool on = this->sun_on_;
  const float k = this->full_ ? SUN_FK : 1.0f;
  if (!on) {
    // Солнце зашло — память картинок вернуть
    fx_sprite_free(this->sun_core_);
    fx_sprite_free(this->sun_cor_);
    this->sun_k_ = 0;
  } else if (k != this->sun_k_ || !this->sun_core_.buf) {
    this->sun_k_ = k;
    draw_core(this->sun_core_, k);
    draw_corona(this->sun_cor_, k, this->sun_ang_, this->sun_t_);
  }
  const int x = this->full_ ? SUN_FX : SUN_X, y = this->full_ ? SUN_FY : SUN_Y, h = sun_half(k);
  this->mark_(this->sun_spot_, on && this->sun_core_.buf, x - h, y - h, 2 * h + 1, 2 * h + 1);
  // Длинные лучи — только когда часы убраны: иначе они легли бы на цифры.
  // Маска рисуется один раз (на плате это около десятой доли секунды) и
  // остаётся, пока светит солнце: часы уходят и возвращаются часто
  const bool sky = on && this->full_;
  if (sky && !this->sun_sky_.buf)
    draw_sky(this->sun_sky_);
  else if (!on)
    fx_sprite_free(this->sun_sky_);
  this->mark_(this->sun_sky_spot_, sky && this->sun_sky_.buf, 0, 0, 466, 466);
}

void WeatherFx::sun_(uint32_t now) {
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
  if (this->sun_sky_spot_.on) {
    const lv_area_t a = {oc.x1, oc.y1, oc.x1 + 465, oc.y1 + 465};
    fx_draw(layer, this->sun_sky_, a, dim ? LV_OPA_COVER : 200, lv_color_hex(SKY_C));
  }
  if (!this->sun_spot_.on)
    return;
  const lv_area_t box = {this->sun_spot_.a.x1 + oc.x1, this->sun_spot_.a.y1 + oc.y1, this->sun_spot_.a.x2 + oc.x1,
                         this->sun_spot_.a.y2 + oc.y1};
  if (!fx_hit(box, clip))
    return;
  const int cx = (this->full_ ? SUN_FX : SUN_X) + oc.x1, cy = (this->full_ ? SUN_FY : SUN_Y) + oc.y1;
  // Венец чуть «дышит» — яркость плавает с периодом 5 с
  const float br = 0.9f + 0.1f * sinf(PI2 * this->sun_t_ / 5.0f);
  fx_draw(layer, this->sun_cor_, fx_sprite_area(this->sun_cor_, cx, cy), (lv_opa_t) (255 * br),
          lv_color_hex(COR_C[dim ? 1 : 0]));
  fx_draw(layer, this->sun_core_, fx_sprite_area(this->sun_core_, cx, cy), LV_OPA_COVER);
  if (this->hol_ != HOL_MASLENITSA)
    return;
  // Масленица: солнце — румяный блин с весёлым лицом
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  auto disc = [&](int x, int y, int r, uint32_t c, lv_opa_t o) {
    lv_area_t a = {x - r, y - r, x + r, y + r};
    fill.radius = LV_RADIUS_CIRCLE;
    fill.color = lv_color_hex(c);
    fill.opa = o;
    lv_draw_fill(layer, &fill, &a);
  };
  disc(cx, cy, (int) (DISC_R * this->sun_k_), 0xF2B84B, LV_OPA_COVER);
  static const int8_t SPOTS[5][3] = {{-9, -8, 2}, {10, -6, 2}, {-11, 6, 1}, {9, 9, 2}, {1, -12, 1}};
  for (const auto &sp : SPOTS)
    disc(cx + sp[0], cy + sp[1], sp[2], 0xC98A2E, 200);
  fill.radius = 1;
  fill.opa = LV_OPA_COVER;
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
  smile.opa = LV_OPA_COVER;
  lv_draw_arc(layer, &smile);
}

}  // namespace weather_fx
}  // namespace esphome

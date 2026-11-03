// Луна в текущей фазе.
//
// Луна — готовая картинка RGB565A8, которая перерисовывается, только когда
// заметно сменилась фаза (раз в пару часов), размер (полнолуние,
// суперлуние), краснота затмения или яркость экрана. Освещённость каждой
// точки диска считается как у шара: граница света и тени — половина эллипса,
// поэтому в первую четверть край прямой, а между четвертью и полнолунием
// освещённая часть выпуклая. Тёмные «моря» лежат на своих местах. Тёмная
// часть диска чуть светится пепельным светом — сильнее всего у тонкого
// серпа, как в жизни; вокруг — ореол со стороны освещённого края, у полной
// луны — кругом.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

// Моря видимой стороны Луны (северное полушарие, луна «прямо»): середина
// (x вправо, y вниз, радиус диска — 1), полуоси и насколько темнее
struct Mare {
  float x, y, rx, ry, k;
};
static const Mare MARIA[] = {
    {-0.58f, 0.00f, 0.32f, 0.58f, 0.15f},   // Океан Бурь
    {-0.28f, -0.40f, 0.28f, 0.24f, 0.18f},  // Море Дождей
    {0.18f, -0.36f, 0.17f, 0.17f, 0.18f},   // Море Ясности
    {0.36f, -0.08f, 0.22f, 0.19f, 0.17f},   // Море Спокойствия
    {0.70f, -0.30f, 0.12f, 0.14f, 0.18f},   // Море Кризисов
    {0.58f, 0.18f, 0.14f, 0.19f, 0.14f},    // Море Изобилия
    {0.36f, 0.30f, 0.10f, 0.10f, 0.13f},    // Море Нектара
    {-0.18f, 0.38f, 0.20f, 0.15f, 0.13f},   // Море Облаков
    {-0.02f, -0.70f, 0.42f, 0.08f, 0.11f},  // Море Холода
};
static const float MOON_GLOW = 8.0f;  // ореол за краем диска, px

void WeatherFx::moon_sprite_() {
  const int r = this->moon_r_;
  const int h = r + (int) MOON_GLOW, n = 2 * h + 1;
  FxSprite &s = this->moon_spr_;
  if (!fx_sprite_init(s, n, n, true))
    return;
  s.ax = s.ay = h;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + n * n * 2;
  const bool dim = this->dim_;
  // Цвет освещённой части: в затмение — медно-красный и темнее
  const lv_color_t lit_c =
      lv_color_mix(lv_color_hex(0xB5482B), lv_color_hex(dim ? 0xFFF8E1 : 0xE8E0C4), (uint8_t) (255 * this->moon_red_));
  const float dark = 1.0f - 0.45f * this->moon_red_;
  // Фаза: 0 — новолуние, 0,5 — полнолуние. Направление на Солнце
  // (x вправо, z к нам): растущая освещена справа, убывающая — слева
  const float phi = 6.2831853f * this->moon_phase_;
  const float sx = sinf(phi), sz = -cosf(phi);
  const float illum = (1.0f - cosf(phi)) * 0.5f;          // освещённая доля диска
  const float earth = 0.11f * (1.0f + cosf(phi)) * 0.5f;  // пепельный свет
  // Ореол: тёплый белый, в затмение — тусклый красноватый
  const float glow_a = (dim ? 95.0f : 70.0f) * (0.4f + 0.6f * illum) * (1.0f - 0.6f * this->moon_red_);
  const lv_color_t glow_c =
      lv_color_mix(lv_color_hex(0xC0603A), lv_color_hex(0xFFF3C4), (uint8_t) (255 * this->moon_red_));
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = (float) (x - h), dy = (float) (y - h), d = sqrtf(dx * dx + dy * dy);
      const float cov = fx_clamp01(r + 0.5f - d);
      // Ореол — от освещённого края: у серпа только с его стороны, у полной
      // луны — кругом
      const float side = fx_smooth(-0.1f, 0.6f, (d > 0 ? dx / d : 0.0f) * sx);
      const float g = (1.0f - fx_smooth(r - 1.0f, r + MOON_GLOW, d)) * (illum * illum + (1 - illum * illum) * side);
      const float ga = std::max(0.0f, glow_a * g * g + fx_dither(x, y) * 6.0f);
      const float a = cov * 255.0f + (1.0f - cov) * ga;
      if (a < 0.5f)
        continue;
      float cr = glow_c.red, cg = glow_c.green, cb = glow_c.blue;
      if (cov > 0) {
        // Точка шара под этим пикселем и её освещённость
        const float u = std::min(1.0f, dx / r), v = std::min(1.0f, dy / r);
        const float z = sqrtf(std::max(0.0f, 1.0f - u * u - v * v));
        const float l = u * sx + z * sz;
        const float lit = fx_smooth(-0.04f, 0.07f, l);
        // Яркость лунного грунта (закон Ломмеля — Зелигера): полная луна
        // светится ровно, к границе света и тени темнеет — солнце там у
        // горизонта, — а у освещённого края ярче
        const float ls = std::max(0.0f, l);
        const float bright = lit * std::min(1.12f, 2.0f * ls / (ls + z + 0.03f));
        float alb = 1.0f;
        for (const Mare &m : MARIA) {
          const float q = (u - m.x) * (u - m.x) / (m.rx * m.rx) + (v - m.y) * (v - m.y) / (m.ry * m.ry);
          alb -= m.k * (1.0f - fx_smooth(0.0f, 1.3f, q));
        }
        alb = std::max(0.5f, alb);
        const float k = bright * alb * dark;
        const float e = earth * alb * (1.0f - lit);
        const float mr = lit_c.red * k + 120.0f * e, mg = lit_c.green * k + 134.0f * e,
                    mb = lit_c.blue * k + 160.0f * e;
        // Край диска: смесь с ореолом по доле, которую закрывает диск
        const float wd = cov * 255.0f / a, wg = 1.0f - wd;
        cr = mr * wd + cr * wg;
        cg = mg * wd + cg * wg;
        cb = mb * wd + cb * wg;
      }
      rgb[y * n + x] = lv_color_to_u16(lv_color_make((uint8_t) std::min(255.0f, cr), (uint8_t) std::min(255.0f, cg),
                                                     (uint8_t) std::min(255.0f, cb)));
      alpha[y * n + x] = (uint8_t) std::min(255.0f, a + 0.5f);
    }
  }
  fx_sprite_done(s);
  this->moon_dim_ = dim;
}

void WeatherFx::moon_paint_(lv_layer_t *layer, int cx, int cy) {
  if (this->moon_spr_.buf)
    fx_draw(layer, this->moon_spr_, fx_sprite_area(this->moon_spr_, cx, cy), LV_OPA_COVER);
}

void WeatherFx::moon_(uint32_t utc, int force) {
  if (!this->stars_on_ || (utc < 1600000000u && !force))
    return;
  // Полнолуние, суперлуние, затмение — по небу раз в минуту или по выбору
  // «Показать праздник»
  int ev = this->moon_ev_;
  float red = this->moon_red_;
  const int64_t minute = utc / 60;
  if (force) {
    ev = force;
    red = force == HOL_ECLIPSE ? 1.0f : 0.0f;
  } else if (minute != this->moon_ev_min_) {
    this->moon_ev_min_ = minute;
    ev = moon_event(utc, &red);
  }
  const int r_moon = ev == HOL_SUPERMOON ? 24 : (ev == HOL_FULL_MOON || ev == HOL_ECLIPSE ? 20 : MOON_R);
  const bool changed = ev != this->moon_ev_ || r_moon != this->moon_r_ || std::fabs(red - this->moon_red_) > 0.02f;
  // Фаза по среднему синодическому месяцу от новолуния 6 января 2000 года.
  // Точность — несколько часов, на рисунке 30 px это не видно. В полнолуние
  // по выбору — ровно полная
  double ph = 0.5;
  if (!force) {
    const double days = (utc - 947182440.0) / 86400.0;
    ph = fmod(days / 29.530588853, 1.0);
    if (ph < 0)
      ph += 1.0;
    if (ev)
      ph = 0.5;
  }
  // Перерисовывать, только когда фаза заметно сменилась (раз в пару часов)
  // или сменилась яркость экрана
  if (this->moon_spot_.on && !changed && std::fabs(ph - this->moon_phase_) < 0.003 && this->dim_ == this->moon_dim_)
    return;
  if (this->moon_spot_.on)
    this->invalidate_(this->moon_spot_.a);
  this->moon_ev_ = ev;
  this->moon_red_ = red;
  this->moon_r_ = r_moon;
  this->moon_phase_ = (float) ph;
  this->moon_sprite_();
  const int r = r_moon + 8;
  this->mark_(this->moon_spot_, true, MOON_X - r, MOON_Y - r, 2 * r + 1, 2 * r + 1);
}

}  // namespace weather_fx
}  // namespace esphome

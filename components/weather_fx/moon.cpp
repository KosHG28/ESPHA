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
//
// Луна стоит на своём месте в небе: идёт по той же дуге, что и солнце
// (sky.cpp), и видна, только пока над горизонтом. У горизонта она тусклее и
// желтеет. Днём, если не слишком тонкая и не рядом с солнцем, — бледная, без
// ореола и пепельного света, как в жизни на голубом небе.
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
  const bool dim = this->dim_, day = this->moon_day_;
  // Цвет освещённой части: в затмение — медно-красный и темнее, у горизонта
  // — желтее, днём — бледный голубовато-белый
  lv_color_t lit_c =
      lv_color_mix(lv_color_hex(0xB5482B), lv_color_hex(dim ? 0xFFF8E1 : 0xE8E0C4), (uint8_t) (255 * this->moon_red_));
  if (this->moon_low_)
    lit_c = lv_color_mix(lv_color_hex(0xFFC27A), lit_c, (uint8_t) (this->moon_low_ * 140 / 8));
  if (day)
    lit_c = lv_color_hex(dim ? 0xE4ECF8 : 0xC4D0E2);
  const float dark = 1.0f - 0.45f * this->moon_red_;
  // Фаза: 0 — новолуние, 0,5 — полнолуние. Направление на Солнце
  // (x вправо, z к нам): растущая освещена справа, убывающая — слева
  const float phi = 6.2831853f * this->moon_phase_;
  const float sx = sinf(phi), sz = -cosf(phi);
  const float illum = (1.0f - cosf(phi)) * 0.5f;  // освещённая доля диска
  // Пепельный свет; днём его не видно, как и ореола
  const float earth = day ? 0.0f : 0.11f * (1.0f + cosf(phi)) * 0.5f;
  // Ореол: тёплый белый, в затмение — тусклый красноватый
  const float glow_a = day ? 0.0f : (dim ? 95.0f : 70.0f) * (0.4f + 0.6f * illum) * (1.0f - 0.6f * this->moon_red_);
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
        if (day) {
          // Днём видна только освещённая часть, сквозь моря «просвечивает»
          // небо: они бледнее
          const float v = std::min(1.0f, bright);
          const float o = cov * v * 255.0f;
          if (o < 0.5f)
            continue;
          const float k = 0.62f + 0.38f * (alb - 0.5f) * 2.0f;
          rgb[y * n + x] = lv_color_to_u16(lv_color_make((uint8_t) (lit_c.red * k), (uint8_t) (lit_c.green * k),
                                                         (uint8_t) std::min(255.0f, lit_c.blue * (0.2f + 0.8f * k))));
          alpha[y * n + x] = (uint8_t) std::min(255.0f, o * (0.7f + 0.3f * alb) + 0.5f);
          continue;
        }
        const float k = bright * alb * dark;
        const float e = earth * alb * (1.0f - lit);
        const float mr = lit_c.red * k + 120.0f * e, mg = lit_c.green * k + 134.0f * e,
                    mb = lit_c.blue * k + 160.0f * e;
        // Край диска: смесь с ореолом по доле, которую закрывает диск
        const float wd = cov * 255.0f / a, wg = 1.0f - wd;
        cr = mr * wd + cr * wg;
        cg = mg * wd + cg * wg;
        cb = mb * wd + cb * wg;
      } else if (day) {
        continue;
      }
      rgb[y * n + x] = lv_color_to_u16(lv_color_make((uint8_t) std::min(255.0f, cr), (uint8_t) std::min(255.0f, cg),
                                                     (uint8_t) std::min(255.0f, cb)));
      alpha[y * n + x] = (uint8_t) std::min(255.0f, a + 0.5f);
    }
  }
  fx_sprite_done(s);
  this->moon_dim_ = dim;
  this->moon_day_drawn_ = day;
  this->moon_low_drawn_ = this->moon_low_;
}

void WeatherFx::moon_paint_(lv_layer_t *layer, int cx, int cy) {
  // Днём луна бледная — полупрозрачная; у горизонта — тусклее
  const float o = (this->moon_day_ ? (this->dim_ ? 200.0f : 170.0f) : 255.0f) * this->moon_fade_;
  if (this->moon_spr_.buf)
    fx_draw(layer, this->moon_spr_, fx_sprite_area(this->moon_spr_, cx, cy), (lv_opa_t) o);
}

// Луны не видно: убрать её и всё, что ей уступало место
void WeatherFx::moon_hide_() {
  if (!this->moon_spot_.on && this->moon_phase_ < 0)
    return;
  this->mark_(this->moon_spot_, false, 0, 0, 1, 1);
  this->moon_phase_ = -1;
  fx_sprite_free(this->moon_spr_);
  this->stars_show_();
}

void WeatherFx::moon_(const Params &p) {
  // Ночью луна — вместе со звёздами, днём, бледная, — вместе с солнцем
  const bool night = this->stars_on_, day = !night && this->sun_on_;
  const int force = p.moon_force;
  const uint32_t utc = p.utc;
  if (!night && !day) {
    this->moon_hide_();
    return;
  }
  if (utc < 1600000000u && !force)
    return;
  // Где Луна на небе дома. Под горизонтом её не видно; проверке полнолуния
  // или гало («Показать праздник», «Фон погоды: Гало») она нужна всё равно —
  // тогда на прежнем месте слева
  float fx = MOON_X, fy = MOON_Y, fade = 1.0f;
  int low = 0;
  bool up = false;
  if (this->eph_ok_) {
    up = this->dome_(this->moon_path_, true, &fx, &fy);
    if (up) {
      const float alt = (float) this->moon_path_.alt;
      fade = fx_smooth(0.125f, 2.5f, alt);
      low = (int) lroundf(8.0f * (1.0f - fx_smooth(0.125f, 9.0f, alt)));
    }
    // Днём тонкий серп и луну рядом с солнцем не видно
    if (up && day) {
      const int h = MOON_R + 8;
      const lv_area_t &s = this->sun_spot_.a;
      const bool near =
          this->sun_spot_.on && fx + h + 6 >= s.x1 && fx - h - 6 <= s.x2 && fy + h + 6 >= s.y1 && fy - h - 6 <= s.y2;
      up = this->moon_lit_ >= 0.15f && !near;
    }
  }
  if (!up && night && (force || this->halo_mode_ == 3)) {
    up = true;
    fx = MOON_X;
    fy = MOON_Y;
    fade = 1.0f;
    low = 0;
  }
  // На Рождество Вифлеемская звезда — на своём месте, луна ей не мешает
  if (up && this->bstar_on_ && this->bstar_spot_.on) {
    const lv_area_t &b = this->bstar_spot_.a;
    const int h = MOON_R + 8;
    up = !(fx + h >= b.x1 && fx - h <= b.x2 && fy + h >= b.y1 && fy - h <= b.y2);
  }
  if (!up) {
    this->moon_hide_();
    return;
  }
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
  // Фаза — по долготам Луны и Солнца; без места — по среднему синодическому
  // месяцу от новолуния 6 января 2000 года (точность — несколько часов, на
  // рисунке 30 px это не видно). В полнолуние и по выбору — ровно полная
  double ph = 0.5;
  if (!force && !ev) {
    if (this->eph_ok_) {
      ph = this->moon_ph_now_;
    } else {
      const double days = (utc - 947182440.0) / 86400.0;
      ph = fmod(days / 29.530588853, 1.0);
      if (ph < 0)
        ph += 1.0;
    }
  }
  // Днём луна не желтеет: она бледная
  if (day)
    low = 0;
  const int x = (int) lroundf(fx), y = (int) lroundf(fy);
  const bool moved = !this->moon_spot_.on || x != this->moon_x_ || y != this->moon_y_;
  const bool look = day != this->moon_day_drawn_ || low != this->moon_low_drawn_;
  const bool faded = std::fabs(fade - this->moon_fade_) > 0.02f;
  // Перерисовывать, только когда фаза заметно сменилась (раз в пару часов),
  // луна сдвинулась, сменился её вид или яркость экрана
  if (!moved && !changed && !look && !faded && std::fabs(ph - this->moon_phase_) < 0.003 &&
      this->dim_ == this->moon_dim_)
    return;
  if (this->moon_spot_.on)
    this->invalidate_(this->moon_spot_.a);
  this->moon_fade_ = fade;
  this->moon_day_ = day;
  this->moon_low_ = low;
  if (changed || look || std::fabs(ph - this->moon_phase_) >= 0.003 || this->dim_ != this->moon_dim_ ||
      !this->moon_spr_.buf || r_moon != this->moon_r_) {
    this->moon_ev_ = ev;
    this->moon_red_ = red;
    this->moon_r_ = r_moon;
    this->moon_phase_ = (float) ph;
    this->moon_sprite_();
  }
  this->moon_x_ = x;
  this->moon_y_ = y;
  const int r = r_moon + 8;
  this->mark_(this->moon_spot_, true, x - r, y - r, 2 * r + 1, 2 * r + 1);
  // Звёзды за луной гаснут, вышедшие из-за неё — зажигаются
  if (moved)
    this->stars_show_();
}

}  // namespace weather_fx
}  // namespace esphome

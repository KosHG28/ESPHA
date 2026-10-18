// Радуга: днём после дождя или пунктом «Радуга».
//
// Готовая картинка RGB565A8: плавный спектр от красного снаружи до
// фиолетового внутри, без полос, мягкие края, концы дуги тают. Над ней —
// бледная вторая радуга с обратным порядком цветов, под дугой небо чуть
// светлее — как в жизни. Появляется и исчезает плавно, за три секунды:
// картинка одна, меняется только непрозрачность. Прежде семь дуг LVGL
// заново пересчитывались на каждом участке, который их задевал.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

// Середина дуги, внешний радиус и ширина основной радуги; вторая — выше
static const float RB_CX = 233.0f, RB_CY = 190.0f, RB_R = 124.0f, RB_W = 30.0f;
static const float RB2_R = 150.0f, RB2_W = 14.0f;
// Картинка — прямоугольник вокруг верхней части дуг
static const int RB_X0 = 80, RB_Y0 = 38, RB_X1 = 386, RB_Y1 = 156;
static const uint32_t RB_FADE_MS = 3000;  // проявляется и тает

// Цвет радуги в доле t поперёк дуги: 0 — красный край, 1 — фиолетовый
static void spectrum(float t, float *r, float *g, float *b) {
  // Опорные цвета: красный, оранжевый, жёлтый, зелёный, голубой, синий,
  // фиолетовый — между ними плавно
  static const float C[7][3] = {{255, 52, 40},  {255, 140, 20}, {255, 220, 40}, {60, 210, 90},
                                {50, 170, 235}, {70, 90, 230},  {150, 60, 210}};
  const float x = fx_clamp01(t) * 6.0f;
  const int i = std::min(5, (int) x);
  const float f = x - i;
  *r = C[i][0] + (C[i + 1][0] - C[i][0]) * f;
  *g = C[i][1] + (C[i + 1][1] - C[i][1]) * f;
  *b = C[i][2] + (C[i + 1][2] - C[i][2]) * f;
}

void WeatherFx::rainbow_sprite_() {
  const int w = RB_X1 - RB_X0 + 1, h = RB_Y1 - RB_Y0 + 1;
  FxSprite &s = this->rb_spr_;
  if (!fx_sprite_init(s, w, h, true))
    return;
  s.ax = s.ay = 0;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + w * h * 2;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const float dx = RB_X0 + x + 0.5f - RB_CX, dy = RB_Y0 + y + 0.5f - RB_CY;
      if (dy >= 0)
        continue;
      const float d = sqrtf(dx * dx + dy * dy);
      // Концы дуги тают: угол от вертикали
      const float ang = fabsf(fx_atan2(dx, -dy));
      const float ends = 1.0f - fx_smooth(0.95f, 1.30f, ang);
      if (ends <= 0)
        continue;
      float a = 0, r = 0, g = 0, b = 0;
      // Основная радуга: поперёк — плавный спектр, края мягкие
      const float t = (RB_R - d) / RB_W;
      if (t > -0.1f && t < 1.1f) {
        spectrum(t, &r, &g, &b);
        a = 0.85f * fx_smooth(-0.1f, 0.12f, t) * (1.0f - fx_smooth(0.82f, 1.1f, t));
      }
      // Вторая радуга: бледнее, цвета в обратном порядке
      const float t2 = (d - (RB2_R - RB2_W)) / RB2_W;
      if (a <= 0 && t2 > -0.1f && t2 < 1.1f) {
        spectrum(t2, &r, &g, &b);
        a = 0.28f * fx_smooth(-0.1f, 0.2f, t2) * (1.0f - fx_smooth(0.8f, 1.1f, t2));
      }
      // Под дугой небо чуть светлее
      if (a <= 0 && d < RB_R - RB_W) {
        r = g = b = 255;
        a = 0.06f * fx_smooth(RB_R - RB_W - 50.0f, RB_R - RB_W, d);
      }
      a *= ends;
      if (a <= 0.002f)
        continue;
      rgb[y * w + x] = lv_color_to_u16(lv_color_make((uint8_t) r, (uint8_t) g, (uint8_t) b));
      alpha[y * w + x] = (uint8_t) std::min(255.0f, std::max(0.0f, a * 255.0f + fx_dither(x, y) * 5.0f));
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::rainbow_(uint32_t now) {
  // Яркость идёт к цели ступенями по 1/12 — каждая ступень перерисовывает
  // прямоугольник радуги
  const float target = this->rainbow_on_ ? 1.0f : 0.0f;
  if (this->rb_level_ == target || !this->rb_ms_) {
    this->rb_ms_ = now;
    return;
  }
  const float step = (now - this->rb_ms_) / (float) RB_FADE_MS;
  if (step < 1.0f / 12.0f)
    return;
  this->rb_ms_ = now;
  if (!this->rb_spr_.buf)
    this->rainbow_sprite_();
  this->rb_level_ =
      target > this->rb_level_ ? std::min(target, this->rb_level_ + step) : std::max(target, this->rb_level_ - step);
  const lv_area_t a = {RB_X0, RB_Y0, RB_X1, RB_Y1};
  this->invalidate_(a);
  if (this->rb_level_ <= 0) {
    fx_sprite_free(this->rb_spr_);
    this->update_root_();
  }
}

void WeatherFx::rainbow_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (this->rb_level_ <= 0 || !this->rb_spr_.buf)
    return;
  const lv_area_t a = {RB_X0 + oc.x1, RB_Y0 + oc.y1, RB_X1 + oc.x1, RB_Y1 + oc.y1};
  if (fx_hit(a, clip))
    fx_draw(layer, this->rb_spr_, a, (lv_opa_t) ((this->dim_ ? 235 : 175) * this->rb_level_));
}

}  // namespace weather_fx
}  // namespace esphome

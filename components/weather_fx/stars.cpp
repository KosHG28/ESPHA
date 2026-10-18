// Звёзды ясной ночи: мягко светящиеся точки разных оттенков.
//
// Каждая звезда — готовая маска A8 с ярким ядром и ореолом, цвет задаётся
// при рисовании: голубовато-белые, белые, жёлтые и оранжевые — как
// настоящие звёзды разной температуры. Самые яркие — с четырьмя тонкими
// лучиками-бликами. Мерцание меняет только непрозрачность. Звёзды
// созвездия — такие же, размер — по звёздной величине.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

// Оттенки звёзд и их доли: из 20 звёзд 8 белых, 5 голубоватых, 5 жёлтых,
// 2 оранжевые
static const uint32_t STAR_TINT[4] = {0xF4F6FF, 0xC8DAFF, 0xFFF0C8, 0xFFC890};
static const uint8_t TINT_OF[20] = {0, 1, 0, 2, 0, 1, 2, 0, 3, 0, 1, 2, 0, 1, 2, 0, 3, 1, 2, 0};

// Звезда: ядро радиуса core и ореол до glow; spike — длина лучиков (0 —
// без них)
static void draw_star(FxSprite &s, float core, float glow, float spike) {
  const int h = (int) ceilf(std::max(glow, spike)) + 1, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = (float) (x - h), dy = (float) (y - h), d = sqrtf(dx * dx + dy * dy);
      const float c = fx_clamp01(core + 0.5f - d);
      const float g = 1.0f - fx_smooth(core * 0.5f, glow, d);
      float v = std::max(c, 0.55f * g * g);
      if (spike > 0) {
        // Лучики: вдоль осей, тонкие, к концу тают
        const float ax = fabsf(dx), ay = fabsf(dy);
        const float sx = fx_clamp01(0.75f - ay) * (1.0f - fx_smooth(0, spike, ax));
        const float sy = fx_clamp01(0.75f - ax) * (1.0f - fx_smooth(0, spike, ay));
        v = std::max(v, 0.85f * std::max(sx, sy));
      }
      s.buf[y * n + x] = (uint8_t) (255.0f * v + 0.5f);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::star_sprites_() {
  if (this->st_spr_[0].buf)
    return;
  draw_star(this->st_spr_[0], 1.05f, 3.0f, 0);     // обычная
  draw_star(this->st_spr_[1], 1.35f, 4.0f, 6.0f);  // яркая, с лучиками
  // Созвездие: по размеру звезды 4, 6, 8, 10 px
  for (int k = 0; k < 4; k++) {
    const float sz = 4.0f + 2.0f * k;
    draw_star(this->con_spr_[k], sz * 0.42f, sz * 0.5f + 3.0f, k == 3 ? 9.0f : 0.0f);
  }
}

// Звёзды на небе: оттенок и яркие с лучиками. Яркость задаёт мерцание
void WeatherFx::stars_layout_() {
  this->star_sprites_();
  for (int i = 0; i < NST; i++) {
    this->sttint_[i] = TINT_OF[i % 20];
    // Яркая каждая восьмая: при часах — одна-две, на весь экран — три
    this->stbig_[i] = i % 8 == 3;
    this->stopa_[i] = 160;
  }
}

// Прямоугольник звезды i на небе (относительно слоя)
void WeatherFx::star_mark_(int i, bool on) {
  const FxSprite &s = this->st_spr_[this->stbig_[i] ? 1 : 0];
  const int h = s.ax;
  // Звезда — в середине прежнего квадратика 3×3
  this->mark_(this->sts_[i], on && s.buf, this->stx_[i] + 1 - h, this->sty_[i] + 1 - h, 2 * h + 1, 2 * h + 1);
}

void WeatherFx::stars_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const int pal = this->dim_ ? 255 : 210;
  for (int i = 0; i < NST; i++) {
    const Spot &s = this->sts_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    fx_draw(layer, this->st_spr_[this->stbig_[i] ? 1 : 0], a, (lv_opa_t) (this->stopa_[i] * pal / 255),
            lv_color_hex(STAR_TINT[this->sttint_[i]]));
  }
}

// Звезда созвездия размера sz (4–10 px) с серединой в (x, y), экранные
// координаты
void WeatherFx::con_star_paint_(lv_layer_t *layer, int x, int y, int sz) {
  const FxSprite &s = this->con_spr_[std::min(3, std::max(0, (sz - 4) / 2))];
  fx_draw(layer, s, fx_sprite_area(s, x, y), LV_OPA_COVER, lv_color_hex(this->dim_ ? 0xFFF6DC : 0xE6DCC2));
}

}  // namespace weather_fx
}  // namespace esphome

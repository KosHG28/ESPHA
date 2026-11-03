// Гало в морозный день и морозную ночь.
//
// В мороз в воздухе висят ледяные иголки и пластинки, и свет луны или солнца
// преломляется в них, как в призмах: вокруг светила встаёт кольцо в 22°.
// Внутренний край кольца резкий, наружу оно тает. Слева и справа от солнца
// на кольце — «ложные солнца» (паргелии): яркие пятна, красноватые со
// стороны солнца, с белёсым хвостом наружу. Когда часы на экране, солнце —
// в «шапке» круга, и кольцу там тесно: видно одно ложное солнце справа.
//
// Всё неподвижно: кольцо — маска A8, ложные солнца — картинки RGB565A8;
// рисуются, когда гало появилось, и перерисовываются, только если то, что
// лежит под ними, само меняется.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

// Кольцо: радиус по внутреннему краю, px (у солнца — при масштабе 1, когда
// часы убраны), и как быстро тает наружу
static const float HALO_MOON_R = 42.0f, HALO_SUN_R = 72.0f, HALO_FADE = 4.5f;
// Ложное солнце в «шапке» — на таком расстоянии справа от солнца
static const int DOG_CAP_DX = 58;
// Картинка ложного солнца и где в ней пятно (со стороны солнца)
static const int DOG_W = 62, DOG_H = 63, DOG_AX = 23, DOG_AY = 31;

// Кольцо радиуса r: резко встаёт у внутреннего края и тает наружу
static void draw_ring(FxSprite &s, float r) {
  const int h = (int) ceilf(r + HALO_FADE * 3.0f) + 1, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = (float) (x - h), dy = (float) (y - h), d = sqrtf(dx * dx + dy * dy);
      if (d < r - 2.0f || d > r + HALO_FADE * 3.0f)
        continue;
      const float v = fx_smooth(r - 1.5f, r + 0.8f, d) * expf(-std::max(0.0f, d - r - 0.8f) / HALO_FADE);
      const float a = v * 255.0f + fx_dither(x, y) * 4.0f;
      if (a > 0.5f)
        s.buf[y * n + x] = (uint8_t) std::min(255.0f, a);
    }
  }
  fx_sprite_done(s);
}

// Ложное солнце справа от солнца (dir = 1) или слева (−1): пятно вытянуто
// вверх, со стороны солнца — красное, потом жёлтое и белое, наружу тянется
// голубоватый хвост
static void draw_dog(FxSprite &s, int dir) {
  if (!fx_sprite_init(s, DOG_W, DOG_H, true))
    return;
  s.ax = dir > 0 ? DOG_AX : DOG_W - 1 - DOG_AX;
  s.ay = DOG_AY;
  for (int y = 0; y < DOG_H; y++) {
    for (int x = 0; x < DOG_W; x++) {
      // lx — от солнца наружу
      const float lx = (float) (dir * (x - s.ax)), ly = (float) (y - s.ay);
      const float core = expf(-fx_sq(lx / 5.0f) - fx_sq(ly / 9.5f));
      const float glow = 0.45f * expf(-fx_sq(lx / 9.0f) - fx_sq(ly / 12.0f));
      const float tail = lx > 0 ? 0.5f * expf(-fx_sq(ly / 2.0f)) * powf(fx_clamp01(1.0f - lx / 38.0f), 1.5f) : 0;
      const float a = std::max(std::max(core, glow), tail);
      if (a < 0.01f)
        continue;
      FxMix m;
      m.over(0xFF5A30, 1);
      m.over(0xFFC94A, fx_smooth(-5.0f, -1.5f, lx));
      m.over(0xFFFDF0, fx_smooth(-1.5f, 3.0f, lx));
      m.over(0xDCEBFF, fx_smooth(6.0f, 18.0f, lx));
      FxMix o;
      o.r = m.r * a;
      o.g = m.g * a;
      o.b = m.b * a;
      o.a = a;
      fx_put(s.buf, DOG_W * DOG_H, y * DOG_W + x, o, x, y);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::halo_() {
  // Что видно: кольцо у луны (если она яркая — от трети до двух третей
  // месяца, или по выбору «Фона погоды»), кольцо и ложные солнца у солнца
  int want = 0;
  if ((this->halo_mode_ == 1 || this->halo_mode_ == 3) && this->stars_on_ && this->moon_spot_.on &&
      this->moon_phase_ >= 0 && (this->halo_mode_ == 3 || (this->moon_phase_ > 0.3f && this->moon_phase_ < 0.7f)))
    want = 1;
  else if (this->halo_mode_ == 2 && this->sun_on_ && this->sun_spot_.on)
    want = 2;
  if (want == this->halo_on_ && this->full_ == this->halo_full_)
    return;
  this->halo_on_ = want;
  this->halo_full_ = this->full_;
  // Рисунки — заново под то, что видно
  this->mark_(this->halo_spot_, false, 0, 0, 1, 1);
  for (auto &d : this->dog_spot_)
    this->mark_(d, false, 0, 0, 1, 1);
  fx_sprite_free(this->halo_ring_);
  for (auto &d : this->halo_dog_)
    fx_sprite_free(d);
  auto ring = [this](int cx, int cy, float r) {
    this->halo_r_ = r;
    draw_ring(this->halo_ring_, r);
    if (this->halo_ring_.buf) {
      const lv_area_t a = fx_sprite_area(this->halo_ring_, cx, cy);
      this->mark_(this->halo_spot_, true, a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1);
    }
  };
  auto dog = [this](int i, int dir, int x, int y) {
    draw_dog(this->halo_dog_[i], dir);
    if (this->halo_dog_[i].buf) {
      const lv_area_t a = fx_sprite_area(this->halo_dog_[i], x, y);
      this->mark_(this->dog_spot_[i], true, a.x1, a.y1, a.x2 - a.x1 + 1, a.y2 - a.y1 + 1);
    }
  };
  if (want == 1) {
    ring(MOON_X, MOON_Y, HALO_MOON_R);
  } else if (want == 2) {
    int sx, sy;
    float k;
    this->sun_place_(&sx, &sy, &k);
    if (this->full_) {
      const float r = HALO_SUN_R * k;
      ring(sx, sy, r);
      dog(0, 1, sx + (int) r + 2, sy);
      dog(1, -1, sx - (int) r - 2, sy);
    } else {
      dog(0, 1, sx + DOG_CAP_DX, sy);
    }
  }
}

void WeatherFx::halo_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->halo_on_)
    return;
  const bool dim = this->dim_;
  const Spot &s = this->halo_spot_;
  if (s.on && this->halo_ring_.buf) {
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    // Участок целиком внутри кольца (у самого светила — его венец
    // меняется часто) или целиком снаружи — кольцу там рисовать нечего
    const float cx = (a.x1 + a.x2) * 0.5f, cy = (a.y1 + a.y2) * 0.5f;
    const float fx = std::max(fabsf(clip.x1 - cx), fabsf(clip.x2 - cx)),
                fy = std::max(fabsf(clip.y1 - cy), fabsf(clip.y2 - cy));
    const float nx = std::max(0.0f, std::max(clip.x1 - cx, cx - clip.x2)),
                ny = std::max(0.0f, std::max(clip.y1 - cy, cy - clip.y2));
    const float r = this->halo_r_;
    if (fx_hit(a, clip) && fx * fx + fy * fy >= fx_sq(r - 2.0f) && nx * nx + ny * ny <= fx_sq(r + HALO_FADE * 3.0f)) {
      // У луны кольцо холоднее, у солнца — теплее
      const bool moon = this->halo_on_ == 1;
      const lv_opa_t o = (lv_opa_t) (moon ? (dim ? 120 : 80) : (dim ? 130 : 85));
      fx_draw(layer, this->halo_ring_, a, o, lv_color_hex(moon ? 0xE6EAFF : 0xFFF4E4));
    }
  }
  for (int i = 0; i < 2; i++) {
    const Spot &d = this->dog_spot_[i];
    if (!d.on)
      continue;
    const lv_area_t a = {d.a.x1 + oc.x1, d.a.y1 + oc.y1, d.a.x2 + oc.x1, d.a.y2 + oc.y1};
    if (fx_hit(a, clip))
      fx_draw(layer, this->halo_dog_[i], a, dim ? LV_OPA_COVER : 225);
  }
}

}  // namespace weather_fx
}  // namespace esphome

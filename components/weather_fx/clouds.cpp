// Облака и молния.
//
// Облако — готовая картинка: пухлые клубы, слитые в одно тело с ровным
// основанием и неровным, «пушистым» краем; у каждого клуба верх освещён, низ
// в тени. Форма считается один раз в две маски A8 — тело и свет, — а из них
// под нужные цвета (пасмурно, приглушённый экран, вспышка молнии) собирается
// цветная картинка RGB565A8: на экран облако ложится одним смешиванием.
//
// Молния — ветвистая: главный канал с изломами от облака вниз и две-три
// ветки в стороны. Тоже две маски: тонкая белая сердцевина и широкое
// сине-фиолетовое свечение. Удар — два всполоха и затухание; в первый
// всполох на миг светлеет всё небо (экран в это время всё равно
// перерисовывается целиком — он вздрагивает от грома), облака подсвечены.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

// Картинка облака
static const int CW = 200, CH = 74;
// Облака плывут у верхнего края круга и не задевают строку с погодой (она
// начинается на 113 px): иначе каждый сдвиг облака перерисовывал бы и её
static const int CLOUD_Y[3] = {34, 39, 16};
// Цвета облака: тело и свет — обычные, в пасмурную погоду и грозу, на
// приглушённом экране; во вспышке молнии
static const uint32_t CLOUD_C[2][2][2] = {
    {{0x1E252E, 0x7C8A9C}, {0x161B22, 0x56616E}},  // обычный экран: облачно, пасмурно
    {{0x3A4555, 0xB4C0CE}, {0x2E3744, 0x8E9AAA}},  // приглушённый
};
static const uint32_t CLOUD_FLASH_C[2] = {0x5A6088, 0xEEF0FF};
// Молния: сердцевина, свечение, вспышка неба
static const uint32_t BOLT_C = 0xFAF8FF, BOLT_GLOW_C = 0x8C9CFF, SKY_FLASH_C = 0x8C9CFF;
static const float BOLT_R = 11.0f;  // радиус свечения главного канала

// Гладкий шум: значения в узлах сетки — от зерна, между узлами — плавно
static float vnoise(float x, float y, uint32_t seed) {
  const int xi = (int) floorf(x), yi = (int) floorf(y);
  const float fx = x - xi, fy = y - yi;
  auto h = [seed](int x, int y) {
    uint32_t n = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + seed * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0f;
  };
  const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
  const float a = h(xi, yi), b = h(xi + 1, yi), c = h(xi, yi + 1), d = h(xi + 1, yi + 1);
  return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

struct Puff {
  float x, y, r;
};

// Плавный минимум: клубы сливаются без острых «талий»
static float smin(float a, float b, float k) {
  const float h = fx_clamp01(0.5f + 0.5f * (b - a) / k);
  return b + (a - b) * h - k * h * (1 - h);
}

// Расстояние до края облака (меньше нуля — внутри)
static float cloud_sd(const Puff *pf, int n, float base, float x, float y) {
  float d = 1e9f;
  for (int i = 0; i < n; i++) {
    const float dx = x - pf[i].x, dy = y - pf[i].y;
    d = smin(d, sqrtf(dx * dx + dy * dy) - pf[i].r, 7.0f);
  }
  return std::max(d, y - base);
}

// Облако v (0..2): тело и свет. Клубы рисуются от задних (верхних) к
// передним (нижним): у каждого свой свет — верх светлый, низ в тени, — так
// что передний клуб тенью ложится на задний и облако выглядит объёмным
static void draw_cloud(FxSprite &body, FxSprite &light, int v) {
  if (!fx_sprite_init(body, CW, CH, false) || !fx_sprite_init(light, CW, CH, false))
    return;
  FxRand rnd(0xC10D5u + v * 7919u);
  const float base = CH - 6.0f;
  Puff pf[12];
  int n = 0;
  // Верхние клубы: крупные в середине, мельче к краям
  const int np = 5 + v % 2;
  for (int i = 0; i < np; i++) {
    const float t = (i + 0.5f) / np;
    const float bump = sinf(t * 3.14159f);
    const float r = 12.0f + 17.0f * bump + rnd.range(-3.0f, 4.0f);
    pf[n++] = {22.0f + t * (CW - 44) + rnd.range(-7.0f, 7.0f), base - r * 0.9f - rnd.range(0.0f, 7.0f) * bump, r};
  }
  // Задние — выше: порядок рисования сверху вниз
  std::sort(pf, pf + n, [](const Puff &a, const Puff &b) { return a.y - a.r < b.y - b.r; });
  // Основание — ряд мелких клубов спереди
  for (int i = 0; i < 4; i++)
    pf[n++] = {22.0f + i * (CW - 44) / 3.0f + rnd.range(-5.0f, 5.0f), base - 10.0f, 11.0f + rnd.range(0.0f, 3.0f)};
  const uint32_t seed = 977u + v * 131u;
  for (int y = 0; y < CH; y++) {
    for (int x = 0; x < CW; x++) {
      const float px = x + 0.5f, py = y + 0.5f;
      // Пушистый край: расстояние до края чуть «гуляет»
      const float fluff =
          (vnoise(px / 7.0f, py / 7.0f, seed) - 0.5f) * 3.5f + (vnoise(px / 3.0f, py / 3.0f, seed + 7) - 0.5f) * 1.2f;
      const float sd = cloud_sd(pf, n, base, px, py) + fluff;
      const float a = fx_clamp01(0.5f - sd / 1.4f);
      if (a <= 0)
        continue;
      // Свет: каждый следующий верхний клуб мягко закрывает предыдущие;
      // ряд основания — только для формы, он весь в тени
      float lit = 0.1f;
      for (int i = 0; i < np; i++) {
        const float dx = px - pf[i].x, dy = py - pf[i].y;
        const float d = sqrtf(dx * dx + dy * dy) + fluff;
        const float cov = fx_smooth(pf[i].r + 0.5f, pf[i].r - 5.0f, d);
        if (cov <= 0)
          continue;
        // Верх клуба освещён, к низу — тень
        const float up = fx_clamp01(0.55f - 0.55f * dy / pf[i].r);
        const float own = std::min(1.0f, 0.06f + 0.9f * up * up);
        lit += (own - lit) * cov;
      }
      // Ровное основание облака — в тени
      lit *= fx_smooth(base + 1.0f, base - 9.0f, py) * 0.6f + 0.4f;
      body.buf[y * CW + x] = (uint8_t) (a * 255.0f + 0.5f);
      light.buf[y * CW + x] = (uint8_t) (fx_clamp01(lit) * a * 255.0f + 0.5f);
    }
  }
  body.ax = body.ay = light.ax = light.ay = 0;
  fx_sprite_done(body);
  fx_sprite_done(light);
}

// ---------------------------------------------------------------------------

// Цветная картинка облака из масок тела и света: цвет тела cb, освещённых
// мест — cl
static void paint_cloud(FxSprite &dst, const FxSprite &body, const FxSprite &light, uint32_t cb, uint32_t cl) {
  if (!body.buf || !fx_sprite_init(dst, CW, CH, true))
    return;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(dst.buf);
  uint8_t *alpha = dst.buf + CW * CH * 2;
  const lv_color_t b = lv_color_hex(cb), l = lv_color_hex(cl);
  for (int i = 0; i < CW * CH; i++) {
    const int a = body.buf[i];
    if (!a)
      continue;
    rgb[i] = lv_color_to_u16(lv_color_mix(l, b, (uint8_t) std::min(255, light.buf[i] * 255 / a)));
    alpha[i] = (uint8_t) a;
  }
  dst.ax = dst.ay = 0;
  fx_sprite_done(dst);
}

void WeatherFx::clouds_apply_(bool relayout) {
  const int pal = this->dim_ ? 1 : 0, heavy = (this->ncl_ >= 3 || this->storm_) ? 1 : 0;
  const int look = 1 + pal * 2 + heavy;
  for (int i = 0; i < NC; i++) {
    const bool on = i < this->ncl_;
    if (on && !this->cloud_body_[i].buf)
      draw_cloud(this->cloud_body_[i], this->cloud_light_[i], i);
    // Цвета сменились или облако новое — собрать картинку заново; облака,
    // подсвеченные молнией, нужны только в грозу
    if (on && (look != this->cloud_look_ || !this->cloud_img_[i].buf)) {
      paint_cloud(this->cloud_img_[i], this->cloud_body_[i], this->cloud_light_[i], CLOUD_C[pal][heavy][0],
                  CLOUD_C[pal][heavy][1]);
      if (this->cloud_spot_[i].on)
        this->invalidate_(this->cloud_spot_[i].a);
    }
    if (on && this->storm_ && !this->cloud_flash_[i].buf)
      paint_cloud(this->cloud_flash_[i], this->cloud_body_[i], this->cloud_light_[i], CLOUD_FLASH_C[0],
                  CLOUD_FLASH_C[1]);
    if (!this->storm_)
      fx_sprite_free(this->cloud_flash_[i]);
    if (relayout)
      this->cx_[i] = 40 + i * 150 - 90;
    this->mark_(this->cloud_spot_[i], on && this->cloud_img_[i].buf, (int) this->cx_[i], CLOUD_Y[i], CW, CH);
  }
  this->cloud_look_ = look;
  if (!this->storm_) {
    this->end_strike_();
    fx_sprite_free(this->bolt_core_);
    fx_sprite_free(this->bolt_glow_);
  }
}

void WeatherFx::clouds_() {
  // Облака медленно ползут вправо (2–3 px/с), у каждого своя скорость.
  // Сдвигаем только когда меняется целый пиксель: каждый сдвиг перерисовывает
  // всё облако целиком, поэтому чем медленнее, тем дешевле
  for (int i = 0; i < this->ncl_; i++) {
    this->cx_[i] += this->k_ * (0.10f + i * 0.03f);
    if (this->cx_[i] > 466)
      this->cx_[i] = -CW;
    this->mark_(this->cloud_spot_[i], this->cloud_spot_[i].on, (int) this->cx_[i], CLOUD_Y[i], CW, CH);
  }
}

// Молния: главный канал от облака вниз — ломаная с изломами — и 2–3 ветки
// в стороны, к концу тоньше и тусклее. Маски сердцевины и свечения
void WeatherFx::bolt_build_(int x0, int y0) {
  struct Seg {
    float x0, y0, x1, y1, w, k, r;
  };
  static const int NSEG = 64;
  Seg seg[NSEG];
  int ns = 0;
  float mx[32], my[32];
  int nm = 0;
  float x = 0, y = 0, slope = 0;
  const float len = rnd_(170, 250);
  mx[nm] = x;
  my[nm++] = y;
  while (y < len && nm < 32) {
    // Изломы: наклон каждого отрезка случайный, но помнит предыдущий
    slope = std::min(0.9f, std::max(-0.9f, slope * 0.35f + rnd_(-70, 71) / 100.0f));
    const float dy = rnd_(8, 17);
    x += slope * dy;
    y += dy;
    mx[nm] = x;
    my[nm++] = y;
  }
  for (int k = 1; k < nm && ns < NSEG; k++) {
    const float t = k / (float) nm;
    seg[ns++] = {mx[k - 1], my[k - 1], mx[k], my[k], 2.6f - 1.0f * t, 1.0f, BOLT_R};
  }
  // Ветки: от точек верхних двух третей канала — вниз и в сторону
  const int nb = rnd_(2, 4);
  for (int b = 0; b < nb && ns < NSEG - 1; b++) {
    const int from = rnd_(1, std::max(2, nm * 2 / 3));
    float bx = mx[from], by = my[from];
    const float side = rnd_(0, 2) ? 1.0f : -1.0f;
    float bs = side * rnd_(45, 110) / 100.0f;
    const float blen = rnd_(30, 90);
    float done = 0;
    while (done < blen && ns < NSEG) {
      bs = std::min(1.4f, std::max(-1.4f, bs + rnd_(-35, 36) / 100.0f));
      const float dy = rnd_(6, 12);
      const float nx = bx + bs * dy, ny = by + dy;
      const float t = done / blen;
      seg[ns++] = {bx, by, nx, ny, 1.5f - 0.7f * t, 0.8f - 0.45f * t, 7.0f};
      done += dy;
      bx = nx;
      by = ny;
    }
  }
  // Рамка всех отрезков с запасом под свечение
  float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
  for (int i = 0; i < ns; i++) {
    x1 = std::min(x1, std::min(seg[i].x0, seg[i].x1));
    x2 = std::max(x2, std::max(seg[i].x0, seg[i].x1));
    y1 = std::min(y1, std::min(seg[i].y0, seg[i].y1));
    y2 = std::max(y2, std::max(seg[i].y0, seg[i].y1));
  }
  const int ox = (int) floorf(x1 - BOLT_R - 1), oy = (int) floorf(y1 - BOLT_R - 1);
  const int w = (int) ceilf(x2 + BOLT_R + 1) - ox + 1, h = (int) ceilf(y2 + BOLT_R + 1) - oy + 1;
  if (!fx_sprite_init(this->bolt_core_, w, h, false) || !fx_sprite_init(this->bolt_glow_, w, h, false))
    return;
  uint8_t *core = this->bolt_core_.buf, *glow = this->bolt_glow_.buf;
  for (int i = 0; i < ns; i++) {
    const Seg &s = seg[i];
    const float dx = s.x1 - s.x0, dy = s.y1 - s.y0, l2 = std::max(1e-3f, dx * dx + dy * dy);
    const int sx0 = std::max(0, (int) floorf(std::min(s.x0, s.x1) - s.r) - ox),
              sx1 = std::min(w - 1, (int) ceilf(std::max(s.x0, s.x1) + s.r) - ox);
    const int sy0 = std::max(0, (int) floorf(std::min(s.y0, s.y1) - s.r) - oy),
              sy1 = std::min(h - 1, (int) ceilf(std::max(s.y0, s.y1) + s.r) - oy);
    for (int py = sy0; py <= sy1; py++) {
      for (int px = sx0; px <= sx1; px++) {
        const float qx = px + ox + 0.5f, qy = py + oy + 0.5f;
        const float t = fx_clamp01(((qx - s.x0) * dx + (qy - s.y0) * dy) / l2);
        const float ex = s.x0 + t * dx - qx, ey = s.y0 + t * dy - qy;
        const float d = sqrtf(ex * ex + ey * ey);
        const int c = (int) (255.0f * s.k * fx_clamp01(s.w * 0.5f + 0.5f - d));
        uint8_t &cp = core[py * w + px];
        if (c > cp)
          cp = (uint8_t) std::min(c, 255);
        if (d < s.r) {
          const float f = 1.0f - d / s.r;
          const int g = (int) (230.0f * s.k * f * f);
          uint8_t &gp = glow[py * w + px];
          if (g > gp)
            gp = (uint8_t) std::min(g, 255);
        }
      }
    }
  }
  this->bolt_core_.ax = this->bolt_core_.ay = this->bolt_glow_.ax = this->bolt_glow_.ay = 0;
  fx_sprite_done(this->bolt_core_);
  fx_sprite_done(this->bolt_glow_);
  this->bolt_x_ = x0 + ox;
  this->bolt_y_ = y0 + oy;
}

void WeatherFx::end_strike_() {
  if (!this->striking_)
    return;
  this->striking_ = false;
  this->bolt_on_ = false;
  if (this->shake_) {
    this->shake_ = 0;
    lv_obj_set_style_translate_x(lv_screen_active(), 0, 0);
  }
  this->bolt_light_(0, false, false);
  this->next_strike_ = millis() + rnd_(5000, 15000);
}

// Яркость молнии, подсветка облаков и вспышка неба: перерисовать то, что
// изменилось
void WeatherFx::bolt_light_(uint8_t opa, bool lit, bool sky) {
  if (opa != this->bolt_opa_) {
    this->bolt_opa_ = opa;
    if (this->bolt_core_.buf)
      this->mark_(this->bolt_spot_, opa > 0, this->bolt_x_, this->bolt_y_, this->bolt_core_.dsc.header.w,
                  this->bolt_core_.dsc.header.h);
    if (this->bolt_spot_.on)
      this->invalidate_(this->bolt_spot_.a);
  }
  if (lit != this->clouds_lit_) {
    this->clouds_lit_ = lit;
    for (int i = 0; i < this->ncl_; i++)
      if (this->cloud_spot_[i].on)
        this->invalidate_(this->cloud_spot_[i].a);
  }
  if (sky != this->sky_flash_) {
    this->sky_flash_ = sky;
    const lv_area_t all = {0, 0, 465, 465};
    this->invalidate_(all);
  }
}

void WeatherFx::lightning_(uint32_t now) {
  if (!this->storm_)
    return;
  if (!this->striking_) {
    if (this->next_strike_ == 0)
      this->next_strike_ = now + rnd_(2000, 6000);
    if ((int32_t) (now - this->next_strike_) < 0)
      return;
    // Начало — из-под облака, если оно над серединой экрана, иначе сверху
    int x0 = rnd_(130, 336), y0 = 40;
    for (int i = 0; i < this->ncl_; i++) {
      const int mid = (int) this->cx_[i] + CW / 2;
      if (mid > 120 && mid < 346) {
        x0 = mid + rnd_(-30, 31);
        y0 = CLOUD_Y[i] + CH - 14;
        break;
      }
    }
    this->bolt_build_(x0, y0);
    this->striking_ = true;
    this->strike_t0_ = now;
  }
  // Удар: всполох 0–60 мс, слабое послесвечение, второй всполох 130–200 мс
  // и затухание к 340 мс
  const uint32_t t = now - this->strike_t0_;
  if (t >= 340) {
    this->end_strike_();
    return;
  }
  // Гром: экран вздрагивает на первых долях удара
  const int off = t < 50 ? 3 : (t < 100 ? -3 : (t < 150 ? 2 : 0));
  if (off != this->shake_) {
    this->shake_ = off;
    lv_obj_set_style_translate_x(lv_screen_active(), off, 0);
  }
  uint8_t opa;
  if (t < 60)
    opa = 255;
  else if (t < 130)
    opa = 70;
  else if (t < 200)
    opa = 235;
  else
    opa = (uint8_t) (235 * (340 - t) / 140);
  // Вспышка неба — пока экран вздрагивает в первый раз: он и так
  // перерисовывается целиком
  this->bolt_light_(opa, opa >= 200, t < 50);
  this->bolt_on_ = opa >= 200;
}

void WeatherFx::sky_flash_paint_(lv_layer_t *layer, const lv_area_t &clip) {
  if (!this->sky_flash_)
    return;
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.color = lv_color_hex(SKY_FLASH_C);
  fill.opa = this->dim_ ? 46 : 30;
  lv_draw_fill(layer, &fill, &clip);
}

void WeatherFx::clouds_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  auto screen = [&](const Spot &s) {
    return lv_area_t{s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  };
  // Молния — под облаками: она выходит из-под облака
  if (this->bolt_spot_.on && this->bolt_opa_) {
    const lv_area_t a = screen(this->bolt_spot_);
    if (fx_hit(a, clip)) {
      fx_draw(layer, this->bolt_glow_, a, (lv_opa_t) (this->bolt_opa_ * (this->dim_ ? 235 : 200) / 255),
              lv_color_hex(BOLT_GLOW_C));
      fx_draw(layer, this->bolt_core_, a, this->bolt_opa_, lv_color_hex(BOLT_C));
    }
  }
  for (int i = 0; i < this->ncl_; i++) {
    if (!this->cloud_spot_[i].on)
      continue;
    const lv_area_t a = screen(this->cloud_spot_[i]);
    if (!fx_hit(a, clip))
      continue;
    const FxSprite &img = this->clouds_lit_ && this->cloud_flash_[i].buf ? this->cloud_flash_[i] : this->cloud_img_[i];
    fx_draw(layer, img, a, LV_OPA_COVER);
  }
}

}  // namespace weather_fx
}  // namespace esphome

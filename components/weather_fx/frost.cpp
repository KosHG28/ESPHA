// Мороз: иней по краю экрана, искорки на кончиках кристаллов и сосульки под
// цифрами часов.
//
// Иней — морозный узор, как на оконном стекле: плотная кайма у края (внизу
// толще), перистые кристаллы, ветвящиеся под 60°, — к середине они тают, —
// шестилучевые звёздочки льда и крупа. Узор собирается один раз в маску
// прозрачности во весь экран (A8, в PSRAM) и рисуется ледяным цветом одной
// картинкой.
#include "fx_draw.h"

namespace esphome {
namespace weather_fx {

static const float PI_F = 3.14159265f;

void WeatherFx::build_frost_() {
  // Морозный узор, как на оконном стекле: плотная кайма у края (внизу
  // толще), перистые кристаллы, ветвящиеся под 60°, — к середине они тают,
  // — мелкие шестилучевые звёздочки и крупа. Всё рисуется сглаженно в маску
  // прозрачности; узор одинаковый при каждом включении — постоянное зерно
  static const int W = 466, NA = 1024;
  // Толщина каймы по углу (ниже) — таблица на время сборки
  float *thick_at = static_cast<float *>(lv_malloc((NA + 1) * sizeof(float)));
  if (!thick_at)
    return;
  if (!this->frost_buf_) {
    this->frost_buf_ = static_cast<uint8_t *>(lv_malloc(W * W));
    if (!this->frost_buf_) {
      lv_free(thick_at);
      return;
    }
  }
  uint8_t *b = this->frost_buf_;
  memset(b, 0, W * W);
  uint32_t seed = 20261004u;
  auto rnd01 = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0f;
  };
  auto put = [b](int x, int y, float a) {
    if ((unsigned) x >= (unsigned) W || (unsigned) y >= (unsigned) W || a <= 0)
      return;
    uint8_t &px = b[y * W + x];
    const int v = (int) a;
    if (v > px)
      px = (uint8_t) std::min(v, 255);
  };
  // Сглаженный отрезок толщиной w: яркость — по расстоянию до оси
  auto seg = [&](float x0, float y0, float x1, float y1, float w, float a) {
    const int bx0 = (int) floorf(std::min(x0, x1) - w - 1), bx1 = (int) ceilf(std::max(x0, x1) + w + 1);
    const int by0 = (int) floorf(std::min(y0, y1) - w - 1), by1 = (int) ceilf(std::max(y0, y1) + w + 1);
    const float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
    for (int y = by0; y <= by1; y++) {
      for (int x = bx0; x <= bx1; x++) {
        float t = l2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / l2 : 0.0f;
        t = std::min(std::max(t, 0.0f), 1.0f);
        const float ex = x0 + t * dx - x, ey = y0 + t * dy - y;
        const float cov = w * 0.5f + 0.5f - sqrtf(ex * ex + ey * ey);
        if (cov > 0)
          put(x, y, a * std::min(cov, 1.0f));
      }
    }
  };

  // Кайма у края и изморозь-крупа. Толщина каймы зависит только от угла —
  // она считается заранее для 1024 углов, а не синусами в каждой точке
  // кольца: так весь узор собирается почти вдвое быстрее
  const float p1 = rnd01() * 6.28f, p2 = rnd01() * 6.28f, p3 = rnd01() * 6.28f;
  for (int i = 0; i <= NA; i++) {
    const float ang = i * 2.0f * PI_F / NA - PI_F;
    const float n = 0.5f * sinf(3 * ang + p1) + 0.3f * sinf(7 * ang + p2) + 0.2f * sinf(13 * ang + p3);
    thick_at[i] = 9.0f + 9.0f * std::max(0.0f, sinf(ang)) + 5.0f * n;
  }
  for (int y = 0; y < W; y++) {
    // Только точки кольца: от 192 до 233,5 px от середины
    const float dy = y - 232.5f;
    if (dy * dy > 233.5f * 233.5f)
      continue;
    const float outer = sqrtf(233.5f * 233.5f - dy * dy);
    const float inner2 = 192.0f * 192.0f - dy * dy;
    const float inner = inner2 > 0 ? sqrtf(inner2) : 0.0f;
    for (int x = std::max(0, (int) floorf(232.5f - outer)); x <= std::min(W - 1, (int) ceilf(232.5f + outer)); x++) {
      const float dx = x - 232.5f, r2 = dx * dx + dy * dy;
      if (fabsf(dx) < inner - 1.0f || r2 < 192.0f * 192.0f || r2 > 233.5f * 233.5f)
        continue;
      const float edge = 233.0f - sqrtf(r2);
      const float u = (fx_atan2(dy, dx) + PI_F) / (2.0f * PI_F) * NA;
      const int ui = std::min(NA - 1, std::max(0, (int) u));
      const float thick = thick_at[ui] + (thick_at[ui + 1] - thick_at[ui]) * (u - ui);
      float a = 0;
      if (edge < thick) {
        const float k = 1.0f - edge / thick;
        a = 100.0f * k * k;
      }
      if (edge < thick * 1.5f && rnd01() < 0.05f)
        a = std::max(a, 45.0f + rnd01() * 110.0f);
      put(x, y, a);
    }
  }
  lv_free(thick_at);

  // Перистые кристаллы: ствол от края внутрь, по бокам — ветки под 60°,
  // у длинных веток — свои веточки. Чем ближе к кончику, тем ветки короче
  struct Br {
    float x, y, dir, len, w, a;
    int level;
  };
  Br st[160];
  int sp = 0;
  this->nfsp_ = 0;
  static const int SEEDS = 46;
  for (int i = 0; i < SEEDS; i++) {
    const float ang = i * 2.0f * PI_F / SEEDS + (rnd01() - 0.5f) * 0.12f;
    const float bottom = std::max(0.0f, sinf(ang));
    // Слева и справа — короче, чтобы не залезать на цифры
    const float side = std::fabs(cosf(ang));
    float len = (16.0f + rnd01() * 26.0f + 12.0f * bottom) * (1.0f - 0.35f * side);
    if (rnd01() < 0.25f)
      len *= 0.55f;  // между крупными — мелкие
    const float x = 232.5f + 232.0f * cosf(ang), y = 232.5f + 232.0f * sinf(ang);
    st[sp++] = {x, y, ang + PI_F + (rnd01() - 0.5f) * 0.7f, len, 1.5f, 225.0f, 0};
    while (sp > 0) {
      const Br br = st[--sp];
      float bx = br.x, by = br.y, d = br.dir;
      const float step = 2.5f;
      const float gap = br.level == 0 ? 4.5f : 3.5f;
      float next = gap;
      for (float tr = 0; tr < br.len; tr += step) {
        const float nx = bx + cosf(d) * step, ny = by + sinf(d) * step;
        // Тает к середине круга
        const float depth = 233.0f - sqrtf((nx - 232.5f) * (nx - 232.5f) + (ny - 232.5f) * (ny - 232.5f));
        const float fade = std::min(std::max(1.0f - depth / 75.0f, 0.3f), 1.0f);
        seg(bx, by, nx, ny, br.w * (1.0f - 0.4f * tr / br.len), br.a * fade);
        bx = nx;
        by = ny;
        d += (rnd01() - 0.5f) * 0.12f;
        if (br.level < 2 && tr + step >= next) {
          next += gap;
          const float rest = br.len - tr;
          const float bl = rest * (br.level == 0 ? 0.55f : 0.5f) * (0.8f + rnd01() * 0.4f);
          if (bl > 3.0f)
            for (int sgn = -1; sgn <= 1 && sp < 158; sgn += 2)
              st[sp++] = {bx, by, d + sgn * (PI_F / 3), bl, br.w * 0.7f, br.a * 0.8f, br.level + 1};
        }
      }
      // Кончик ствола — место для искорки
      if (br.level == 0 && br.len > 20 && this->nfsp_ < NFSP && rnd01() < 0.45f) {
        this->fsp_x_[this->nfsp_] = (int) bx;
        this->fsp_y_[this->nfsp_] = (int) by;
        this->fsp_ph_[this->nfsp_] = rnd01() * 6.28f;
        this->nfsp_++;
      }
    }
  }

  // Шестилучевые звёздочки льда у края
  for (int i = 0; i < 18; i++) {
    const float ang = rnd01() * 2.0f * PI_F;
    const float r = 200.0f + rnd01() * 24.0f;
    const float cx = 232.5f + r * cosf(ang), cy = 232.5f + r * sinf(ang);
    const float size = 2.5f + rnd01() * 2.5f, rot = rnd01() * PI_F;
    for (int k = 0; k < 6; k++) {
      const float a = rot + k * PI_F / 3;
      seg(cx, cy, cx + cosf(a) * size, cy + sinf(a) * size, 0.9f, 180.0f);
    }
  }

  lv_image_dsc_t &d = this->frost_dsc_;
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_A8;
  d.header.flags = 0;
  d.header.w = W;
  d.header.h = W;
  d.header.stride = W;
  d.data_size = W * W;
  d.data = b;
  lv_image_cache_drop(&d);
  for (int i = 0; i < this->nfsp_; i++)
    this->fsp_opa_[i] = 0;
}

void WeatherFx::free_frost_() {
  if (this->frost_buf_) {
    lv_image_cache_drop(&this->frost_dsc_);
    lv_free(this->frost_buf_);
    this->frost_buf_ = nullptr;
  }
  for (auto &f : this->fsp_spot_)
    this->mark_(f, false, 0, 0, 1, 1);
  fx_sprite_free(this->fsp_spr_);
}

// Искорка — четырёхлучевой блеск: тонкие лучи крестом, покороче — по
// диагоналям, мягкая серединка
static const int FS_W = 13, FS_A = 6;

static void draw_spark(FxSprite &s) {
  if (!fx_sprite_init(s, FS_W, FS_W, false))
    return;
  s.ax = s.ay = FS_A;
  for (int y = 0; y < FS_W; y++) {
    for (int x = 0; x < FS_W; x++) {
      const float dx = (float) (x - FS_A), dy = (float) (y - FS_A), d = sqrtf(dx * dx + dy * dy);
      float v = std::max(fx_clamp01(1.6f - d), 0.45f * fx_sq(fx_clamp01(1.0f - d / 4.0f)));
      // Лучи крестом сужаются к концам
      const float h = fx_clamp01(0.9f - fabsf(dy)) * powf(fx_clamp01(1.0f - fabsf(dx) / 6.5f), 1.5f);
      const float vv = fx_clamp01(0.9f - fabsf(dx)) * powf(fx_clamp01(1.0f - fabsf(dy) / 6.5f), 1.5f);
      // По диагоналям — короче и тусклее
      const float u = (dx + dy) * 0.7071f, w = (dx - dy) * 0.7071f;
      const float g = 0.6f * std::max(fx_clamp01(0.7f - fabsf(w)) * powf(fx_clamp01(1.0f - fabsf(u) / 3.5f), 1.5f),
                                      fx_clamp01(0.7f - fabsf(u)) * powf(fx_clamp01(1.0f - fabsf(w) / 3.5f), 1.5f));
      v = std::max(v, std::max(std::max(h, vv), g));
      s.buf[y * FS_W + x] = (uint8_t) std::min(255.0f, v * 255.0f + 0.5f);
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::frost_sparks_(uint32_t now) {
  // Искорки вспыхивают по очереди: каждая чаще тёмная и изредка блестит
  if (!this->frost_on_ || !this->frost_buf_ || now - this->fsp_ms_ < 70)
    return;
  this->fsp_ms_ = now;
  this->fsp_grp_ = (this->fsp_grp_ + 1) % 3;
  for (int i = this->fsp_grp_; i < this->nfsp_; i += 3) {
    this->fsp_ph_[i] += 0.35f + (i % 4) * 0.06f;
    const float s = std::max(0.0f, sinf(this->fsp_ph_[i]));
    const lv_opa_t o = (lv_opa_t) (255.0f * s * s * s * s);
    if (o == this->fsp_opa_[i] && !(o == 0 && this->fsp_spot_[i].on))
      continue;
    if (this->fsp_spot_[i].on)
      this->invalidate_(this->fsp_spot_[i].a);
    this->fsp_opa_[i] = o;
    this->mark_(this->fsp_spot_[i], o > 8, this->fsp_x_[i] - FS_A, this->fsp_y_[i] - FS_A, FS_W, FS_W);
  }
}

// Иней: узор собирается при включении и освобождает память при выключении;
// сосульки — следом
void WeatherFx::frost_apply_() {
  if (this->frost_on_ && !this->frost_buf_) {
    this->build_frost_();
    draw_spark(this->fsp_spr_);
    lv_obj_invalidate(this->paint_);
  } else if (!this->frost_on_ && this->frost_buf_) {
    this->free_frost_();
    lv_obj_invalidate(this->paint_);
  }
  this->icicles_apply_();
}

void WeatherFx::frost_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  lv_area_t abs;
  // Экранный прямоугольник частицы; false — если не попадает в участок
  auto place = [&](const Spot &s) {
    if (!s.on)
      return false;
    abs = {(int32_t) (s.a.x1 + oc.x1), (int32_t) (s.a.y1 + oc.y1), (int32_t) (s.a.x2 + oc.x1),
           (int32_t) (s.a.y2 + oc.y1)};
    return fx_hit(abs, clip);
  };

  // Иней: морозный узор по краю — готовая маска, ледяным цветом. Внутри
  // круга его нет, так что участки в середине экрана его не рисуют
  if (this->frost_on_ && this->frost_buf_) {
    const int cx = 233 + oc.x1, cy = 233 + oc.y1;
    const int fx = std::max(std::abs(clip.x1 - cx), std::abs(clip.x2 - cx));
    const int fy = std::max(std::abs(clip.y1 - cy), std::abs(clip.y2 - cy));
    if (fx * fx + fy * fy >= 150 * 150) {
      lv_draw_image_dsc_t im;
      lv_draw_image_dsc_init(&im);
      im.src = &this->frost_dsc_;
      im.recolor = lv_color_hex(this->dim_ ? 0xEAF6FF : 0xC4DCF2);
      im.opa = LV_OPA_COVER;
      lv_area_t a = {oc.x1, oc.y1, oc.x1 + 465, oc.y1 + 465};
      im.image_area = a;
      lv_draw_image(layer, &im, &a);
    }
    // Искорки на кончиках кристаллов — четырёхлучевой блеск
    for (int i = 0; i < this->nfsp_; i++) {
      if (!place(this->fsp_spot_[i]))
        continue;
      fx_draw(layer, this->fsp_spr_, abs, this->fsp_opa_[i], lv_color_hex(0xEAF6FF));
    }
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
static FxMix ice_at(float X, float Y, uint32_t ice, int i) {
  FxMix m;
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
    if (i % 3 == 1 && fx_sq(X - ix) + fx_sq(Y - 1.5f - len - 0.9f) < 1.1f)
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
      FxMix px;
      for (int q = 0; q < 4; q++)
        px.add(ice_at(ICE_X0 + x + (q & 1 ? 0.25f : -0.25f), y + (q & 2 ? 0.25f : -0.25f), ice, col[x]), 0.25f);
      fx_put(s.buf, ICE_W * ICE_H, y * ICE_W + x, px, x, y);
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

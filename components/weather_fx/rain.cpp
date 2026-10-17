// Дождь и град на странице часов.
//
// «Падающий» — струи в три слоя глубины: дальние тонкие, короткие, тусклые и
// медленные, ближние — длинные, яркие и быстрые. Все струи наклонены по
// ветру и вместе качаются в порывах. Ближние и средние разбиваются о нижний
// край круга брызгами, изредка ближняя капля бьёт в стекло и остаётся на нём.
//
// «Капли на стекле» — капли разного размера садятся на стекло, сливаются,
// крупные сползают рывками, оставляя за собой дорожку бусинок, и по пути
// вбирают встреченные капли. Сидящие капли со временем высыхают.
//
// Всё рисуется готовыми картинками, нарисованными при запуске: струя — маска
// A8 (цвет и прозрачность задаются при рисовании, так что вспышка молнии
// ничего не стоит), капля на стекле — RGB565A8 с бликом и светлым серпом
// снизу. Картинка дешевле сглаженного кружка или линии LVGL и рисует то, что
// примитивами не нарисовать. Неподвижные капли не перерисовываются вовсе;
// за кругом экрана ничего не рисуется — там пиксели не видны.
#include "weather_fx.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// lv_image_cache_drop — во внутренних заголовках LVGL
#include <lvgl_private.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace weather_fx {

static const float CX = 233.0f, CY = 233.0f, RAD = 233.0f;

// Слои глубины падающего дождя: скорость, px за 50 мс; длина струи (два
// варианта); толщина; непрозрачность
struct RainLayer {
  float v0, v1;
  int len[2];
  float thick;
  lv_opa_t opa;
};
static const RainLayer LAYERS[3] = {
    {13.0f, 16.0f, {15, 19}, 1.5f, 190},  // дальний: 260–320 px/с
    {19.0f, 23.0f, {24, 30}, 2.0f, 230},  // средний: 380–460 px/с
    {26.0f, 31.0f, {38, 46}, 2.8f, 255},  // ближний: 520–620 px/с
};
// Наклон струй при полном ветре (тангенс угла от вертикали): ~30°
static const float MAX_TAN = 0.58f;
// Картинки струй перерисовываются, когда угол меняется на 1,5°
static const float ANG_STEP = 0.026f;
// Цвета струй по слоям (дальний, средний, ближний) и брызг: обычные и для
// приглушённого экрана — там ярче, иначе при низкой яркости не видно
static const uint32_t STREAK_C[2][3] = {{0x5687B0, 0x7DB2DD, 0xB4DDFA}, {0x86B5DC, 0xB2DAF6, 0xE6F5FF}};
static const uint32_t SPLASH_C[2] = {0x9FD0F2, 0xE2F4FF};
static const uint32_t FLASH_C = 0xEAF6FF;  // струи во вспышке молнии

// Капли на стекле: поперечник по размеру, px, и скорость сползания, px/с
static const int GLASS_D[7] = {4, 6, 9, 12, 16, 21, 27};
static const float SLIDE_V[7] = {0, 0, 0, 26, 46, 74, 108};
// Сколько капель сползают одновременно, не больше
static const int MAX_SLIDE = 4;
// Цвета капли на стекле: тело, свет снизу, блик — обычные и яркие
static const uint32_t DROP_BODY[2] = {0x173650, 0x2D6390};
static const uint32_t DROP_GLOW[2] = {0x7EC6F2, 0xC4E9FF};

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static float smooth(float e0, float e1, float x) {
  const float t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}
// Нижний край круга под точкой x
static float bottom_y(float x) {
  const float d = x - CX;
  return CY + sqrtf(std::max(0.0f, RAD * RAD - d * d));
}
static bool hit_area(const lv_area_t &a, const lv_area_t &clip) {
  return a.x1 <= clip.x2 && a.x2 >= clip.x1 && a.y1 <= clip.y2 && a.y2 >= clip.y1;
}

// Буфер картинки нужного размера. Память — из кучи LVGL (на плате это PSRAM),
// выделяется один раз и потом только переиспользуется
static bool sprite_init(FxSprite &s, int w, int h, bool rgb) {
  const int bytes = w * h * (rgb ? 3 : 1);
  if (s.cap < bytes) {
    if (s.buf)
      lv_free(s.buf);
    s.buf = static_cast<uint8_t *>(lv_malloc(bytes));
    s.cap = s.buf ? bytes : 0;
    if (!s.buf)
      return false;
  }
  memset(s.buf, 0, bytes);
  lv_image_dsc_t &d = s.dsc;
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = rgb ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_A8;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = rgb ? w * 2 : w;
  d.data = s.buf;
  d.data_size = bytes;
  return true;
}

// Струи падают «пучками»: в дальнем слое по четыре струи, в среднем по три,
// ближние — по одной. Струи одного слоя падают с одной скоростью, так что
// пучок выглядит естественно, а перерисовывается одним участком — это втрое
// дешевле отдельных струй. Рисуется пучок отдельными узкими картинками
// струй: одна общая картинка была бы почти пустой, а смешивание пикселей
// идёт по всей картинке, включая прозрачные. kind: 0–2 — рисунки пучков
// дальнего слоя, 3–5 — среднего, 6–7 — ближние струи двух длин
static const uint8_t KIND_LAYER[8] = {0, 0, 0, 1, 1, 1, 2, 2};

// Пучок: сколько струй, сдвиг головы каждой от головы первой (первая ниже
// всех), длина (0 — короткая картинка слоя, 1 — длинная) и яркость
struct Cluster {
  uint8_t n;
  int8_t ox[4], oy[4];
  uint8_t var[4];
  uint8_t k[4];
};

static const Cluster &cluster(int kind) {
  static Cluster tab[8];
  static bool ready = false;
  if (!ready) {
    ready = true;
    for (int kd = 0; kd < 8; kd++) {
      Cluster &c = tab[kd];
      const int layer = KIND_LAYER[kd];
      if (layer == 2) {
        c.n = 1;
        c.ox[0] = c.oy[0] = 0;
        c.var[0] = (uint8_t) (kd - 6);
        c.k[0] = 255;
        continue;
      }
      // У каждого рисунка свой постоянный разброс — пучки не похожи друг на друга
      uint32_t seed = 2654435761u * (uint32_t) (kd + 1);
      auto r = [&seed](int lo, int hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (int) ((seed >> 8) % (uint32_t) (hi - lo + 1));
      };
      c.n = layer == 0 ? 4 : 3;
      c.ox[0] = c.oy[0] = 0;
      c.var[0] = (uint8_t) r(0, 1);
      c.k[0] = 255;
      int side = r(0, 1) ? 1 : -1;
      for (int m = 1; m < c.n; m++) {
        // Через одну — по разные стороны от первой, дальше и выше
        const int far = (m + 1) / 2;
        c.ox[m] = (int8_t) (side * (far == 1 ? r(layer == 0 ? 6 : 9, layer == 0 ? 13 : 17)
                                              : r(layer == 0 ? 15 : 20, layer == 0 ? 24 : 30)));
        c.oy[m] = (int8_t) -r(8 + (far - 1) * 14, (layer == 0 ? 30 : 34) + (far - 1) * 14);
        c.var[m] = (uint8_t) r(0, 1);
        c.k[m] = (uint8_t) r(165, 255);
        side = -side;
      }
    }
  }
  return tab[kind];
}

// Струя: сглаженный отрезок от хвоста (почти прозрачного) к голове (яркой),
// к голове чуть толще — там сама капля. Наклон tan_a — на сколько px струя
// сдвигается вбок за 1 px вниз. Маска A8, точка привязки — голова
static void draw_streak(FxSprite &s, float len, float thick, float tan_a) {
  const float c = 1.0f / sqrtf(1.0f + tan_a * tan_a), sn = tan_a * c;
  const float dx = len * sn, dy = len * c;
  const float pad = thick * 0.5f + 1.5f;
  const int w = (int) ceilf(fabsf(dx) + 2 * pad), h = (int) ceilf(dy + 2 * pad);
  if (!sprite_init(s, w, h, false))
    return;
  const float hx = dx >= 0 ? w - pad : pad, hy = h - pad;
  const float tx = hx - dx, ty = hy - dy;
  s.ax = (int) hx;
  s.ay = (int) hy;
  const float l2 = len * len;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      // Далеко от струи — пусто, подвыборки не нужны
      {
        const float px = x + 0.5f, py = y + 0.5f;
        const float t = clamp01(((px - tx) * dx + (py - ty) * dy) / l2);
        const float ex = tx + t * dx - px, ey = ty + t * dy - py;
        const float reach = thick * 0.5f + 0.75f;
        if (ex * ex + ey * ey > reach * reach)
          continue;
      }
      float acc = 0;
      for (int sy = 0; sy < 3; sy++) {
        for (int sx = 0; sx < 3; sx++) {
          const float px = x + (sx + 0.5f) / 3.0f, py = y + (sy + 0.5f) / 3.0f;
          const float t = clamp01(((px - tx) * dx + (py - ty) * dy) / l2);
          const float ex = tx + t * dx - px, ey = ty + t * dy - py;
          const float half = thick * 0.5f * (0.55f + 0.45f * t * t);
          if (ex * ex + ey * ey > half * half)
            continue;
          // Вдоль струи: хвост мягко тает, у головы ярче всего
          acc += 0.12f + 0.88f * t * sqrtf(t);
        }
      }
      s.buf[y * w + x] = (uint8_t) std::min(255.0f, acc / 9.0f * 255.0f + 0.5f);
    }
  }
  lv_image_cache_drop(&s.dsc);
}

// Капля на стекле, освещённая сверху: тёмное полупрозрачное тело, у края
// плотнее; снизу светлый серп — свет проходит каплю, как линзу, и
// собирается у нижнего края; сверху слева — блик. egg — сползающая капля:
// низ круглый, верх вытянут вверх, как у настоящей, а за ней тянется тонкая
// мокрая дорожка, тающая кверху. RGB565A8, точка привязки — центр круглой
// части
static void draw_drop(FxSprite &s, int d, bool egg, int pal) {
  const float r = d * 0.5f;
  const float top = egg ? r * 1.8f : r;
  const float trail = egg ? d * 1.5f : 0.0f;
  const int w = d + 2, h = (int) ceilf(r + top + trail) + 2;
  if (!sprite_init(s, w, h, true))
    return;
  const float cx = w * 0.5f, cy = h - 1 - r, ytop = cy - top;
  s.ax = (int) cx;
  s.ay = (int) cy;
  const lv_color_t body = lv_color_hex(DROP_BODY[pal]), glow = lv_color_hex(DROP_GLOW[pal]);
  const float bR = body.red, bG = body.green, bB = body.blue;
  const float gR = glow.red, gG = glow.green, gB = glow.blue;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + w * h * 2;
  const int SS = 3;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float aR = 0, aG = 0, aB = 0, aA = 0;
      for (int sy = 0; sy < SS; sy++) {
        for (int sx = 0; sx < SS; sx++) {
          const float px = x + (sx + 0.5f) / SS, py = y + (sy + 0.5f) / SS;
          if (py < ytop) {
            // Мокрая дорожка над сползающей каплей: узкая, к концу тает
            const float t = (ytop - py) / std::max(trail, 1.0f);
            const float half = std::max(0.55f, r * 0.19f * (1.0f - 0.5f * t));
            if (t > 1.0f || std::fabs(px - cx) > half)
              continue;
            const float a = 0.36f * (1.0f - t) * (1.0f - t);
            aR += (bR + (gR - bR) * 0.45f) * a;
            aG += (bG + (gG - bG) * 0.45f) * a;
            aB += (bB + (gB - bB) * 0.45f) * a;
            aA += a;
            continue;
          }
          // Координаты на «диске» капли: u вправо, v вниз, край — 1
          float u, v;
          if (py >= cy) {
            u = (px - cx) / r;
            v = (py - cy) / r;
          } else {
            const float vv = (py - cy) / top;  // −1 у макушки
            // К макушке сползающая капля сужается
            u = (px - cx) / (r * (1.0f - (egg ? 0.22f : 0.0f) * -vv));
            v = vv;
          }
          const float rr2 = u * u + v * v;
          if (rr2 > 1.0f)
            continue;
          const float rr = sqrtf(rr2);
          // Тело: в середине прозрачнее, к краю плотнее
          float a = 0.40f + 0.42f * rr2 * rr2;
          float cr = bR, cg = bG, cb = bB;
          // Свет снизу: серп у нижнего края и мягкое свечение низа
          const float ring = smooth(0.50f, 0.97f, rr) * smooth(0.0f, 0.80f, v);
          const float low = smooth(-0.15f, 1.0f, v) * 0.32f * (1.0f - 0.4f * rr);
          const float g = clamp01(ring + low);
          cr += (gR - cr) * g;
          cg += (gG - cg) * g;
          cb += (gB - cb) * g;
          a = std::max(a, g * 0.95f);
          // Тонкий светлый ободок сверху — отражение на краю
          const float rim = smooth(0.86f, 0.99f, rr) * smooth(0.15f, 0.9f, -v) * 0.45f;
          cr += (gR - cr) * rim;
          cg += (gG - cg) * rim;
          cb += (gB - cb) * rim;
          a = std::max(a, rim);
          // Блик сверху слева: вытянутый вдоль края овал
          const float hx = (u + 0.34f) * 0.82f + (v + 0.40f) * 0.57f;
          const float hy = -(u + 0.34f) * 0.57f + (v + 0.40f) * 0.82f;
          // Мягкий край блика: (1 − d²)³ — почти как гауссов, но без экспоненты
          const float d2 = hx * hx / (0.36f * 0.36f) + hy * hy / (0.22f * 0.22f);
          const float sp1 = d2 < 1.0f ? 1.0f - d2 : 0.0f;
          const float spec = sp1 * sp1 * sp1;
          cr += (255 - cr) * spec;
          cg += (255 - cg) * spec;
          cb += (255 - cb) * spec;
          a = std::max(a, spec);
          aR += cr * a;
          aG += cg * a;
          aB += cb * a;
          aA += a;
        }
      }
      const int i = y * w + x;
      if (aA <= 0) {
        rgb[i] = 0;
        alpha[i] = 0;
        continue;
      }
      const lv_color_t c = lv_color_make((uint8_t) (aR / aA), (uint8_t) (aG / aA), (uint8_t) (aB / aA));
      rgb[i] = lv_color_to_u16(c);
      alpha[i] = (uint8_t) std::min(255.0f, aA / (SS * SS) * 255.0f + 0.5f);
    }
  }
  lv_image_cache_drop(&s.dsc);
}

// Бусинка — крошечная капелька на стекле: светлая точка с мягким краем,
// ловит свет. RGB565A8 5×5, точка привязки — середина
static void draw_bead(FxSprite &s, int pal) {
  const int n = 5;
  if (!sprite_init(s, n, n, true))
    return;
  s.ax = 2;
  s.ay = 2;
  const lv_color_t glow = lv_color_hex(DROP_GLOW[pal]);
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + n * n * 2;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float acc = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          const float u = (x + (sx + 0.5f) / 4 - 2.5f) / 2.0f, v = (y + (sy + 0.5f) / 4 - 2.5f) / 2.0f;
          acc += clamp01(1.0f - sqrtf(u * u + v * v));
        }
      const float k = acc / 16.0f;
      const float wt = clamp01(k * 1.8f - 0.6f);  // середина — почти белая
      const lv_color_t c = lv_color_make((uint8_t) (glow.red + (255 - glow.red) * wt),
                                         (uint8_t) (glow.green + (255 - glow.green) * wt),
                                         (uint8_t) (glow.blue + (255 - glow.blue) * wt));
      rgb[y * n + x] = lv_color_to_u16(c);
      alpha[y * n + x] = (uint8_t) std::min(255.0f, k * 1.7f * 255.0f);
    }
  }
  lv_image_cache_drop(&s.dsc);
}

// Капелька брызг: мягкая точка 3×3; вспышка удара — сплюснутый овал 9×3
static void draw_dot(FxSprite &s, int w, int h) {
  if (!sprite_init(s, w, h, false))
    return;
  s.ax = w / 2;
  s.ay = h / 2;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const float u = (x + 0.5f - w * 0.5f) / (w * 0.5f), v = (y + 0.5f - h * 0.5f) / (h * 0.5f);
      const float k = clamp01(1.15f - sqrtf(u * u + v * v));
      s.buf[y * w + x] = (uint8_t) (255.0f * std::min(1.0f, k * 1.6f));
    }
  }
  lv_image_cache_drop(&s.dsc);
}

// ---------------------------------------------------------------------------

bool WeatherFx::visible_(int x, int y, int w, int h) const {
  // Ближайшая к центру экрана точка прямоугольника — внутри круга?
  const float nx = std::min(std::max(CX, (float) x), (float) (x + w - 1));
  const float ny = std::min(std::max(CY, (float) y), (float) (y + h - 1));
  return (nx - CX) * (nx - CX) + (ny - CY) * (ny - CY) < RAD * RAD;
}

void WeatherFx::streak_sprites_(int q) {
  this->streak_q_ = q;
  const float t = tanf(q * ANG_STEP);
  for (int k = 0; k < NSTV; k++)
    draw_streak(this->streak_[k], LAYERS[k / 2].len[k % 2], LAYERS[k / 2].thick, t);
}

void WeatherFx::glass_sprites_(int pal) {
  if (this->drop_ok_[pal])
    return;
  // Самая мелкая — бусинка; сползают только капли от 12 px, у мелких
  // вытянутая не нужна
  draw_bead(this->drop_[pal][0][0], pal);
  draw_bead(this->drop_[pal][0][1], pal);
  for (int s = 1; s < NGSZ; s++) {
    draw_drop(this->drop_[pal][s][0], GLASS_D[s], false, pal);
    draw_drop(this->drop_[pal][s][1], GLASS_D[s], s >= 3, pal);
  }
  this->drop_ok_[pal] = true;
}

// Градина: белёсый ледяной шарик с бликом
void WeatherFx::hail_sprite_() {
  if (this->hail_spr_.buf)
    return;
  const int d = 8;
  FxSprite &s = this->hail_spr_;
  if (!sprite_init(s, d, d, true))
    return;
  s.ax = d / 2;
  s.ay = d / 2;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(s.buf);
  uint8_t *alpha = s.buf + d * d * 2;
  for (int y = 0; y < d; y++) {
    for (int x = 0; x < d; x++) {
      const float u = (x + 0.5f - d * 0.5f) / (d * 0.5f), v = (y + 0.5f - d * 0.5f) / (d * 0.5f);
      const float rr = sqrtf(u * u + v * v);
      const float cov = clamp01((1.0f - rr) * d * 0.5f + 0.5f);
      const float spec = expf(-((u + 0.35f) * (u + 0.35f) + (v + 0.35f) * (v + 0.35f)) / 0.08f);
      const float shade = 0.72f + 0.28f * (1.0f - rr);
      const int base_r = (int) (200 * shade), base_g = (int) (222 * shade), base_b = (int) (238 * shade);
      const lv_color_t c = lv_color_make((uint8_t) (base_r + (255 - base_r) * spec),
                                         (uint8_t) (base_g + (255 - base_g) * spec),
                                         (uint8_t) (base_b + (255 - base_b) * spec));
      rgb[y * d + x] = lv_color_to_u16(c);
      alpha[y * d + x] = (uint8_t) (cov * 245);
    }
  }
  lv_image_cache_drop(&s.dsc);
}

void WeatherFx::rain_hide_() {
  for (auto &s : this->rs_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (int j = 0; j < NSPL; j++) {
    this->mark_(this->sps_[j], false, 0, 0, 1, 1);
    this->splife_[j] = 0;
  }
  for (int i = 0; i < NGL; i++) {
    this->mark_(this->gds_[i], false, 0, 0, 1, 1);
    this->gdst_[i] = 0;
  }
  for (int j = 0; j < NBD; j++) {
    this->mark_(this->bds_[j], false, 0, 0, 1, 1);
    this->bdlife_[j] = 0;
  }
}

void WeatherFx::rain_apply_(const Params &p, bool relayout) {
  const int cnt = std::min(std::max(p.count, 0), ND);
  this->hail_ = p.mode == 4;
  // Дождь (и дождь в мокром снеге) — каплями на стекле, если так выбрано.
  // Град всегда падает
  this->glass_on_ = !this->hail_ && p.rain_style == 0;
  // В мокром снеге дождь вдвое слабее
  this->nd_ = (p.mode == 1 || p.mode == 4) ? cnt : (p.mode == 3 ? cnt / 2 : 0);
  this->pal_ = p.dim ? 1 : 0;
  for (int i = 0; i < 3; i++)
    this->c_streak_[i] = lv_color_hex(STREAK_C[this->pal_][i]);
  this->c_splash_ = lv_color_hex(SPLASH_C[this->pal_]);
  // Струй (пучков) и градин — по силе дождя: это около двух струй на единицу
  // силы. На стекле струй нет
  const int nrs = this->nd_ == 0 || this->glass_on_ ? 0 : std::min(NRS, this->nd_);
  // Капель на стекле: в стиле «на стекле» — много, при падающем дожде —
  // несколько, от ударов ближних капель
  this->ngl_max_ = this->nd_ == 0 || this->hail_ ? 0
                   : this->glass_on_           ? std::min(NGL, this->nd_ + 10)
                                               : std::min(NGL, 4 + this->nd_ / 3);
  if (this->ngl_max_)
    this->glass_sprites_(this->pal_);
  if (!this->dot_.buf) {
    draw_dot(this->dot_, 3, 3);
    draw_dot(this->flash_, 9, 3);
  }
  if (this->hail_)
    this->hail_sprite_();
  if (nrs && !this->streak_[0].buf)
    this->streak_sprites_(0);
  // Слой погоды виден, только если на нём что-то есть — теперь и дождь
  this->update_root_();
  if (!relayout)
    return;

  // Новая погода — всё с чистого листа: струи сразу по всему экрану, на
  // стекле уже сидит часть капель
  this->rain_hide_();
  this->nrs_ = nrs;
  for (int i = 0; i < nrs; i++) {
    // Слои вперемешку: из каждых 10 — 4 пучка дальних, 3 пучка средних и
    // 3 ближние струи; рисунки пучков чередуются
    const int m = i % 10, v = i + i / 10;
    this->rkind_[i] = (uint8_t) (this->hail_ ? 6 : (m < 4 ? v % 3 : (m < 7 ? 3 + v % 3 : 6 + v % 2)));
    this->rland_[i] = false;
    this->respawn_streak_(i, this->tan_a_, true);
  }
  this->gd_spawn_ms_ = 0;
  if (this->glass_on_) {
    for (int n = 0; n < this->ngl_max_ / 2; n++) {
      const float a = rnd_(0, 628) / 100.0f, d = sqrtf(rnd_(0, 1000) / 1000.0f) * 200.0f;
      const int i = this->glass_spawn_(CX + d * cosf(a), CY + d * sinf(a), rnd_(1, 5), rnd_(1500, 9000));
      if (i >= 0)
        this->gdsz_[i] = this->gdtg_[i];
    }
  }
}

void WeatherFx::respawn_streak_(int i, float tan_a, bool anywhere) {
  const RainLayer &l = LAYERS[KIND_LAYER[this->rkind_[i]]];
  this->rv_[i] = l.v0 + (l.v1 - l.v0) * rnd_(0, 101) / 100.0f;
  this->rland_[i] = false;
  if (this->hail_)
    this->rv_[i] *= 0.85f;
  // Новая струя — над экраном (или где угодно — при смене погоды). Чтобы
  // снос ветром не уносил струи за край, они начинаются против ветра
  const float y = anywhere ? rnd_(-60, 440) : -rnd_(0, 60);
  this->ry_[i] = y;
  this->rx_[i] = rnd_(10, 456) - tan_a * (CY - y) * 0.9f;
  // Изредка ближняя капля бьёт в стекло и остаётся на нём
  this->rglass_[i] = 0;
  if (!this->hail_ && KIND_LAYER[this->rkind_[i]] == 2 && this->ngl_max_ && rnd_(0, 100) < 8)
    this->rglass_[i] = std::max(y + 40.0f, (float) rnd_(60, 400));
}

void WeatherFx::streaks_(float wind) {
  // Наклон — общий для всех струй: по ветру, в порывах качается
  const float want = wind * MAX_TAN;
  const int q = (int) lroundf(atanf(want) / ANG_STEP);
  if (q != this->streak_q_ && !this->hail_)
    this->streak_sprites_(q);
  this->tan_a_ = this->hail_ ? want : tanf(this->streak_q_ * ANG_STEP);
  // Дальние пучки — тусклые и мелкие — двигаются через кадр, вдвое дальше:
  // глазу незаметно, а участков к перерисовке меньше. Половина — на чётных
  // кадрах, половина — на нечётных, чтобы кадры были ровными
  this->rain_tick_ ^= 1;
  for (int i = 0; i < this->nrs_; i++) {
    const int kind = this->rkind_[i];
    float k = this->k_;
    if (!this->hail_ && KIND_LAYER[kind] == 0) {
      this->rfar_k_[i] += k;
      if ((i & 1) != this->rain_tick_)
        continue;
      k = this->rfar_k_[i];
      this->rfar_k_[i] = 0;
    }
    const float v = this->rv_[i];
    this->ry_[i] += k * v;
    this->rx_[i] += k * v * this->tan_a_;
    // Ударилась о стекло — осталась на нём каплей
    if (this->rglass_[i] > 0 && this->ry_[i] >= this->rglass_[i]) {
      const float x = this->rx_[i], y = this->ry_[i];
      if (visible_((int) x - 6, (int) y - 6, 12, 12) && !this->excluded_((int) x - 8, (int) y - 8, 16, 16))
        this->glass_spawn_(x, y, rnd_(2, 5), rnd_(2500, 6000));
      this->respawn_streak_(i, this->tan_a_, false);
    } else {
      // Нижний край круга. Голова коснулась — брызги (у дальних их не видно),
      // градина отскакивает. Струя уходит в землю: ниже края её не видно,
      // и новая начинается, когда за край ушёл и хвост
      const float g = bottom_y(this->rx_[i]) - 6.0f;
      this->rgy_[i] = g;
      if (this->ry_[i] > g && !this->rland_[i]) {
        this->rland_[i] = true;
        if (std::fabs(this->rx_[i] - CX) < 205.0f)
          this->splash_(this->rx_[i], g, this->hail_ ? 3 : KIND_LAYER[kind], wind);
      }
      if (this->rland_[i] || this->rx_[i] < -50 || this->rx_[i] > 516) {
        // Верх пучка (самый высокий хвост) ушёл за край — струя кончилась
        const FxSprite *spr[4];
        lv_area_t r[4];
        lv_opa_t o[4];
        const int n = this->streak_parts_(i, spr, r, o);
        int top = 10000;
        for (int m = 0; m < n; m++)
          top = std::min(top, (int) r[m].y1);
        if (this->hail_ || top > g || this->rx_[i] < -50 || this->rx_[i] > 516)
          this->respawn_streak_(i, this->tan_a_, false);
      }
    }
    // Участок к перерисовке — общий на весь пучок
    const FxSprite *spr[4];
    lv_area_t r[4];
    lv_opa_t o[4];
    const int n = this->streak_parts_(i, spr, r, o);
    lv_area_t u = r[0];
    for (int m = 1; m < n; m++)
      lv_area_join(&u, &u, &r[m]);
    const int w = lv_area_get_width(&u), h = lv_area_get_height(&u);
    this->mark_(this->rs_[i], spr[0]->buf && this->visible_(u.x1, u.y1, w, h) && !this->excluded_(u.x1, u.y1, w, h),
                u.x1, u.y1, w, h);
  }
}

int WeatherFx::streak_parts_(int i, const FxSprite **spr, lv_area_t *rect, lv_opa_t *opa) const {
  const int x0 = (int) this->rx_[i], y0 = (int) this->ry_[i];
  if (this->hail_) {
    const FxSprite &s = this->hail_spr_;
    spr[0] = &s;
    rect[0] = {x0 - s.ax, y0 - s.ay, x0 - s.ax + (int32_t) s.dsc.header.w - 1, y0 - s.ay + (int32_t) s.dsc.header.h - 1};
    opa[0] = LV_OPA_COVER;
    return 1;
  }
  const int kind = this->rkind_[i], layer = KIND_LAYER[kind];
  const Cluster &c = cluster(kind);
  for (int m = 0; m < c.n; m++) {
    const FxSprite &s = this->streak_[layer * 2 + c.var[m]];
    spr[m] = &s;
    const int x = x0 + c.ox[m] - s.ax, y = y0 + c.oy[m] - s.ay;
    rect[m] = {x, y, x + (int32_t) s.dsc.header.w - 1, y + (int32_t) s.dsc.header.h - 1};
    opa[m] = (lv_opa_t) (LAYERS[layer].opa * c.k[m] / 255);
  }
  return c.n;
}

// kind: 0 — дальняя капля (брызг не видно), 1 — средняя, 2 — ближняя,
// 3 — градина (отскакивает)
void WeatherFx::splash_(float x, float y, int kind, float wind) {
  if (kind == 0)
    return;
  auto slot = [this]() {
    for (int j = 0; j < NSPL; j++)
      if (this->splife_[j] <= 0)
        return j;
    return -1;
  };
  auto add = [&](int sk, float vx, float vy, float life) {
    const int j = slot();
    if (j < 0)
      return;
    this->spk_[j] = (uint8_t) sk;
    this->spx_[j] = x;
    this->spy_[j] = y - (sk == 1 ? 0 : 2);
    this->spvx_[j] = vx;
    this->spvy_[j] = vy;
    this->splife_[j] = life;
    this->spmax_[j] = life;
  };
  if (kind == 3) {
    add(2, rnd_(-14, 15) / 10.0f + wind, -rnd_(20, 33) / 10.0f, rnd_(550, 750));
    return;
  }
  if (kind == 1 && rnd_(0, 100) < 45)
    return;
  // Вспышка удара и капельки веером вверх и в стороны
  if (kind == 2)
    add(1, 0, 0, 110);
  // Ближняя капля — корона из 3–5 капелек, взлетают на 10–20 px; средняя —
  // одна-две пониже
  const int n = kind == 2 ? rnd_(3, 6) : rnd_(1, 3);
  for (int m = 0; m < n; m++) {
    const float side = (m % 2 ? 1.0f : -1.0f) * (kind == 2 ? rnd_(8, 30) : rnd_(5, 18)) / 10.0f;
    const float up = (kind == 2 ? rnd_(38, 60) : rnd_(22, 36)) / 10.0f;
    add(0, side + wind * 0.6f, -up, kind == 2 ? rnd_(380, 620) : rnd_(260, 420));
  }
}

void WeatherFx::splashes_() {
  const float k = this->k_;
  for (int j = 0; j < NSPL; j++) {
    if (this->splife_[j] <= 0) {
      this->mark_(this->sps_[j], false, 0, 0, 1, 1);
      continue;
    }
    this->splife_[j] -= this->dt_ms_;
    this->spx_[j] += k * this->spvx_[j];
    this->spy_[j] += k * this->spvy_[j];
    this->spvy_[j] += k * 0.85f;
    // Градина отскакивает ещё раз, слабее
    if (this->spk_[j] == 2 && this->spvy_[j] > 0 && this->spy_[j] > bottom_y(this->spx_[j]) - 6.0f) {
      this->spy_[j] = bottom_y(this->spx_[j]) - 6.0f;
      this->spvy_[j] *= -0.35f;
    }
    // Гаснут к концу жизни; вспышка — сразу
    const float f = clamp01(this->splife_[j] / this->spmax_[j]);
    const lv_opa_t opa = (lv_opa_t) (255.0f * (this->spk_[j] == 1 ? f : std::min(1.0f, f * 2.2f)));
    const FxSprite &sp = this->spk_[j] == 2 ? this->hail_spr_ : (this->spk_[j] == 1 ? this->flash_ : this->dot_);
    const int w = sp.dsc.header.w, h = sp.dsc.header.h;
    const int x = (int) this->spx_[j] - sp.ax, y = (int) this->spy_[j] - sp.ay;
    const bool on = this->splife_[j] > 0 && sp.buf && this->visible_(x, y, w, h) && !this->excluded_(x, y, w, h);
    Spot &s = this->sps_[j];
    // Стоит на месте, но гаснет — перерисовать и так
    if (on && s.on && s.a.x1 == x && s.a.y1 == y && opa != this->spopa_[j])
      this->invalidate_(s.a);
    this->spopa_[j] = opa;
    this->mark_(s, on, x, y, w, h);
  }
}

// Сколько капель стало после слияния двух: по площади
static int merged_size(int a, int b) {
  const float d = sqrtf((float) (GLASS_D[a] * GLASS_D[a] + GLASS_D[b] * GLASS_D[b]));
  for (int s = 0; s < 7; s++)
    if (GLASS_D[s] >= d * 0.92f)
      return s;
  return 6;
}

void WeatherFx::glass_mark_(int i) {
  const bool slide = this->gdst_[i] == 2 && this->gdsz_[i] >= 3;
  const FxSprite &sp = this->drop_[this->pal_][this->gdsz_[i]][slide ? 1 : 0];
  const int w = sp.dsc.header.w, h = sp.dsc.header.h;
  const int x = (int) this->gdx_[i] - sp.ax, y = (int) this->gdy_[i] - sp.ay;
  this->mark_(this->gds_[i], this->gdst_[i] != 0 && sp.buf && this->visible_(x, y, w, h) && !this->excluded_(x, y, w, h),
              x, y, w, h);
}

// Капля попала на стекло: сливается с соседней, если задела её, иначе
// садится отдельно (если на стекле ещё есть место). Вернёт номер капли или −1
int WeatherFx::glass_spawn_(float x, float y, int size, int life_ms) {
  const float r = GLASS_D[size] * 0.5f;
  for (int j = 0; j < NGL; j++) {
    if (this->gdst_[j] != 1 && this->gdst_[j] != 3)
      continue;
    const float rj = GLASS_D[this->gdtg_[j]] * 0.5f;
    const float dx = x - this->gdx_[j], dy = y - this->gdy_[j];
    if (dx * dx + dy * dy < (r + rj) * (r + rj) * 0.72f) {
      // Сливаются: капля растёт и чуть сдвигается к упавшей
      const int ns = merged_size(this->gdtg_[j], size);
      const float wgt = (float) (GLASS_D[size] * GLASS_D[size]) / (GLASS_D[ns] * GLASS_D[ns]);
      this->gdx_[j] += dx * wgt;
      this->gdy_[j] += dy * wgt;
      this->gdtg_[j] = (uint8_t) ns;
      this->gdst_[j] = 1;
      this->gdlife_[j] = std::max(this->gdlife_[j], (float) life_ms);
      if (ns >= 4 && this->gdslide_[j] <= 0)
        this->gdslide_[j] = rnd_(250, 1800);
      this->glass_mark_(j);
      return j;
    }
  }
  int alive = 0, free_i = -1;
  for (int j = 0; j < NGL; j++) {
    if (this->gdst_[j])
      alive++;
    else if (free_i < 0)
      free_i = j;
  }
  if (free_i < 0 || alive >= this->ngl_max_)
    return -1;
  const int i = free_i;
  this->gdx_[i] = x;
  this->gdy_[i] = y;
  this->gdtg_[i] = (uint8_t) size;
  // Появляется не сразу целиком: за пару кадров растекается до своего размера
  this->gdsz_[i] = (uint8_t) std::max(0, size - 2);
  this->gdst_[i] = 1;
  this->gdlife_[i] = life_ms;
  this->gdslide_[i] = size >= 4 ? rnd_(400, 2600) : (size == 3 && rnd_(0, 100) < 20 ? rnd_(1500, 4500) : 0);
  this->gdv_[i] = 0;
  this->gdvt_[i] = 0;
  this->gdmt_[i] = 0;
  this->gdtr_[i] = 0;
  this->glass_mark_(i);
  // При ударе вокруг разлетаются мелкие капельки — от крупной капли чаще
  if (rnd_(0, 100) < (size >= 4 ? 70 : 30)) {
    const int n = rnd_(1, size >= 4 ? 4 : 3);
    for (int m = 0; m < n; m++) {
      const float a = rnd_(0, 628) / 100.0f, dist = r * rnd_(13, 24) / 10.0f + 2.0f;
      this->bead_(x + cosf(a) * dist, y + sinf(a) * dist, size >= 5 && m == 0 ? 1 : 0, rnd_(900, 3500));
    }
  }
  return i;
}

void WeatherFx::bead_(float x, float y, int size, int life_ms) {
  int j = -1;
  float least = 1e9f;
  for (int m = 0; m < NBD; m++) {
    if (this->bdlife_[m] <= 0) {
      j = m;
      break;
    }
    if (this->bdlife_[m] < least) {
      least = this->bdlife_[m];
      j = m;
    }
  }
  this->bdx_[j] = x;
  this->bdy_[j] = y;
  this->bdsz_[j] = (uint8_t) size;
  this->bdlife_[j] = life_ms;
  this->bead_mark_(j);
}

void WeatherFx::bead_mark_(int j) {
  const FxSprite &sp = this->drop_[this->pal_][this->bdsz_[j]][0];
  const int w = sp.dsc.header.w, h = sp.dsc.header.h;
  const int x = (int) this->bdx_[j] - sp.ax, y = (int) this->bdy_[j] - sp.ay;
  this->mark_(this->bds_[j], this->bdlife_[j] > 0 && sp.buf && this->visible_(x, y, w, h) && !this->excluded_(x, y, w, h),
              x, y, w, h);
}

void WeatherFx::glass_(float dt) {
  const float k = this->k_;
  // Новые капли на стекле — тем чаще, чем сильнее дождь
  if (this->glass_on_ && this->ngl_max_) {
    this->gd_spawn_ms_ -= dt;
    if (this->gd_spawn_ms_ <= 0) {
      this->gd_spawn_ms_ = rnd_(50, 151) / 100.0f * 3600.0f / std::max(this->nd_, 4);
      const float a = rnd_(0, 628) / 100.0f, d = sqrtf(rnd_(0, 1000) / 1000.0f) * 205.0f;
      const float x = CX + d * cosf(a), y = CY + d * sinf(a);
      // Размер: чаще мелкие и средние, изредка крупная
      static const uint8_t W[7] = {0, 22, 30, 24, 15, 7, 2};
      int pick = rnd_(0, 100), size = 1;
      for (int s = 1; s < 7; s++) {
        if (pick < W[s]) {
          size = s;
          break;
        }
        pick -= W[s];
      }
      const int g = GLASS_D[size];
      if (!this->excluded_((int) x - g, (int) y - g, 2 * g, 2 * g))
        this->glass_spawn_(x, y, size, rnd_(4000, 9000) + size * 700);
    }
  }
  int sliders = 0;
  for (int i = 0; i < NGL; i++)
    if (this->gdst_[i] == 2)
      sliders++;
  for (int i = 0; i < NGL; i++) {
    const uint8_t st = this->gdst_[i];
    if (!st)
      continue;
    bool changed = false;
    // Растекается при появлении и растёт после слияния — шаг за кадр
    if (this->gdsz_[i] != this->gdtg_[i]) {
      this->gdsz_[i] += this->gdsz_[i] < this->gdtg_[i] ? 1 : -1;
      changed = true;
    }
    if (st == 1) {
      this->gdlife_[i] -= dt;
      if (this->gdslide_[i] > 0) {
        this->gdslide_[i] -= dt;
        if (this->gdslide_[i] <= 0) {
          if (sliders < MAX_SLIDE && this->gdtg_[i] >= 3) {
            this->gdst_[i] = 2;
            this->gdv_[i] = 0;
            this->gdvt_[i] = SLIDE_V[this->gdtg_[i]];
            this->gdmt_[i] = rnd_(150, 400);
            this->gdtr_[i] = rnd_(6, 14);
            sliders++;
            changed = true;
          } else if (this->gdtg_[i] >= 3) {
            this->gdslide_[i] = rnd_(300, 1200);  // ждёт, пока сползут другие
          }
        }
      }
      if (this->gdst_[i] == 1 && this->gdlife_[i] <= 0) {
        this->gdst_[i] = 3;
        this->gdlife_[i] = 0;
      }
    } else if (st == 3) {
      // Высыхает: мельчает на шаг раз в ~0,2 с, пока не исчезнет
      this->gdlife_[i] -= dt;
      if (this->gdlife_[i] <= 0) {
        if (this->gdtg_[i] == 0) {
          this->gdst_[i] = 0;
          this->mark_(this->gds_[i], false, 0, 0, 1, 1);
          continue;
        }
        this->gdtg_[i]--;
        this->gdlife_[i] = rnd_(170, 260);
      }
    } else {
      // Сползает рывками: то задерживается, то разгоняется
      this->gdmt_[i] -= dt;
      if (this->gdmt_[i] <= 0) {
        this->gdmt_[i] = rnd_(120, 380);
        static const float MUL[5] = {0.0f, 0.35f, 0.8f, 1.25f, 1.75f};
        static const uint8_t MW[5] = {12, 22, 34, 22, 10};
        int pick = rnd_(0, 100), m = 2;
        for (int s = 0; s < 5; s++) {
          if (pick < MW[s]) {
            m = s;
            break;
          }
          pick -= MW[s];
        }
        this->gdvt_[i] = SLIDE_V[this->gdtg_[i]] * MUL[m];
      }
      this->gdv_[i] += (this->gdvt_[i] - this->gdv_[i]) * std::min(1.0f, 0.3f * k);
      const float dy = this->gdv_[i] * dt / 1000.0f;
      this->gdy_[i] += dy;
      // Петляет: изредка смещается вбок на долю пикселя
      if (rnd_(0, 100) < 12)
        this->gdx_[i] += rnd_(-10, 11) / 20.0f;
      this->gdtr_[i] -= dy;
      const float r = GLASS_D[this->gdsz_[i]] * 0.5f;
      // След: там, где кончается мокрая дорожка, остаются бусинки — неровно,
      // с пропусками. Капля понемногу теряет воду
      if (this->gdtr_[i] <= 0) {
        this->gdtr_[i] = rnd_(4, 17);
        if (rnd_(0, 100) < 80)
          this->bead_(this->gdx_[i] + rnd_(-15, 16) / 10.0f, this->gdy_[i] - r * 4.6f + rnd_(-2, 3),
                      this->gdsz_[i] >= 5 && rnd_(0, 100) < 35 ? 1 : 0, rnd_(1800, 5500));
        if (rnd_(0, 100) < 9 && this->gdtg_[i] > 2)
          this->gdtg_[i]--;
        if (this->gdtg_[i] < 3) {
          // Обмелела — остановилась
          this->gdst_[i] = 1;
          this->gdlife_[i] = rnd_(1500, 4000);
          this->gdslide_[i] = 0;
        }
      }
      // Вбирает капли и бусинки на пути
      for (int j = 0; j < NGL; j++) {
        if (j == i || this->gdst_[j] == 0 || this->gdst_[j] == 2)
          continue;
        const float rj = GLASS_D[this->gdsz_[j]] * 0.5f;
        const float ddx = this->gdx_[j] - this->gdx_[i], ddy = this->gdy_[j] - this->gdy_[i];
        if (ddy > -r * 0.5f && ddx * ddx + ddy * ddy < (r + rj) * (r + rj) * 0.7f) {
          this->gdst_[j] = 0;
          this->mark_(this->gds_[j], false, 0, 0, 1, 1);
          this->gdtg_[i] = (uint8_t) merged_size(this->gdtg_[i], this->gdsz_[j]);
          this->gdx_[i] += ddx * 0.3f;
        }
      }
      for (int j = 0; j < NBD; j++) {
        if (this->bdlife_[j] <= 0)
          continue;
        const float ddx = this->bdx_[j] - this->gdx_[i], ddy = this->bdy_[j] - this->gdy_[i];
        if (std::fabs(ddx) < r * 0.8f && ddy > -r * 0.6f && ddy < r) {
          this->bdlife_[j] = 0;
          this->mark_(this->bds_[j], false, 0, 0, 1, 1);
        }
      }
      // Сползла за нижний край
      if (this->gdy_[i] - r > bottom_y(this->gdx_[i])) {
        this->gdst_[i] = 0;
        this->mark_(this->gds_[i], false, 0, 0, 1, 1);
        continue;
      }
      changed = true;
    }
    if (changed)
      this->glass_mark_(i);
  }
  // Бусинки высыхают: сначала мельчают, потом исчезают
  for (int j = 0; j < NBD; j++) {
    if (this->bdlife_[j] <= 0)
      continue;
    this->bdlife_[j] -= dt;
    if (this->bdlife_[j] > 0)
      continue;
    if (this->bdsz_[j] > 0) {
      this->bdsz_[j]--;
      this->bdlife_[j] = rnd_(600, 1400);
    }
    this->bead_mark_(j);
  }
}

void WeatherFx::rain_frame_(float wind) {
  if (this->nrs_)
    this->streaks_(wind);
  this->splashes_();
  bool glass = this->ngl_max_ > 0;
  for (int i = 0; i < NGL && !glass; i++)
    glass = this->gdst_[i] != 0;
  for (int j = 0; j < NBD && !glass; j++)
    glass = this->bdlife_[j] > 0;
  if (glass)
    this->glass_(this->dt_ms_);
}

void WeatherFx::rain_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip, bool glass) {
  lv_draw_image_dsc_t im;
  lv_draw_image_dsc_init(&im);
  lv_area_t a;
  auto place = [&](const Spot &s) {
    if (!s.on)
      return false;
    a = {(int32_t) (s.a.x1 + oc.x1), (int32_t) (s.a.y1 + oc.y1), (int32_t) (s.a.x2 + oc.x1),
         (int32_t) (s.a.y2 + oc.y1)};
    return hit_area(a, clip);
  };
  auto draw = [&](const FxSprite &sp, lv_opa_t opa) {
    if (!sp.buf)
      return;
    im.src = &sp.dsc;
    im.opa = opa;
    im.image_area = a;
    lv_draw_image(layer, &im, &a);
  };
  if (!glass) {
    // Струи — цветом своего слоя; во вспышке молнии все белые. Ниже края
    // круга струя ушла в землю — там её не рисовать
    const bool flash = this->bolt_on_;
    const lv_area_t keep = layer->_clip_area;
    for (int i = 0; i < this->nrs_; i++) {
      if (!place(this->rs_[i]))
        continue;
      const FxSprite *spr[4];
      lv_area_t r[4];
      lv_opa_t o[4];
      const int n = this->streak_parts_(i, spr, r, o);
      if (!this->hail_)
        im.recolor = flash ? lv_color_hex(FLASH_C) : this->c_streak_[KIND_LAYER[this->rkind_[i]]];
      const int32_t gy = (int32_t) this->rgy_[i] + oc.y1;
      if (gy < layer->_clip_area.y2)
        layer->_clip_area.y2 = gy;
      for (int m = 0; m < n && layer->_clip_area.y2 >= layer->_clip_area.y1; m++) {
        a = {r[m].x1 + oc.x1, r[m].y1 + oc.y1, r[m].x2 + oc.x1, r[m].y2 + oc.y1};
        if (hit_area(a, layer->_clip_area))
          draw(*spr[m], flash ? (lv_opa_t) LV_OPA_COVER : o[m]);
      }
      layer->_clip_area = keep;
    }
    // Брызги и отскоки
    im.recolor = this->c_splash_;
    for (int j = 0; j < NSPL; j++) {
      if (!place(this->sps_[j]))
        continue;
      const uint8_t sk = this->spk_[j];
      draw(sk == 2 ? this->hail_spr_ : (sk == 1 ? this->flash_ : this->dot_), this->spopa_[j]);
    }
    return;
  }
  // Бусинки и капли на стекле — поверх всего остального дождя
  for (int j = 0; j < NBD; j++)
    if (place(this->bds_[j]))
      draw(this->drop_[this->pal_][this->bdsz_[j]][0], LV_OPA_COVER);
  for (int i = 0; i < NGL; i++) {
    if (!place(this->gds_[i]))
      continue;
    const bool slide = this->gdst_[i] == 2 && this->gdsz_[i] >= 3;
    draw(this->drop_[this->pal_][this->gdsz_[i]][slide ? 1 : 0], LV_OPA_COVER);
  }
}

}  // namespace weather_fx
}  // namespace esphome

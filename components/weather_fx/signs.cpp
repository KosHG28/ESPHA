// Сердечки 14 февраля, тюльпаны и мимоза 8 Марта, воздушные шарики 1 июня
// и летучие мыши на Хэллоуин.
//
// Всё — готовые картинки, нарисованные по мере надобности: сердечко —
// глянцевое, с бликом; тюльпан — чашечка из трёх лепестков на стебле с
// листом; мимоза — веточка пушистых жёлтых шариков; воздушный шарик — с
// бликом, узелком и ниточкой. Сердечки и шарики, всплывая, кренятся туда,
// куда их качает; цветы, падая, раскачиваются. Наклоны нарисованы заранее
// (поворачивать картинки при рисовании плата не умеет), каждый наклон —
// когда впервые понадобился. Летучая мышь машет крыльями — четыре кадра
// взмаха, силуэт рисуется её цветом.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

// По набору (2 — сердечки, 3 — цветы, 4 — шарики): кадров наклона,
// наибольший наклон (рад), пикселей на единицу рисунка у дальних и ближних
static const int SIGN_TILTS[5] = {0, 0, 9, 9, 7};
static const float SIGN_MAX_TILT[5] = {0, 0, 0.40f, 0.55f, 0.26f};
static const float SIGN_SCALE[5][2] = {{0, 0}, {0, 0}, {5.5f, 9.5f}, {8.0f, 12.5f}, {5.5f, 8.5f}};
static const int NVAR = 4;  // цветов (у цветов — тюльпан, мимоза, тюльпан, мимоза)
// Летучая мышь: размах полукрыла у дальней и ближней, кадров взмаха
static const float BAT_SPAN[2] = {10.0f, 14.0f};
static const int NBATF = 4;

// Цвет и непрозрачность точки рисунка
struct Px {
  float a, r, g, b;
};

static Px px_col(lv_color_t c, float k, float a = 1.0f) {
  return {a, std::min(255.0f, c.red * k), std::min(255.0f, c.green * k), std::min(255.0f, c.blue * k)};
}

// Подмешать белый блик долей s
static Px px_spec(Px p, float s) {
  p.r += (255.0f - p.r) * s;
  p.g += (255.0f - p.g) * s;
  p.b += (255.0f - p.b) * s;
  return p;
}

// Расстояние от точки до отрезка
static float seg_dist(float x, float y, float x0, float y0, float x1, float y1) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float t = fx_clamp01(((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy));
  const float ex = x0 + t * dx - x, ey = y0 + t * dy - y;
  return sqrtf(ex * ex + ey * ey);
}

// Сердечко: кривая (x² + y² − 1)³ = x² y³; объём — от глубины внутри
// кривой, свет сверху слева, белый блик на левой половинке
static Px heart_at(float X, float Y, lv_color_t c) {
  const float x = X, y = Y + 0.12f;
  const float q = x * x + y * y - 1.0f;
  const float f = q * q * q - x * x * y * y * y;
  if (f > 0)
    return {0, 0, 0, 0};
  const float h = sqrtf(std::min(1.0f, -f));
  const float rim = 1.0f - h;
  const float br = 0.68f + 0.32f * h + 0.16f * (-0.5f * x + 0.6f * y) - 0.28f * rim * rim * rim * rim;
  const float sx = (x + 0.46f) / 0.27f, sy = (y - 0.56f) / 0.16f;
  return px_spec(px_col(c, br), 0.85f * (1.0f - fx_smooth(0.1f, 1.0f, sx * sx + sy * sy)));
}

// Тюльпан: чашечка из трёх лепестков (средний светлее), стебель и лист
static Px tulip_at(float X, float Y, lv_color_t petal, lv_color_t leaf) {
  const float ax = fabsf(X);
  if (Y > 0.02f && Y < 0.98f) {
    const float lo = (0.42f - Y) / 0.40f;
    const float hw = Y < 0.42f ? 0.40f * sqrtf(fx_clamp01(1.0f - lo * lo)) : 0.40f - 0.06f * (Y - 0.42f) / 0.56f;
    const float top = 0.96f - 0.17f * fabsf(sinf(3.14159265f * X / 0.30f)) - 0.12f * (ax / 0.4f) * (ax / 0.4f);
    if (ax < hw && Y < top) {
      const float mid = ax < 0.15f ? 1.08f : 0.88f;
      const float vert = 0.70f + 0.36f * fx_clamp01((Y - 0.05f) / 0.9f);
      const float edge = 1.0f - 0.25f * fx_smooth(hw - 0.12f, hw, ax);
      const float seam = 1.0f - 0.2f * (1.0f - fx_smooth(0.0f, 0.035f, fabsf(ax - 0.15f)));
      Px p = px_col(petal, mid * vert * edge * seam);
      // Блик на среднем лепестке
      const float sx = (X + 0.04f) / 0.06f, sy = (Y - 0.58f) / 0.2f;
      return px_spec(p, 0.35f * (1.0f - fx_smooth(0.2f, 1.0f, sx * sx + sy * sy)));
    }
  }
  // Лист: длинный, от стебля вверх и вправо, с прожилкой посередине
  {
    const float x0 = 0.03f, y0 = -0.66f, x1 = 0.44f, y1 = 0.02f;
    const float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    const float t = ((X - x0) * dx + (Y - y0) * dy) / (len * len);
    const float off = ((X - x0) * dy - (Y - y0) * dx) / len;
    if (t > 0 && t < 1) {
      const float w = 0.13f * powf(sinf(3.14159265f * t), 0.8f);
      if (fabsf(off) < w)
        return px_col(leaf, fabsf(off) < 0.012f ? 0.75f : (off > 0 ? 1.0f : 0.82f));
    }
  }
  // Стебель — чуть изогнутый
  const float stem_x = 0.05f * (-Y) * (-Y) * (Y < 0 ? 1.0f : 0.0f);
  if (Y < 0.1f && Y > -1.0f && fabsf(X - stem_x) < 0.045f)
    return px_col(leaf, X > stem_x ? 0.8f : 1.0f);
  return {0, 0, 0, 0};
}

// Мимоза: пушистые шарики на веточке и два перистых листа
static const float MIMOSA_BALLS[10][3] = {
    {0.0f, 0.84f, 0.16f},   {-0.22f, 0.70f, 0.15f}, {0.22f, 0.67f, 0.15f}, {-0.05f, 0.53f, 0.17f},
    {-0.35f, 0.44f, 0.14f}, {0.30f, 0.40f, 0.15f},  {0.06f, 0.27f, 0.16f}, {-0.24f, 0.17f, 0.13f},
    {0.28f, 0.12f, 0.13f},  {0.0f, 0.0f, 0.12f},
};

static Px mimosa_at(float X, float Y, lv_color_t ball, lv_color_t leaf) {
  // Шарики: сверху вниз — нижние лежат поверх
  for (int i = 9; i >= 0; i--) {
    const float *b = MIMOSA_BALLS[i];
    const float dx = X - b[0], dy = Y - b[1];
    if (fabsf(dx) > b[2] || fabsf(dy) > b[2])
      continue;
    const float d = sqrtf(dx * dx + dy * dy);
    if (d < b[2]) {
      const float k = 0.78f + 0.32f * (1.0f - d / b[2]) + 0.15f * (-dx + dy) / b[2];
      return px_col(ball, k);
    }
  }
  // Веточка с черенками к шарикам
  const float stem_x = -0.06f * (1.0f - Y) * 0.5f;
  if (Y < 0.8f && Y > -1.0f && fabsf(X - stem_x) < 0.035f)
    return px_col(leaf, 0.85f);
  for (int i = 0; i < 9; i += 2) {
    const float *b = MIMOSA_BALLS[i];
    if (seg_dist(X, Y, -0.03f, b[1] - 0.18f, b[0], b[1]) < 0.025f)
      return px_col(leaf, 0.8f);
  }
  // Перистые листья: зубчики — чередованием поперёк
  static const float LEAVES[2][4] = {{-0.03f, -0.36f, -0.46f, -0.06f}, {-0.04f, -0.58f, 0.42f, -0.30f}};
  for (const auto &l : LEAVES) {
    const float dx = l[2] - l[0], dy = l[3] - l[1], len = sqrtf(dx * dx + dy * dy);
    const float t = ((X - l[0]) * dx + (Y - l[1]) * dy) / (len * len);
    const float off = ((X - l[0]) * dy - (Y - l[1]) * dx) / len;
    if (t > 0 && t < 1 && fabsf(off) < 0.10f * sinf(3.14159265f * t)) {
      const float comb = 0.65f + 0.35f * fabsf(sinf(t * 38.0f));
      return px_col(leaf, 0.7f + 0.3f * comb, 0.6f + 0.4f * comb);
    }
  }
  return {0, 0, 0, 0};
}

// Воздушный шарик: овал, чуть уже книзу, блик сверху слева и отблеск снизу
// справа, узелок
static Px balloon_at(float X, float Y, lv_color_t c) {
  const float rx = 0.78f * (Y < 0 ? 1.0f - 0.10f * (-Y) : 1.0f);
  const float q = (X / rx) * (X / rx) + Y * Y;
  if (q < 1.0f) {
    const float lx = X + 0.32f, ly = Y - 0.42f;
    const float br = (1.14f - 0.40f * sqrtf(lx * lx + ly * ly)) * (0.82f + 0.18f * sqrtf(1.0f - q));
    Px p = px_col(c, br);
    // Блик — наклонённый овал
    const float u = (X + 0.34f) * 0.82f + (Y - 0.46f) * 0.57f, v = -(X + 0.34f) * 0.57f + (Y - 0.46f) * 0.82f;
    const float s1 = 1.0f - fx_smooth(0.15f, 1.0f, (u / 0.13f) * (u / 0.13f) + (v / 0.24f) * (v / 0.24f));
    const float s2 =
        1.0f - fx_smooth(0.2f, 1.0f,
                         ((X - 0.36f) / 0.09f) * ((X - 0.36f) / 0.09f) + ((Y + 0.48f) / 0.12f) * ((Y + 0.48f) / 0.12f));
    return px_spec(p, std::max(0.9f * s1, 0.3f * s2));
  }
  // Узелок
  if (Y < -0.96f && Y > -1.14f && fabsf(X) < 0.03f + 0.55f * (Y + 1.14f))
    return px_col(c, 0.7f);
  return {0, 0, 0, 0};
}

// Летучая мышь: тело, голова с ушками и два крыла с фестонами по заднему
// краю. Кадр взмаха: крылья вверху, посередине, внизу, посередине
static float bat_at(float X, float Y, int frame) {
  const float ax = fabsf(X);
  // Тело и голова
  if ((X / 0.16f) * (X / 0.16f) + ((Y + 0.05f) / 0.30f) * ((Y + 0.05f) / 0.30f) < 1.0f)
    return 1.0f;
  if (X * X + (Y - 0.30f) * (Y - 0.30f) < 0.15f * 0.15f)
    return 1.0f;
  // Ушки: острые треугольники над головой
  if (Y > 0.36f && Y < 0.58f) {
    const float t = (Y - 0.36f) / 0.22f;
    const float cx = 0.085f + 0.035f * t, w = 0.055f * (1.0f - t);
    if (fabsf(ax - cx) < w)
      return 1.0f;
  }
  // Крылья
  if (ax < 0.10f || ax > 1.0f)
    return 0.0f;
  static const float TIP[NBATF] = {0.62f, 0.18f, -0.30f, 0.18f};
  static const float ARCH[NBATF] = {0.20f, 0.16f, -0.06f, 0.16f};
  const float s = (ax - 0.10f) / 0.90f;
  const float lead = 0.12f + (TIP[frame] - 0.12f) * s + ARCH[frame] * sinf(3.14159265f * s);
  float trail = lead - (0.54f * powf(1.0f - s, 0.75f) + 0.03f);
  // Фестоны: задний край — дугами между «пальцами»
  trail += 0.10f * fabsf(sinf(3.0f * 3.14159265f * s)) * (1.0f - 0.3f * s);
  return (Y < lead && Y > trail) ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------------------

void WeatherFx::signs_apply_() {
  const int set = this->nf_ && this->fset_ >= 2 && this->fset_ <= 4 ? this->fset_ : -1;
  if (set != this->sign_set_) {
    fx_sheet_free(this->sign_sheet_[0]);
    fx_sheet_free(this->sign_sheet_[1]);
    this->sign_set_ = -1;
    if (set > 0) {
      bool ok = true;
      for (int sz = 0; sz < 2; sz++) {
        const float sc = SIGN_SCALE[set][sz];
        int w, h, ax, ay;
        if (set == 4) {
          // Шарик с ниточкой: точка привязки — середина шара
          ax = (int) ceilf(1.25f * sc) + 1;
          ay = (int) ceilf(1.15f * sc) + 1;
          w = 2 * ax + 1;
          h = ay + (int) ceilf(2.85f * sc) + 2;
        } else {
          ax = ay = (int) ceilf((set == 2 ? 1.35f : 1.06f) * sc) + 1;
          w = h = 2 * ax + 1;
        }
        FxSheet &sh = this->sign_sheet_[sz];
        ok = ok && fx_sheet_init(sh, NVAR * SIGN_TILTS[set], w, h, true);
        sh.ax = ax;
        sh.ay = ay;
      }
      if (ok) {
        this->sign_set_ = set;
      } else {
        fx_sheet_free(this->sign_sheet_[0]);
        fx_sheet_free(this->sign_sheet_[1]);
      }
    }
  } else if (this->dim_ != this->sign_dim_) {
    // Другая яркость — другие цвета: картинки нарисуются заново
    fx_sheet_reset(this->sign_sheet_[0]);
    fx_sheet_reset(this->sign_sheet_[1]);
  }
  this->sign_dim_ = this->dim_;
  // Летучие мыши
  if (this->bats_on_ && !this->bat_sheet_[0].buf) {
    for (int sz = 0; sz < 2; sz++) {
      const float S = BAT_SPAN[sz];
      FxSheet &sh = this->bat_sheet_[sz];
      if (!fx_sheet_init(sh, NBATF, 2 * (int) ceilf(S) + 3, (int) ceilf(1.45f * S) + 3, false))
        continue;
      sh.ax = (int) ceilf(S) + 1;
      sh.ay = (int) ceilf(0.78f * S) + 1;
    }
  } else if (!this->bats_on_) {
    fx_sheet_free(this->bat_sheet_[0]);
    fx_sheet_free(this->bat_sheet_[1]);
  }
}

// Нарисовать картинку знака, если её ещё нет
void WeatherFx::sign_render_(int idx, int sz) {
  FxSheet &sh = this->sign_sheet_[sz];
  uint8_t *p = fx_sheet_take(sh, idx);
  if (!p)
    return;
  const int set = this->sign_set_, nt = SIGN_TILTS[set];
  const int var = idx / nt, ti = idx % nt;
  const float tilt = SIGN_MAX_TILT[set] * (2.0f * ti / (nt - 1) - 1.0f);
  const float sc = SIGN_SCALE[set][sz], ct = cosf(tilt), st = sinf(tilt);
  const bool dim = this->dim_;
  const lv_color_t c = set == 2 ? (sz ? this->c_heart_[var] : this->c_heart_s_[var])
                                : (sz ? this->c_set_[set][var] : this->c_set_s_[set][var]);
  lv_color_t green = lv_color_hex(dim ? 0x7BD36A : 0x4E9A3E);
  if (!sz)
    green = lv_color_mix(green, lv_color_black(), dim ? 190 : 140);
  const lv_color_t string = lv_color_hex(dim ? 0xE6E6E6 : 0xAEB0B4);
  // Ниточка шарика висит вниз от узелка, чуть волной, не поворачиваясь
  // вместе с шаром — её тянет вниз
  const float kx = -1.14f * sc * st, ky = 1.14f * sc * ct;  // узелок на картинке
  const float slen = 1.6f * sc;
  uint16_t *rgb = reinterpret_cast<uint16_t *>(p);
  uint8_t *alpha = p + sh.w * sh.h * 2;
  for (int y = 0; y < sh.h; y++) {
    for (int x = 0; x < sh.w; x++) {
      Px acc{0, 0, 0, 0};
      for (int q = 0; q < 4; q++) {
        const float dx = x + 0.25f + 0.5f * (q & 1) - (sh.ax + 0.5f);
        const float dy = y + 0.25f + 0.5f * (q >> 1) - (sh.ay + 0.5f);
        const float X = (dx * ct + dy * st) / sc, Y = -(-dx * st + dy * ct) / sc;
        Px v;
        if (set == 2)
          v = heart_at(X, Y, c);
        else if (set == 3)
          v = var % 2 ? mimosa_at(X, Y, c, green) : tulip_at(X, Y, c, green);
        else
          v = balloon_at(X, Y, c);
        if (set == 4 && v.a <= 0 && dy > ky) {
          // Ниточка: волна, к концу отклоняется против крена
          const float t = (dy - ky) / slen;
          if (t < 1.0f) {
            const float sx = kx + 0.10f * sc * sinf(t * 5.0f + 0.6f) * t - tilt * 0.6f * slen * t;
            if (fabsf(dx - sx) < 0.45f)
              v = px_col(string, 1.0f, 0.85f);
          }
        }
        acc.a += v.a;
        acc.r += v.r * v.a;
        acc.g += v.g * v.a;
        acc.b += v.b * v.a;
      }
      if (acc.a <= 0)
        continue;
      rgb[y * sh.w + x] = lv_color_to_u16(
          lv_color_make((uint8_t) (acc.r / acc.a), (uint8_t) (acc.g / acc.a), (uint8_t) (acc.b / acc.a)));
      alpha[y * sh.w + x] = (uint8_t) std::min(255.0f, acc.a / 4.0f * 255.0f + 0.5f);
    }
  }
  fx_sheet_crop(sh, idx);
  fx_sheet_done(sh, idx);
}

// Знак i: какой кадр (крен — туда, куда его сейчас качает) и где он
void WeatherFx::sign_mark_(int i) {
  const int sz = i % 2;
  FxSheet &sh = this->sign_sheet_[sz];
  if (this->sign_set_ < 0 || !sh.buf) {
    this->mark_(this->fs_[i], false, 0, 0, 1, 1);
    return;
  }
  const int nt = SIGN_TILTS[this->sign_set_];
  const int ti = (int) lroundf((cosf(this->fph_[i]) + 1.0f) * 0.5f * (nt - 1));
  const int base = ((i / 2) % NVAR) * nt;
  int idx = base + ti;
  if (!fx_sheet_ready(sh, idx)) {
    // Новых картинок — не больше пары за кадр, чтобы не было заминок; пока
    // нужный наклон не нарисован — ближайший готовый, а если готовых этого
    // цвета ещё нет (в самом начале) — знак появится кадром-другим позже:
    // прежде в первом кадре рисовались все картинки разом
    if (this->render_budget_ > 0) {
      this->render_budget_--;
      this->sign_render_(idx, sz);
    } else {
      int near = -1;
      for (int d = 1; d < nt && near < 0; d++) {
        if (ti - d >= 0 && fx_sheet_ready(sh, base + ti - d))
          near = base + ti - d;
        else if (ti + d < nt && fx_sheet_ready(sh, base + ti + d))
          near = base + ti + d;
      }
      if (near < 0) {
        this->mark_(this->fs_[i], false, 0, 0, 1, 1);
        return;
      }
      idx = near;
    }
  }
  this->fidx_[i] = (uint8_t) idx;
  // Рамка — по рисунку кадра: у каждого наклона своя
  const lv_area_t a = fx_sheet_area(sh, idx, (int) this->fx_[i], (int) this->fy_[i]);
  const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
  this->mark_(this->fs_[i], !this->excluded_(a.x1, a.y1, w, h), a.x1, a.y1, w, h);
}

void WeatherFx::signs_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  // Сначала дальние (мелкие), потом ближние
  for (int pass = 0; pass < 2; pass++) {
    for (int i = pass; i < this->nf_; i += 2) {
      const Spot &s = this->fs_[i];
      if (!s.on)
        continue;
      const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
      if (fx_hit(a, clip))
        fx_sheet_draw(layer, this->sign_sheet_[pass], this->fidx_[i], a, LV_OPA_COVER);
    }
  }
}

// ---- Летучие мыши ----

void WeatherFx::bat_render_(int sz, int frame) {
  FxSheet &sh = this->bat_sheet_[sz];
  uint8_t *p = fx_sheet_take(sh, frame);
  if (!p)
    return;
  const float S = BAT_SPAN[sz];
  for (int y = 0; y < sh.h; y++) {
    for (int x = 0; x < sh.w; x++) {
      float acc = 0;
      for (int q = 0; q < 9; q++) {
        const float X = (x + ((q % 3) + 0.5f) / 3.0f - (sh.ax + 0.5f)) / S;
        const float Y = -(y + ((q / 3) + 0.5f) / 3.0f - (sh.ay + 0.5f)) / S;
        acc += bat_at(X, Y, frame);
      }
      p[y * sh.w + x] = (uint8_t) (acc / 9.0f * 255.0f + 0.5f);
    }
  }
  fx_sheet_crop(sh, frame);
  fx_sheet_done(sh, frame);
}

// Мышь i в точке (x, y): кадр взмаха — по времени, у каждой свой
void WeatherFx::bat_mark_(int i, int x, int y) {
  const int sz = i % 2;
  FxSheet &sh = this->bat_sheet_[sz];
  if (!sh.buf) {
    this->mark_(this->bat_spot_[i], false, 0, 0, 1, 1);
    return;
  }
  const int frame = (int) ((millis() / 85 + i * 3) % NBATF);
  this->bat_frame_[i] = (uint8_t) frame;
  this->bat_render_(sz, frame);
  const lv_area_t a = fx_sheet_area(sh, frame, x, y);
  const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
  this->mark_(this->bat_spot_[i], !this->excluded_(a.x1, a.y1, w, h), a.x1, a.y1, w, h);
}

void WeatherFx::bats_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  const bool dim = this->dim_;
  for (int i = 0; i < NBAT; i++) {
    const Spot &s = this->bat_spot_[i];
    if (!s.on)
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    const int sz = i % 2;
    fx_sheet_draw(layer, this->bat_sheet_[sz], this->bat_frame_[i], a, LV_OPA_COVER,
                  lv_color_hex(dim ? (sz ? 0xC9B6F0 : 0x9E8CC8) : (sz ? 0x8E7CC3 : 0x5E507F)));
  }
}

}  // namespace weather_fx
}  // namespace esphome

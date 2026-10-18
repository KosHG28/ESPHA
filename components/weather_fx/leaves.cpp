// Листопад: обычные и кленовые листья.
//
// Лист — готовая картинка: силуэт с черешком и прожилками (прожилки чуть
// прозрачнее — на чёрном они темнее). Каждый лист нарисован заранее в 24
// поворотах и трёх наклонах: целиком, вполоборота и почти ребром, — так
// падающий лист раскачивается, кружится и переворачивается, показывая
// тёмную изнанку. Картинка — маска A8, цвет задаётся при рисовании.
//
// Лист падает, как настоящий: качается из стороны в сторону, наклоняясь в
// сторону движения, в крайних точках замедляется. Упав на нижний край
// круга, лежит несколько секунд и тает.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

static const int NLROT = 24;  // поворотов
static const int NLFLIP = 3;  // наклонов: целиком, вполоборота, ребром
static const float FLIP_K[NLFLIP] = {1.0f, 0.65f, 0.36f};
static const float LEAF_R[2] = {10.0f, 14.0f};  // половина длины листа: дальний и ближний
static const int MASTER = 96;                   // образец листа, px
static const float PI_L = 3.14159265f;

// Сглаженный отрезок в образце: v — яркость, w — толщина (в долях образца)
struct LeafSeg {
  float x0, y0, x1, y1, w, v;
};

static float seg_d(const LeafSeg &s, float x, float y) {
  const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
  const float t = fx_clamp01(((x - s.x0) * dx + (y - s.y0) * dy) / (dx * dx + dy * dy));
  const float ex = s.x0 + t * dx - x, ey = s.y0 + t * dy - y;
  return sqrtf(ex * ex + ey * ey);
}

// Край листа. Обычный лист — овал, широкий ближе к черешку, с острым
// кончиком: полуширина зависит только от u (вдоль листа от черешка −1 к
// кончику +1). Кленовый — пять заострённых лопастей с зубцами и глубокими
// выемками: расстояние до края от середины листа зависит только от
// направления
static float plain_half_width(float u) {
  if (u < -0.72f || u > 1.0f)
    return -1.0f;
  const float t = (u + 0.72f) / 1.72f;
  return 0.5f * powf(sinf(PI_L * powf(t, 0.8f)), 0.85f);
}

static const float MAPLE_CX = -0.1f;  // середина кленового листа
static float maple_radius(float th) {
  static const float TH[5] = {0.0f, 0.95f, -0.95f, 1.95f, -1.95f};
  static const float LEN[5] = {1.05f, 0.9f, 0.9f, 0.56f, 0.56f};
  static const float WID[5] = {0.62f, 0.58f, 0.58f, 0.5f, 0.5f};
  float r = 0.36f;
  for (int k = 0; k < 5; k++) {
    const float dx = fabsf(th - TH[k]) / WID[k];
    if (dx >= 1)
      continue;
    // Лопасть заострённая, по бокам у середины — по зубцу
    const float tooth = 0.17f * std::max(0.0f, 1.0f - fabsf(dx - 0.55f) / 0.13f);
    r = std::max(r, LEN[k] * (powf(1.0f - dx, 0.8f) + tooth));
  }
  return r;
}

// Образец листа: маска MASTER×MASTER с пустой рамкой в пиксель (чтобы при
// выборке не проверять границы), лист длиной во весь образец
static const int MP = MASTER + 2;
static void draw_master(uint8_t *m, int shape) {
  LeafSeg veins[16];
  int nv = 0;
  // Черешок — снаружи силуэта, прожилки — внутри
  LeafSeg stem;
  if (shape == 0) {
    stem = {-1.0f, 0, -0.68f, 0, 0.07f, 1.0f};
    veins[nv++] = {-0.72f, 0, 0.9f, 0, 0.05f, 0.5f};
    static const float AT[4] = {-0.45f, -0.15f, 0.15f, 0.45f};
    for (float a : AT) {
      const float l = 0.42f - 0.12f * (a + 0.45f);
      veins[nv++] = {a, 0, a + l * 0.77f, l * 0.64f, 0.035f, 0.65f};
      veins[nv++] = {a, 0, a + l * 0.77f, -l * 0.64f, 0.035f, 0.65f};
    }
  } else {
    stem = {-1.0f, 0, -0.35f, 0, 0.06f, 1.0f};
    static const float ANG[5] = {0.0f, 0.96f, -0.96f, 2.0f, -2.0f};
    static const float LEN[5] = {1.0f, 0.85f, 0.85f, 0.55f, 0.55f};
    for (int k = 0; k < 5; k++)
      veins[nv++] = {MAPLE_CX, 0, MAPLE_CX + LEN[k] * cosf(ANG[k]), LEN[k] * sinf(ANG[k]), 0.045f, 0.5f};
  }
  // Край листа — заранее: по столбцам (обычный) или по 256 направлениям (клён)
  float edge[2 * MASTER];
  float rad[256];
  const float sc = MASTER / 2.0f;
  if (shape == 0) {
    for (int i = 0; i < 2 * MASTER; i++)
      edge[i] = plain_half_width((i + 0.5f) / (2 * sc) - 1.0f);
  } else {
    for (int i = 0; i < 256; i++)
      rad[i] = maple_radius((i + 0.5f) / 256.0f * 2 * PI_L - PI_L);
  }
  memset(m, 0, MP * MP);
  for (int y = 0; y < MASTER; y++) {
    for (int x = 0; x < MASTER; x++) {
      // 2×2 подвыборки
      float acc = 0;
      for (int s = 0; s < 4; s++) {
        const int sx = 2 * x + (s & 1);
        const float u = (sx + 0.5f) / (2 * sc) - 1.0f, v = (y + 0.25f + 0.5f * (s >> 1)) / sc - 1.0f;
        bool in;
        if (shape == 0) {
          in = fabsf(v) < edge[sx];
        } else {
          const float dx = u - MAPLE_CX, d2 = dx * dx + v * v;
          const int ti = std::min(255, (int) ((fx_atan2(v, dx) + PI_L) / (2 * PI_L) * 256));
          in = d2 < rad[ti] * rad[ti];
        }
        float val = 0;
        if (in) {
          val = 1.0f;
          for (int i = 0; i < nv; i++)
            if (seg_d(veins[i], u, v) < veins[i].w * 0.5f)
              val = std::min(val, veins[i].v);
        } else if (seg_d(stem, u, v) < stem.w * 0.5f) {
          val = stem.v;
        }
        acc += val;
      }
      m[(y + 1) * MP + x + 1] = (uint8_t) (acc / 4.0f * 255.0f + 0.5f);
    }
  }
}

// Значение образца в точке (u, v) — с интерполяцией между пикселями
static inline float master_at(const uint8_t *m, float u, float v) {
  const float x = (u + 1.0f) * MASTER / 2.0f + 0.5f, y = (v + 1.0f) * MASTER / 2.0f + 0.5f;
  if (x < 0 || y < 0 || x >= MP - 1 || y >= MP - 1)
    return 0.0f;
  const int x0 = (int) x, y0 = (int) y;
  const float fx = x - x0, fy = y - y0;
  const uint8_t *p = m + y0 * MP + x0;
  const float a = p[0] + (p[1] - p[0]) * fx;
  const float b = p[MP] + (p[MP + 1] - p[MP]) * fx;
  return a + (b - a) * fy;
}

// Сторона картинки листа размера sz
static int leaf_n(int sz) { return 2 * (int) ceilf(LEAF_R[sz]) + 3; }

// Номер картинки в наборе
static int leaf_idx(int shape, int sz, int flip, int rot) { return ((shape * 2 + sz) * NLFLIP + flip) * NLROT + rot; }
static const int NLEAF_IMG = 2 * 2 * NLFLIP * NLROT;

// ---------------------------------------------------------------------------

// Память под все картинки и их описания. Сами картинки рисуются, когда
// впервые понадобятся (leaf_frame_): все 288 сразу — это полсекунды работы,
// а так листопад начинается сразу, а картинки дорисовываются по ходу
bool WeatherFx::leaf_sprites_() {
  if (this->leaf_dsc_)
    return true;
  if (!this->leaf_master_) {
    this->leaf_master_ = static_cast<uint8_t *>(lv_malloc(2 * MP * MP));
    if (!this->leaf_master_)
      return false;
    draw_master(this->leaf_master_, 0);
    draw_master(this->leaf_master_ + MP * MP, 1);
  }
  int bytes = 0;
  for (int sz = 0; sz < 2; sz++)
    bytes += 2 * NLFLIP * NLROT * leaf_n(sz) * leaf_n(sz);
  this->leaf_buf_ = static_cast<uint8_t *>(lv_malloc(bytes));
  this->leaf_dsc_ = static_cast<lv_image_dsc_t *>(lv_malloc(NLEAF_IMG * sizeof(lv_image_dsc_t)));
  if (!this->leaf_buf_ || !this->leaf_dsc_) {
    this->leaf_free_();
    return false;
  }
  memset(this->leaf_dsc_, 0, NLEAF_IMG * sizeof(lv_image_dsc_t));
  memset(this->leaf_ready_, 0, sizeof(this->leaf_ready_));
  uint8_t *out = this->leaf_buf_;
  for (int shape = 0; shape < 2; shape++) {
    for (int sz = 0; sz < 2; sz++) {
      const int n = leaf_n(sz);
      for (int fl = 0; fl < NLFLIP; fl++) {
        for (int rot = 0; rot < NLROT; rot++) {
          lv_image_dsc_t &d = this->leaf_dsc_[leaf_idx(shape, sz, fl, rot)];
          d.header.magic = LV_IMAGE_HEADER_MAGIC;
          d.header.cf = LV_COLOR_FORMAT_A8;
          d.header.w = n;
          d.header.h = n;
          d.header.stride = n;
          d.data = out;
          d.data_size = n * n;
          out += n * n;
        }
      }
    }
  }
  return true;
}

// Нарисовать картинку листа, если её ещё нет: образец, повёрнутый и
// наклонённый, с 2×2 подвыборками
void WeatherFx::leaf_frame_(int idx) {
  if (this->leaf_ready_[idx >> 3] & (1 << (idx & 7)))
    return;
  this->leaf_ready_[idx >> 3] |= (uint8_t) (1 << (idx & 7));
  const int rot = idx % NLROT, fl = (idx / NLROT) % NLFLIP, sz = (idx / (NLROT * NLFLIP)) % 2,
            shape = idx / (NLROT * NLFLIP * 2);
  const uint8_t *m = this->leaf_master_ + shape * MP * MP;
  lv_image_dsc_t &d = this->leaf_dsc_[idx];
  uint8_t *out = const_cast<uint8_t *>(d.data);
  const int n = leaf_n(sz), h = n / 2;
  const float r = LEAF_R[sz];
  const float a = rot * (2.0f * PI_L / NLROT), c = cosf(a) / r, s = sinf(a) / r;
  const float kf = 1.0f / FLIP_K[fl];
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      // Экранная точка → координаты листа: обратный поворот, поперёк —
      // растянуть обратно на наклон
      float acc = 0;
      for (int q = 0; q < 4; q++) {
        const float px = x - h - 0.25f + 0.5f * (q & 1), py = y - h - 0.25f + 0.5f * (q >> 1);
        acc += master_at(m, px * c + py * s, (-px * s + py * c) * kf);
      }
      out[y * n + x] = (uint8_t) std::min(255.0f, acc * 0.25f + 0.5f);
    }
  }
  lv_image_cache_drop(&d);
}

void WeatherFx::leaf_free_() {
  if (this->leaf_dsc_) {
    for (int i = 0; i < NLEAF_IMG; i++)
      if (this->leaf_dsc_[i].data)
        lv_image_cache_drop(&this->leaf_dsc_[i]);
    lv_free(this->leaf_dsc_);
    this->leaf_dsc_ = nullptr;
  }
  if (this->leaf_buf_) {
    lv_free(this->leaf_buf_);
    this->leaf_buf_ = nullptr;
  }
}

void WeatherFx::leaves_apply_(const Params &p, bool relayout) {
  const int cnt = std::min(std::max(p.count, 0), ND);
  // Листопад (обычные и кленовые вперемешку) или кленовые листья 1 сентября
  const int n = (p.mode == 5 || p.mode == 9) ? std::min(cnt, NLF) : 0;
  this->leaf_maple_only_ = p.mode == 9;
  // Памяти на картинки не нашлось — без листьев
  const bool ok = !n || this->leaf_sprites_();
  if (!relayout && ok)
    return;
  for (auto &s : this->lfs_)
    this->mark_(s, false, 0, 0, 1, 1);
  this->nlf_ = ok ? n : 0;
  if (!this->nlf_)
    this->leaf_free_();
  for (int i = 0; i < this->nlf_; i++)
    this->leaf_respawn_(i, true);
}

void WeatherFx::leaf_respawn_(int i, bool anywhere) {
  const bool big = i % 2;
  this->lfx_[i] = rnd_(20, 446);
  this->lfy_[i] = anywhere ? rnd_(-30, 400) : -24 - rnd_(0, 60);
  this->lfv_[i] = big ? 1.1f + (i % 3) * 0.2f : 0.6f + (i % 3) * 0.15f;
  this->lfph_[i] = rnd_(0, 628) / 100.0f;
  this->lfrot_[i] = rnd_(0, 628) / 100.0f;
  // Каждый третий лист кружится, остальные только раскачиваются
  this->lfspin_[i] = i % 3 == 0 ? (rnd_(0, 2) ? 1 : -1) * rnd_(25, 60) / 1000.0f : 0.0f;
  // Переворачивается каждый второй, у каждого свой ритм
  this->lfflip_[i] = rnd_(0, 628) / 100.0f;
  this->lfflipw_[i] = i % 2 == 0 ? rnd_(20, 45) / 1000.0f : 0.0f;
  this->lfrest_[i] = 0;
  this->lfopa_[i] = LV_OPA_COVER;
}

void WeatherFx::leaves_frame_(float wind) {
  const float k = this->k_;
  for (int i = 0; i < this->nlf_; i++) {
    const bool big = i % 2;
    if (this->lfrest_[i] > 0) {
      // Лежит на краю: подождать, потом растаять за ~0,8 с ступенями по
      // ~70 мс — каждая ступень перерисовывает только этот лист
      this->lfrest_[i] -= this->dt_ms_;
      if (this->lfrest_[i] > 800)
        continue;
      const int opa = std::max(0, (int) (255 * this->lfrest_[i] / 800.0f)) & ~31;
      if (opa != this->lfopa_[i]) {
        this->lfopa_[i] = (lv_opa_t) opa;
        if (this->lfs_[i].on)
          this->invalidate_(this->lfs_[i].a);
      }
      if (this->lfrest_[i] <= 0) {
        this->mark_(this->lfs_[i], false, 0, 0, 1, 1);
        this->leaf_respawn_(i, false);
      }
      continue;
    }
    // Качается, в крайних точках замедляясь; ветер сносит
    this->lfph_[i] += k * ((big ? 0.10f : 0.08f) + i * 0.004f);
    if (this->lfph_[i] > 2 * PI_L)
      this->lfph_[i] -= 2 * PI_L;
    const float sw = sinf(this->lfph_[i]);
    this->lfy_[i] += k * this->lfv_[i] * (0.65f + 0.35f * fabsf(sw));
    float x = this->lfx_[i] + k * (cosf(this->lfph_[i]) * (big ? 1.5f : 0.9f) + wind * (big ? 2.6f : 1.8f));
    if (x < 10)
      x += 446;
    else if (x > 456)
      x -= 446;
    this->lfx_[i] = x;
    this->lfrot_[i] += k * this->lfspin_[i];
    this->lfflip_[i] += k * this->lfflipw_[i];
    // Долетел до нижнего края — лечь
    if (this->lfy_[i] > fx_bottom(x) - (big ? 5.0f : 3.0f) - this->cover_ * 10.0f) {
      this->lfrest_[i] = rnd_(2500, 5000);
      this->lfspin_[i] = 0;
      this->lfflipw_[i] = 0;
    }
    const int n = leaf_n(big ? 1 : 0), h = n / 2;
    const int x1 = (int) x - h, y1 = (int) this->lfy_[i] - h;
    // Наклон к движению: лист кренится туда, куда качается
    float ang = fmodf(this->lfrot_[i] + 0.6f * sw, 2 * PI_L);
    if (ang < 0)
      ang += 2 * PI_L;
    const int rot = (int) (ang / (2 * PI_L) * NLROT + 0.5f) % NLROT;
    // Переворот: насколько лист повёрнут ребром и какой стороной к нам
    const float cf = cosf(this->lfflip_[i]), af = fabsf(cf);
    const int fl = af > 0.8f ? 0 : (af > 0.45f ? 1 : 2);
    this->lfback_[i] = cf < 0;
    const int shape = this->leaf_maple_only_ ? 1 : (i / 2) % 2;
    int idx = leaf_idx(shape, big ? 1 : 0, fl, rot);
    if (!(this->leaf_ready_[idx >> 3] & (1 << (idx & 7)))) {
      // Новых картинок — не больше пары за кадр; пока нужный поворот не
      // нарисован — ближайший готовый
      int near = -1;
      if (this->render_budget_ <= 0)
        for (int d = 1; d < NLROT / 2 && near < 0; d++)
          for (int sgn = -1; sgn <= 1 && near < 0; sgn += 2) {
            const int j = leaf_idx(shape, big ? 1 : 0, fl, (rot + sgn * d + NLROT) % NLROT);
            if (this->leaf_ready_[j >> 3] & (1 << (j & 7)))
              near = j;
          }
      if (near >= 0) {
        idx = near;
      } else {
        this->render_budget_--;
        this->leaf_frame_(idx);
      }
    }
    this->lfidx_[i] = (uint16_t) idx;
    this->mark_(this->lfs_[i], this->visible_(x1, y1, n, n) && !this->excluded_(x1, y1, n, n), x1, y1, n, n);
  }
}

void WeatherFx::leaves_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  if (!this->leaf_dsc_)
    return;
  for (int i = 0; i < this->nlf_; i++) {
    const Spot &s = this->lfs_[i];
    if (!s.on || !this->lfopa_[i])
      continue;
    const lv_area_t a = {s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
    if (!fx_hit(a, clip))
      continue;
    const bool big = i % 2;
    lv_color_t c = big ? this->c_leaf_[(i / 2) % 4] : this->c_leaf_s_[(i / 2) % 4];
    if (this->lfback_[i])
      c = lv_color_mix(c, lv_color_black(), 165);  // изнанка темнее
    lv_draw_image_dsc_t im;
    lv_draw_image_dsc_init(&im);
    im.src = &this->leaf_dsc_[this->lfidx_[i]];
    im.opa = this->lfopa_[i];
    im.recolor = c;
    im.image_area = a;
    lv_draw_image(layer, &im, &a);
  }
}

}  // namespace weather_fx
}  // namespace esphome

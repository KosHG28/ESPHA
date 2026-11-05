// Ясное ночное небо: созвездие, которое сейчас видно над домом, и планеты.
//
// Созвездие — по звёздному времени и координатам дома: звёзды каталога
// (sky.h) проецируются на небо, из видимых фигур берётся крупная и высокая,
// поворачивается и вписывается в «шапку» круга, а когда часы убраны — в
// большой прямоугольник справа. Пунктир, звёзды и подпись рисует слой
// погоды. Планеты — по приближённым орбитам JPL: при часах — строкой с
// цветными точками, без часов — картинками по дуге слева внизу.
//
// Здесь же — где сейчас на небе дома Солнце и Луна (их рисуют sun.cpp и
// moon.cpp): по дуге от восхода слева до захода справа. Луна заходит на
// место созвездия — оно вписывается рядом. И сумерки: после заката сначала
// проступают планеты и яркие звёзды, потом остальные и само созвездие.
#include "fx_draw.h"

#include "planets.h"
#include "sky.h"

namespace esphome {
namespace weather_fx {

static const float PI_F = 3.14159265f;

// Созвездие вписывается в «шапку» над строкой погоды: центр и размеры
// прямоугольника, подпись — над ним. Когда часы убраны (режим ожидания) —
// в большой прямоугольник справа: слева по центру там погода на улице, над
// ней луна
struct SkyBox {
  float cx, cy, w, h;
  int cap_x, cap_y;
};
static const SkyBox BOX_TOP = {233.0f, 82.0f, 280.0f, 60.0f, 233, 24};
static const SkyBox BOX_FULL = {300.0f, 225.0f, 210.0f, 230.0f, 300, 78};
// Куда вписывается созвездие сейчас: BOX_TOP или BOX_FULL без той части,
// где луна (con_box_)
static SkyBox g_box = BOX_TOP;
static const int CAP_W = 220;
// Созвездие видно, если его середина не ниже 25° над горизонтом
static const float CON_MIN_ALT = 0.4226f;  // sin 25°
// Сменять созвездие раз в 10 минут, пересчитывать поворот раз в 5
static const int CON_SLOT_S = 600, CON_RECALC_MIN = 5;
// Планеты — строкой внизу, под комнатной температурой: точка и название,
// по центру. Ширина строки в этом месте круга — около 300 px
static const int PL_Y = 408, PL_W = 74;
static const lv_area_t PL_AREA = {233 - 2 * PL_W - 4, PL_Y - 16, 233 + 2 * PL_W + 4, PL_Y + 16};
// Без часов планеты — крупными картинками по дуге слева внизу, под погодой
// на улице: до трёх самых ярких, название справа
static const int PL_FULL_N = 3;
static const int PL_SLOT[PL_FULL_N][2] = {{196, 398}, {122, 352}, {80, 300}};
static const lv_area_t PL_AREA_FULL = {40, 280, 330, 440};
static const lv_image_dsc_t *const PL_IMG[5] = {nullptr, &pl_venus, &pl_jupiter, &pl_mars, &pl_saturn};
struct PlanetEl {
  const char *name;
  uint32_t color;
  int size;
  float el[6], rate[6];  // a, e, I, L, долгота перигелия, долгота узла и их изменение за век
};
// Орбиты по приближённым формулам JPL (E. M. Standish), годятся на 1800–2050
// годы с точностью лучше градуса. [0] — Земля, остальные — по яркости
static const PlanetEl PLANETS[5] = {
    {"",
     0,
     0,
     {1.00000261f, 0.01671123f, -0.00001531f, 100.46457166f, 102.93768193f, 0.0f},
     {0.00000562f, -0.00004392f, -0.01294668f, 35999.37244981f, 0.32327364f, 0.0f}},
    {"Венера",
     0xFFF4D6,
     10,
     {0.72333566f, 0.00677672f, 3.39467605f, 181.97909950f, 131.60246718f, 76.67984255f},
     {0.00000390f, -0.00004107f, -0.00078890f, 58517.81538729f, 0.00268329f, -0.27769418f}},
    {"Юпитер",
     0xFFE0B0,
     8,
     {5.20288700f, 0.04838624f, 1.30439695f, 34.39644051f, 14.72847983f, 100.47390909f},
     {-0.00011607f, -0.00013253f, -0.00183714f, 3034.74612775f, 0.21252668f, 0.20469106f}},
    {"Марс",
     0xFF8A5C,
     7,
     {1.52371034f, 0.09339410f, 1.84969142f, -4.55343205f, -23.94362959f, 49.55953891f},
     {0.00001847f, 0.00007882f, -0.00813131f, 19140.30268499f, 0.44441088f, -0.29257343f}},
    {"Сатурн",
     0xE8D08C,
     7,
     {9.53667594f, 0.05386179f, 2.48599187f, 49.95424423f, 92.59887831f, 113.66242448f},
     {-0.00125060f, -0.00050991f, 0.00193609f, 1222.49362201f, -0.41897216f, -0.28867794f}},
};

// Большая Медведица на случай, когда время или место неизвестны: так она
// выглядит, если смотреть на север осенним вечером. x вправо, y вверх
static const float DIPPER[7][2] = {
    {-138, -103}, {-72, -79}, {-19, -93}, {38, -100}, {44, -34}, {125, -48}, {119, -111},
};
static const uint8_t DIPPER_ORDER[7] = {6, 5, 4, 3, 0, 1, 2};  // в порядке каталога

struct V3 {
  float e, n, u;  // восток, север, зенит
};

static float dot(const V3 &a, const V3 &b) { return a.e * b.e + a.n * b.n + a.u * b.u; }

static V3 norm(const V3 &a) {
  const float l = sqrtf(dot(a, a));
  return l > 1e-6f ? V3{a.e / l, a.n / l, a.u / l} : V3{0, 0, 1};
}

// Направление на звезду в системе «восток — север — зенит». lst — местное
// звёздное время в радианах, sphi и cphi — синус и косинус широты
static V3 star_dir(const SkyStar &s, float lst, float sphi, float cphi) {
  const float ha = lst - s.ra * (PI_F / 12.0f);
  const float dec = s.dec * (PI_F / 180.0f);
  const float sd = sinf(dec), cd = cosf(dec), sh = sinf(ha), ch = cosf(ha);
  return {-cd * sh, sd * cphi - cd * sphi * ch, sd * sphi + cd * cphi * ch};
}

// Как фигура выглядит, если смотреть прямо на неё: проекция на плоскость
// взгляда, «вверх» — к зениту. Возвращает высоту середины (синус) и самой
// низкой звезды
static void project(const SkyCon &c, float lst, float sphi, float cphi, float (*pts)[2], float &mid_u, float &low_u) {
  V3 v[10];
  V3 m{0, 0, 0};
  low_u = 1.0f;
  for (int i = 0; i < c.n; i++) {
    v[i] = star_dir(SKY_STARS[c.first + i], lst, sphi, cphi);
    m = {m.e + v[i].e, m.n + v[i].n, m.u + v[i].u};
    low_u = std::min(low_u, v[i].u);
  }
  const V3 d = norm(m);
  mid_u = d.u;
  // «Вверх» — к зениту; если фигура почти в зените — к северу
  V3 up = {-d.u * d.e, -d.u * d.n, 1.0f - d.u * d.u};
  if (dot(up, up) < 0.0025f)
    up = {-d.n * d.e, 1.0f - d.n * d.n, -d.n * d.u};
  up = norm(up);
  const V3 right = {d.n * up.u - d.u * up.n, d.u * up.e - d.e * up.u, d.e * up.n - d.n * up.e};
  for (int i = 0; i < c.n; i++) {
    const float k = std::max(dot(v[i], d), 0.2f);
    pts[i][0] = dot(v[i], right) / k;
    pts[i][1] = dot(v[i], up) / k;
  }
}

// Размер звезды на экране по звёздной величине
static int star_size(float mag) { return mag < 0.7f ? 10 : (mag < 2.0f ? 8 : (mag < 3.0f ? 6 : 4)); }

// Вписать фигуру, повёрнутую на ang, в «шапку» круга: по центру
// прямоугольника, с сохранением пропорций и так, чтобы ни одна звезда не
// вышла за край круга. Возвращает больший из размеров фигуры в пикселях
static float fit(const float (*src)[2], const int *sz, int n, float ang, int *ox, int *oy) {
  float pts[10][2];
  const float ca = cosf(ang), sa = sinf(ang);
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (int i = 0; i < n; i++) {
    pts[i][0] = src[i][0] * ca - src[i][1] * sa;
    pts[i][1] = src[i][0] * sa + src[i][1] * ca;
    x0 = std::min(x0, pts[i][0]);
    x1 = std::max(x1, pts[i][0]);
    y0 = std::min(y0, pts[i][1]);
    y1 = std::max(y1, pts[i][1]);
  }
  const float bw = std::max(x1 - x0, 1e-4f), bh = std::max(y1 - y0, 1e-4f);
  const float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
  float k = std::min(g_box.w / bw, g_box.h / bh);
  // Край круга: звезда со смещением d от середины фигуры встаёт в точку
  // c + k·d (c — центр прямоугольника относительно центра экрана) и должна
  // остаться ближе R к центру. Наибольшее k — положительный корень
  // |d|²k² + 2(c·d)k + |c|² − R² = 0. Считается сразу, без подбора
  const float cx = g_box.cx - 233.0f, cy = g_box.cy - 233.0f;
  for (int i = 0; i < n; i++) {
    const float dx = pts[i][0] - mx, dy = -(pts[i][1] - my);
    const float dd = dx * dx + dy * dy;
    if (dd < 1e-12f)
      continue;
    const float r = 224.0f - sz[i] / 2 - 3;
    const float cd = cx * dx + cy * dy, cc = cx * cx + cy * cy - r * r;
    const float disc = cd * cd - dd * cc;
    if (disc > 0)
      k = std::min(k, (-cd + sqrtf(disc)) / dd);
  }
  for (int i = 0; i < n; i++) {
    ox[i] = (int) lroundf(g_box.cx + (pts[i][0] - mx) * k);
    oy[i] = (int) lroundf(g_box.cy - (pts[i][1] - my) * k);
  }
  return std::max(bw, bh) * k;
}

// «Шапка» над строкой погоды широкая и низкая: фигура, которая сейчас стоит
// в небе «вертикально», в ней получилась бы мелкой. Поэтому её можно
// повернуть — но на наименьший угол, при котором она выходит почти такой же
// крупной, как в самом удачном повороте. Возвращает этот угол, в size —
// размер в пикселях
static float best_turn(const float (*pts)[2], const int *sz, int n, float &size) {
  static const int STEPS = 24;  // через 15°
  float span[STEPS];
  int ox[10], oy[10];
  float top = 0;
  for (int i = 0; i < STEPS; i++) {
    span[i] = fit(pts, sz, n, i * (2.0f * PI_F / STEPS), ox, oy);
    top = std::max(top, span[i]);
  }
  for (int d = 0; d <= STEPS / 2; d++) {
    for (int sgn = 0; sgn < 2; sgn++) {
      const int i = ((sgn ? -d : d) + STEPS) % STEPS;
      if (span[i] >= 0.85f * top) {
        size = span[i];
        return i * (2.0f * PI_F / STEPS);
      }
    }
  }
  size = span[0];
  return 0;
}

void WeatherFx::layout_con_(int idx, const float (*pts)[2], int n) {
  const SkyCon &c = SKY_CONS[idx];
  n = std::min(n, (int) NCS);
  int sz[NCS]{}, ox[NCS], oy[NCS];
  for (int i = 0; i < n; i++)
    sz[i] = star_size(SKY_STARS[c.first + i].mag);
  float size;
  fit(pts, sz, n, best_turn(pts, sz, n, size), ox, oy);
  // Подпись — над фигурой (без часов — сдвигается от луны, cap_place_)
  const int lh = lv_font_get_line_height(this->font_cap_);
  int cap_x = g_box.cap_x, cap_y = g_box.cap_y;
  {
    lv_point_t ts;
    lv_text_get_size(&ts, c.name, this->font_cap_, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    this->cap_half_ = (int) ts.x / 2 + 2;
  }
  this->cap_ok_ = true;
  if (this->full_) {
    lv_area_t fig = {10000, 10000, -10000, -10000};
    for (int i = 0; i < n; i++) {
      fig.x1 = std::min<int32_t>(fig.x1, ox[i] - 10);
      fig.y1 = std::min<int32_t>(fig.y1, oy[i] - 10);
      fig.x2 = std::max<int32_t>(fig.x2, ox[i] + 10);
      fig.y2 = std::max<int32_t>(fig.y2, oy[i] + 10);
    }
    this->cap_ok_ = this->cap_place_(fig, &cap_x, &cap_y);
  }

  // Если фигура та же, почти не повернулась и подпись на месте — не
  // перерисовывать
  bool same = this->con_on_ && this->con_idx_ == idx && this->cn_ == n && this->cap_area_.y1 == cap_y &&
              this->cap_area_.x1 == cap_x - CAP_W / 2;
  for (int i = 0; same && i < n; i++)
    same = std::abs(ox[i] - this->csx_[i]) <= 1 && std::abs(oy[i] - this->csy_[i]) <= 1;
  if (same)
    return;

  if (this->con_on_) {
    this->invalidate_(this->con_area_);
    if (this->con_idx_ != idx || this->cap_area_.y1 != cap_y || this->cap_area_.x1 != cap_x - CAP_W / 2)
      this->invalidate_(this->cap_area_);
  }
  this->con_idx_ = idx;
  this->con_name_ = c.name;
  this->cn_ = n;
  lv_area_t a = {10000, 10000, -10000, -10000};
  for (int i = 0; i < n; i++) {
    this->csx_[i] = ox[i];
    this->csy_[i] = oy[i];
    this->csz_[i] = sz[i];
    // int32_t на ESP32 — long, поэтому тип указан явно. Запас — на ореол
    // и лучики светящейся звезды
    a.x1 = std::min<int32_t>(a.x1, ox[i] - 10);
    a.y1 = std::min<int32_t>(a.y1, oy[i] - 10);
    a.x2 = std::max<int32_t>(a.x2, ox[i] + 10);
    a.y2 = std::max<int32_t>(a.y2, oy[i] + 10);
  }
  // Пунктир: чёрточки по 5 px через 5 px, с отступом от звёзд
  this->ndash_ = 0;
  for (int l = 0; l < c.nl; l++) {
    const int s0 = SKY_LINKS[c.lfirst + l][0], s1 = SKY_LINKS[c.lfirst + l][1];
    if (s0 >= n || s1 >= n)
      continue;
    const float x0 = ox[s0], y0 = oy[s0], x1 = ox[s1], y1 = oy[s1];
    const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    if (len < 1.0f)
      continue;
    const float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
    const float g0 = sz[s0] / 2 + 4.0f, g1 = sz[s1] / 2 + 4.0f;
    for (float d = g0; d + 5.0f <= len - g1 && this->ndash_ < NDASH; d += 10.0f) {
      lv_point_precise_t *pt = this->dash_[this->ndash_++];
      pt[0].x = (lv_value_precise_t) (x0 + ux * d);
      pt[0].y = (lv_value_precise_t) (y0 + uy * d);
      pt[1].x = (lv_value_precise_t) (x0 + ux * (d + 5.0f));
      pt[1].y = (lv_value_precise_t) (y0 + uy * (d + 5.0f));
    }
  }
  this->con_area_ = a;
  this->cap_area_ = {cap_x - CAP_W / 2, cap_y, cap_x + CAP_W / 2, cap_y + lh};
  this->con_on_ = true;
  this->invalidate_(this->con_area_);
  this->invalidate_(this->cap_area_);
}

// Гелиоцентрические координаты планеты в плоскости эклиптики, а. е.
static void helio(const PlanetEl &p, double T, double &x, double &y, double &z) {
  double v[6];
  for (int i = 0; i < 6; i++)
    v[i] = p.el[i] + p.rate[i] * T;
  const double a = v[0], e = v[1], inc = v[2] * M_PI / 180.0;
  const double w = v[4] * M_PI / 180.0, node = v[5] * M_PI / 180.0;
  double m = fmod(v[3] - v[4], 360.0) * M_PI / 180.0;
  double ea = m;
  for (int i = 0; i < 8; i++)
    ea -= (ea - e * sin(ea) - m) / (1.0 - e * cos(ea));
  const double xp = a * (cos(ea) - e), yp = a * sqrt(1.0 - e * e) * sin(ea);
  const double om = w - node;
  const double co = cos(om), so = sin(om), cn = cos(node), sn = sin(node), ci = cos(inc), si = sin(inc);
  x = (co * cn - so * sn * ci) * xp + (-so * cn - co * sn * ci) * yp;
  y = (co * sn + so * cn * ci) * xp + (-so * sn + co * cn * ci) * yp;
  z = (so * si) * xp + (co * si) * yp;
}

void WeatherFx::planets_(double jd, float lst, float sphi, float cphi) {
  // Какие планеты сейчас выше 5° над горизонтом — в порядке яркости
  const double T = (jd - 2451545.0) / 36525.0;
  double ex, ey, ez;
  helio(PLANETS[0], T, ex, ey, ez);
  const double eps = 23.43928 * M_PI / 180.0;
  int idx[4], n = 0;
  for (int k = 1; k < 5; k++) {
    double x, y, z;
    helio(PLANETS[k], T, x, y, z);
    x -= ex;
    y -= ey;
    z -= ez;
    const double ye = y * cos(eps) - z * sin(eps), ze = y * sin(eps) + z * cos(eps);
    SkyStar s;
    s.ra = (float) (atan2(ye, x) * 12.0 / M_PI);
    s.dec = (float) (asin(ze / sqrt(x * x + ye * ye + ze * ze)) * 180.0 / M_PI);
    s.mag = 0;
    if (star_dir(s, lst, sphi, cphi).u > 0.087f)
      idx[n++] = k;
  }
  bool same = n == this->npl_;
  for (int i = 0; same && i < n; i++)
    same = idx[i] == this->pl_idx_[i];
  if (same)
    return;
  this->npl_ = n;
  for (int i = 0; i < n; i++)
    this->pl_idx_[i] = idx[i];
  this->invalidate_(this->full_ ? PL_AREA_FULL : PL_AREA);
}

void WeatherFx::sky_(const Params &p) {
  if (!this->stars_on_)
    return;
  // На Рождество на месте созвездия — Вифлеемская звезда, на Хэллоуин в
  // небе только летучие мыши
  if (this->hol_ == HOL_CHRISTMAS || this->bats_on_) {
    if (this->con_on_) {
      this->invalidate_(this->con_area_);
      this->invalidate_(this->cap_area_);
      this->con_on_ = false;
    }
    if (this->bats_on_ && this->npl_) {
      this->npl_ = 0;
      this->invalidate_(this->full_ ? PL_AREA_FULL : PL_AREA);
    }
    this->con_force_ = true;
    return;
  }
  // Луна зашла на место созвездия или ушла с него — вписать фигуру заново;
  // наползла на подпись — переставить подпись
  {
    int x1, y1, x2, y2;
    this->con_box_(&x1, &y1, &x2, &y2);
    const SkyBox &base = this->full_ ? BOX_FULL : BOX_TOP;
    const float w = (float) (x2 - x1), h = (float) (y2 - y1);
    const SkyBox nb = {x1 + w / 2, y1 + h / 2, w, h, base.cap_x, base.cap_y};
    if (nb.cx != g_box.cx || nb.cy != g_box.cy || nb.w != g_box.w || nb.h != g_box.h) {
      g_box = nb;
      this->con_force_ = true;
    }
    if (this->full_ && this->con_on_ && this->cap_ok_ && this->moon_spot_.on) {
      const lv_area_t &m = this->moon_spot_.a, &a = this->cap_area_;
      const int cx = (a.x1 + a.x2) / 2;
      if (m.x1 - 4 <= cx + this->cap_half_ && m.x2 + 4 >= cx - this->cap_half_ && m.y1 - 4 <= a.y2 && m.y2 + 4 >= a.y1)
        this->con_force_ = true;
    }
  }
  const bool known = p.utc > 1600000000u && !std::isnan(p.lat) && !std::isnan(p.lon);
  const int64_t minute = known ? (int64_t) (p.utc / 60) : -1;
  if (!this->con_force_) {
    if (known && this->con_min_ >= 0 && minute >= this->con_min_ && minute - this->con_min_ < CON_RECALC_MIN)
      return;
    if (!known && this->con_min_ == -1)
      return;
  }
  this->con_force_ = false;
  this->con_min_ = minute;

  float pts[NCS][2];
  if (!known) {
    for (int i = 0; i < 7; i++) {
      pts[DIPPER_ORDER[i]][0] = DIPPER[i][0];
      pts[DIPPER_ORDER[i]][1] = DIPPER[i][1];
    }
    this->layout_con_(0, pts, 7);
    return;
  }

  // Местное звёздное время по часам и долготе
  const float lst = (float) (local_sidereal(julian_day(p.utc), p.lon) * M_PI / 180.0);
  const float phi = p.lat * (PI_F / 180.0f), sphi = sinf(phi), cphi = cosf(phi);
  this->planets_(p.utc / 86400.0 + 2440587.5, lst, sphi, cphi);

  // Какие фигуры сейчас над горизонтом и хорошо видны. Совсем мелкие на
  // экране (Лира в неудачном повороте) — только если больше показать нечего
  int good[SKY_NCON], ngood = 0, seen[SKY_NCON], nseen = 0, best = 0;
  float best_u = -2.0f;
  for (int i = 0; i < SKY_NCON; i++) {
    float mid_u, low_u;
    project(SKY_CONS[i], lst, sphi, cphi, pts, mid_u, low_u);
    if (mid_u > best_u) {
      best_u = mid_u;
      best = i;
    }
    if (mid_u < CON_MIN_ALT || low_u < 0.14f)  // вся фигура выше ~8°
      continue;
    seen[nseen++] = i;
    int sz[NCS];
    for (int k = 0; k < SKY_CONS[i].n; k++)
      sz[k] = star_size(SKY_STARS[SKY_CONS[i].first + k].mag);
    float size;
    best_turn(pts, sz, SKY_CONS[i].n, size);
    if (size >= 120.0f)
      good[ngood++] = i;
  }
  const int *list = ngood ? good : seen;
  const int nlist = ngood ? ngood : nseen;
  int pick = best;
  if (nlist) {
    // Каждые 10 минут — следующее из видимых; в пределах этих 10 минут —
    // то же, что уже на экране, если оно всё ещё в списке
    const int64_t slot = p.utc / CON_SLOT_S;
    bool keep = false;
    for (int i = 0; i < nlist; i++)
      keep |= list[i] == this->con_idx_;
    pick = (keep && slot == this->con_slot_) ? this->con_idx_ : list[slot % nlist];
    this->con_slot_ = slot;
  }
  float mid_u, low_u;
  project(SKY_CONS[pick], lst, sphi, cphi, pts, mid_u, low_u);
  this->layout_con_(pick, pts, SKY_CONS[pick].n);
}

// ---- Солнце и луна по настоящему небу, сумерки ----

// Дуги, по которым идут по небу солнце и луна: светило восходит на левом
// конце дуги, к полудню поднимается на середину, заходит на правом конце.
// Середина, высота концов («горизонт»), полуширина и высота дуги — у
// светила, которое на юге стоит низко (10°, как солнце зимой) и высоко (60°,
// как летом), и у луны. При часах дуга — в «шапке» над строкой погоды; луна
// идёт чуть ниже солнца — под подписью созвездия. Без часов — над погодой
// на улице, а подпись созвездия уступает луне место сама
struct Dome {
  float cx, yh, a, b_lo, b_hi, mb_lo, mb_hi;
};
static const Dome DOME_TOP = {233.0f, 158.0f, 168.0f, 82.0f, 90.0f, 74.0f, 80.0f};
static const Dome DOME_FULL = {233.0f, 170.0f, 176.0f, 92.0f, 116.0f, 92.0f, 116.0f};

void WeatherFx::ephem_(const Params &p) {
  if (p.utc < 1600000000u || std::isnan(p.lat) || std::isnan(p.lon)) {
    this->eph_ok_ = false;
    return;
  }
  // Раз в 5 с: за это время солнце и луна сдвигаются по дуге меньше чем на
  // пиксель
  if (this->eph_ok_ && p.utc >= this->eph_utc_ && p.utc - this->eph_utc_ < 5)
    return;
  this->eph_ok_ = true;
  this->eph_utc_ = p.utc;
  this->sun_path_ = sun_path(p.utc, p.lat, p.lon);
  this->moon_path_ = moon_path(p.utc, p.lat, p.lon, &this->moon_ph_now_, &this->moon_lit_);
}

// Где на экране светило с путём s (относительно слоя). Часовой угол от
// восхода до захода раскладывается по дуге равномерно: у горизонта светило
// поднимается почти отвесно, в полдень идёт почти вбок — как в жизни. false —
// оно под горизонтом
bool WeatherFx::dome_(const SkyPath &s, bool moon, float *x, float *y) const {
  if (s.ha_set <= 0.0 || std::fabs(s.ha) > s.ha_set)
    return false;
  const Dome &d = this->full_ ? DOME_FULL : DOME_TOP;
  const float th = (float) (s.ha / s.ha_set) * (PI_F / 2.0f);
  const float lo = moon ? d.mb_lo : d.b_lo, hi = moon ? d.mb_hi : d.b_hi;
  const float b = lo + (hi - lo) * fx_clamp01(((float) s.alt_top - 10.0f) / 50.0f);
  *x = d.cx + d.a * sinf(th);
  *y = d.yh - b * cosf(th);
  return true;
}

// Сумерки: на какой высоте Солнца начинают проступать и проступают целиком
// планеты, яркие звёзды, остальные звёзды и созвездие с пунктиром и
// подписью. Вечером — по порядку, утром гаснут в обратном
static const float TW_FROM[4] = {-0.8f, -1.5f, -3.0f, -3.5f};
static const float TW_TO[4] = {-2.5f, -4.0f, -6.5f, -7.0f};

void WeatherFx::twilight_() {
  if (!this->stars_on_)
    return;
  uint8_t q[4];
  for (int k = 0; k < 4; k++) {
    // Время или место неизвестны или солнце высоко, а звёзды есть (проверка
    // «Ясная ночь» днём) — всё сразу
    float v = 1.0f;
    if (this->eph_ok_ && this->sun_path_.alt < 2.0)
      v = fx_smooth(TW_FROM[k], TW_TO[k], (float) this->sun_path_.alt);
    q[k] = (uint8_t) lroundf(v * 16.0f);
  }
  if (memcmp(q, this->tw_q_, sizeof(q)) == 0)
    return;
  if (q[0] != this->tw_q_[0] && this->npl_)
    this->invalidate_(this->full_ ? PL_AREA_FULL : PL_AREA);
  if (this->con_on_) {
    this->invalidate_(this->con_area_);
    this->invalidate_(this->cap_area_);
  }
  memcpy(this->tw_q_, q, sizeof(q));
  this->stars_show_();
}

// Куда вписать созвездие: «шапка» над строкой погоды или большой
// прямоугольник справа — без того, что заслоняет луна. Если луна заходит в
// прямоугольник, остаётся самая большая его часть слева, справа, сверху или
// снизу от неё. Край луны берётся с шагом 8 px, чтобы фигура не
// перестраивалась вслед за ней каждую минуту
void WeatherFx::con_box_(int *x1, int *y1, int *x2, int *y2) const {
  const SkyBox &b = this->full_ ? BOX_FULL : BOX_TOP;
  *x1 = (int) (b.cx - b.w / 2);
  *x2 = (int) (b.cx + b.w / 2);
  *y1 = (int) (b.cy - b.h / 2);
  *y2 = (int) (b.cy + b.h / 2);
  if (!this->moon_spot_.on)
    return;
  const lv_area_t &m = this->moon_spot_.a;
  const int mx1 = (int) ((m.x1 - 6) & ~7), my1 = (int) ((m.y1 - 6) & ~7);
  const int mx2 = (int) ((m.x2 + 13) & ~7), my2 = (int) ((m.y2 + 13) & ~7);
  if (mx1 > *x2 || mx2 < *x1 || my1 > *y2 || my2 < *y1)
    return;
  // Части прямоугольника слева, справа, сверху и снизу от луны — самая
  // большая из тех, куда фигура ещё влезает
  const int cand[4][4] = {{*x1, *y1, mx1, *y2}, {mx2, *y1, *x2, *y2}, {*x1, *y1, *x2, my1}, {*x1, my2, *x2, *y2}};
  int best = -1, area = 0;
  for (int i = 0; i < 4; i++) {
    const int w = cand[i][2] - cand[i][0], h = cand[i][3] - cand[i][1];
    if (w >= 60 && h >= 36 && w * h > area) {
      area = w * h;
      best = i;
    }
  }
  if (best >= 0) {
    *x1 = cand[best][0];
    *y1 = cand[best][1];
    *x2 = cand[best][2];
    *y2 = cand[best][3];
  }
}

// Где подписать созвездие без часов: луна ходит по всему верху неба. По
// порядку: на обычном месте; прямо над фигурой; сбоку от луны на обычной
// высоте; под фигурой. Подпись не заходит на луну, звёзды фигуры, планеты,
// погоду на улице и за край круга. fig — где фигура, вместе с ореолом звёзд.
// Возвращает, нашлось ли такое место (если нет — подпись на обычном месте)
bool WeatherFx::cap_place_(const lv_area_t &fig, int *x, int *y) const {
  const int lh = lv_font_get_line_height(this->font_cap_), half = this->cap_half_;
  auto meets = [](int x1, int y1, int x2, int y2, const lv_area_t &b) {
    return x1 <= b.x2 && x2 >= b.x1 && y1 <= b.y2 && y2 >= b.y1;
  };
  auto fits = [&](int cx, int top) {
    const int x1 = cx - half, x2 = cx + half, y1 = top, y2 = top + lh;
    // Внутри круга — все четыре угла
    for (int k = 0; k < 4; k++) {
      const int px = k & 1 ? x2 : x1, py = k & 2 ? y2 : y1;
      if ((px - 233) * (px - 233) + (py - 233) * (py - 233) > 224 * 224)
        return false;
    }
    if (this->moon_spot_.on) {
      const lv_area_t &m = this->moon_spot_.a;
      if (meets(x1, y1, x2, y2, {m.x1 - 4, m.y1 - 4, m.x2 + 4, m.y2 + 4}))
        return false;
    }
    if (meets(x1, y1, x2, y2, {fig.x1 + 6, fig.y1 + 6, fig.x2 - 6, fig.y2 - 6}))
      return false;
    // Планеты — картинка и название справа от неё
    for (int i = 0; i < std::min(this->npl_, PL_FULL_N); i++) {
      const lv_image_dsc_t *img = PL_IMG[this->pl_idx_[i]];
      const int pw = img->header.w, ph = img->header.h, pcx = PL_SLOT[i][0], pcy = PL_SLOT[i][1];
      if (meets(x1, y1, x2, y2,
                {pcx - pw / 2, std::min(pcy - ph / 2, pcy - 9), pcx + pw / 2 + 96, std::max(pcy + ph / 2, pcy + 11)}))
        return false;
    }
    // Погода на улице — слева по центру
    return !meets(x1, y1, x2, y2, {14, 205, 176, 262});
  };
  const SkyBox &b = BOX_FULL;
  const int fcx = (int) (fig.x1 + fig.x2) / 2;
  int cand[5][2] = {
      {b.cap_x, b.cap_y}, {fcx, (int) fig.y1 - lh}, {b.cap_x, b.cap_y}, {b.cap_x, b.cap_y}, {fcx, (int) fig.y2 + 2}};
  if (this->moon_spot_.on) {
    const lv_area_t &m = this->moon_spot_.a;
    cand[2][0] = (int) m.x1 - 6 - half;
    cand[3][0] = (int) m.x2 + 6 + half;
  }
  for (const auto &c : cand) {
    if (std::abs(c[0] - b.cap_x) <= 110 && fits(c[0], c[1])) {
      *x = c[0];
      *y = c[1];
      return true;
    }
  }
  *x = b.cap_x;
  *y = b.cap_y;
  return false;
}

// Часы ушли или вернулись: созвездие и планеты — заново под весь круг или
// под «шапку»
void WeatherFx::sky_full_() {
  g_box = this->full_ ? BOX_FULL : BOX_TOP;
  this->invalidate_(PL_AREA);
  this->invalidate_(PL_AREA_FULL);
  if (this->con_on_) {
    this->invalidate_(this->con_area_);
    this->invalidate_(this->cap_area_);
  }
  this->con_force_ = true;
}

void WeatherFx::sky_apply_() {
  // Созвездие гаснет вместе со звёздами; зажигает его sky_()
  if (!this->stars_on_ && this->con_on_) {
    this->invalidate_(this->con_area_);
    this->invalidate_(this->cap_area_);
    this->con_on_ = false;
  }
  if (!this->stars_on_ && this->npl_) {
    this->npl_ = 0;
    this->invalidate_(this->full_ ? PL_AREA_FULL : PL_AREA);
  }
}

void WeatherFx::sky_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  lv_area_t abs;
  auto to_abs = [&](const lv_area_t &a) {
    abs = {(int32_t) (a.x1 + oc.x1), (int32_t) (a.y1 + oc.y1), (int32_t) (a.x2 + oc.x1), (int32_t) (a.y2 + oc.y1)};
    return fx_hit(abs, clip);
  };
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.opa = LV_OPA_COVER;
  // Сумерки: что насколько проступило (twilight_)
  auto tw = [this](int k) { return (lv_opa_t) (this->tw_q_[k] * 255 / 16); };
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.opa = tw(3);
  const bool dim = this->dim_;

  // Созвездие: пунктир, звёзды и подпись. В сумерки сначала проступают его
  // яркие звёзды, потом остальные, последними — пунктир и подпись
  if (this->con_on_ && this->tw_q_[1]) {
    if (to_abs(this->con_area_)) {
      ln.width = 2;
      ln.color = lv_color_hex(dim ? 0x6A7FA8 : 0x34435E);
      for (int i = 0; i < (ln.opa > LV_OPA_MIN ? this->ndash_ : 0); i++) {
        const lv_point_precise_t *d = this->dash_[i];
        lv_area_t b = {(int32_t) std::min(d[0].x, d[1].x) + oc.x1 - 2, (int32_t) std::min(d[0].y, d[1].y) + oc.y1 - 2,
                       (int32_t) std::max(d[0].x, d[1].x) + oc.x1 + 2, (int32_t) std::max(d[0].y, d[1].y) + oc.y1 + 2};
        if (!fx_hit(b, clip))
          continue;
        ln.p1 = {d[0].x + oc.x1, d[0].y + oc.y1};
        ln.p2 = {d[1].x + oc.x1, d[1].y + oc.y1};
        lv_draw_line(layer, &ln);
      }
      // Звёзды созвездия — светящиеся, крупнее по звёздной величине
      for (int i = 0; i < this->cn_; i++) {
        const int x = this->csx_[i] + oc.x1, y = this->csy_[i] + oc.y1;
        lv_area_t a = {x - 10, y - 10, x + 10, y + 10};
        if (fx_hit(a, clip))
          this->con_star_paint_(layer, x, y, this->csz_[i], tw(this->csz_[i] >= 8 ? 1 : 2));
      }
    }
    if (this->tw_q_[3] && to_abs(this->cap_area_)) {
      lv_draw_label_dsc_t lb;
      lv_draw_label_dsc_init(&lb);
      lb.text = this->con_name_;
      lb.font = this->font_cap_;
      lb.color = lv_color_hex(dim ? 0x8898B8 : 0x4C5A74);
      lb.align = LV_TEXT_ALIGN_CENTER;
      lb.opa = tw(3);
      lv_draw_label(layer, &lb, &abs);
    }
  }

  // Без часов — планеты картинками по дуге слева внизу, название справа.
  // В сумерки планеты проступают первыми
  if (this->npl_ && this->full_ && this->tw_q_[0]) {
    lv_area_t pa = {PL_AREA_FULL.x1 + oc.x1, PL_AREA_FULL.y1 + oc.y1, PL_AREA_FULL.x2 + oc.x1, PL_AREA_FULL.y2 + oc.y1};
    if (fx_hit(pa, clip)) {
      lv_draw_image_dsc_t im;
      lv_draw_image_dsc_init(&im);
      im.opa = tw(0);
      lv_draw_label_dsc_t lb;
      lv_draw_label_dsc_init(&lb);
      lb.font = this->font_cap_;
      lb.color = lv_color_hex(dim ? 0x9AA8C4 : 0x5C6A84);
      lb.opa = tw(0);
      lb.flag = LV_TEXT_FLAG_EXPAND;
      for (int i = 0; i < std::min(this->npl_, PL_FULL_N); i++) {
        const lv_image_dsc_t *img = PL_IMG[this->pl_idx_[i]];
        const int w = img->header.w, h = img->header.h;
        const int cx = PL_SLOT[i][0] + oc.x1, cy = PL_SLOT[i][1] + oc.y1;
        im.src = img;
        lv_area_t ia = {cx - w / 2, cy - h / 2, cx - w / 2 + w - 1, cy - h / 2 + h - 1};
        if (fx_hit(ia, clip))
          lv_draw_image(layer, &im, &ia);
        lv_area_t ta = {cx + w / 2 + 6, cy - 9, cx + w / 2 + 96, cy + 11};
        lb.text = PLANETS[this->pl_idx_[i]].name;
        if (fx_hit(ta, clip))
          lv_draw_label(layer, &lb, &ta);
      }
    }
  }

  // Планеты: цветная точка и название
  if (this->npl_ && !this->full_ && this->tw_q_[0]) {
    lv_area_t pa = {PL_AREA.x1 + oc.x1, PL_AREA.y1 + oc.y1, PL_AREA.x2 + oc.x1, PL_AREA.y2 + oc.y1};
    if (fx_hit(pa, clip)) {
      lv_draw_label_dsc_t lb;
      lv_draw_label_dsc_init(&lb);
      lb.font = this->font_cap_;
      lb.color = lv_color_hex(dim ? 0x8898B8 : 0x4C5A74);
      lb.opa = tw(0);
      lb.flag = LV_TEXT_FLAG_EXPAND;  // одной строкой, без переноса
      fill.radius = LV_RADIUS_CIRCLE;
      fill.opa = tw(0);
      const int x0 = 233 - this->npl_ * PL_W / 2;
      for (int i = 0; i < this->npl_; i++) {
        const PlanetEl &pl = PLANETS[this->pl_idx_[i]];
        const int cx = x0 + i * PL_W + 6 + oc.x1, cy = PL_Y + oc.y1, r = pl.size / 2;
        lv_area_t dot = {cx - r, cy - r, cx - r + pl.size - 1, cy - r + pl.size - 1};
        fill.color = lv_color_hex(pl.color);
        lv_draw_fill(layer, &fill, &dot);
        lv_area_t ta = {cx + 9, cy - 10, cx + PL_W - 8, cy + 12};
        lb.text = pl.name;
        lv_draw_label(layer, &lb, &ta);
      }
    }
  }
}

}  // namespace weather_fx
}  // namespace esphome

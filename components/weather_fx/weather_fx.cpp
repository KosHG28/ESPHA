#include "weather_fx.h"

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace weather_fx {

// Облака плывут на разной высоте у верхнего края круга
static const int CLOUD_Y[3] = {34, 78, 18};
// Полный снос ветром — при такой скорости и сильнее, м/с
static const float FULL_WIND = 15.0f;
// Длина капли. Она больше шага капли за кадр (5–6 px при 33 мс), поэтому
// старое и новое положение перекрываются и перерисовываются одним куском —
// иначе капля на мгновение пропадала бы между «стереть» и «нарисовать»
static const int DROP_LEN = 20;
static const int HAIL_SIZE = 6;
// Сколько живут брызги, мс
static const float SPLASH_MS = 250.0f;

int WeatherFx::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

void WeatherFx::show_(lv_obj_t *o, bool v) {
  if (v)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void WeatherFx::bind(lv_obj_t *root, lv_obj_t *drops, lv_obj_t *flakes, lv_obj_t *splash, lv_obj_t *clouds,
                     lv_obj_t *stars, lv_obj_t *bolt, lv_obj_t *glow) {
  this->root_ = root;
  this->drops_box_ = drops;
  this->flakes_box_ = flakes;
  this->splash_box_ = splash;
  this->clouds_box_ = clouds;
  this->stars_box_ = stars;
  this->bolt_ = bolt;
  this->glow_ = glow;
  this->bound_ = root && drops && flakes && splash && clouds && stars && bolt && glow &&
                 lv_obj_get_child_count(drops) >= (uint32_t) ND && lv_obj_get_child_count(flakes) >= (uint32_t) NF &&
                 lv_obj_get_child_count(splash) >= (uint32_t) NS && lv_obj_get_child_count(clouds) >= (uint32_t) NC &&
                 lv_obj_get_child_count(stars) >= (uint32_t) NST;
  this->applied_ = -1;
}

void WeatherFx::add_exclude(int x1, int y1, int x2, int y2) {
  if (this->n_ex_ >= 2)
    return;
  int *e = this->ex_[this->n_ex_++];
  e[0] = x1;
  e[1] = y1;
  e[2] = x2;
  e[3] = y2;
}

bool WeatherFx::excluded_(int x, int y, int w, int h) const {
  for (int i = 0; i < this->n_ex_; i++) {
    const int *e = this->ex_[i];
    if (x + w > e[0] && x < e[2] && y + h > e[1] && y < e[3])
      return true;
  }
  return false;
}

void WeatherFx::place_(lv_obj_t *o, int x, int y, int w, int h, bool &visible) {
  // Прячем до сдвига, показываем после: сдвиг скрытого объекта ничего не
  // перерисовывает, так что за цифрами частица не стоит ни одного кадра
  if (this->excluded_(x, y, w, h)) {
    if (visible) {
      show_(o, false);
      visible = false;
    }
    lv_obj_set_pos(o, x, y);
  } else {
    lv_obj_set_pos(o, x, y);
    if (!visible) {
      show_(o, true);
      visible = true;
    }
  }
}

void WeatherFx::end_strike_() {
  if (!this->striking_)
    return;
  this->striking_ = false;
  this->bolt_on_ = false;
  show_(this->bolt_, false);
  show_(this->glow_, false);
  this->next_strike_ = millis() + rnd_(5000, 15000);
}

void WeatherFx::apply_(const Params &p, bool relayout) {
  const bool dim = p.dim;
  const int cnt = std::min(std::max(p.count, 0), ND);
  this->dim_ = dim;
  this->hail_ = p.mode == 4;
  // Дождь (и дождь в мокром снеге) — каплями на стекле, если так выбрано.
  // Град всегда падает
  this->glass_on_ = !this->hail_ && p.rain_style == 0;
  this->nd_ = (p.mode == 1 || p.mode == 4) ? cnt : (p.mode == 3 ? cnt / 2 : 0);
  this->nf_ = std::min(p.mode == 2 ? cnt : (p.mode == 3 ? cnt / 2 : 0), NF);
  this->ncl_ = std::min(std::max(p.clouds, 0), NC);
  this->storm_ = p.storm;
  this->stars_on_ = p.stars;
  if (!this->storm_)
    this->end_strike_();
  show_(this->root_, this->nd_ || this->nf_ || this->ncl_ || this->storm_ || this->stars_on_);

  // Звёзды — в верхней половине круга, вокруг и выше времени
  for (int i = 0; i < NST; i++) {
    lv_obj_t *s = lv_obj_get_child(this->stars_box_, i);
    if (!this->stars_on_) {
      show_(s, false);
      continue;
    }
    if (relayout) {
      int x, y;
      do {
        x = rnd_(50, 416);
        y = rnd_(30, 220);
      } while ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 215 * 215 ||
               (x > 118 && x < 342 && y > 8 && y < 92));  // место Большой Медведицы
      lv_obj_set_pos(s, x, y);
      this->stph_[i] = rnd_(0, 628) / 100.0f;
    }
    show_(s, true);
  }

  this->show_dipper_(this->stars_on_, dim);

  // В приглушённом режиме палитра ярче: при низкой яркости панели тёмные
  // частицы сливаются с чёрным
  const uint32_t c_drop = this->hail_ ? (dim ? 0xFFFFFF : 0xCFD8DC) : (dim ? 0x7FD4FF : 0x2A6F99);
  for (int i = 0; i < ND; i++) {
    lv_obj_t *d = lv_obj_get_child(this->drops_box_, i);
    if (i >= this->nd_) {
      show_(d, false);
      continue;
    }
    if (this->glass_on_) {
      // Капля на стекле: кружок 4–8 px со светлым ободком
      const int sz = 4 + 2 * (i % 3);
      lv_obj_set_size(d, sz, sz);
      lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_grad_dir(d, LV_GRAD_DIR_NONE, 0);
      lv_obj_set_style_bg_color(d, lv_color_hex(dim ? 0x3F8FC4 : 0x1D4F72), 0);
      lv_obj_set_style_border_width(d, 1, 0);
      lv_obj_set_style_border_color(d, lv_color_hex(dim ? 0xBFE8FF : 0x5FA8D8), 0);
      if (relayout) {
        // Все капли сначала ждут, появляются вразнобой
        this->gst_[i] = 0;
        this->glife_[i] = rnd_(0, 3000);
      }
      if (this->gst_[i] == 0) {
        show_(d, false);
        this->dvis_[i] = false;
      } else {
        show_(d, true);
        this->dvis_[i] = true;
      }
      continue;
    }
    lv_obj_set_style_border_width(d, 0, 0);
    if (this->hail_) {
      lv_obj_set_size(d, HAIL_SIZE, HAIL_SIZE);
      lv_obj_set_style_radius(d, 3, 0);
      lv_obj_set_style_bg_grad_dir(d, LV_GRAD_DIR_NONE, 0);
      lv_obj_set_style_bg_color(d, lv_color_hex(c_drop), 0);
    } else {
      // Капля со «шлейфом»: яркая снизу и растворяется кверху. Глаз видит
      // летящую каплю, а не чёрточку, прыгающую с места на место
      lv_obj_set_size(d, 2, DROP_LEN);
      lv_obj_set_style_radius(d, 1, 0);
      lv_obj_set_style_bg_grad_dir(d, LV_GRAD_DIR_VER, 0);
      lv_obj_set_style_bg_color(d, lv_color_hex(0x000000), 0);
      lv_obj_set_style_bg_grad_color(d, lv_color_hex(c_drop), 0);
    }
    if (relayout) {
      this->dx_[i] = rnd_(40, 426);
      this->dy_[i] = rnd_(0, 440);
    }
    lv_obj_set_pos(d, (int) this->dx_[i], (int) this->dy_[i]);
    show_(d, true);
    this->dvis_[i] = true;
  }

  // Нечётные снежинки крупные и яркие — «близко», чётные мелкие и тусклые — «далеко»
  for (int i = 0; i < NF; i++) {
    lv_obj_t *f = lv_obj_get_child(this->flakes_box_, i);
    if (i >= this->nf_) {
      show_(f, false);
      continue;
    }
    const bool big = i % 2;
    const uint32_t c = big ? (dim ? 0xFFFFFF : 0x9AA4AE) : (dim ? 0xAEB8C2 : 0x59636D);
    lv_obj_set_style_text_color(f, lv_color_hex(c), 0);
    if (relayout) {
      this->fx_[i] = rnd_(30, 430);
      this->fy_[i] = rnd_(0, 440);
      this->fph_[i] = rnd_(0, 628) / 100.0f;
    }
    lv_obj_set_pos(f, (int) this->fx_[i], (int) this->fy_[i]);
    show_(f, true);
    this->fvis_[i] = true;
  }

  for (int i = 0; i < NS; i++) {
    lv_obj_t *s = lv_obj_get_child(this->splash_box_, i);
    lv_obj_set_style_bg_color(s, lv_color_hex(dim ? 0x7FD4FF : 0x2A6F99), 0);
    this->slife_[i] = 0.0f;
    show_(s, false);
  }

  // Молния жёлтая; свечение в приглушённом режиме ярче
  lv_obj_set_style_line_color(this->glow_, lv_color_hex(dim ? 0xFFC107 : 0xFF9800), 0);

  // Облака: при пасмурной погоде и грозе — темнее
  const bool heavy = this->ncl_ >= 3 || this->storm_;
  const uint32_t c_cloud = heavy ? (dim ? 0x3A4452 : 0x161B21) : (dim ? 0x465262 : 0x1D232A);
  for (int i = 0; i < NC; i++) {
    lv_obj_t *c = lv_obj_get_child(this->clouds_box_, i);
    if (i >= this->ncl_) {
      show_(c, false);
      continue;
    }
    for (uint32_t k = 0; k < lv_obj_get_child_count(c); k++)
      lv_obj_set_style_bg_color(lv_obj_get_child(c, k), lv_color_hex(c_cloud), 0);
    if (relayout)
      this->cx_[i] = 40 + i * 150 - 90;
    lv_obj_set_pos(c, (int) this->cx_[i], CLOUD_Y[i]);
    show_(c, true);
  }
}

float WeatherFx::wind_(const Params &p, uint32_t now) {
  if (std::isnan(p.wind_speed) || std::isnan(p.wind_bearing))
    return 0.0f;
  float speed = p.wind_speed;

  // Порывы: раз в 6–20 секунд ветер на 2–3 секунды усиливается до силы
  // порыва — плавно нарастает и спадает по полуволне синуса
  if (!std::isnan(p.wind_gust) && p.wind_gust > speed + 0.5f) {
    if (this->gust_len_ == 0) {
      if (this->next_gust_ == 0)
        this->next_gust_ = now + rnd_(3000, 8000);
      if ((int32_t) (now - this->next_gust_) >= 0) {
        this->gust_t0_ = now;
        this->gust_len_ = rnd_(2000, 3200);
      }
    }
    if (this->gust_len_) {
      uint32_t t = now - this->gust_t0_;
      if (t >= this->gust_len_) {
        this->gust_len_ = 0;
        this->next_gust_ = now + rnd_(6000, 20000);
      } else {
        float env = sinf(3.14159265f * t / this->gust_len_);
        speed += (p.wind_gust - speed) * env;
      }
    }
  } else {
    this->gust_len_ = 0;
  }

  // Направление в метеорологии — откуда дует: западный ветер (270°) несёт
  // осадки вправо
  return -sinf(p.wind_bearing * 0.0174533f) * std::min(speed, FULL_WIND) / FULL_WIND;
}

static float wrap_x(float x) {
  if (x < 20)
    return x + 426;
  if (x > 446)
    return x - 426;
  return x;
}

// Капля разбивается о нижний край круглого экрана
static float ground(float x) {
  float d = x - 233.0f;
  return 233.0f + sqrtf(std::max(0.0f, 233.0f * 233.0f - d * d)) - 8.0f;
}

void WeatherFx::drops_(float wind) {
  for (int i = 0; i < this->nd_; i++) {
    const float k = this->k_;
    // Дождь падает неспешно (140–190 px/с), зато капель много
    this->dy_[i] += k * (this->hail_ ? 7.0f + (i % 3) : 7.0f + (i % 3) * 1.2f);
    this->dx_[i] = wrap_x(this->dx_[i] + k * wind * 3.0f);
    const float h = this->hail_ ? (float) HAIL_SIZE : (float) DROP_LEN;
    const float g = ground(this->dx_[i] + 1);
    if (this->dy_[i] + h > g) {
      // Брызги: две точки разлетаются вверх в стороны (не у каждой капли)
      if (!this->hail_ && (random_uint32() % 10) < 3) {
        int made = 0;
        for (int k = 0; k < NS && made < 2; k++) {
          if (this->slife_[k] > 0)
            continue;
          this->sx_[k] = this->dx_[i];
          this->sy_[k] = g - 3;
          this->svx_[k] = (made ? 1.3f : -1.3f) + wind;
          this->svy_[k] = -2.2f;
          this->slife_[k] = SPLASH_MS;
          lv_obj_t *s = lv_obj_get_child(this->splash_box_, k);
          lv_obj_set_pos(s, (int) this->sx_[k], (int) this->sy_[k]);
          show_(s, true);
          made++;
        }
      }
      // Новая капля появляется сверху, с поправкой на снос, чтобы при
      // сильном ветре не пустела подветренная сторона
      this->dx_[i] = wrap_x(rnd_(40, 426) - wind * 60.0f);
      this->dy_[i] = -DROP_LEN - 2 - rnd_(0, 120);
    }
    const int s = this->hail_ ? HAIL_SIZE : DROP_LEN;
    this->place_(lv_obj_get_child(this->drops_box_, i), (int) this->dx_[i], (int) this->dy_[i],
                 this->hail_ ? HAIL_SIZE : 2, s, this->dvis_[i]);
  }
}

void WeatherFx::splashes_() {
  const float kk = this->k_;
  for (int k = 0; k < NS; k++) {
    if (this->slife_[k] <= 0)
      continue;
    lv_obj_t *s = lv_obj_get_child(this->splash_box_, k);
    this->slife_[k] -= this->dt_ms_;
    if (this->slife_[k] <= 0) {
      this->slife_[k] = 0.0f;
      show_(s, false);
      continue;
    }
    this->sx_[k] += kk * this->svx_[k];
    this->sy_[k] += kk * this->svy_[k];
    this->svy_[k] += kk * 0.7f;
    lv_obj_set_pos(s, (int) this->sx_[k], (int) this->sy_[k]);
  }
}

void WeatherFx::flakes_(float wind) {
  // Крупные снежинки падают быстрее и качаются сильнее — так получается
  // глубина. У каждой свой ритм покачивания
  for (int i = 0; i < this->nf_; i++) {
    const bool big = i % 2;
    const float k = this->k_;
    this->fy_[i] += k * (big ? 1.7f + (i % 3) * 0.25f : 0.8f + (i % 3) * 0.2f);
    this->fph_[i] += k * ((big ? 0.07f : 0.05f) + i * 0.003f);
    this->fx_[i] =
        wrap_x(this->fx_[i] + k * (cosf(this->fph_[i]) * (big ? 0.8f : 0.45f) + wind * (big ? 1.4f : 0.9f)));
    if (this->fy_[i] > 470) {
      this->fy_[i] = -30 - rnd_(0, 40);
      this->fx_[i] = rnd_(30, 430);
    }
    // Размер знака снежинки: мелкие 14 px, крупные 26 px
    const int s = big ? 26 : 14;
    this->place_(lv_obj_get_child(this->flakes_box_, i), (int) this->fx_[i], (int) this->fy_[i], s, s,
                 this->fvis_[i]);
  }
}

void WeatherFx::clouds_() {
  // Облака медленно ползут вправо, у каждого своя скорость
  for (int i = 0; i < this->ncl_; i++) {
    this->cx_[i] += this->k_ * (0.25f + i * 0.08f);
    if (this->cx_[i] > 466)
      this->cx_[i] = -190;
    lv_obj_set_pos(lv_obj_get_child(this->clouds_box_, i), (int) this->cx_[i], CLOUD_Y[i]);
  }
}

void WeatherFx::stars_(uint32_t now) {
  // Звёзды мерцают: яркость меняется по синусу, у каждой свой ритм. Раз в
  // 200 мс — чаще глаз не заметит, а перерисовок меньше
  if (!this->stars_on_ || now - this->star_ms_ < 200)
    return;
  this->star_ms_ = now;
  const int hi = this->dim_ ? 0xFF : 0xC8;
  for (int i = 0; i < NST; i++) {
    this->stph_[i] += 0.25f + (i % 4) * 0.07f;
    const float k = 0.35f + 0.65f * (0.5f + 0.5f * sinf(this->stph_[i]));
    const int v = (int) (hi * k);
    lv_obj_set_style_bg_color(lv_obj_get_child(this->stars_box_, i),
                              lv_color_make((uint8_t) v, (uint8_t) v, (uint8_t) std::min(255, v + 16)), 0);
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
    // Начало — из-под облака, если оно на экране, иначе сверху
    int x0 = rnd_(130, 336), y0 = 40;
    for (int i = 0; i < this->ncl_; i++) {
      int mid = (int) this->cx_[i] + 90;
      if (mid > 120 && mid < 346) {
        x0 = mid;
        y0 = CLOUD_Y[i] + 50;
        break;
      }
    }
    const int n = 7 + rnd_(0, 4);
    const float step = rnd_(170, 250) / (float) (n - 1);
    float x = 0, minx = 0;
    for (int k = 0; k < n; k++) {
      if (k)
        x += rnd_(-24, 25);
      this->bolt_pts_[k].x = x;
      this->bolt_pts_[k].y = k * step;
      minx = std::min(minx, x);
    }
    // Сдвинуть точки так, чтобы слева остался отступ под толщину свечения
    for (int k = 0; k < n; k++)
      this->bolt_pts_[k].x -= minx - 6;
    lv_line_set_points(this->bolt_, this->bolt_pts_, n);
    lv_line_set_points(this->glow_, this->bolt_pts_, n);
    const int px = x0 + (int) minx - 6;
    lv_obj_set_pos(this->bolt_, px, y0);
    lv_obj_set_pos(this->glow_, px, y0);
    this->striking_ = true;
    this->strike_t0_ = now;
  }
  // Два всполоха: 0–80 мс и 160–300 мс
  const uint32_t t = now - this->strike_t0_;
  if (t >= 300) {
    this->end_strike_();
    return;
  }
  const bool on = t < 80 || t >= 160;
  if (on != this->bolt_on_) {
    this->bolt_on_ = on;
    show_(this->glow_, on);
    show_(this->bolt_, on);
  }
}

void WeatherFx::glass_() {
  const float k = this->k_;
  for (int i = 0; i < this->nd_; i++) {
    lv_obj_t *d = lv_obj_get_child(this->drops_box_, i);
    const int sz = 4 + 2 * (i % 3);
    if (this->gst_[i] == 0) {
      // Ждёт своей очереди
      this->glife_[i] -= this->dt_ms_;
      if (this->glife_[i] > 0)
        continue;
      int x, y, tries = 0;
      do {
        x = rnd_(40, 420);
        y = rnd_(30, 400);
      } while (++tries < 20 && ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 205 * 205 ||
                                this->excluded_(x, y, sz, sz)));
      this->dx_[i] = x;
      this->dy_[i] = y;
      this->gt_[i] = 0;
      this->glife_[i] = rnd_(2500, 7000);
      // Примерно каждая третья капля через секунду-две начинает сползать
      this->gslide_[i] = rnd_(0, 100) < 35 ? rnd_(600, 1800) : 0;
      this->gst_[i] = 1;
      lv_obj_set_pos(d, x, y);
      show_(d, true);
      this->dvis_[i] = true;
      continue;
    }
    this->gt_[i] += this->dt_ms_;
    bool gone = this->gt_[i] > this->glife_[i];
    if (!gone && this->gslide_[i] > 0 && this->gt_[i] > this->gslide_[i]) {
      // Сползает ~25 px/с — медленно, без мерцания
      this->dy_[i] += k * 1.25f;
      const float dxc = this->dx_[i] - 233.0f;
      const float bottom = 233.0f + sqrtf(std::max(0.0f, 215.0f * 215.0f - dxc * dxc));
      gone = this->dy_[i] + sz > bottom;
      if (!gone)
        this->place_(d, (int) this->dx_[i], (int) this->dy_[i], sz, sz, this->dvis_[i]);
    }
    if (gone) {
      show_(d, false);
      this->dvis_[i] = false;
      this->gst_[i] = 0;
      this->glife_[i] = rnd_(300, 2000);
    }
  }
}

// Большая Медведица: ковш и ручка, экранные координаты над погодой
static const int DIPPER[7][2] = {
    {133, 70},  // Бенетнаш (конец ручки)
    {182, 52},  // Мицар
    {221, 63},  // Алиот
    {263, 68},  // Мегрец
    {268, 19},  // Дубхе
    {328, 29},  // Мерак
    {323, 76},  // Фекда
};
static const int DIPPER_LINKS[7][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 6}, {6, 5}, {5, 4}, {4, 3}};

void WeatherFx::build_dipper_() {
  // Все объекты — внутри своего прозрачного слоя, который прячется целиком
  lv_obj_t *box = lv_obj_create(this->root_);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 466, 466);
  lv_obj_set_pos(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  // Под звёздами, но над облаками и каплями не важно — это ясная ночь
  lv_obj_move_to_index(box, 0);
  this->dip_box_ = box;

  // Пунктир: чёрточки по 4 px через 4 px, с отступом от звёзд
  this->ndash_ = 0;
  for (const auto &l : DIPPER_LINKS) {
    const float x0 = DIPPER[l[0]][0], y0 = DIPPER[l[0]][1];
    const float x1 = DIPPER[l[1]][0], y1 = DIPPER[l[1]][1];
    const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
    for (float s = 6.0f; s + 4.0f < len - 6.0f && this->ndash_ < NDASH; s += 8.0f) {
      lv_point_precise_t *pt = this->dash_pts_[this->ndash_];
      pt[0].x = (lv_value_precise_t) (x0 + ux * s);
      pt[0].y = (lv_value_precise_t) (y0 + uy * s);
      pt[1].x = (lv_value_precise_t) (x0 + ux * (s + 4.0f));
      pt[1].y = (lv_value_precise_t) (y0 + uy * (s + 4.0f));
      lv_obj_t *ln = lv_line_create(box);
      lv_obj_remove_style_all(ln);
      lv_line_set_points(ln, pt, 2);
      lv_obj_set_style_line_width(ln, 1, 0);
      lv_obj_set_style_line_rounded(ln, false, 0);
      lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
      this->dash_[this->ndash_++] = ln;
    }
  }
  // Звёзды ковша — крупнее и ярче случайных
  for (int i = 0; i < NDIP; i++) {
    lv_obj_t *st = lv_obj_create(box);
    lv_obj_remove_style_all(st);
    const int sz = (i == 1 || i == 2 || i == 4) ? 5 : 4;  // Мицар, Алиот, Дубхе — ярче
    lv_obj_set_size(st, sz, sz);
    lv_obj_set_pos(st, DIPPER[i][0] - sz / 2, DIPPER[i][1] - sz / 2);
    lv_obj_set_style_radius(st, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
    lv_obj_clear_flag(st, LV_OBJ_FLAG_CLICKABLE);
    this->dip_star_[i] = st;
  }
}

void WeatherFx::show_dipper_(bool on, bool dim) {
  if (!this->dip_box_) {
    if (!on)
      return;
    this->build_dipper_();
  }
  if (on) {
    for (int i = 0; i < NDIP; i++)
      lv_obj_set_style_bg_color(this->dip_star_[i], lv_color_hex(dim ? 0xFFF6DC : 0xD8D0B8), 0);
    for (int i = 0; i < this->ndash_; i++)
      lv_obj_set_style_line_color(this->dash_[i], lv_color_hex(dim ? 0x6A7FA8 : 0x34435E), 0);
  }
  show_(this->dip_box_, on);
}

void WeatherFx::frame(const Params &p) {
  if (!this->bound_)
    return;

  const int cnt = std::min(std::max(p.count, 0), ND);
  const int sig = p.mode | (cnt << 4) | (std::min(std::max(p.clouds, 0), NC) << 10) | ((p.storm ? 1 : 0) << 12) |
                  ((p.dim ? 1 : 0) << 13) | ((p.stars ? 1 : 0) << 14) | ((p.rain_style ? 1 : 0) << 15);
  if (sig != this->applied_) {
    // Если поменялась только яркость — перекрашиваем, но не перемешиваем
    const int no_dim = ~(1 << 13);
    const bool relayout = this->applied_ < 0 || (sig & no_dim) != (this->applied_ & no_dim);
    this->applied_ = sig;
    this->apply_(p, relayout);
  }

  const bool any = this->nd_ || this->nf_ || this->ncl_ || this->storm_ || this->stars_on_;
  if (!any || !p.active) {
    this->end_strike_();
    this->last_ms_ = 0;
    return;
  }
  const uint32_t now = millis();
  // Сколько прошло с прошлого кадра. После паузы (другая страница, экран
  // выключен) — как один обычный кадр, чтобы частицы не прыгали
  uint32_t dt = this->last_ms_ ? now - this->last_ms_ : 50;
  if (dt > 100)
    dt = 50;
  this->last_ms_ = now;
  this->dt_ms_ = (float) dt;
  this->k_ = dt / 50.0f;
  const float wind = this->wind_(p, now);
  this->stars_(now);
  if (this->glass_on_) {
    this->glass_();
  } else {
    this->drops_(wind);
    this->splashes_();
  }
  this->flakes_(wind);
  this->clouds_();
  this->lightning_(now);
}

}  // namespace weather_fx
}  // namespace esphome

#include "weather_fx.h"

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace weather_fx {

// Облака плывут у верхнего края круга и не задевают строку с погодой
// (она начинается на 113 px): иначе каждый сдвиг облака перерисовывал бы и её
static const int CLOUD_Y[3] = {26, 40, 12};
// Полный снос ветром — при такой скорости и сильнее, м/с
static const float FULL_WIND = 15.0f;
// Падающая капля: 2×20 px — тусклый «хвост» 14 px и яркая «голова» 6 px.
// Длина больше шага за кадр (5–6 px при 33 мс), так что старое и новое место
// перекрываются и перерисовываются одним куском
static const int DROP_LEN = 20;
static const int DROP_HEAD = 6;
static const int HAIL_SIZE = 6;
// Сколько живут брызги, мс
static const float SPLASH_MS = 250.0f;
// Снежинки: три формы Noto Sans Symbols 2 и две Material Design Icons
static const uint32_t FLAKE_CP[5] = {0x2744, 0x2745, 0x2746, 0xF0717, 0xF0F2A};

int WeatherFx::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

void WeatherFx::show_(lv_obj_t *o, bool v) {
  if (v)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void paint_cb(lv_event_t *e) {
  auto *self = static_cast<WeatherFx *>(lv_event_get_user_data(e));
  self->paint(lv_event_get_layer(e));
}

void WeatherFx::bind(lv_obj_t *root, lv_obj_t *clouds, lv_obj_t *bolt, lv_obj_t *glow, const lv_font_t *flake_s,
                     const lv_font_t *flake_l) {
  this->root_ = root;
  this->clouds_box_ = clouds;
  this->bolt_ = bolt;
  this->glow_ = glow;
  this->font_s_ = flake_s;
  this->font_l_ = flake_l;
  if (!root || !clouds || !bolt || !glow || !flake_s || !flake_l ||
      lv_obj_get_child_count(clouds) < (uint32_t) NC)
    return;

  // Холст во весь слой погоды, поверх облаков: на нём рисуются все частицы
  lv_obj_t *pt = lv_obj_create(root);
  lv_obj_remove_style_all(pt);
  lv_obj_set_size(pt, 466, 466);
  lv_obj_set_pos(pt, 0, 0);
  lv_obj_clear_flag(pt, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(pt, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(pt, paint_cb, LV_EVENT_DRAW_MAIN, this);
  this->paint_ = pt;

  // Размеры знаков снежинок — для отметки участков к перерисовке
  for (int i = 0; i < NF; i++) {
    const lv_font_t *f = (i % 2) ? flake_l : flake_s;
    this->fw_[i] = lv_font_get_glyph_width(f, FLAKE_CP[i % 5], 0) + 2;
    this->fh_[i] = lv_font_get_line_height(f) + 2;
  }
  this->bound_ = true;
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

void WeatherFx::invalidate_(const lv_area_t &a) {
  // Координаты частиц — относительно холста; LVGL ждёт экранные
  lv_area_t oc;
  lv_obj_get_coords(this->paint_, &oc);
  lv_area_t abs = {(int32_t) (a.x1 + oc.x1), (int32_t) (a.y1 + oc.y1), (int32_t) (a.x2 + oc.x1),
                   (int32_t) (a.y2 + oc.y1)};
  lv_obj_invalidate_area(this->paint_, &abs);
}

void WeatherFx::mark_(Spot &s, bool on, int x, int y, int w, int h) {
  if (!on && !s.on)
    return;
  lv_area_t n = {x, y, x + w - 1, y + h - 1};
  if (on && s.on && n.x1 == s.a.x1 && n.y1 == s.a.y1 && n.x2 == s.a.x2 && n.y2 == s.a.y2)
    return;
  if (on && s.on && n.x1 <= s.a.x2 + 4 && s.a.x1 <= n.x2 + 4 && n.y1 <= s.a.y2 + 4 && s.a.y1 <= n.y2 + 4) {
    // Старое и новое место рядом — один общий участок
    lv_area_t u = {std::min(n.x1, s.a.x1), std::min(n.y1, s.a.y1), std::max(n.x2, s.a.x2),
                   std::max(n.y2, s.a.y2)};
    this->invalidate_(u);
  } else {
    if (s.on)
      this->invalidate_(s.a);
    if (on)
      this->invalidate_(n);
  }
  s.a = n;
  s.on = on;
}

void WeatherFx::hide_all_() {
  for (auto &s : this->ds_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (auto &s : this->fs_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (auto &s : this->ss_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (auto &s : this->sts_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (auto &l : this->slife_)
    l = 0;
}

void WeatherFx::paint(lv_layer_t *layer) {
  lv_area_t oc;
  lv_obj_get_coords(this->paint_, &oc);
  const lv_area_t &clip = layer->_clip_area;
  lv_area_t abs;
  // Экранный прямоугольник частицы; false — если не попадает в участок
  auto place = [&](const Spot &s) {
    if (!s.on)
      return false;
    abs = {(int32_t) (s.a.x1 + oc.x1), (int32_t) (s.a.y1 + oc.y1), (int32_t) (s.a.x2 + oc.x1),
           (int32_t) (s.a.y2 + oc.y1)};
    return abs.x1 <= clip.x2 && abs.x2 >= clip.x1 && abs.y1 <= clip.y2 && abs.y2 >= clip.y1;
  };

  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.opa = LV_OPA_COVER;

  // Звёзды
  fill.radius = 1;
  for (int i = 0; i < NST; i++) {
    if (!place(this->sts_[i]))
      continue;
    fill.color = this->stc_[i];
    lv_draw_fill(layer, &fill, &abs);
  }

  // Капли и градины
  for (int i = 0; i < ND; i++) {
    if (!place(this->ds_[i]))
      continue;
    if (this->glass_on_) {
      // Капля на стекле — кружок со светлым ободком
      fill.radius = LV_RADIUS_CIRCLE;
      fill.color = this->c_drop_;
      lv_draw_fill(layer, &fill, &abs);
      lv_draw_border_dsc_t b;
      lv_draw_border_dsc_init(&b);
      b.radius = LV_RADIUS_CIRCLE;
      b.color = this->c_rim_;
      b.width = 1;
      b.opa = LV_OPA_COVER;
      lv_draw_border(layer, &b, &abs);
    } else if (this->hail_) {
      fill.radius = 3;
      fill.color = this->c_drop_;
      lv_draw_fill(layer, &fill, &abs);
    } else {
      // Тусклый хвост и яркая голова
      fill.radius = 0;
      lv_area_t tail = abs, head = abs;
      tail.y2 = abs.y1 + DROP_LEN - DROP_HEAD - 1;
      head.y1 = tail.y2 + 1;
      fill.color = this->c_tail_;
      lv_draw_fill(layer, &fill, &tail);
      fill.color = this->c_drop_;
      lv_draw_fill(layer, &fill, &head);
    }
  }

  // Брызги
  fill.radius = 1;
  fill.color = this->c_splash_;
  for (int i = 0; i < NS; i++) {
    if (!place(this->ss_[i]))
      continue;
    lv_draw_fill(layer, &fill, &abs);
  }

  // Снежинки
  lv_draw_letter_dsc_t let;
  lv_draw_letter_dsc_init(&let);
  let.opa = LV_OPA_COVER;
  for (int i = 0; i < NF; i++) {
    if (!place(this->fs_[i]))
      continue;
    const bool big = i % 2;
    let.font = big ? this->font_l_ : this->font_s_;
    let.color = big ? this->c_flake_l_ : this->c_flake_s_;
    let.unicode = FLAKE_CP[i % 5];
    lv_point_t pt = {abs.x1 + 1, abs.y1 + 1};
    lv_draw_letter(layer, &let, &pt);
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

  // В приглушённом режиме палитра ярче: при низкой яркости панели тёмные
  // частицы сливаются с чёрным
  if (this->glass_on_) {
    this->c_drop_ = lv_color_hex(dim ? 0x3F8FC4 : 0x1D4F72);
    this->c_rim_ = lv_color_hex(dim ? 0xBFE8FF : 0x5FA8D8);
  } else if (this->hail_) {
    this->c_drop_ = lv_color_hex(dim ? 0xFFFFFF : 0xCFD8DC);
  } else {
    this->c_drop_ = lv_color_hex(dim ? 0x7FD4FF : 0x2A6F99);
    this->c_tail_ = lv_color_hex(dim ? 0x2E5F80 : 0x123447);
  }
  this->c_splash_ = lv_color_hex(dim ? 0x7FD4FF : 0x2A6F99);
  this->c_flake_s_ = lv_color_hex(dim ? 0xAEB8C2 : 0x59636D);
  this->c_flake_l_ = lv_color_hex(dim ? 0xFFFFFF : 0x9AA4AE);

  if (relayout) {
    // Новая погода — всё с чистого листа
    this->hide_all_();
    for (int i = 0; i < ND; i++) {
      this->dx_[i] = rnd_(40, 426);
      this->dy_[i] = rnd_(0, 440);
      this->gst_[i] = 0;
      this->glife_[i] = rnd_(0, 3000);
    }
    for (int i = 0; i < NF; i++) {
      this->fx_[i] = rnd_(30, 430);
      this->fy_[i] = rnd_(0, 440);
      this->fph_[i] = rnd_(0, 628) / 100.0f;
    }
    // Звёзды — в верхней половине круга, вокруг и выше времени, но не на
    // месте Большой Медведицы
    for (int i = 0; i < NST; i++) {
      int x, y;
      do {
        x = rnd_(50, 416);
        y = rnd_(30, 220);
      } while ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 215 * 215 ||
               (x > 82 && x < 372 && y > 22 && y < 122));
      this->stx_[i] = x;
      this->sty_[i] = y;
      this->stph_[i] = rnd_(0, 628) / 100.0f;
      this->stc_[i] = lv_color_hex(0x808890);
    }
  } else {
    // Поменялась только яркость — перекрасить то, что на экране
    for (auto &s : this->ds_)
      if (s.on)
        this->invalidate_(s.a);
    for (auto &s : this->fs_)
      if (s.on)
        this->invalidate_(s.a);
  }
  if (this->stars_on_) {
    for (int i = 0; i < NST; i++)
      this->mark_(this->sts_[i], true, this->stx_[i], this->sty_[i], 3, 3);
  } else {
    for (auto &s : this->sts_)
      this->mark_(s, false, 0, 0, 1, 1);
  }
  // Лишние капли и снежинки — убрать
  for (int i = this->nd_; i < ND; i++)
    this->mark_(this->ds_[i], false, 0, 0, 1, 1);
  for (int i = this->nf_; i < NF; i++)
    this->mark_(this->fs_[i], false, 0, 0, 1, 1);

  this->show_dipper_(this->stars_on_, dim);

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
  const float k = this->k_;
  const int w = this->hail_ ? HAIL_SIZE : 2;
  const int h = this->hail_ ? HAIL_SIZE : DROP_LEN;
  for (int i = 0; i < this->nd_; i++) {
    // Дождь падает неспешно (140–190 px/с)
    this->dy_[i] += k * (this->hail_ ? 7.0f + (i % 3) : 7.0f + (i % 3) * 1.2f);
    this->dx_[i] = wrap_x(this->dx_[i] + k * wind * 3.0f);
    const float g = ground(this->dx_[i] + 1);
    if (this->dy_[i] + h > g) {
      // Брызги: две точки разлетаются вверх в стороны (не у каждой капли)
      if (!this->hail_ && (random_uint32() % 10) < 3) {
        int made = 0;
        for (int j = 0; j < NS && made < 2; j++) {
          if (this->slife_[j] > 0)
            continue;
          this->sx_[j] = this->dx_[i];
          this->sy_[j] = g - 3;
          this->svx_[j] = (made ? 1.3f : -1.3f) + wind;
          this->svy_[j] = -2.2f;
          this->slife_[j] = SPLASH_MS;
          made++;
        }
      }
      // Новая капля сверху, с поправкой на снос
      this->dx_[i] = wrap_x(rnd_(40, 426) - wind * 60.0f);
      this->dy_[i] = -DROP_LEN - 2 - rnd_(0, 120);
    }
    const int x = (int) this->dx_[i], y = (int) this->dy_[i];
    this->mark_(this->ds_[i], !this->excluded_(x, y, w, h), x, y, w, h);
  }
}

void WeatherFx::glass_() {
  const float k = this->k_;
  for (int i = 0; i < this->nd_; i++) {
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
      this->mark_(this->ds_[i], true, x, y, sz, sz);
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
      if (!gone) {
        const int x = (int) this->dx_[i], y = (int) this->dy_[i];
        this->mark_(this->ds_[i], !this->excluded_(x, y, sz, sz), x, y, sz, sz);
      }
    }
    if (gone) {
      this->mark_(this->ds_[i], false, 0, 0, 1, 1);
      this->gst_[i] = 0;
      this->glife_[i] = rnd_(300, 2000);
    }
  }
}

void WeatherFx::splashes_() {
  const float kk = this->k_;
  for (int j = 0; j < NS; j++) {
    if (this->slife_[j] <= 0) {
      this->mark_(this->ss_[j], false, 0, 0, 1, 1);
      continue;
    }
    this->slife_[j] -= this->dt_ms_;
    this->sx_[j] += kk * this->svx_[j];
    this->sy_[j] += kk * this->svy_[j];
    this->svy_[j] += kk * 0.7f;
    this->mark_(this->ss_[j], this->slife_[j] > 0, (int) this->sx_[j], (int) this->sy_[j], 3, 3);
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
    const int x = (int) this->fx_[i], y = (int) this->fy_[i];
    this->mark_(this->fs_[i], !this->excluded_(x, y, this->fw_[i], this->fh_[i]), x, y, this->fw_[i],
                this->fh_[i]);
  }
}

void WeatherFx::clouds_() {
  // Облака медленно ползут вправо, у каждого своя скорость. Сдвигаем только
  // когда меняется целый пиксель — иначе LVGL зря пересчитывает раскладку
  for (int i = 0; i < this->ncl_; i++) {
    const int before = (int) this->cx_[i];
    this->cx_[i] += this->k_ * (0.25f + i * 0.08f);
    if (this->cx_[i] > 466)
      this->cx_[i] = -190;
    if ((int) this->cx_[i] != before)
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
    this->stc_[i] = lv_color_make((uint8_t) v, (uint8_t) v, (uint8_t) std::min(255, v + 16));
    if (this->sts_[i].on)
      this->invalidate_(this->sts_[i].a);
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

// Большая Медведица: ковш и ручка во всю «шапку» круга над строкой погоды
// (она начинается на 113 px). Экранные координаты; все звёзды не дальше
// 225 px от центра, чтобы не уходить за край круга
static const int DIPPER[7][2] = {
    {95, 103},   // Бенетнаш (конец ручки)
    {161, 79},   // Мицар
    {214, 93},   // Алиот
    {271, 100},  // Мегрец
    {277, 34},   // Дубхе
    {358, 48},   // Мерак
    {352, 111},  // Фекда
};
static const int DIPPER_LINKS[7][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 6}, {6, 5}, {5, 4}, {4, 3}};

void WeatherFx::build_dipper_() {
  // Все объекты — внутри своего прозрачного слоя, который прячется целиком.
  // Он неподвижен, поэтому обычные объекты LVGL здесь ничего не стоят
  lv_obj_t *box = lv_obj_create(this->root_);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, 466, 466);
  lv_obj_set_pos(box, 0, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_to_index(box, 0);
  this->dip_box_ = box;

  // Пунктир: чёрточки по 5 px через 5 px, с отступом от звёзд
  this->ndash_ = 0;
  for (const auto &l : DIPPER_LINKS) {
    const float x0 = DIPPER[l[0]][0], y0 = DIPPER[l[0]][1];
    const float x1 = DIPPER[l[1]][0], y1 = DIPPER[l[1]][1];
    const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    const float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
    for (float s = 8.0f; s + 5.0f < len - 8.0f && this->ndash_ < NDASH; s += 10.0f) {
      lv_point_precise_t *pt = this->dash_pts_[this->ndash_];
      pt[0].x = (lv_value_precise_t) (x0 + ux * s);
      pt[0].y = (lv_value_precise_t) (y0 + uy * s);
      pt[1].x = (lv_value_precise_t) (x0 + ux * (s + 5.0f));
      pt[1].y = (lv_value_precise_t) (y0 + uy * (s + 5.0f));
      lv_obj_t *ln = lv_line_create(box);
      lv_obj_remove_style_all(ln);
      lv_line_set_points(ln, pt, 2);
      lv_obj_set_style_line_width(ln, 2, 0);
      lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
      this->dash_[this->ndash_++] = ln;
    }
  }
  // Звёзды ковша — крупнее и ярче случайных
  for (int i = 0; i < NDIP; i++) {
    lv_obj_t *st = lv_obj_create(box);
    lv_obj_remove_style_all(st);
    const int sz = (i == 1 || i == 2 || i == 4) ? 8 : 6;  // Мицар, Алиот, Дубхе — ярче
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
  // выключен, касание) — как один обычный кадр, чтобы частицы не прыгали
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

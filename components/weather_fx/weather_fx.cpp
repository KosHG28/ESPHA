#include "weather_fx.h"

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "sky.h"

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
// Листья: обычный и кленовый, из Material Design Icons
static const uint32_t LEAF_CP[2] = {0xF032A, 0xF0C93};
// Осенние цвета: оранжевый, тёмно-оранжевый, красный, жёлтый
static const uint32_t LEAF_COLORS[4] = {0xE67E22, 0xD35400, 0xC0392B, 0xF1C40F};
static const uint32_t LEAF_COLORS_DIM[4] = {0xFFA24D, 0xFF7A26, 0xFF5A4A, 0xFFE04D};

static uint32_t glyph_cp(int set, int i) { return set ? LEAF_CP[(i / 2) % 2] : FLAKE_CP[i % 5]; }
// Солнце — справа в «шапке» круга, мимо значка двери посередине
static const int SUN_X = 300, SUN_Y = 74, SUN_R = 40;
// Созвездие вписывается в «шапку» над строкой погоды: центр и размеры
// прямоугольника, подпись — над ним
static const float BOX_CX = 233.0f, BOX_CY = 82.0f, BOX_W = 280.0f, BOX_H = 60.0f;
static const int CAP_Y = 24, CAP_W = 220;
// Созвездие видно, если его середина не ниже 25° над горизонтом
static const float CON_MIN_ALT = 0.4226f;  // sin 25°
// Сменять созвездие раз в 10 минут, пересчитывать поворот раз в 5
static const int CON_SLOT_S = 600, CON_RECALC_MIN = 5;
// Гирлянда: радиус, цвета лампочек
static const int GAR_R = 221;
static const uint32_t GAR_PAL[4] = {0xFF3B30, 0xFFD60A, 0x30D158, 0x0A84FF};
static const float PI_F = 3.14159265f;

int WeatherFx::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

void WeatherFx::show_(lv_obj_t *o, bool v) {
  if (v)
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void paint_front_cb(lv_event_t *e) {
  auto *self = static_cast<WeatherFx *>(lv_event_get_user_data(e));
  self->paint_front(lv_event_get_layer(e));
}

static void paint_back_cb(lv_event_t *e) {
  auto *self = static_cast<WeatherFx *>(lv_event_get_user_data(e));
  self->paint_back(lv_event_get_layer(e));
}

static bool hit(const lv_area_t &a, const lv_area_t &clip) {
  return a.x1 <= clip.x2 && a.x2 >= clip.x1 && a.y1 <= clip.y2 && a.y2 >= clip.y1;
}

void WeatherFx::bind(lv_obj_t *root, lv_obj_t *clouds, lv_obj_t *bolt, lv_obj_t *glow, const lv_font_t *flake_s,
                     const lv_font_t *flake_l, const lv_font_t *caption) {
  this->root_ = root;
  this->clouds_box_ = clouds;
  this->bolt_ = bolt;
  this->glow_ = glow;
  this->font_s_ = flake_s;
  this->font_l_ = flake_l;
  this->font_cap_ = caption;
  if (!root || !clouds || !bolt || !glow || !flake_s || !flake_l || !caption ||
      lv_obj_get_child_count(clouds) < (uint32_t) NC)
    return;

  // Два холста во весь слой погоды: передний — поверх облаков (осадки,
  // гирлянда), задний — под ними (небо)
  auto canvas = [root, this](lv_event_cb_t cb) {
    lv_obj_t *pt = lv_obj_create(root);
    lv_obj_remove_style_all(pt);
    lv_obj_set_size(pt, 466, 466);
    lv_obj_set_pos(pt, 0, 0);
    lv_obj_clear_flag(pt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(pt, cb, LV_EVENT_DRAW_MAIN, this);
    return pt;
  };
  this->paint_ = canvas(paint_front_cb);
  this->back_ = canvas(paint_back_cb);
  lv_obj_move_to_index(this->back_, 0);

  // Лампочки гирлянды — по кругу у самого края экрана
  for (int i = 0; i < NG; i++) {
    const float a = (i + 0.5f) * 2.0f * PI_F / NG;
    this->gx_[i] = 233 + (int) lroundf(GAR_R * cosf(a));
    this->gy_[i] = 233 + (int) lroundf(GAR_R * sinf(a));
  }

  // Где именно рисуется знак снежинки. lv_draw_letter считает точку
  // серединой знака по горизонтали и линией основания по вертикали, так что
  // знак лежит выше и левее точки. Участок к перерисовке должен закрывать его
  // целиком — иначе при сдвиге края знака не стираются и тянутся шлейфом
  for (int set = 0; set < 2; set++) {
    for (int i = 0; i < NF; i++) {
      const lv_font_t *f = (i % 2) ? flake_l : flake_s;
      lv_font_glyph_dsc_t g;
      const int lh = lv_font_get_line_height(f);
      if (lv_font_get_glyph_dsc(f, &g, glyph_cp(set, i), 0) && g.box_w > 0 && g.box_h > 0) {
        this->fox_[set][i] = g.ofs_x - g.adv_w / 2 - 2;
        this->foy_[set][i] = -g.box_h - g.ofs_y - 2;
        this->fw_[set][i] = g.box_w + 4;
        this->fh_[set][i] = g.box_h + 4;
      } else {
        // Нет данных о знаке — с большим запасом вокруг точки
        this->fox_[set][i] = -lh;
        this->foy_[set][i] = -lh;
        this->fw_[set][i] = 2 * lh;
        this->fh_[set][i] = 2 * lh;
      }
    }
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
  this->mark_(this->met_spot_, false, 0, 0, 1, 1);
  this->met_on_ = false;
}

void WeatherFx::paint_back(lv_layer_t *layer) {
  lv_area_t oc;
  lv_obj_get_coords(this->back_, &oc);
  const lv_area_t &clip = layer->_clip_area;
  lv_area_t abs;
  auto to_abs = [&](const lv_area_t &a) {
    abs = {(int32_t) (a.x1 + oc.x1), (int32_t) (a.y1 + oc.y1), (int32_t) (a.x2 + oc.x1), (int32_t) (a.y2 + oc.y1)};
    return hit(abs, clip);
  };
  auto place = [&](const Spot &s) { return s.on && to_abs(s.a); };

  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.opa = LV_OPA_COVER;
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.opa = LV_OPA_COVER;
  const bool dim = this->dim_;

  // Солнце: мягкий ореол, диск и медленно вращающиеся лучи
  if (place(this->sun_spot_)) {
    const int cx = SUN_X + oc.x1, cy = SUN_Y + oc.y1;
    auto disc = [&](int r, uint32_t c, lv_opa_t o) {
      lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
      fill.radius = LV_RADIUS_CIRCLE;
      fill.color = lv_color_hex(c);
      fill.opa = o;
      lv_draw_fill(layer, &fill, &a);
    };
    disc(30, 0xFFA000, dim ? 44 : 30);
    disc(24, 0xFFB300, dim ? 70 : 50);
    ln.width = 3;
    ln.round_start = 1;
    ln.round_end = 1;
    ln.color = lv_color_hex(dim ? 0xFFCA28 : 0xFFB300);
    for (int k = 0; k < 12; k++) {
      const float a = this->sun_ang_ + k * (2.0f * PI_F / 12);
      const float r0 = 24.0f, r1 = (k % 2) ? 31.0f : 36.0f;
      const float c = cosf(a), sn = sinf(a);
      ln.p1.x = (lv_value_precise_t) (cx + r0 * c);
      ln.p1.y = (lv_value_precise_t) (cy + r0 * sn);
      ln.p2.x = (lv_value_precise_t) (cx + r1 * c);
      ln.p2.y = (lv_value_precise_t) (cy + r1 * sn);
      lv_draw_line(layer, &ln);
    }
    disc(17, dim ? 0xFFD54F : 0xFFC21A, LV_OPA_COVER);
    fill.opa = LV_OPA_COVER;
    ln.round_start = 0;
    ln.round_end = 0;
  }

  // Мерцающие звёзды
  fill.radius = 1;
  for (int i = 0; i < NST; i++) {
    if (!place(this->sts_[i]))
      continue;
    fill.color = this->stc_[i];
    lv_draw_fill(layer, &fill, &abs);
  }

  // Созвездие: пунктир, звёзды и подпись
  if (this->con_on_) {
    if (to_abs(this->con_area_)) {
      ln.width = 2;
      ln.color = lv_color_hex(dim ? 0x6A7FA8 : 0x34435E);
      for (int i = 0; i < this->ndash_; i++) {
        const lv_point_precise_t *d = this->dash_[i];
        lv_area_t b = {(int32_t) std::min(d[0].x, d[1].x) + oc.x1 - 2, (int32_t) std::min(d[0].y, d[1].y) + oc.y1 - 2,
                       (int32_t) std::max(d[0].x, d[1].x) + oc.x1 + 2, (int32_t) std::max(d[0].y, d[1].y) + oc.y1 + 2};
        if (!hit(b, clip))
          continue;
        ln.p1 = {d[0].x + oc.x1, d[0].y + oc.y1};
        ln.p2 = {d[1].x + oc.x1, d[1].y + oc.y1};
        lv_draw_line(layer, &ln);
      }
      fill.radius = LV_RADIUS_CIRCLE;
      fill.color = lv_color_hex(dim ? 0xFFF6DC : 0xD8D0B8);
      for (int i = 0; i < this->cn_; i++) {
        const int r = this->csz_[i] / 2;
        lv_area_t a = {this->csx_[i] - r + oc.x1, this->csy_[i] - r + oc.y1, this->csx_[i] - r + this->csz_[i] - 1 + oc.x1,
                       this->csy_[i] - r + this->csz_[i] - 1 + oc.y1};
        if (hit(a, clip))
          lv_draw_fill(layer, &fill, &a);
      }
    }
    if (to_abs(this->cap_area_)) {
      lv_draw_label_dsc_t lb;
      lv_draw_label_dsc_init(&lb);
      lb.text = this->con_name_;
      lb.font = this->font_cap_;
      lb.color = lv_color_hex(dim ? 0x8898B8 : 0x4C5A74);
      lb.align = LV_TEXT_ALIGN_CENTER;
      lb.opa = LV_OPA_COVER;
      lv_draw_label(layer, &lb, &abs);
    }
  }

  // Метеор: хвост из трёх отрезков — от тусклого к яркому, и яркая голова
  if (place(this->met_spot_)) {
    ln.width = 2;
    ln.round_start = 1;
    ln.round_end = 1;
    ln.color = lv_color_hex(0xE8F0FF);
    const float hx = this->met_hx_ + oc.x1, hy = this->met_hy_ + oc.y1;
    const float tx = this->met_tx_ + oc.x1, ty = this->met_ty_ + oc.y1;
    static const lv_opa_t SEG[3] = {60, 140, 255};
    for (int k = 0; k < 3; k++) {
      const float a = k / 3.0f, b = (k + 1) / 3.0f;
      ln.p1 = {(lv_value_precise_t) (tx + (hx - tx) * a), (lv_value_precise_t) (ty + (hy - ty) * a)};
      ln.p2 = {(lv_value_precise_t) (tx + (hx - tx) * b), (lv_value_precise_t) (ty + (hy - ty) * b)};
      ln.opa = (lv_opa_t) (SEG[k] * this->met_opa_ / 255);
      lv_draw_line(layer, &ln);
    }
    fill.radius = LV_RADIUS_CIRCLE;
    fill.color = lv_color_white();
    fill.opa = this->met_opa_;
    lv_area_t h = {(int32_t) hx - 2, (int32_t) hy - 2, (int32_t) hx + 2, (int32_t) hy + 2};
    lv_draw_fill(layer, &fill, &h);
  }
}

void WeatherFx::paint_front(lv_layer_t *layer) {
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
    return hit(abs, clip);
  };

  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.opa = LV_OPA_COVER;

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

  // Снежинки или листья
  lv_draw_letter_dsc_t let;
  lv_draw_letter_dsc_init(&let);
  let.opa = LV_OPA_COVER;
  const int set = this->leaves_ ? 1 : 0;
  for (int i = 0; i < NF; i++) {
    if (!place(this->fs_[i]))
      continue;
    const bool big = i % 2;
    let.font = big ? this->font_l_ : this->font_s_;
    if (set)
      let.color = big ? this->c_leaf_[(i / 2) % 4] : this->c_leaf_s_[(i / 2) % 4];
    else
      let.color = big ? this->c_flake_l_ : this->c_flake_s_;
    let.unicode = glyph_cp(set, i);
    // Обратно из прямоугольника знака к точке рисования
    lv_point_t pt = {abs.x1 - this->fox_[set][i], abs.y1 - this->foy_[set][i]};
    lv_draw_letter(layer, &let, &pt);
  }

  // Гирлянда: тёмно-зелёный провод по краю и перемигивающиеся лампочки
  if (this->gar_on_) {
    // Провод рисуем, только если участок задевает кольцо
    const int cx = 233 + oc.x1, cy = 233 + oc.y1;
    const int x1 = clip.x1 - cx, x2 = clip.x2 - cx, y1 = clip.y1 - cy, y2 = clip.y2 - cy;
    const int nx = std::max(x1, std::max(0, -x2)), ny = std::max(y1, std::max(0, -y2));
    const int fx = std::max(std::abs(x1), std::abs(x2)), fy = std::max(std::abs(y1), std::abs(y2));
    if (nx * nx + ny * ny <= (GAR_R + 5) * (GAR_R + 5) && fx * fx + fy * fy >= (GAR_R - 4) * (GAR_R - 4)) {
      lv_draw_arc_dsc_t arc;
      lv_draw_arc_dsc_init(&arc);
      arc.center = {cx, cy};
      arc.radius = GAR_R + 1;
      arc.width = 2;
      arc.start_angle = 0;
      arc.end_angle = 360;
      arc.color = lv_color_hex(0x1E4A2A);
      arc.opa = LV_OPA_COVER;
      lv_draw_arc(layer, &arc);
    }
    fill.radius = LV_RADIUS_CIRCLE;
    for (int i = 0; i < NG; i++) {
      if (!place(this->gs_[i]))
        continue;
      const lv_color_t c = lv_color_hex(GAR_PAL[i % 4]);
      const bool lit = (i + this->gar_step_) % 3 != 0;
      const int x = this->gx_[i] + oc.x1, y = this->gy_[i] + oc.y1;
      if (lit) {
        lv_area_t g = {x - 7, y - 7, x + 7, y + 7};
        fill.color = c;
        fill.opa = 70;
        lv_draw_fill(layer, &fill, &g);
      }
      lv_area_t b = {x - 4, y - 4, x + 4, y + 4};
      fill.color = lit ? c : lv_color_mix(c, lv_color_black(), 70);
      fill.opa = LV_OPA_COVER;
      lv_draw_fill(layer, &fill, &b);
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
  this->nf_ = std::min(p.mode == 2 || p.mode == 5 ? cnt : (p.mode == 3 ? cnt / 2 : 0), NF);
  this->leaves_ = p.mode == 5;
  this->ncl_ = std::min(std::max(p.clouds, 0), NC);
  this->storm_ = p.storm;
  if (p.stars && !this->stars_on_)
    this->con_force_ = true;
  this->stars_on_ = p.stars;
  this->sun_on_ = p.sun;
  // Провод гирлянды идёт по всему кругу — при включении и выключении
  // перерисовать слой целиком
  if (p.garland != this->gar_on_)
    lv_obj_invalidate(this->paint_);
  this->gar_on_ = p.garland;
  if (!this->storm_)
    this->end_strike_();
  show_(this->root_,
        this->nd_ || this->nf_ || this->ncl_ || this->storm_ || this->stars_on_ || this->sun_on_ || this->gar_on_);

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
  // Листья: ближние — яркие, дальние — притушенные
  for (int k = 0; k < 4; k++) {
    this->c_leaf_[k] = lv_color_hex(dim ? LEAF_COLORS_DIM[k] : LEAF_COLORS[k]);
    this->c_leaf_s_[k] = lv_color_mix(this->c_leaf_[k], lv_color_black(), dim ? 190 : 140);
  }

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
    // месте созвездия
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
    // Поменялись яркость, солнце или гирлянда — перерисовать слой целиком,
    // это разовая перерисовка
    lv_obj_invalidate(this->back_);
    lv_obj_invalidate(this->paint_);
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

  // Созвездие гаснет вместе со звёздами; зажигает его sky_()
  if (!this->stars_on_ && this->con_on_) {
    this->invalidate_(this->con_area_);
    this->invalidate_(this->cap_area_);
    this->con_on_ = false;
  }
  if (!this->stars_on_ && this->met_on_) {
    this->mark_(this->met_spot_, false, 0, 0, 1, 1);
    this->met_on_ = false;
  }
  this->mark_(this->sun_spot_, this->sun_on_, SUN_X - SUN_R, SUN_Y - SUN_R, 2 * SUN_R + 1, 2 * SUN_R + 1);
  for (int i = 0; i < NG; i++)
    this->mark_(this->gs_[i], this->gar_on_, this->gx_[i] - 8, this->gy_[i] - 8, 17, 17);

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

void WeatherFx::glass_(float k, float dt) {
  for (int i = 0; i < this->nd_; i++) {
    const int sz = 4 + 2 * (i % 3);
    if (this->gst_[i] == 0) {
      // Ждёт своей очереди
      this->glife_[i] -= dt;
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
    this->gt_[i] += dt;
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

void WeatherFx::flakes_(float wind, float ks) {
  // Крупные снежинки падают быстрее и качаются сильнее — так получается
  // глубина. У каждой свой ритм покачивания. Мелкие медленные, их двигаем
  // только на редких тиках (ks > 0)
  for (int i = 0; i < this->nf_; i++) {
    const bool big = i % 2;
    const float k = big ? this->k_ : ks;
    if (k <= 0)
      continue;
    if (this->leaves_) {
      // Листья падают медленнее, раскачиваются шире и сильнее летят по ветру
      this->fy_[i] += k * (big ? 1.1f + (i % 3) * 0.2f : 0.6f + (i % 3) * 0.15f);
      this->fph_[i] += k * ((big ? 0.10f : 0.08f) + i * 0.004f);
      this->fx_[i] =
          wrap_x(this->fx_[i] + k * (cosf(this->fph_[i]) * (big ? 1.5f : 0.9f) + wind * (big ? 2.6f : 1.8f)));
    } else {
      this->fy_[i] += k * (big ? 1.7f + (i % 3) * 0.25f : 0.8f + (i % 3) * 0.2f);
      this->fph_[i] += k * ((big ? 0.07f : 0.05f) + i * 0.003f);
      this->fx_[i] =
          wrap_x(this->fx_[i] + k * (cosf(this->fph_[i]) * (big ? 0.8f : 0.45f) + wind * (big ? 1.4f : 0.9f)));
    }
    if (this->fy_[i] > 470) {
      this->fy_[i] = -30 - rnd_(0, 40);
      this->fx_[i] = rnd_(30, 430);
    }
    const int set = this->leaves_ ? 1 : 0;
    const int w = this->fw_[set][i], h = this->fh_[set][i];
    const int x = (int) this->fx_[i] + this->fox_[set][i], y = (int) this->fy_[i] + this->foy_[set][i];
    this->mark_(this->fs_[i], !this->excluded_(x, y, w, h), x, y, w, h);
  }
}

void WeatherFx::clouds_() {
  // Облака медленно ползут вправо (2–3 px/с), у каждого своя скорость.
  // Сдвигаем только когда меняется целый пиксель: каждый сдвиг перерисовывает
  // всё облако целиком, поэтому чем медленнее, тем дешевле
  for (int i = 0; i < this->ncl_; i++) {
    const int before = (int) this->cx_[i];
    this->cx_[i] += this->k_ * (0.10f + i * 0.03f);
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
  float k = std::min(BOX_W / bw, BOX_H / bh);
  for (int it = 0; it < 40; it++) {
    bool ok = true;
    for (int i = 0; i < n; i++) {
      const float x = BOX_CX + (pts[i][0] - mx) * k, y = BOX_CY - (pts[i][1] - my) * k;
      ox[i] = (int) lroundf(x);
      oy[i] = (int) lroundf(y);
      const float dx = x - 233.0f, dy = y - 233.0f;
      if (sqrtf(dx * dx + dy * dy) + sz[i] / 2 + 3 > 224.0f)
        ok = false;
    }
    if (ok)
      break;
    k *= 0.94f;
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
  int sz[NCS], ox[NCS], oy[NCS];
  for (int i = 0; i < n; i++)
    sz[i] = star_size(SKY_STARS[c.first + i].mag);
  float size;
  fit(pts, sz, n, best_turn(pts, sz, n, size), ox, oy);

  // Если фигура та же и почти не повернулась — не перерисовывать
  bool same = this->con_on_ && this->con_idx_ == idx && this->cn_ == n;
  for (int i = 0; same && i < n; i++)
    same = std::abs(ox[i] - this->csx_[i]) <= 1 && std::abs(oy[i] - this->csy_[i]) <= 1;
  if (same)
    return;

  if (this->con_on_) {
    this->invalidate_(this->con_area_);
    if (this->con_idx_ != idx)
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
    // int32_t на ESP32 — long, поэтому тип указан явно
    a.x1 = std::min<int32_t>(a.x1, ox[i] - 6);
    a.y1 = std::min<int32_t>(a.y1, oy[i] - 6);
    a.x2 = std::max<int32_t>(a.x2, ox[i] + 6);
    a.y2 = std::max<int32_t>(a.y2, oy[i] + 6);
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
  this->cap_area_ = {233 - CAP_W / 2, CAP_Y, 233 + CAP_W / 2, CAP_Y + lv_font_get_line_height(this->font_cap_)};
  this->con_on_ = true;
  this->invalidate_(this->con_area_);
  this->invalidate_(this->cap_area_);
}

void WeatherFx::sky_(const Params &p) {
  if (!this->stars_on_)
    return;
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
  const double d = p.utc / 86400.0 + 2440587.5 - 2451545.0;
  double gmst = fmod(280.46061837 + 360.98564736629 * d + p.lon, 360.0);
  if (gmst < 0)
    gmst += 360.0;
  const float lst = (float) (gmst * M_PI / 180.0);
  const float phi = p.lat * (PI_F / 180.0f), sphi = sinf(phi), cphi = cosf(phi);

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

void WeatherFx::meteor_(uint32_t now) {
  if (!this->stars_on_) {
    this->next_met_ = 0;
    return;
  }
  if (!this->met_on_) {
    if (this->next_met_ == 0)
      this->next_met_ = now + rnd_(15000, 45000);
    if ((int32_t) (now - this->next_met_) < 0)
      return;
    // Пролетает по верхней части неба наискосок вниз, влево или вправо
    const bool right = rnd_(0, 2);
    const float a = rnd_(12, 33) * (PI_F / 180.0f);
    const float sp = rnd_(32, 43) / 100.0f;  // px/мс
    this->met_vx_ = (right ? sp : -sp) * cosf(a);
    this->met_vy_ = sp * sinf(a);
    this->met_x0_ = right ? rnd_(90, 250) : rnd_(216, 376);
    this->met_y0_ = rnd_(30, 71);
    // Не ниже 150 px — к цифрам времени не подлетает
    this->met_len_ = std::min((uint32_t) rnd_(450, 651), (uint32_t) ((150.0f - this->met_y0_) / this->met_vy_));
    this->met_t0_ = now;
    this->met_on_ = true;
  }
  const uint32_t t = now - this->met_t0_;
  if (t >= this->met_len_) {
    this->mark_(this->met_spot_, false, 0, 0, 1, 1);
    this->met_on_ = false;
    this->next_met_ = now + rnd_(30000, 90000);
    return;
  }
  const float sp = sqrtf(this->met_vx_ * this->met_vx_ + this->met_vy_ * this->met_vy_);
  const float tail = std::min(60.0f, sp * t);
  this->met_hx_ = this->met_x0_ + this->met_vx_ * t;
  this->met_hy_ = this->met_y0_ + this->met_vy_ * t;
  this->met_tx_ = this->met_hx_ - this->met_vx_ / sp * tail;
  this->met_ty_ = this->met_hy_ - this->met_vy_ / sp * tail;
  // Вспыхивает за 80 мс и гаснет за последние 150 мс
  float o = 1.0f;
  if (t < 80)
    o = t / 80.0f;
  if (t + 150 > this->met_len_)
    o = std::min(o, (this->met_len_ - t) / 150.0f);
  this->met_opa_ = (lv_opa_t) (255 * o);
  const int x = (int) std::min(this->met_hx_, this->met_tx_) - 4, y = (int) std::min(this->met_hy_, this->met_ty_) - 4;
  const int w = (int) std::abs(this->met_hx_ - this->met_tx_) + 9, h = (int) std::abs(this->met_hy_ - this->met_ty_) + 9;
  // Меняется яркость — перерисовать, даже если рамка та же
  if (this->met_spot_.on)
    this->invalidate_(this->met_spot_.a);
  this->mark_(this->met_spot_, true, x, y, w, h);
}

void WeatherFx::sun_(uint32_t now) {
  // Лучи поворачиваются на оборот за две минуты, шаг — 4 раза в секунду
  if (!this->sun_on_ || now - this->sun_ms_ < 250)
    return;
  const uint32_t dt = this->sun_ms_ ? std::min<uint32_t>(now - this->sun_ms_, 1000) : 250;
  this->sun_ms_ = now;
  this->sun_ang_ += dt * (2.0f * PI_F / 120000.0f);
  if (this->sun_ang_ > 2.0f * PI_F)
    this->sun_ang_ -= 2.0f * PI_F;
  if (this->sun_spot_.on)
    this->invalidate_(this->sun_spot_.a);
}

void WeatherFx::garland_(uint32_t now) {
  // Лампочки перемигиваются: каждый шаг гаснет каждая третья, по кругу
  if (!this->gar_on_ || now - this->gar_ms_ < 600)
    return;
  this->gar_ms_ = now;
  this->gar_step_ = (this->gar_step_ + 1) % 3;
  for (auto &g : this->gs_)
    if (g.on)
      this->invalidate_(g.a);
}

void WeatherFx::frame(const Params &p) {
  if (!this->bound_)
    return;

  const int cnt = std::min(std::max(p.count, 0), ND);
  const int sig = p.mode | (cnt << 4) | (std::min(std::max(p.clouds, 0), NC) << 10) | ((p.storm ? 1 : 0) << 12) |
                  ((p.dim ? 1 : 0) << 13) | ((p.stars ? 1 : 0) << 14) | ((p.rain_style ? 1 : 0) << 15) |
                  ((p.sun ? 1 : 0) << 16) | ((p.garland ? 1 : 0) << 17);
  if (sig != this->applied_) {
    // Если поменялись только яркость, солнце или гирлянда — перекрашиваем,
    // но осадки не перемешиваем
    const int keep = ~((1 << 13) | (1 << 16) | (1 << 17));
    const bool relayout = this->applied_ < 0 || (sig & keep) != (this->applied_ & keep);
    this->applied_ = sig;
    this->apply_(p, relayout);
  }

  const bool any =
      this->nd_ || this->nf_ || this->ncl_ || this->storm_ || this->stars_on_ || this->sun_on_ || this->gar_on_;
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
  this->slow_ms_ += dt;
  float ks = 0;
  if (this->slow_ms_ >= 60) {
    ks = this->slow_ms_ / 50.0f;
    this->slow_ms_ = 0;
  }
  const float wind = this->wind_(p, now);
  this->sky_(p);
  this->stars_(now);
  this->meteor_(now);
  this->sun_(now);
  this->garland_(now);
  if (this->glass_on_) {
    if (ks > 0)
      this->glass_(ks, ks * 50.0f);
  } else {
    this->drops_(wind);
    this->splashes_();
  }
  this->flakes_(wind, ks);
  this->clouds_();
  this->lightning_(now);
}

}  // namespace weather_fx
}  // namespace esphome

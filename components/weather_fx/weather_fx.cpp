#include "weather_fx.h"

#include <algorithm>
#include <cstring>

// Список участков к перерисовке (inv_areas) есть только во внутренних
// заголовках LVGL
#include <lvgl_private.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "fx_draw.h"
#include "planets.h"
#include "sky.h"

namespace esphome {
namespace weather_fx {

// Полный снос ветром — при такой скорости и сильнее, м/с
static const float FULL_WIND = 15.0f;
// Осенние цвета листьев (leaves.cpp): оранжевый, тёмно-оранжевый, красный, жёлтый
static const uint32_t LEAF_COLORS[4] = {0xE67E22, 0xD35400, 0xC0392B, 0xF1C40F};
static const uint32_t LEAF_COLORS_DIM[4] = {0xFFA24D, 0xFF7A26, 0xFF5A4A, 0xFFE04D};

// Сердечки 14 февраля (signs.cpp): розовые и красные
static const uint32_t HEART_COLORS[4] = {0xFF4081, 0xE91E63, 0xFF1744, 0xF06292};
static const uint32_t HEART_COLORS_DIM[4] = {0xFF80AB, 0xFF4F8B, 0xFF5C7A, 0xFF9CC0};

// 8 Марта: тюльпаны (розовые, коралловые) и веточки мимозы (жёлтые)
static const uint32_t FLOWER_COLORS[4] = {0xFF4F7B, 0xFFD23F, 0xFF6B4A, 0xFFE45C};
// 1 июня: воздушные шарики
static const uint32_t BALLOON_COLORS[4] = {0xFF5252, 0x40C4FF, 0xFFD740, 0x69F0AE};
// Пасхальные яйца
static const uint32_t EGG_COLORS[4] = {0xE53935, 0x1E88E5, 0xFDD835, 0x43A047};

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
    {"", 0, 0, {1.00000261f, 0.01671123f, -0.00001531f, 100.46457166f, 102.93768193f, 0.0f},
     {0.00000562f, -0.00004392f, -0.01294668f, 35999.37244981f, 0.32327364f, 0.0f}},
    {"Венера", 0xFFF4D6, 10, {0.72333566f, 0.00677672f, 3.39467605f, 181.97909950f, 131.60246718f, 76.67984255f},
     {0.00000390f, -0.00004107f, -0.00078890f, 58517.81538729f, 0.00268329f, -0.27769418f}},
    {"Юпитер", 0xFFE0B0, 8, {5.20288700f, 0.04838624f, 1.30439695f, 34.39644051f, 14.72847983f, 100.47390909f},
     {-0.00011607f, -0.00013253f, -0.00183714f, 3034.74612775f, 0.21252668f, 0.20469106f}},
    {"Марс", 0xFF8A5C, 7, {1.52371034f, 0.09339410f, 1.84969142f, -4.55343205f, -23.94362959f, 49.55953891f},
     {0.00001847f, 0.00007882f, -0.00813131f, 19140.30268499f, 0.44441088f, -0.29257343f}},
    {"Сатурн", 0xE8D08C, 7, {9.53667594f, 0.05386179f, 2.48599187f, 49.95424423f, 92.59887831f, 113.66242448f},
     {-0.00125060f, -0.00050991f, 0.00193609f, 1222.49362201f, -0.41897216f, -0.28867794f}},
};
// Луна — слева от строки погоды, в стороне от созвездия и цифр
static const int MOON_X = 84, MOON_Y = 150, MOON_R = 15;
// Самолёт: скорость, px/мс
static const float AIR_SPEED = 0.022f;
// «Матрица»: шаг столбцов и высота знака
static const int MX_X0 = 45, MX_DX = 47, MX_STEP = 18;
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

static void frame_timer_cb(lv_timer_t *t) {
  auto *self = static_cast<WeatherFx *>(lv_timer_get_user_data(t));
  self->frame(self->params_for_timer());
}

// Порог склейки участков перерисовки, px (см. set_area_join)
static int32_t area_join_px = 2500;

void WeatherFx::set_area_join(int px) { area_join_px = std::max(px, 0); }

// Перед кадром: жадно склеиваем пары участков, пока общий прямоугольник
// выходит не больше чем на area_join_px лишних пикселей и влезает в буфер
// отрисовки за один проход. LVGL сам склеивает только перекрывающиеся
// участки, а 20 капель дождя в разных местах — это 20 проходов и 20 посылок
static void join_areas_cb(lv_event_t *e) {
  auto *d = static_cast<lv_display_t *>(lv_event_get_user_data(e));
  if (area_join_px <= 0 || d == nullptr || d->inv_p < 2 || d->buf_act == nullptr)
    return;
  const int32_t buf_px = (int32_t) (d->buf_act->data_size / lv_color_format_get_size(d->color_format));
  lv_area_t *a = d->inv_areas;
  uint32_t n = d->inv_p;
  for (int pass = 0; pass < 4; pass++) {
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
      for (uint32_t j = i + 1; j < n;) {
        lv_area_t u;
        lv_area_join(&u, &a[i], &a[j]);
        const int32_t su = (int32_t) lv_area_get_size(&u);
        const int32_t waste = su - (int32_t) lv_area_get_size(&a[i]) - (int32_t) lv_area_get_size(&a[j]);
        if (su <= buf_px && waste <= area_join_px) {
          a[i] = u;
          a[j] = a[--n];
          changed = true;
        } else {
          j++;
        }
      }
    }
    if (!changed)
      break;
  }
  d->inv_p = n;
}

static bool hit(const lv_area_t &a, const lv_area_t &clip) {
  return a.x1 <= clip.x2 && a.x2 >= clip.x1 && a.y1 <= clip.y2 && a.y2 >= clip.y1;
}

void WeatherFx::bind(lv_obj_t *root, const lv_font_t *caption) {
  this->root_ = root;
  this->font_cap_ = caption;
  if (!root || !caption)
    return;

  // Всё рисует сам слой погоды: небо, облака и молнию — в своей отрисовке
  // (LV_EVENT_DRAW_MAIN), осадки и гирлянду — после неё (LV_EVENT_DRAW_POST).
  // Отдельные объекты для этого не нужны, а каждый объект LVGL обходит на
  // каждом перерисовываемом участке, перечитывая десятки свойств стиля: два
  // холста во весь экран стоили около четверти цены каждого участка, а
  // облака из объектов-клубов — ещё больше на каждом участке у верха экрана
  lv_obj_add_event_cb(root, paint_back_cb, LV_EVENT_DRAW_MAIN, this);
  lv_obj_add_event_cb(root, paint_front_cb, LV_EVENT_DRAW_POST, this);
  this->paint_ = root;
  this->back_ = root;
  // Слой погоды — непрозрачный чёрный, как и страница под ним: тогда LVGL
  // начинает отрисовку каждого участка прямо с него и не рисует страницу
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  static bool join_hooked = false;
  if (!join_hooked) {
    join_hooked = true;
    lv_display_t *d = lv_obj_get_display(root);
    lv_display_add_event_cb(d, join_areas_cb, LV_EVENT_REFR_START, d);
    // Системный слой LVGL (курсор мыши и отладочные надписи) прошивке не
    // нужен, но LVGL обходил бы его на каждом участке каждого кадра
    lv_obj_add_flag(lv_display_get_layer_sys(d), LV_OBJ_FLAG_HIDDEN);
  }


  // Знак «матрицы» — цифра подписи созвездия: над линией основания
  {
    lv_font_glyph_dsc_t g;
    if (lv_font_get_glyph_dsc(caption, &g, '0', 0) && g.box_h > 0) {
      this->mx_ox_ = g.ofs_x - g.adv_w / 2 - 2;
      this->mx_oy_ = -g.box_h - g.ofs_y - 2;
    } else {
      this->mx_ox_ = -10;
      this->mx_oy_ = -16;
    }
  }
  this->bound_ = true;
  this->applied_ = -1;

  // Кадр анимации — таймер LVGL с периодом перерисовки экрана. Он стоит в
  // списке таймеров раньше таймера перерисовки, поэтому шаг анимации и
  // отрисовка идут в одном проходе: ровно один шаг на кадр. Отдельный таймер
  // ESPHome с тем же периодом «плавал» относительно перерисовки — иногда два
  // шага на кадр (рывок), иногда ни одного (заминка). На паузе LVGL (экран
  // выключен) таймер тоже стоит
  this->timer_ = lv_timer_create(frame_timer_cb, 33, this);
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
  // Часы убраны — осадкам обходить нечего
  if (this->full_)
    return false;
  for (int i = 0; i < this->n_ex_; i++) {
    const int *e = this->ex_[i];
    if (x + w > e[0] && x < e[2] && y + h > e[1] && y < e[3])
      return true;
  }
  return false;
}

void WeatherFx::invalidate_(const lv_area_t &a) {
  // Слой погоды — во весь экран, поэтому участок отмечается прямо в экране,
  // без обхода всех родителей, как это делает lv_obj_invalidate_area: на
  // десятках частиц за кадр этот обход заметен. Только если слой виден и
  // его страница сейчас на экране — иначе рисовать нечего
  lv_obj_t *root = this->paint_;
  if (lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_screen(root) != lv_screen_active())
    return;
  // Координаты частиц — относительно слоя; LVGL ждёт экранные
  lv_area_t oc;
  lv_obj_get_coords(root, &oc);
  lv_area_t abs = {(int32_t) (a.x1 + oc.x1), (int32_t) (a.y1 + oc.y1), (int32_t) (a.x2 + oc.x1),
                   (int32_t) (a.y2 + oc.y1)};
  if (lv_area_intersect(&abs, &abs, &oc))
    lv_inv_area(lv_obj_get_display(root), &abs);
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
  this->rain_hide_();
  for (auto &s : this->fs_)
    this->mark_(s, false, 0, 0, 1, 1);
  for (auto &s : this->sts_)
    this->mark_(s, false, 0, 0, 1, 1);
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
  // Круг с центром (x, y) в экранных координатах
  auto disc_at = [&](int x, int y, int r, uint32_t c, lv_opa_t o) {
    lv_area_t a = {x - r, y - r, x + r, y + r};
    fill.radius = LV_RADIUS_CIRCLE;
    fill.color = lv_color_hex(c);
    fill.opa = o;
    lv_draw_fill(layer, &fill, &a);
  };

  // Вспышка молнии на миг освещает всё небо (clouds.cpp)
  this->sky_flash_paint_(layer, clip);

  // Зарево у горизонта на восходе и закате: тёплый градиент снизу вверх
  if (this->horizon_ > 0.01f) {
    lv_area_t ga = {oc.x1, oc.y1 + 250, oc.x1 + 465, oc.y1 + 465};
    if (hit(ga, clip)) {
      lv_draw_fill_dsc_t g;
      lv_draw_fill_dsc_init(&g);
      g.opa = (lv_opa_t) (this->horizon_ * (dim ? 150.0f : 110.0f));
      g.grad.dir = LV_GRAD_DIR_VER;
      g.grad.stops_count = 2;
      const lv_color_t c = lv_color_hex(this->dawn_ ? 0xFF8FA3 : 0xFF5E3A);
      g.grad.stops[0].color = c;
      g.grad.stops[0].opa = LV_OPA_TRANSP;
      g.grad.stops[0].frac = 0;
      g.grad.stops[1].color = c;
      g.grad.stops[1].opa = LV_OPA_COVER;
      g.grad.stops[1].frac = 255;
      lv_draw_fill(layer, &g, &ga);
    }
  }

  // Солнце (sun.cpp)
  this->sun_paint_(layer, oc, clip);

  // Вифлеемская звезда: восемь лучей — длинный вниз — чуть «дышат»
  // (extras.cpp)
  this->bstar_paint_(layer, oc, clip);

  // Радуга (rainbow.cpp)
  this->rainbow_paint_(layer, oc, clip);

  // Луна в текущей фазе (moon.cpp). В полнолуние крупнее, в суперлуние —
  // ещё крупнее, в затмение наливается красным
  if (place(this->moon_spot_) && this->moon_phase_ >= 0)
    this->moon_paint_(layer, MOON_X + oc.x1, MOON_Y + oc.y1);

  // Мерцающие звёзды (stars.cpp)
  this->stars_paint_(layer, oc, clip);
  fill.opa = LV_OPA_COVER;

  // Хэллоуин: летучие мыши вместо звёзд (signs.cpp)
  if (this->bats_on_)
    this->bats_paint_(layer, oc, clip);

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
      // Звёзды созвездия — светящиеся, крупнее по звёздной величине
      for (int i = 0; i < this->cn_; i++) {
        const int x = this->csx_[i] + oc.x1, y = this->csy_[i] + oc.y1;
        lv_area_t a = {x - 10, y - 10, x + 10, y + 10};
        if (hit(a, clip))
          this->con_star_paint_(layer, x, y, this->csz_[i]);
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

  // Без часов — планеты картинками по дуге слева внизу, название справа
  if (this->npl_ && this->full_) {
    lv_area_t pa = {PL_AREA_FULL.x1 + oc.x1, PL_AREA_FULL.y1 + oc.y1, PL_AREA_FULL.x2 + oc.x1,
                    PL_AREA_FULL.y2 + oc.y1};
    if (hit(pa, clip)) {
      lv_draw_image_dsc_t im;
      lv_draw_image_dsc_init(&im);
      lv_draw_label_dsc_t lb;
      lv_draw_label_dsc_init(&lb);
      lb.font = this->font_cap_;
      lb.color = lv_color_hex(dim ? 0x9AA8C4 : 0x5C6A84);
      lb.opa = LV_OPA_COVER;
      lb.flag = LV_TEXT_FLAG_EXPAND;
      for (int i = 0; i < std::min(this->npl_, PL_FULL_N); i++) {
        const lv_image_dsc_t *img = PL_IMG[this->pl_idx_[i]];
        const int w = img->header.w, h = img->header.h;
        const int cx = PL_SLOT[i][0] + oc.x1, cy = PL_SLOT[i][1] + oc.y1;
        im.src = img;
        lv_area_t ia = {cx - w / 2, cy - h / 2, cx - w / 2 + w - 1, cy - h / 2 + h - 1};
        if (hit(ia, clip))
          lv_draw_image(layer, &im, &ia);
        lv_area_t ta = {cx + w / 2 + 6, cy - 9, cx + w / 2 + 96, cy + 11};
        lb.text = PLANETS[this->pl_idx_[i]].name;
        if (hit(ta, clip))
          lv_draw_label(layer, &lb, &ta);
      }
    }
  }

  // Планеты: цветная точка и название
  if (this->npl_ && !this->full_) {
    lv_area_t pa = {PL_AREA.x1 + oc.x1, PL_AREA.y1 + oc.y1, PL_AREA.x2 + oc.x1, PL_AREA.y2 + oc.y1};
    if (hit(pa, clip)) {
      lv_draw_label_dsc_t lb;
      lv_draw_label_dsc_init(&lb);
      lb.font = this->font_cap_;
      lb.color = lv_color_hex(dim ? 0x8898B8 : 0x4C5A74);
      lb.opa = LV_OPA_COVER;
      lb.flag = LV_TEXT_FLAG_EXPAND;  // одной строкой, без переноса
      fill.radius = LV_RADIUS_CIRCLE;
      fill.opa = LV_OPA_COVER;
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

  // Метеор: светящийся след и яркая голова (extras.cpp)
  this->meteor_paint_(layer, oc, clip);

  // Самолёт: красный и зелёный огни на крыльях, между ними — белая
  // проблесковая вспышка (двойная, раз в 1,2 с)
  if (place(this->air_spot_)) {
    const int x = (int) this->air_x_ + oc.x1, y = (int) this->air_y_ + oc.y1;
    const uint32_t t = (millis() - this->air_t0_) % 1200;
    if (t < 70 || (t > 160 && t < 230)) {
      disc_at(x, y, 5, 0xFFFFFF, dim ? 90 : 60);
      disc_at(x, y, 2, 0xFFFFFF, LV_OPA_COVER);
    }
    disc_at(x - 7, y + 1, 1, 0xFF3B30, LV_OPA_COVER);
    disc_at(x + 7, y + 1, 1, 0x30D158, LV_OPA_COVER);
  }

  // Молния и облака — поверх неба (clouds.cpp)
  this->clouds_paint_(layer, oc, clip);
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

  // Струи дождя, градины и брызги (rain.cpp), снег (snow.cpp), листья
  // (leaves.cpp)
  this->rain_paint_(layer, oc, clip);
  this->snow_paint_(layer, oc, clip);
  this->leaves_paint_(layer, oc, clip);

  // Сердечки, цветы или шарики (signs.cpp)
  this->signs_paint_(layer, oc, clip);

  // Сугроб по нижнему краю (snow.cpp) — поверх падающего снега
  this->cover_paint_(layer, oc, clip);

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
    // Искорки на кончиках кристаллов
    fill.radius = LV_RADIUS_CIRCLE;
    for (int i = 0; i < this->nfsp_; i++) {
      if (!place(this->fsp_spot_[i]))
        continue;
      const int x = this->fsp_x_[i] + oc.x1, y = this->fsp_y_[i] + oc.y1;
      lv_area_t halo = {x - 3, y - 3, x + 3, y + 3};
      fill.color = lv_color_hex(0xDDEEFF);
      fill.opa = (lv_opa_t) (this->fsp_opa_[i] / 3);
      lv_draw_fill(layer, &fill, &halo);
      lv_area_t core = {x - 1, y - 1, x + 1, y + 1};
      fill.color = lv_color_white();
      fill.opa = this->fsp_opa_[i];
      lv_draw_fill(layer, &fill, &core);
    }
    fill.opa = LV_OPA_COVER;
  }
  // Сосульки под цифрами (extras.cpp)
  this->icicles_paint_(layer, oc, clip);

  // Свечи (candles.cpp)
  this->candles_paint_(layer, oc, clip);

  // Марево в жару: волнистые струйки тёплого воздуха поднимаются и тают
  // (extras.cpp)
  this->heat_paint_(layer, oc, clip);

  // Салют: ракеты, вспышки, искры и дым (fireworks.cpp)
  if (this->fw_on_)
    this->fireworks_paint_(layer, oc, clip);

  // Воздушный змей и ракета с дымом (extras.cpp)
  this->kite_paint_(layer, oc, clip);
  this->rocket_paint_(layer, oc, clip);

  // Светлячки: мягкий жёлто-зелёный ореол и яркая точка, мерцают
  if (this->ff_on_) {
    fill.radius = LV_RADIUS_CIRCLE;
    for (int i = 0; i < NFF; i++) {
      if (!place(this->ff_spot_[i]))
        continue;
      const float k = this->ffk_[i];
      const int x = this->ffpx_[i] + oc.x1, y = this->ffpy_[i] + oc.y1;
      lv_area_t halo = {x - 6, y - 6, x + 6, y + 6};
      fill.color = lv_color_hex(0xB8FF3C);
      fill.opa = (lv_opa_t) (80.0f * k);
      lv_draw_fill(layer, &fill, &halo);
      lv_area_t core = {x - 2, y - 2, x + 2, y + 2};
      fill.color = lv_color_hex(0xF4FF8A);
      fill.opa = (lv_opa_t) (255.0f * k);
      lv_draw_fill(layer, &fill, &core);
    }
    fill.opa = LV_OPA_COVER;
  }

  // Пасхальные яйца катятся по низу: цветное яйцо и светлая полоска,
  // которая бежит по нему — будто оно вертится
  for (int i = 0; i < NEGG; i++) {
    if (!place(this->egg_spot_[i]))
      continue;
    const int cx = (abs.x1 + abs.x2) / 2, cy = (abs.y1 + abs.y2) / 2;
    lv_area_t egg = {cx - 13, cy - 9, cx + 13, cy + 9};
    fill.radius = 9;
    fill.opa = LV_OPA_COVER;
    fill.color = lv_color_hex(EGG_COLORS[i % 4]);
    lv_draw_fill(layer, &fill, &egg);
    const int off = (int) fmodf(this->egg_x_[i] * 0.8f, 30.0f) - 15;
    if (std::abs(off) <= 9) {
      lv_area_t band = {cx + off - 2, cy - 7, cx + off + 2, cy + 7};
      fill.radius = 1;
      fill.color = lv_color_hex(0xFFFFFF);
      lv_draw_fill(layer, &fill, &band);
    }
    lv_area_t dot = {cx - off / 2 - 1, cy - 1, cx - off / 2 + 1, cy + 1};
    fill.radius = 1;
    fill.color = lv_color_hex(0xFFE082);
    lv_draw_fill(layer, &fill, &dot);
  }

  // «Матрица»: столбцы нулей и единиц, яркая голова и гаснущий хвост
  if (this->mx_on_) {
    lv_draw_letter_dsc_t mx;
    lv_draw_letter_dsc_init(&mx);
    mx.font = this->font_cap_;
    for (int c = 0; c < NMX; c++) {
      if (!place(this->mx_spot_[c]))
        continue;
      const int x = MX_X0 + c * MX_DX + oc.x1;
      for (int j = 0; j < MX_LEN; j++) {
        const int y = (int) this->mx_y_[c] - j * MX_STEP + oc.y1;
        mx.color = lv_color_hex(j == 0 ? 0xD8FFD8 : 0x22DD55);
        mx.opa = (lv_opa_t) (j == 0 ? 255 : 230 - j * 26);
        mx.unicode = '0' + this->mx_ch_[c][j];
        lv_point_t pt = {x, y};
        lv_draw_letter(layer, &mx, &pt);
      }
    }
  }

  // Гирлянда: провод по краю и перемигивающиеся лампочки (garland.cpp)
  this->garland_paint_(layer, oc, clip);
}

void WeatherFx::apply_(const Params &p, bool relayout) {
  const bool dim = p.dim;
  const int cnt = std::min(std::max(p.count, 0), ND);
  // Сменилась палитра — перерисовать всё: луна, солнце и созвездие
  // неподвижны и сами не обновятся
  if (dim != this->dim_ && this->back_) {
    lv_obj_invalidate(this->back_);
    lv_obj_invalidate(this->paint_);
  }
  this->dim_ = dim;
  const bool signs = p.mode == 2 || (p.mode >= 5 && p.mode <= 9);
  static const int SET_OF_MODE[10] = {0, 0, 0, 0, 0, 1, 2, 3, 4, 5};
  this->fset_ = p.mode >= 0 && p.mode <= 9 ? SET_OF_MODE[p.mode] : 0;
  // Снег — в snow.cpp, листья — в leaves.cpp, знаками шрифта всплывает и
  // падает остальное: сердечки, цветы, шарики
  this->nf_ = signs && this->fset_ >= 2 && this->fset_ <= 4 ? std::min(cnt, NF) : 0;

  // Праздник: что рисовать сверх погоды
  const int hol = p.holiday;
  if (hol != this->hol_)
    this->con_force_ = true;  // созвездие могло быть спрятано (Рождество, Хэллоуин)
  this->hol_ = hol;
  this->night_ = p.night;
  this->bats_on_ = hol == HOL_HALLOWEEN && p.night;
  this->bstar_apply_(hol == HOL_CHRISTMAS);
  this->rocket_apply_(hol == HOL_COSMOS);
  if (hol != HOL_EASTER) {
    for (auto &e : this->egg_spot_)
      this->mark_(e, false, 0, 0, 1, 1);
    this->egg_on_ = false;
    this->next_egg_ = 0;
  }
  if (!this->bats_on_)
    for (auto &b : this->bat_spot_)
      this->mark_(b, false, 0, 0, 1, 1);
  if (p.fireflies && !this->ff_on_) {
    // Светлячки — над травой внизу круга, у каждого своё место и ритм
    for (int i = 0; i < NFF; i++) {
      int x, y;
      do {
        x = rnd_(60, 406);
        y = rnd_(290, 420);
      } while ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 205 * 205);
      this->ffx_[i] = x;
      this->ffy_[i] = y;
      for (auto &ph : this->ffph_[i])
        ph = rnd_(0, 628) / 100.0f;
    }
  }
  if (!p.fireflies)
    for (auto &f : this->ff_spot_)
      this->mark_(f, false, 0, 0, 1, 1);
  this->ff_on_ = p.fireflies;
  if (p.matrix && !this->mx_on_) {
    for (int c = 0; c < NMX; c++) {
      this->mx_y_[c] = -rnd_(0, 300);
      this->mx_v_[c] = rnd_(40, 80) / 10.0f;
      for (auto &ch : this->mx_ch_[c])
        ch = rnd_(0, 2);
    }
  }
  if (!p.matrix)
    for (auto &m : this->mx_spot_)
      this->mark_(m, false, 0, 0, 1, 1);
  this->mx_on_ = p.matrix;
  this->fw_on_ = p.fireworks;
  // Радуга проявляется и тает сама (rainbow_)
  this->rainbow_on_ = p.rainbow;
  this->kite_mode_ = p.kite;
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
  this->frost_on_ = p.frost;
  this->update_root_();
  // Луна — вместе со звёздами; фазу задаёт moon_()
  if (!this->stars_on_) {
    this->mark_(this->moon_spot_, false, 0, 0, 1, 1);
    this->moon_phase_ = -1;
    fx_sprite_free(this->moon_spr_);
  }
  // Салют кончился или змей больше не летает — убрать с экрана
  if (!this->fw_on_)
    this->fireworks_stop_();
  if (!this->kite_mode_ && this->kite_on_)
    this->kite_stop_();

  // В приглушённом режиме палитра ярче: при низкой яркости панели тёмные
  // частицы сливаются с чёрным. Цвета дождя — в rain_apply_()
  // Листья: ближние — яркие, дальние — притушенные
  for (int k = 0; k < 4; k++) {
    this->c_leaf_[k] = lv_color_hex(dim ? LEAF_COLORS_DIM[k] : LEAF_COLORS[k]);
    this->c_leaf_s_[k] = lv_color_mix(this->c_leaf_[k], lv_color_black(), dim ? 190 : 140);
    this->c_heart_[k] = lv_color_hex(dim ? HEART_COLORS_DIM[k] : HEART_COLORS[k]);
    this->c_heart_s_[k] = lv_color_mix(this->c_heart_[k], lv_color_black(), dim ? 190 : 140);
    // Цветы, шарики и клёны: в приглушённом режиме светлее
    const uint32_t base[3] = {FLOWER_COLORS[k], BALLOON_COLORS[k], LEAF_COLORS[k]};
    for (int s = 3; s < NSET; s++) {
      const lv_color_t c = lv_color_hex(base[s - 3]);
      this->c_set_[s][k] = dim ? lv_color_mix(lv_color_white(), c, 60) : c;
      this->c_set_s_[s][k] = lv_color_mix(this->c_set_[s][k], lv_color_black(), dim ? 190 : 140);
    }
  }
  // Картинки сердечек, цветов, шариков и летучих мышей (signs.cpp)
  this->signs_apply_();

  if (relayout) {
    // Новая погода — всё с чистого листа
    this->hide_all_();
    for (int i = 0; i < NF; i++) {
      this->fx_[i] = rnd_(30, 430);
      this->fy_[i] = rnd_(0, 440);
      this->fph_[i] = rnd_(0, 628) / 100.0f;
    }
    // Звёзды — в верхней половине круга, вокруг и выше времени, но не на
    // месте созвездия. Без часов — по всему кругу, кроме погоды слева,
    // созвездия справа с подписью и строки планет
    this->nst_ = this->full_ ? NST : 12;
    for (int i = 0; i < this->nst_; i++) {
      int x, y;
      bool bad;
      do {
        x = rnd_(30, 436);
        y = this->full_ ? rnd_(30, 440) : rnd_(30, 220);
        bad = (x - 233) * (x - 233) + (y - 233) * (y - 233) > 215 * 215 ||
              (std::abs(x - MOON_X) < MOON_R + 12 && std::abs(y - MOON_Y) < MOON_R + 12);
        if (this->full_)
          bad = bad || (x < 182 && y > 192 && y < 274) ||                // погода на улице
                (x > 182 && x < 424 && y > 70 && y < 370) ||            // созвездие и подпись
                (x < 330 && y > 282);                                   // планеты
        else
          bad = bad || x < 50 || x > 416 || (x > 82 && x < 372 && y > 22 && y < 122);
      } while (bad);
      this->stx_[i] = x;
      this->sty_[i] = y;
      this->stph_[i] = rnd_(0, 628) / 100.0f;
    }
    // Оттенки звёзд и яркие с лучиками (stars.cpp)
    this->stars_layout_();
  } else {
    // Поменялись яркость, солнце или гирлянда — перерисовать слой целиком,
    // это разовая перерисовка
    lv_obj_invalidate(this->back_);
    lv_obj_invalidate(this->paint_);
  }
  if (this->stars_on_ && !this->bats_on_) {
    this->star_sprites_();
    for (int i = 0; i < NST; i++)
      this->star_mark_(i, i < this->nst_);
  } else {
    for (auto &s : this->sts_)
      this->mark_(s, false, 0, 0, 1, 1);
  }
  // Дождь и град: струи, брызги, их картинки и цвета
  this->rain_apply_(p, relayout);
  // Снег и сугроб, листопад
  this->snow_apply_(p, relayout);
  this->leaves_apply_(p, relayout);
  // Лишние падающие знаки — убрать
  for (int i = this->nf_; i < NF; i++)
    this->mark_(this->fs_[i], false, 0, 0, 1, 1);

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
  // Иней: узор собирается при включении и освобождает память при выключении
  if (this->frost_on_ && !this->frost_buf_) {
    this->build_frost_();
    lv_obj_invalidate(this->paint_);
  } else if (!this->frost_on_ && this->frost_buf_) {
    this->free_frost_();
    lv_obj_invalidate(this->paint_);
  }
  this->icicles_apply_();
  if (!this->stars_on_ && this->met_on_) {
    this->mark_(this->met_spot_, false, 0, 0, 1, 1);
    this->met_on_ = false;
  }
  if (!this->stars_on_)
    fx_sprite_free(this->met_spr_);
  this->sun_apply_();
  this->garland_apply_();

  // Облака: цвет по погоде и яркости, места при новой раскладке (clouds.cpp)
  this->clouds_apply_(relayout);
  // Слой виден, только если на нём что-то есть — теперь, когда всё настроено
  this->update_root_();
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

// Нижний край круглого экрана (чуть выше самого края): по нему катятся
// пасхальные яйца
static float ground(float x) {
  float d = x - 233.0f;
  return 233.0f + sqrtf(std::max(0.0f, 233.0f * 233.0f - d * d)) - 8.0f;
}

void WeatherFx::flakes_(float wind, float ks) {
  // Крупные знаки летят быстрее и качаются сильнее — так получается
  // глубина. У каждого свой ритм покачивания. Мелкие медленные, их двигаем
  // только на редких тиках (ks > 0)
  for (int i = 0; i < this->nf_; i++) {
    const bool big = i % 2;
    const float k = big ? this->k_ : ks;
    if (k <= 0)
      continue;
    if (this->fset_ == 2 || this->fset_ == 4) {
      // Сердечки и воздушные шарики медленно всплывают снизу вверх, покачиваясь
      this->fy_[i] -= k * (big ? 0.9f + (i % 3) * 0.15f : 0.55f + (i % 3) * 0.1f);
      this->fph_[i] += k * ((big ? 0.06f : 0.05f) + i * 0.003f);
      this->fx_[i] = wrap_x(this->fx_[i] + k * (cosf(this->fph_[i]) * (big ? 0.9f : 0.6f) + wind * 0.5f));
      if (this->fy_[i] < -30) {
        this->fy_[i] = 490 + rnd_(0, 40);
        this->fx_[i] = rnd_(30, 430);
      }
    } else {
      // Цветы падают медленно, раскачиваются широко и летят по ветру
      this->fy_[i] += k * (big ? 1.1f + (i % 3) * 0.2f : 0.6f + (i % 3) * 0.15f);
      this->fph_[i] += k * ((big ? 0.10f : 0.08f) + i * 0.004f);
      this->fx_[i] =
          wrap_x(this->fx_[i] + k * (cosf(this->fph_[i]) * (big ? 1.5f : 0.9f) + wind * (big ? 2.6f : 1.8f)));
      // Упал за нижний край — снова сверху. Только для падающих: всплывшее
      // сердечко начинает заново как раз ниже края, и прежде эта проверка
      // тут же отбрасывала его наверх — после первого раза сердечки и
      // шарики больше не появлялись
      if (this->fy_[i] > 470) {
        this->fy_[i] = -30 - rnd_(0, 40);
        this->fx_[i] = rnd_(30, 430);
      }
    }
    this->sign_mark_(i);
  }
}

void WeatherFx::stars_(uint32_t now) {
  // Звёзды мерцают: яркость меняется по синусу, у каждой свой ритм. Каждая
  // звезда меняется раз в 200 мс — чаще глаз не заметит. Звёзды поделены на
  // три группы, которые меняются по очереди раз в ~67 мс: если менять все
  // 12 разом, этот кадр выходит втрое тяжелее и на нём спотыкается метеор
  if (!this->stars_on_ || this->bats_on_ || now - this->star_ms_ < 60)
    return;
  this->star_ms_ = now;
  this->star_grp_ = (this->star_grp_ + 1) % 3;
  for (int i = this->star_grp_; i < NST; i += 3) {
    this->stph_[i] += 0.25f + (i % 4) * 0.07f;
    const float k = 0.35f + 0.65f * (0.5f + 0.5f * sinf(this->stph_[i]));
    this->stopa_[i] = (lv_opa_t) (255 * k);
    if (this->sts_[i].on)
      this->invalidate_(this->sts_[i].a);
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
  this->cap_area_ = {g_box.cap_x - CAP_W / 2, g_box.cap_y, g_box.cap_x + CAP_W / 2,
                     g_box.cap_y + lv_font_get_line_height(this->font_cap_)};
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
    this->mark_(this->fsp_spot_[i], o > 8, this->fsp_x_[i] - 4, this->fsp_y_[i] - 4, 9, 9);
  }
}

void WeatherFx::plane_(uint32_t now) {
  if (!this->plane_mode_)
    return;
  if (!this->air_on_) {
    if (this->next_air_ == 0)
      this->next_air_ = now + (this->plane_mode_ == 2 ? 1500 : rnd_(120000, 480000));
    if ((int32_t) (now - this->next_air_) < 0)
      return;
    this->air_on_ = true;
    this->air_t0_ = now;
    this->air_v_ = rnd_(0, 2) ? AIR_SPEED : -AIR_SPEED;
    this->air_y_ = rnd_(45, 140);
  }
  const float t = now - this->air_t0_;
  this->air_x_ = (this->air_v_ > 0 ? -20.0f : 486.0f) + this->air_v_ * t;
  const float y = this->air_y_;
  if (this->air_x_ < -30.0f || this->air_x_ > 496.0f) {
    this->mark_(this->air_spot_, false, 0, 0, 1, 1);
    this->air_on_ = false;
    this->next_air_ = now + (this->plane_mode_ == 2 ? rnd_(3000, 6000) : rnd_(240000, 600000));
    return;
  }
  const int x = (int) this->air_x_, yy = (int) y;
  if (this->air_spot_.on)
    this->invalidate_(this->air_spot_.a);  // мигает — перерисовать
  this->mark_(this->air_spot_, true, x - 10, yy - 6, 21, 13);
}

void WeatherFx::update_root_() {
  show_(this->root_, this->nd_ || this->nf_ || this->nsn_ || this->nlf_ || this->cover_spot_.on || this->ncl_ ||
                         this->storm_ || this->stars_on_ || this->sun_on_ || this->gar_on_ || this->fw_on_ ||
                         this->kite_mode_ || this->rainbow_on_ || this->rb_level_ > 0 || this->frost_on_ ||
                         this->extras_drawn_());
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

void WeatherFx::moon_(uint32_t utc, int force) {
  if (!this->stars_on_ || (utc < 1600000000u && !force))
    return;
  // Полнолуние, суперлуние, затмение — по небу раз в минуту или по выбору
  // «Показать праздник»
  int ev = this->moon_ev_;
  float red = this->moon_red_;
  const int64_t minute = utc / 60;
  if (force) {
    ev = force;
    red = force == HOL_ECLIPSE ? 1.0f : 0.0f;
  } else if (minute != this->moon_ev_min_) {
    this->moon_ev_min_ = minute;
    ev = moon_event(utc, &red);
  }
  const int r_moon = ev == HOL_SUPERMOON ? 24 : (ev == HOL_FULL_MOON || ev == HOL_ECLIPSE ? 20 : MOON_R);
  const bool changed = ev != this->moon_ev_ || r_moon != this->moon_r_ || std::fabs(red - this->moon_red_) > 0.02f;
  // Фаза по среднему синодическому месяцу от новолуния 6 января 2000 года.
  // Точность — несколько часов, на рисунке 30 px это не видно. В полнолуние
  // по выбору — ровно полная
  double ph = 0.5;
  if (!force) {
    const double days = (utc - 947182440.0) / 86400.0;
    ph = fmod(days / 29.530588853, 1.0);
    if (ph < 0)
      ph += 1.0;
    if (ev)
      ph = 0.5;
  }
  // Перерисовывать, только когда фаза заметно сменилась (раз в пару часов)
  // или сменилась яркость экрана
  if (this->moon_spot_.on && !changed && std::fabs(ph - this->moon_phase_) < 0.003 && this->dim_ == this->moon_dim_)
    return;
  if (this->moon_spot_.on)
    this->invalidate_(this->moon_spot_.a);
  this->moon_ev_ = ev;
  this->moon_red_ = red;
  this->moon_r_ = r_moon;
  this->moon_phase_ = (float) ph;
  this->moon_sprite_();
  const int r = r_moon + 8;
  this->mark_(this->moon_spot_, true, MOON_X - r, MOON_Y - r, 2 * r + 1, 2 * r + 1);
}

void WeatherFx::fireflies_(uint32_t now) {
  // Светлячки кружат у своего места и мерцают — раз в ~66 мс
  if (!this->ff_on_ || now - this->ff_ms_ < 60)
    return;
  this->ff_ms_ = now;
  const float t = now / 1000.0f;
  for (int i = 0; i < NFF; i++) {
    const float *ph = this->ffph_[i];
    const int x = (int) (this->ffx_[i] + 18.0f * sinf(t * (0.35f + i * 0.03f) + ph[0]));
    const int y = (int) (this->ffy_[i] + 10.0f * sinf(t * (0.5f + i * 0.02f) + ph[1]));
    // Вспыхивает на пару секунд и надолго гаснет
    const float b = sinf(t * (0.9f + i * 0.07f) + ph[2]);
    const float k = b > 0.3f ? (b - 0.3f) / 0.7f : 0.0f;
    const bool was = this->ff_spot_[i].on;
    if (was && k != this->ffk_[i])
      this->invalidate_(this->ff_spot_[i].a);
    this->ffk_[i] = k;
    this->ffpx_[i] = x;
    this->ffpy_[i] = y;
    this->mark_(this->ff_spot_[i], k > 0.02f, x - 6, y - 6, 13, 13);
  }
}

void WeatherFx::bats_() {
  if (!this->bats_on_)
    return;
  const float k = this->k_;
  for (int i = 0; i < NBAT; i++) {
    if (this->bat_v_[i] == 0) {
      // Первый взлёт: где и как быстро летит, в какую сторону
      this->bat_x_[i] = rnd_(40, 420);
      this->bat_y_[i] = 50 + i * 35 + rnd_(0, 20);
      this->bat_v_[i] = (rnd_(0, 2) ? 1.0f : -1.0f) * rnd_(14, 26) / 10.0f;
      this->bat_ph_[i] = rnd_(0, 628) / 100.0f;
    }
    this->bat_x_[i] = wrap_x(this->bat_x_[i] + k * this->bat_v_[i]);
    this->bat_ph_[i] += k * 0.09f;
    // Летит волной и дёргается от взмахов
    const float y = this->bat_y_[i] + 14.0f * sinf(this->bat_ph_[i]) + 3.0f * sinf(this->bat_ph_[i] * 7.0f);
    this->bat_mark_(i, (int) this->bat_x_[i], (int) y);
  }
}

void WeatherFx::eggs_(uint32_t now) {
  if (this->hol_ != HOL_EASTER)
    return;
  if (!this->egg_on_) {
    if (this->next_egg_ == 0)
      this->next_egg_ = now + 1500;
    if ((int32_t) (now - this->next_egg_) < 0)
      return;
    this->egg_on_ = true;
    this->egg_t0_ = now;
  }
  // Три яйца одно за другим катятся слева направо по краю круга
  const float t = now - this->egg_t0_;
  bool any = false;
  for (int i = 0; i < NEGG; i++) {
    const float x = -30.0f - i * 46.0f + 0.09f * t;
    this->egg_x_[i] = x;
    const bool on = x > -20.0f && x < 486.0f;
    any |= x < 486.0f;
    const int cx = (int) x, cy = (int) ground(x) - 6;
    if (on && this->egg_spot_[i].on)
      this->invalidate_(this->egg_spot_[i].a);  // полоска бежит — перерисовать
    this->mark_(this->egg_spot_[i], on, cx - 14, cy - 10, 29, 21);
  }
  if (!any) {
    this->egg_on_ = false;
    this->next_egg_ = now + rnd_(15000, 30000);
  }
}

void WeatherFx::matrix_() {
  if (!this->mx_on_)
    return;
  const float k = this->k_;
  for (int c = 0; c < NMX; c++) {
    this->mx_y_[c] += k * this->mx_v_[c];
    const float top = this->mx_y_[c] - (MX_LEN - 1) * MX_STEP;
    if (top > 480) {
      this->mx_y_[c] = -rnd_(0, 200);
      this->mx_v_[c] = rnd_(40, 80) / 10.0f;
    }
    // Знаки в столбце то и дело меняются
    if (rnd_(0, 3) == 0)
      this->mx_ch_[c][rnd_(0, MX_LEN)] ^= 1;
    const int x = MX_X0 + c * MX_DX;
    const int y0 = (int) top + this->mx_oy_, y1 = (int) this->mx_y_[c] + 6;
    if (this->mx_spot_[c].on)
      this->invalidate_(this->mx_spot_[c].a);
    this->mark_(this->mx_spot_[c], y1 > 0 && y0 < 466, x - 10, y0, 21, y1 - y0);
  }
}

void WeatherFx::frame(const Params &p) {
  if (!this->bound_)
    return;

  const int cnt = std::min(std::max(p.count, 0), ND);
  const int sig = p.mode | (cnt << 4) | (std::min(std::max(p.clouds, 0), NC) << 10) | ((p.storm ? 1 : 0) << 12) |
                  ((p.dim ? 1 : 0) << 13) | ((p.stars ? 1 : 0) << 14) |
                  ((p.sun ? 1 : 0) << 16) | ((p.garland ? 1 : 0) << 17) | ((p.fireworks ? 1 : 0) << 18) |
                  ((std::min(std::max(p.kite, 0), 3)) << 19) | ((p.rainbow ? 1 : 0) << 21) |
                  ((p.frost ? 1 : 0) << 22) | ((std::min(std::max(p.holiday, 0), 31)) << 23) |
                  ((p.night ? 1 : 0) << 28) | ((p.fireflies ? 1 : 0) << 29) | ((p.matrix ? 1 : 0) << 30);
  this->fw_pal_ = p.fw_palette;
  if (p.full != this->full_) {
    // Часы ушли или вернулись: небо — заново под весь круг или под «шапку»
    this->full_ = p.full;
    g_box = p.full ? BOX_FULL : BOX_TOP;
    this->invalidate_(PL_AREA);
    this->invalidate_(PL_AREA_FULL);
    if (this->con_on_) {
      this->invalidate_(this->con_area_);
      this->invalidate_(this->cap_area_);
    }
    this->con_force_ = true;
    this->applied_ = -1;
  }
  if (sig != this->applied_) {
    // Если поменялись только яркость, солнце, гирлянда или праздник —
    // перекрашиваем, но осадки не перемешиваем
    const int keep = ~((1 << 13) | (1 << 16) | (1 << 17) | (1 << 18) | (3 << 19) | (1 << 21) | (1 << 22) |
                       (31 << 23) | (1 << 28) | (1 << 29) | (1 << 30));
    const bool relayout = this->applied_ < 0 || (sig & keep) != (this->applied_ & keep);
    this->applied_ = sig;
    this->apply_(p, relayout);
  }

  // Зарево, марево и самолёт меняются без перестройки погоды
  const float glow = std::min(std::max(p.glow, 0.0f), 1.0f);
  if (std::fabs(glow - this->horizon_) > 0.02f || (glow > 0.01f && p.dawn != this->dawn_)) {
    const bool vis = (glow > 0.01f) != (this->horizon_ > 0.01f);
    this->horizon_ = glow;
    this->dawn_ = p.dawn;
    lv_area_t ga = {0, 250, 465, 465};
    this->invalidate_(ga);
    if (vis)
      this->update_root_();
  }
  if (p.heat != this->heat_on_) {
    this->heat_apply_(p.heat);
    this->update_root_();
  }
  if (p.candles != this->cnd_on_) {
    this->candles_apply_(p.candles);
    this->update_root_();
  }
  if (p.plane != this->plane_mode_) {
    this->plane_mode_ = p.plane;
    this->next_air_ = 0;
    if (!p.plane && this->air_on_) {
      this->mark_(this->air_spot_, false, 0, 0, 1, 1);
      this->air_on_ = false;
    }
    this->update_root_();
  }

  const bool any = this->nd_ || this->nf_ || this->nsn_ || this->nlf_ || this->cover_ > 0 || this->ncl_ ||
                   this->storm_ || this->stars_on_ || this->sun_on_ || this->gar_on_ || this->fw_on_ ||
                   this->kite_mode_ || this->rainbow_on_ || this->rb_level_ > 0 || this->frost_on_ ||
                   this->extras_drawn_();
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
  this->render_budget_ = 2;
  this->sky_(p);
  this->moon_(p.utc, p.moon_force);
  this->stars_(now);
  this->bstar_(now);
  this->bats_();
  this->rocket_(now);
  this->fireflies_(now);
  this->eggs_(now);
  this->matrix_();
  this->frost_sparks_(now);
  this->heat_();
  this->plane_(now);
  this->candles_(now);
  this->meteor_(now);
  this->sun_(now);
  this->rainbow_(now);
  this->garland_(now);
  this->fireworks_(now);
  this->kite_(now, wind);
  this->rain_frame_(wind);
  this->snow_frame_(wind);
  this->cover_step_(now);
  this->leaves_frame_(wind);
  this->flakes_(wind, ks);
  this->clouds_();
  this->lightning_(now);
}

}  // namespace weather_fx
}  // namespace esphome

#include "weather_fx.h"

#include <algorithm>
#include <cstring>

// Список участков к перерисовке (inv_areas) есть только во внутренних
// заголовках LVGL
#include <lvgl_private.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "fx_draw.h"

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

  // Вспышка молнии на миг освещает всё небо (clouds.cpp)
  this->sky_flash_paint_(layer, clip);

  // Зарево у горизонта на восходе и закате: тёплый свет снизу (extras.cpp)
  this->glow_paint_(layer, oc, clip);

  // Зарница за горизонтом (seasons.cpp), гало вокруг солнца или луны —
  // позади них (halo.cpp)
  this->hl_paint_(layer, oc, clip);
  this->halo_paint_(layer, oc, clip);

  // Солнце (sun.cpp)
  this->sun_paint_(layer, oc, clip);

  // Вифлеемская звезда: восемь лучей — длинный вниз — чуть «дышат»
  // (extras.cpp)
  this->bstar_paint_(layer, oc, clip);

  // Радуга (rainbow.cpp)
  this->rainbow_paint_(layer, oc, clip);

  // Луна в текущей фазе на своём месте в небе (moon.cpp). В полнолуние
  // крупнее, в суперлуние — ещё крупнее, в затмение наливается красным,
  // днём — бледная
  if (place(this->moon_spot_) && this->moon_phase_ >= 0)
    this->moon_paint_(layer, this->moon_x_ + oc.x1, this->moon_y_ + oc.y1);

  // Мерцающие звёзды (stars.cpp)
  this->stars_paint_(layer, oc, clip);

  // Хэллоуин: летучие мыши вместо звёзд (signs.cpp)
  if (this->bats_on_)
    this->bats_paint_(layer, oc, clip);

  // Созвездие с подписью и планеты (sky.cpp)
  this->sky_paint_(layer, oc, clip);

  // Метеор: светящийся след и яркая голова (extras.cpp)
  this->meteor_paint_(layer, oc, clip);

  // Самолёт с огнями (extras.cpp)
  this->plane_paint_(layer, oc, clip);

  // Молния и облака — поверх неба (clouds.cpp)
  this->clouds_paint_(layer, oc, clip);
}

void WeatherFx::paint_front(lv_layer_t *layer) {
  lv_area_t oc;
  lv_obj_get_coords(this->paint_, &oc);
  const lv_area_t &clip = layer->_clip_area;

  // Струи дождя, градины и брызги (rain.cpp), снег (snow.cpp), листья
  // (leaves.cpp)
  this->rain_paint_(layer, oc, clip);
  this->snow_paint_(layer, oc, clip);
  this->leaves_paint_(layer, oc, clip);

  // Лепестки яблонь и тополиный пух (seasons.cpp)
  this->seasons_paint_(layer, oc, clip);

  // Сердечки, цветы или шарики (signs.cpp)
  this->signs_paint_(layer, oc, clip);

  // Сугроб по нижнему краю (snow.cpp) — поверх падающего снега
  this->cover_paint_(layer, oc, clip);

  // Иней с искорками и сосульки под цифрами (frost.cpp)
  this->frost_paint_(layer, oc, clip);
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

  // Светлячки (extras.cpp), пасхальные яйца и «Матрица» (holiday.cpp)
  this->fireflies_paint_(layer, oc, clip);
  this->eggs_paint_(layer, oc, clip);
  this->matrix_paint_(layer, oc, clip);

  // Гирлянда: провод по краю и перемигивающиеся лампочки (garland.cpp)
  this->garland_paint_(layer, oc, clip);
}

void WeatherFx::apply_(const Params &p, bool relayout, bool restar) {
  const bool dim = p.dim;
  const int cnt = std::min(std::max(p.count, 0), ND);
  // Сменилась палитра — перерисовать всё: луна, солнце и созвездие
  // неподвижны и сами не обновятся
  if (dim != this->dim_ && this->back_) {
    lv_obj_invalidate(this->back_);
    lv_obj_invalidate(this->paint_);
  }
  this->dim_ = dim;
  // Снег — в snow.cpp, листья — в leaves.cpp, картинками всплывает и
  // падает остальное: сердечки, цветы, шарики (signs.cpp). Набор знаков
  // (fset_) меняется, только когда нужен другой: пока последние знаки
  // долетают, они остаются прежними
  static const int SET_OF_MODE[10] = {0, 0, 0, 0, 0, 1, 2, 3, 4, 5};
  const int set = p.mode >= 6 && p.mode <= 8 ? SET_OF_MODE[p.mode] : -1;
  const int nf_want = set > 0 ? std::min(cnt, NF) : 0;
  if (relayout && set > 0 && set != this->fset_) {
    // Другие знаки — прежние (у них другие картинки) убрать сразу
    for (auto &s : this->fs_)
      this->mark_(s, false, 0, 0, 1, 1);
    memset(this->flife_, 0, sizeof(this->flife_));
    this->nf_ = 0;
    this->fset_ = set;
  }
  if (relayout) {
    // Недостающие знаки появляются из-за края — сердечки и шарики снизу,
    // цветы сверху — один за другим, лишние долетают до края
    const bool rise = this->fset_ == 2 || this->fset_ == 4;
    this->nf_ = fx_relife(
        this->flife_, NF, [nf_want](int i) { return i < nf_want; },
        [this, rise](int i) {
          this->mark_(this->fs_[i], false, 0, 0, 1, 1);
          this->fx_[i] = rnd_(30, 430);
          this->fy_[i] = rise ? fx_bottom(this->fx_[i]) + 20 + rnd_(0, 300) : fx_top(this->fx_[i]) - 20 - rnd_(0, 300);
          this->fph_[i] = rnd_(0, 628) / 100.0f;
        });
  }

  // Праздник: что рисовать сверх погоды
  const int hol = p.holiday;
  if (hol != this->hol_)
    this->con_force_ = true;  // созвездие могло быть спрятано (Рождество, Хэллоуин)
  this->hol_ = hol;
  this->night_ = p.night;
  this->bats_on_ = hol == HOL_HALLOWEEN && p.night;
  this->bstar_apply_(hol == HOL_CHRISTMAS);
  this->rocket_apply_(hol == HOL_COSMOS);
  this->eggs_apply_(hol == HOL_EASTER);
  if (!this->bats_on_)
    for (auto &b : this->bat_spot_)
      this->mark_(b, false, 0, 0, 1, 1);
  this->fireflies_apply_(p.fireflies);
  this->matrix_apply_(p.matrix);
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
  // Луна — ночью вместе со звёздами, днём (бледная) — вместе с солнцем;
  // где она и какая — решает moon_()
  if (!this->stars_on_ && !this->sun_on_)
    this->moon_hide_();
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

  if (restar) {
    // Звёзды зажглись или часы ушли и вернулись — звёзды и метеор заново
    for (auto &s : this->sts_)
      this->mark_(s, false, 0, 0, 1, 1);
    this->mark_(this->met_spot_, false, 0, 0, 1, 1);
    this->met_on_ = false;
    // Звёзды — в верхней половине круга, вокруг и выше времени, но не на
    // месте созвездия. Без часов — по всему кругу, кроме погоды слева,
    // созвездия справа с подписью и строки планет. Звёзды, которые закроет
    // луна, гаснут, пока она рядом (stars_show_)
    this->nst_ = this->full_ ? NST : 12;
    for (int i = 0; i < this->nst_; i++) {
      int x, y;
      bool bad;
      do {
        x = rnd_(30, 436);
        y = this->full_ ? rnd_(30, 440) : rnd_(30, 220);
        bad = (x - 233) * (x - 233) + (y - 233) * (y - 233) > 215 * 215;
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
  }
  if (!relayout && !restar) {
    // Поменялись яркость, солнце или гирлянда — перерисовать слой целиком,
    // это разовая перерисовка
    lv_obj_invalidate(this->back_);
    lv_obj_invalidate(this->paint_);
  }
  // Звёзды: сколько проступило в сумерки (sky.cpp), какие не за луной
  this->twilight_();
  this->stars_show_();
  // Дождь и град: струи, брызги, их картинки и цвета
  this->rain_apply_(p, relayout);
  // Снег и сугроб, листопад
  this->snow_apply_(p, relayout);
  this->leaves_apply_(p, relayout);
  // Лепестки яблонь и тополиный пух (seasons.cpp)
  this->seasons_apply_(p, relayout);

  // Созвездие и планеты гаснут вместе со звёздами (sky.cpp); иней и
  // сосульки (frost.cpp)
  this->sky_apply_();
  this->frost_apply_();
  if (!this->stars_on_ && this->met_on_) {
    this->mark_(this->met_spot_, false, 0, 0, 1, 1);
    this->met_on_ = false;
  }
  if (!this->stars_on_)
    fx_sprite_free(this->met_spr_);
  this->sun_apply_();
  this->halo_();
  this->glow_apply_();
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

void WeatherFx::flakes_(float wind, float ks) {
  // Крупные знаки летят быстрее и качаются сильнее — так получается
  // глубина. У каждого свой ритм покачивания. Мелкие медленные, их двигаем
  // только на редких тиках (ks > 0)
  for (int i = 0; i < this->nf_; i++) {
    if (this->flife_[i] == LIFE_OFF)
      continue;
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
        // Всплыло за верхний край — снова снизу; праздник кончился — всё
        if (this->flife_[i] == LIFE_END) {
          this->flife_[i] = LIFE_OFF;
          this->mark_(this->fs_[i], false, 0, 0, 1, 1);
          continue;
        }
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
        if (this->flife_[i] == LIFE_END) {
          this->flife_[i] = LIFE_OFF;
          this->mark_(this->fs_[i], false, 0, 0, 1, 1);
          continue;
        }
        this->fy_[i] = -30 - rnd_(0, 40);
        this->fx_[i] = rnd_(30, 430);
      }
    }
    this->sign_mark_(i);
  }
  // Последние знаки долетели — их картинки больше не нужны
  const int was = this->nf_;
  this->nf_ = fx_life_top(this->flife_, this->nf_);
  if (was && !this->nf_)
    this->signs_apply_();
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

void WeatherFx::update_root_() {
  show_(this->root_, this->nd_ || this->nrs_ || this->nf_ || this->nsn_ || this->nlf_ || this->cover_spot_.on ||
                         this->ncl_ || this->cl_on_() || this->storm_ || this->stars_on_ || this->sun_on_ ||
                         this->gar_on_ || this->fw_on_ || this->kite_mode_ || this->rainbow_on_ ||
                         this->rb_level_ > 0 || this->frost_on_ || this->extras_drawn_());
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
  // Где сейчас Солнце и Луна на небе дома (sky.cpp)
  this->ephem_(p);
  // Гало появляется и уходит само (halo_): ему нужны солнце или луна
  this->halo_mode_ = p.halo;
  bool refull = false;
  if (p.full != this->full_) {
    // Часы ушли или вернулись: небо — заново под весь круг или под «шапку»;
    // осадки идут дальше как шли
    this->full_ = p.full;
    this->sky_full_();
    refull = true;
  }
  if (sig != this->applied_ || refull) {
    // Сменилась погода — осадки и облака меняются плавно (rain_apply_ и
    // остальные); если поменялись только яркость, солнце, гирлянда или
    // праздник — перекрашиваем, осадки не трогаем. Звёзды раскладываются
    // заново, только когда зажглись или часы ушли и вернулись
    const int keep = ~((1 << 13) | (1 << 16) | (1 << 17) | (1 << 18) | (3 << 19) | (1 << 21) | (1 << 22) |
                       (31 << 23) | (1 << 28) | (1 << 29) | (1 << 30));
    const bool first = this->applied_ < 0;
    const bool relayout = first || (sig & keep) != (this->applied_ & keep);
    const bool restar = first || refull || (p.stars && !this->stars_on_);
    this->applied_ = sig;
    this->apply_(p, relayout, restar);
  }

  // Зарево, марево и самолёт меняются без перестройки погоды
  const float glow = std::min(std::max(p.glow, 0.0f), 1.0f);
  if (std::fabs(glow - this->horizon_) > 0.02f || (glow > 0.01f && p.dawn != this->dawn_)) {
    const bool vis = (glow > 0.01f) != (this->horizon_ > 0.01f);
    this->horizon_ = glow;
    this->dawn_ = p.dawn;
    this->glow_apply_();
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
    this->plane_apply_(p.plane);
    this->update_root_();
  }
  if (p.heat_lightning != this->hl_mode_) {
    this->hl_apply_(p.heat_lightning);
    this->update_root_();
  }

  const bool any = this->nd_ || this->nrs_ || this->nf_ || this->nsn_ || this->nlf_ || this->cover_ > 0 || this->ncl_ ||
                   this->cl_on_() || this->storm_ || this->stars_on_ || this->sun_on_ || this->gar_on_ ||
                   this->fw_on_ || this->kite_mode_ || this->rainbow_on_ || this->rb_level_ > 0 || this->frost_on_ ||
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
  // Луна — раньше созвездия и звёзд: они уступают ей место
  this->moon_(p);
  this->twilight_();
  this->sky_(p);
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
  this->halo_();
  this->hl_(now);
  this->rainbow_(now);
  this->garland_(now);
  this->fireworks_(now);
  this->kite_(now, wind);
  this->rain_frame_(wind);
  this->snow_frame_(wind);
  this->cover_step_(now);
  this->leaves_frame_(wind);
  this->seasons_frame_(wind);
  this->flakes_(wind, ks);
  this->clouds_();
  this->lightning_(now);
}

}  // namespace weather_fx
}  // namespace esphome

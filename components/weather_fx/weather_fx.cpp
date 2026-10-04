#include "weather_fx.h"

#include <algorithm>
#include <cstring>

// Список участков к перерисовке (inv_areas) есть только во внутренних
// заголовках LVGL
#include <lvgl_private.h>

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

// Сердечки 14 февраля: обычное и «карточное», розовые и красные
static const uint32_t HEART_CP[2] = {0xF02D1, 0xF08D0};
static const uint32_t HEART_COLORS[4] = {0xFF4081, 0xE91E63, 0xFF1744, 0xF06292};
static const uint32_t HEART_COLORS_DIM[4] = {0xFF80AB, 0xFF4F8B, 0xFF5C7A, 0xFF9CC0};
// Салют: цвета вспышек
static const uint32_t FW_COLORS[6] = {0xFF5252, 0xFFD740, 0x69F0AE, 0x40C4FF, 0xE040FB, 0xFFFFFF};

// 8 Марта: тюльпаны (розовые, коралловые) и веточки мимозы (жёлтые)
static const uint32_t FLOWER_CP[2] = {0xF09F1, 0xF024A};
static const uint32_t FLOWER_COLORS[4] = {0xFF4F7B, 0xFFD23F, 0xFF6B4A, 0xFFE45C};
// 1 июня: воздушные шарики
static const uint32_t BALLOON_CP[1] = {0xF0A26};
static const uint32_t BALLOON_COLORS[4] = {0xFF5252, 0x40C4FF, 0xFFD740, 0x69F0AE};
// 1 сентября: кленовые листья
static const uint32_t MAPLE_CP[1] = {0xF0C93};
// Хэллоуин: летучая мышь
static const uint32_t BAT_CP[1] = {0xF0B5F};
// Салют: бело-сине-красный (12 июня) и красно-золотой (23 февраля)
static const uint32_t FW_TRICOLOR[3] = {0xFFFFFF, 0x2962FF, 0xFF1744};
static const uint32_t FW_RED[3] = {0xFF1744, 0xFFC400, 0xFF6D00};
// Пасхальные яйца
static const uint32_t EGG_COLORS[4] = {0xE53935, 0x1E88E5, 0xFDD835, 0x43A047};

static uint32_t glyph_cp(int set, int i) {
  switch (set) {
    case 1:
      return LEAF_CP[(i / 2) % 2];
    case 2:
      return HEART_CP[(i / 2) % 2];
    case 3:
      return FLOWER_CP[(i / 2) % 2];
    case 4:
      return BALLOON_CP[0];
    case 5:
      return MAPLE_CP[0];
    default:
      return FLAKE_CP[i % 5];
  }
}
// Солнце — справа в «шапке» круга, мимо значка двери посередине
static const int SUN_X = 300, SUN_Y = 74, SUN_R = 40;
// Созвездие вписывается в «шапку» над строкой погоды: центр и размеры
// прямоугольника, подпись — над ним. Когда часы убраны (режим ожидания) —
// в большой прямоугольник справа: слева по центру там погода на улице, над
// ней луна
struct SkyBox {
  float cx, cy, w, h;
  int cap_x, cap_y;
};
static const SkyBox BOX_TOP = {233.0f, 82.0f, 280.0f, 60.0f, 233, 24};
static const SkyBox BOX_FULL = {300.0f, 236.0f, 210.0f, 250.0f, 300, 78};
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
// Радуга — дугой по верху круга, над строкой погоды
static const int RB_X = 233, RB_Y = 190, RB_R = 120, RB_W = 4;
static const uint32_t RB_COLORS[7] = {0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x32ADE6, 0x3F51B5, 0x9C27B0};
// Вифлеемская звезда — в «шапке» круга слева от середины, мимо солнца
static const int BSTAR_X = 190, BSTAR_Y = 66, BSTAR_R = 36;
// Ракета летит 4,5 с снизу слева вверх направо, над строкой погоды
static const uint32_t RK_MS = 4500;
static const float PUFF_MS = 900.0f;
// Сосульки: полочка под цифрами часов и минут и сосульки на ней — x,
// длина и ширина у основания. Не ниже строки «Ясно»
static const int ICE_Y = 265;
static const int ICE_SHELF[2][2] = {{74, 218}, {248, 392}};
static const int16_t ICICLES[][3] = {
    {82, 9, 5},  {97, 14, 6},  {113, 7, 4},  {128, 12, 6}, {146, 6, 4},  {161, 13, 5}, {179, 9, 5},
    {196, 11, 6}, {211, 6, 4}, {256, 8, 4},  {271, 13, 6}, {289, 7, 5},  {305, 12, 5}, {323, 6, 4},
    {339, 14, 6}, {357, 8, 5}, {372, 11, 5}, {386, 6, 4},
};
// Самолёт: скорость, px/мс
static const float AIR_SPEED = 0.022f;
// Свечи «Романтики»: основание на дуге радиусом 200 px по низу круга, высота
// у каждой своя; пламя — над фитилём
static const int CND_X[5] = {75, 145, 233, 321, 391}, CND_Y[5] = {356, 413, 433, 413, 356};
static const int CND_H[5] = {30, 38, 26, 36, 32};
static const int CND_GLOW = 24;
// «Матрица»: шаг столбцов и высота знака
static const int MX_X0 = 45, MX_DX = 47, MX_STEP = 18;
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
  static bool join_hooked = false;
  if (!join_hooked) {
    join_hooked = true;
    lv_display_t *d = lv_obj_get_display(root);
    lv_display_add_event_cb(d, join_areas_cb, LV_EVENT_REFR_START, d);
  }

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
  for (int set = 0; set < NSET; set++) {
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
  // Летучая мышь — тот же расчёт рамки знака, мелкая и крупная
  for (int sz = 0; sz < 2; sz++) {
    const lv_font_t *f = sz ? flake_l : flake_s;
    lv_font_glyph_dsc_t g;
    const int lh = lv_font_get_line_height(f);
    if (lv_font_get_glyph_dsc(f, &g, BAT_CP[0], 0) && g.box_w > 0 && g.box_h > 0) {
      this->bat_ox_[sz] = g.ofs_x - g.adv_w / 2 - 2;
      this->bat_oy_[sz] = -g.box_h - g.ofs_y - 2;
      this->bat_w_[sz] = g.box_w + 4;
      this->bat_h_[sz] = g.box_h + 4;
    } else {
      this->bat_ox_[sz] = -lh;
      this->bat_oy_[sz] = -lh;
      this->bat_w_[sz] = 2 * lh;
      this->bat_h_[sz] = 2 * lh;
    }
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
  // Круг с центром (x, y) в экранных координатах
  auto disc_at = [&](int x, int y, int r, uint32_t c, lv_opa_t o) {
    lv_area_t a = {x - r, y - r, x + r, y + r};
    fill.radius = LV_RADIUS_CIRCLE;
    fill.color = lv_color_hex(c);
    fill.opa = o;
    lv_draw_fill(layer, &fill, &a);
  };

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
    if (this->hol_ == HOL_MASLENITSA) {
      // Масленица: солнце — румяный блин с весёлым лицом
      disc(17, 0xF2B84B, LV_OPA_COVER);
      static const int8_t SPOTS[5][3] = {{-9, -8, 2}, {10, -6, 2}, {-11, 6, 1}, {9, 9, 2}, {1, -12, 1}};
      for (const auto &sp : SPOTS)
        disc_at(cx + sp[0], cy + sp[1], sp[2], 0xC98A2E, 200);
      fill.radius = 1;
      fill.opa = LV_OPA_COVER;
      fill.color = lv_color_hex(0x5A3A12);
      lv_area_t eye_l = {cx - 7, cy - 5, cx - 5, cy - 3}, eye_r = {cx + 5, cy - 5, cx + 7, cy - 3};
      lv_draw_fill(layer, &fill, &eye_l);
      lv_draw_fill(layer, &fill, &eye_r);
      lv_draw_arc_dsc_t smile;
      lv_draw_arc_dsc_init(&smile);
      smile.center = {cx, cy};
      smile.radius = 9;
      smile.width = 2;
      smile.start_angle = 30;
      smile.end_angle = 150;
      smile.color = lv_color_hex(0x5A3A12);
      smile.opa = LV_OPA_COVER;
      lv_draw_arc(layer, &smile);
    } else {
      disc(17, dim ? 0xFFD54F : 0xFFC21A, LV_OPA_COVER);
    }
    fill.opa = LV_OPA_COVER;
    ln.round_start = 0;
    ln.round_end = 0;
  }

  // Вифлеемская звезда: мягкое сияние, восемь лучей — длинный вниз — и
  // яркая сердцевина. Лучи чуть «дышат»
  if (place(this->bstar_spot_)) {
    const int cx = BSTAR_X + oc.x1, cy = BSTAR_Y + oc.y1;
    disc_at(cx, cy, 22, 0xFFE9A8, dim ? 40 : 26);
    disc_at(cx, cy, 11, 0xFFF3C4, dim ? 90 : 64);
    lv_draw_triangle_dsc_t tr;
    lv_draw_triangle_dsc_init(&tr);
    tr.color = lv_color_hex(dim ? 0xFFFFFF : 0xFFF3C4);
    tr.opa = 235;
    const float tw = 0.85f + 0.15f * sinf(this->bstar_ph_);
    for (int k = 0; k < 8; k++) {
      const float a = k * (PI_F / 4) - PI_F / 2;
      const float len = (k == 4 ? 34.0f : (k % 2 ? 13.0f : 24.0f)) * (k % 2 ? 2.0f - tw : tw);
      const float w = k % 2 ? 1.6f : 2.6f;
      const float c = cosf(a), sn = sinf(a);
      tr.p[0] = {(lv_value_precise_t) (cx - sn * w), (lv_value_precise_t) (cy + c * w)};
      tr.p[1] = {(lv_value_precise_t) (cx + sn * w), (lv_value_precise_t) (cy - c * w)};
      tr.p[2] = {(lv_value_precise_t) (cx + c * len), (lv_value_precise_t) (cy + sn * len)};
      lv_draw_triangle(layer, &tr);
    }
    disc_at(cx, cy, 4, 0xFFFFFF, LV_OPA_COVER);
  }

  // Радуга: семь дуг от красной снаружи до фиолетовой внутри
  if (this->rainbow_on_) {
    lv_area_t rb = {RB_X - RB_R - 2 + oc.x1, RB_Y - RB_R - 2 + oc.y1, RB_X + RB_R + 2 + oc.x1, RB_Y + oc.y1};
    if (hit(rb, clip)) {
      lv_draw_arc_dsc_t arc;
      lv_draw_arc_dsc_init(&arc);
      arc.center = {RB_X + oc.x1, RB_Y + oc.y1};
      arc.width = RB_W;
      arc.start_angle = 198;
      arc.end_angle = 342;
      arc.opa = dim ? 170 : 120;
      for (int k = 0; k < 7; k++) {
        arc.radius = RB_R - k * RB_W;
        arc.color = lv_color_hex(RB_COLORS[k]);
        lv_draw_arc(layer, &arc);
      }
    }
  }

  // Луна: светлый диск, на который наползает чёрная тень — от фазы. Растущая
  // светится справа, убывающая — слева. Поверх — слабый ореол: тёмная часть
  // диска от него чуть видна, как пепельный свет
  // В полнолуние луна крупнее, в суперлуние — ещё крупнее, в затмение
  // наливается красным
  if (place(this->moon_spot_) && this->moon_phase_ >= 0) {
    const int cx = MOON_X + oc.x1, cy = MOON_Y + oc.y1, r = this->moon_r_;
    fill.radius = LV_RADIUS_CIRCLE;
    fill.opa = LV_OPA_COVER;
    fill.color = lv_color_mix(lv_color_hex(0xB5482B), lv_color_hex(dim ? 0xFFF8E1 : 0xE8E0C4),
                              (uint8_t) (255 * this->moon_red_));
    lv_area_t disc = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_fill(layer, &fill, &disc);
    const float ph = this->moon_phase_;
    const float k = ph < 0.5f ? ph / 0.5f : (1.0f - ph) / 0.5f;  // 0 новолуние … 1 полнолуние
    const int dx = (int) (k * (2 * r + 2)) * (ph < 0.5f ? -1 : 1);
    if (std::abs(dx) <= 2 * r) {
      fill.color = lv_color_black();
      lv_area_t sh = {cx - r + dx, cy - r - 1, cx + r + dx, cy + r + 1};
      lv_draw_fill(layer, &fill, &sh);
    }
    fill.color = lv_color_hex(0xFFF3C4);
    fill.opa = dim ? 34 : 24;
    lv_area_t halo = {cx - r - 7, cy - r - 7, cx + r + 7, cy + r + 7};
    lv_draw_fill(layer, &fill, &halo);
    fill.opa = LV_OPA_COVER;
  }

  // Мерцающие звёзды
  fill.radius = 1;
  fill.opa = LV_OPA_COVER;
  for (int i = 0; i < NST; i++) {
    if (!place(this->sts_[i]))
      continue;
    fill.color = this->stc_[i];
    lv_draw_fill(layer, &fill, &abs);
  }

  // Хэллоуин: летучие мыши вместо звёзд
  if (this->bats_on_) {
    lv_draw_letter_dsc_t let;
    lv_draw_letter_dsc_init(&let);
    let.opa = LV_OPA_COVER;
    let.unicode = BAT_CP[0];
    for (int i = 0; i < NBAT; i++) {
      if (!place(this->bat_spot_[i]))
        continue;
      const int sz = i % 2;
      let.font = sz ? this->font_l_ : this->font_s_;
      let.color = lv_color_hex(dim ? (sz ? 0xC9B6F0 : 0x9E8CC8) : (sz ? 0x8E7CC3 : 0x5E507F));
      lv_point_t pt = {abs.x1 - this->bat_ox_[sz], abs.y1 - this->bat_oy_[sz]};
      lv_draw_letter(layer, &let, &pt);
    }
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

  // Планеты: цветная точка и название
  if (this->npl_) {
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
  const int set = this->fset_;
  for (int i = 0; i < NF; i++) {
    if (!place(this->fs_[i]))
      continue;
    const bool big = i % 2;
    let.font = big ? this->font_l_ : this->font_s_;
    if (set == 2)
      let.color = big ? this->c_heart_[(i / 2) % 4] : this->c_heart_s_[(i / 2) % 4];
    else if (set == 1)
      let.color = big ? this->c_leaf_[(i / 2) % 4] : this->c_leaf_s_[(i / 2) % 4];
    else if (set == 0)
      let.color = big ? this->c_flake_l_ : this->c_flake_s_;
    else
      let.color = big ? this->c_set_[set][(i / 2) % 4] : this->c_set_s_[set][(i / 2) % 4];
    let.unicode = glyph_cp(set, i);
    // Обратно из прямоугольника знака к точке рисования
    lv_point_t pt = {abs.x1 - this->fox_[set][i], abs.y1 - this->foy_[set][i]};
    lv_draw_letter(layer, &let, &pt);
  }

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

    // Сосульки: ледяная полочка под цифрами и сосульки с бликом
    lv_area_t ia = {ICE_SHELF[0][0] + oc.x1, ICE_Y + oc.y1, ICE_SHELF[1][1] + oc.x1, ICE_Y + 18 + oc.y1};
    if (hit(ia, clip)) {
      const lv_color_t ice = lv_color_hex(this->dim_ ? 0xDDF0FF : 0xA8CCEA);
      fill.radius = 1;
      fill.color = ice;
      fill.opa = 190;
      for (const auto &sh : ICE_SHELF) {
        lv_area_t ledge = {sh[0] + oc.x1, ICE_Y + oc.y1, sh[1] + oc.x1, ICE_Y + 2 + oc.y1};
        lv_draw_fill(layer, &fill, &ledge);
      }
      lv_draw_triangle_dsc_t tr;
      lv_draw_triangle_dsc_init(&tr);
      for (const auto &ic : ICICLES) {
        const float x = ic[0] + oc.x1, y = ICE_Y + 2 + oc.y1, len = ic[1], w = ic[2] / 2.0f;
        tr.color = ice;
        tr.opa = 200;
        tr.p[0] = {(lv_value_precise_t) (x - w), (lv_value_precise_t) y};
        tr.p[1] = {(lv_value_precise_t) (x + w), (lv_value_precise_t) y};
        tr.p[2] = {(lv_value_precise_t) x, (lv_value_precise_t) (y + len)};
        lv_draw_triangle(layer, &tr);
        tr.color = lv_color_white();
        tr.opa = 210;
        tr.p[0] = {(lv_value_precise_t) (x - w + 0.5f), (lv_value_precise_t) y};
        tr.p[1] = {(lv_value_precise_t) (x - w + 1.8f), (lv_value_precise_t) y};
        tr.p[2] = {(lv_value_precise_t) (x - 0.3f), (lv_value_precise_t) (y + len * 0.7f)};
        lv_draw_triangle(layer, &tr);
      }
      fill.opa = LV_OPA_COVER;
    }
  }

  // Свечи: тёплый ореол, восковое тело с бликом, фитиль и дрожащее пламя
  if (this->cnd_on_) {
    lv_draw_triangle_dsc_t ft;
    lv_draw_triangle_dsc_init(&ft);
    lv_draw_line_dsc_t wl;
    lv_draw_line_dsc_init(&wl);
    wl.width = 2;
    wl.color = lv_color_hex(0x3A2A1A);
    for (int i = 0; i < NCND; i++) {
      if (!place(this->cnd_spot_[i]))
        continue;
      const int x = CND_X[i] + oc.x1, base = CND_Y[i] + oc.y1, top = base - CND_H[i];
      const float fh = this->cnd_fh_[i], dx = this->cnd_dx_[i];
      const int fy = top - 4 - (int) (fh * 0.5f);
      fill.radius = LV_RADIUS_CIRCLE;
      fill.color = lv_color_hex(0xFF9F43);
      // Ореол — три круга друг в друге: к пламени теплее и ярче
      static const int GR[3] = {CND_GLOW, 17, 10};
      static const float GO[3] = {18.0f, 26.0f, 40.0f};
      for (int k = 0; k < 3; k++) {
        fill.opa = (lv_opa_t) (GO[k] * this->cnd_glow_[i]);
        lv_area_t g = {x - GR[k], fy - GR[k], x + GR[k], fy + GR[k]};
        lv_draw_fill(layer, &fill, &g);
      }
      lv_area_t b;
      // Тело свечи
      fill.radius = 3;
      fill.opa = LV_OPA_COVER;
      fill.color = lv_color_hex(0xF3E3C3);
      b = {x - 6, top, x + 6, base};
      lv_draw_fill(layer, &fill, &b);
      fill.color = lv_color_hex(0xD6C29C);
      b = {x + 3, top + 3, x + 5, base - 1};
      lv_draw_fill(layer, &fill, &b);
      fill.radius = LV_RADIUS_CIRCLE;
      fill.color = lv_color_hex(0xFFF6E0);
      b = {x - 6, top + 2, x - 3, top + 8};  // потёк воска
      lv_draw_fill(layer, &fill, &b);
      // Фитиль
      wl.p1 = {(lv_value_precise_t) x, (lv_value_precise_t) top};
      wl.p2 = {(lv_value_precise_t) (x + dx * 0.4f), (lv_value_precise_t) (top - 4)};
      lv_draw_line(layer, &wl);
      // Пламя: капля — овал и острый язычок сверху, внутри светлое ядро
      const float cx = x + dx;
      fill.color = lv_color_hex(0xFFA726);
      fill.opa = 235;
      b = {(int32_t) (cx - 4), (int32_t) (fy - fh * 0.25f), (int32_t) (cx + 4), (int32_t) (fy + fh * 0.5f)};
      lv_draw_fill(layer, &fill, &b);
      ft.color = lv_color_hex(0xFFA726);
      ft.opa = 235;
      ft.p[0] = {(lv_value_precise_t) (cx - 4), (lv_value_precise_t) (fy)};
      ft.p[1] = {(lv_value_precise_t) (cx + 4), (lv_value_precise_t) (fy)};
      ft.p[2] = {(lv_value_precise_t) (cx + dx * 1.5f), (lv_value_precise_t) (fy - fh * 0.75f)};
      lv_draw_triangle(layer, &ft);
      fill.color = lv_color_hex(0xFFF3C4);
      fill.opa = LV_OPA_COVER;
      b = {(int32_t) (cx - 2), (int32_t) (fy), (int32_t) (cx + 2), (int32_t) (fy + fh * 0.45f)};
      lv_draw_fill(layer, &fill, &b);
    }
    fill.radius = 0;
    fill.opa = LV_OPA_COVER;
  }

  // Марево в жару: волнистые струйки тёплого воздуха поднимаются и тают
  if (this->heat_on_) {
    lv_draw_line_dsc_t hl;
    lv_draw_line_dsc_init(&hl);
    hl.width = 2;
    hl.round_start = 1;
    hl.round_end = 1;
    hl.color = lv_color_hex(0xFFC48A);
    for (int i = 0; i < NHEAT; i++) {
      if (!place(this->heat_spot_[i]))
        continue;
      const float a = this->hage_[i];
      const float vis = std::min(a * 4.0f, 1.0f) * (1.0f - a);
      hl.opa = (lv_opa_t) ((this->dim_ ? 130.0f : 90.0f) * vis);
      const float x0 = this->hx_[i] - 13 + oc.x1, y0 = this->hy_[i] + oc.y1;
      for (int k = 0; k < 6; k++) {
        hl.p1 = {(lv_value_precise_t) (x0 + k * 4.4f), (lv_value_precise_t) (y0 + 2.0f * sinf(this->hph_[i] + k * 0.9f))};
        hl.p2 = {(lv_value_precise_t) (x0 + (k + 1) * 4.4f),
                 (lv_value_precise_t) (y0 + 2.0f * sinf(this->hph_[i] + (k + 1) * 0.9f))};
        lv_draw_line(layer, &hl);
      }
    }
  }

  // Салют: искры цветом вспышки, гаснут со временем
  fill.radius = LV_RADIUS_CIRCLE;
  for (int j = 0; j < NB; j++) {
    const Burst &b = this->bursts_[j];
    if (!b.on)
      continue;
    fill.color = b.c;
    fill.opa = this->burst_opa_[j];
    for (int k = 0; k < NP; k++)
      if (place(b.sp[k]))
        lv_draw_fill(layer, &fill, &abs);
  }
  fill.opa = LV_OPA_COVER;

  // Воздушный змей: ромб из четырёх цветных треугольников, крестовина и
  // хвост с бантиками
  if (place(this->kite_spot_)) {
    const float kx = this->kite_x_ + oc.x1, ky = this->kite_y_ + oc.y1;
    const lv_point_precise_t top = {(lv_value_precise_t) kx, (lv_value_precise_t) (ky - 18)};
    const lv_point_precise_t bot = {(lv_value_precise_t) kx, (lv_value_precise_t) (ky + 24)};
    const lv_point_precise_t lft = {(lv_value_precise_t) (kx - 14), (lv_value_precise_t) ky};
    const lv_point_precise_t rgt = {(lv_value_precise_t) (kx + 14), (lv_value_precise_t) ky};
    const lv_point_precise_t mid = {(lv_value_precise_t) kx, (lv_value_precise_t) ky};
    lv_draw_triangle_dsc_t tr;
    lv_draw_triangle_dsc_init(&tr);
    tr.opa = LV_OPA_COVER;
    const lv_point_precise_t quads[4][3] = {{top, lft, mid}, {top, rgt, mid}, {bot, lft, mid}, {bot, rgt, mid}};
    static const uint32_t KITE_C[4] = {0xFF5252, 0xFFD740, 0x40C4FF, 0x69F0AE};
    for (int q = 0; q < 4; q++) {
      tr.p[0] = quads[q][0];
      tr.p[1] = quads[q][1];
      tr.p[2] = quads[q][2];
      tr.color = lv_color_hex(KITE_C[q]);
      lv_draw_triangle(layer, &tr);
    }
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.opa = LV_OPA_COVER;
    ln.width = 1;
    ln.color = lv_color_hex(0x5D4037);
    ln.p1 = top;
    ln.p2 = bot;
    lv_draw_line(layer, &ln);
    ln.p1 = lft;
    ln.p2 = rgt;
    lv_draw_line(layer, &ln);
    ln.color = lv_color_hex(0xCFD8DC);
    ln.p1 = bot;
    for (int j = 0; j < NTAIL; j++) {
      ln.p2 = {this->kite_tail_[j].x + oc.x1, this->kite_tail_[j].y + oc.y1};
      if (j)
        lv_draw_line(layer, &ln);
      ln.p1 = ln.p2;
      if (j == 3 || j == 6 || j == NTAIL - 1) {
        // Бантик на хвосте
        lv_area_t bow = {(int32_t) ln.p2.x - 3, (int32_t) ln.p2.y - 2, (int32_t) ln.p2.x + 3, (int32_t) ln.p2.y + 2};
        fill.radius = 1;
        fill.color = lv_color_hex(j == 6 ? 0xFFD740 : 0xFF5252);
        lv_draw_fill(layer, &fill, &bow);
      }
    }
  }

  // Ракета: дым позади, пламя, оперение, корпус, нос и иллюминатор.
  // Рисуется в своих осях: x — вперёд по курсу
  if (place(this->rk_spot_)) {
    for (int j = 0; j < NPUFF; j++) {
      const float age = this->puff_age_[j];
      if (age >= PUFF_MS)
        continue;
      const int r = 3 + (int) (age / PUFF_MS * 5.0f);
      lv_area_t a = {(int32_t) this->puff_x_[j] + oc.x1 - r, (int32_t) this->puff_y_[j] + oc.y1 - r,
                     (int32_t) this->puff_x_[j] + oc.x1 + r, (int32_t) this->puff_y_[j] + oc.y1 + r};
      fill.radius = LV_RADIUS_CIRCLE;
      fill.color = lv_color_hex(0x9AA4AE);
      fill.opa = (lv_opa_t) (110.0f * (1.0f - age / PUFF_MS));
      lv_draw_fill(layer, &fill, &a);
    }
    fill.opa = LV_OPA_COVER;
  }
  if (place(this->rk_spot_) && this->rk_x_ > -500) {
    const float ca = cosf(this->rk_a_), sa = sinf(this->rk_a_);
    const float bx = this->rk_x_ + oc.x1, by = this->rk_y_ + oc.y1;
    auto P = [&](float lx, float ly) {
      return lv_point_precise_t{(lv_value_precise_t) (bx + lx * ca - ly * sa), (lv_value_precise_t) (by + lx * sa + ly * ca)};
    };
    lv_draw_triangle_dsc_t tr;
    lv_draw_triangle_dsc_init(&tr);
    tr.opa = LV_OPA_COVER;
    auto tri = [&](lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c, uint32_t col) {
      tr.p[0] = a;
      tr.p[1] = b;
      tr.p[2] = c;
      tr.color = lv_color_hex(col);
      lv_draw_triangle(layer, &tr);
    };
    const float fl = this->rk_flame_;
    tri(P(-12, -4), P(-12, 4), P(-12 - fl, 0), 0xFF9800);
    tri(P(-12, -2), P(-12, 2), P(-12 - fl * 0.6f, 0), 0xFFEB3B);
    tri(P(-12, -5), P(-4, -5), P(-17, -11), 0xE53935);
    tri(P(-12, 5), P(-4, 5), P(-17, 11), 0xE53935);
    tri(P(-12, -5), P(9, -5), P(9, 5), 0xECEFF1);
    tri(P(-12, -5), P(9, 5), P(-12, 5), 0xECEFF1);
    tri(P(9, -5), P(18, 0), P(9, 5), 0xE53935);
    const lv_point_precise_t w = P(2, 0);
    lv_area_t win = {(int32_t) w.x - 3, (int32_t) w.y - 3, (int32_t) w.x + 3, (int32_t) w.y + 3};
    fill.radius = LV_RADIUS_CIRCLE;
    fill.color = lv_color_hex(0x4FC3F7);
    lv_draw_fill(layer, &fill, &win);
  }

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
  if (this->shake_) {
    this->shake_ = 0;
    lv_obj_set_style_translate_x(lv_screen_active(), 0, 0);
  }
  show_(this->bolt_, false);
  show_(this->glow_, false);
  this->next_strike_ = millis() + rnd_(5000, 15000);
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
  this->hail_ = p.mode == 4;
  // Дождь (и дождь в мокром снеге) — каплями на стекле, если так выбрано.
  // Град всегда падает
  this->glass_on_ = !this->hail_ && p.rain_style == 0;
  this->nd_ = (p.mode == 1 || p.mode == 4) ? cnt : (p.mode == 3 ? cnt / 2 : 0);
  const bool signs = p.mode == 2 || (p.mode >= 5 && p.mode <= 9);
  this->nf_ = std::min(signs ? cnt : (p.mode == 3 ? cnt / 2 : 0), NF);
  static const int SET_OF_MODE[10] = {0, 0, 0, 0, 0, 1, 2, 3, 4, 5};
  this->fset_ = p.mode >= 0 && p.mode <= 9 ? SET_OF_MODE[p.mode] : 0;

  // Праздник: что рисовать сверх погоды
  const int hol = p.holiday;
  if (hol != this->hol_)
    this->con_force_ = true;  // созвездие могло быть спрятано (Рождество, Хэллоуин)
  this->hol_ = hol;
  this->night_ = p.night;
  this->bats_on_ = hol == HOL_HALLOWEEN && p.night;
  this->bstar_on_ = hol == HOL_CHRISTMAS;
  this->mark_(this->bstar_spot_, this->bstar_on_, BSTAR_X - BSTAR_R, BSTAR_Y - BSTAR_R, 2 * BSTAR_R + 1,
              2 * BSTAR_R + 1);
  if (hol != HOL_COSMOS && this->rk_on_) {
    this->mark_(this->rk_spot_, false, 0, 0, 1, 1);
    this->rk_on_ = false;
  }
  if (hol != HOL_COSMOS)
    this->next_rk_ = 0;
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
  if (p.rainbow != this->rainbow_on_) {
    lv_area_t rb = {RB_X - RB_R - 2, RB_Y - RB_R - 2, RB_X + RB_R + 2, RB_Y};
    this->invalidate_(rb);
  }
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
  }
  // Салют кончился или змей больше не летает — убрать с экрана
  if (!this->fw_on_)
    for (auto &b : this->bursts_) {
      for (auto &sp : b.sp)
        this->mark_(sp, false, 0, 0, 1, 1);
      b.on = false;
    }
  if (!this->kite_mode_ && this->kite_on_) {
    this->mark_(this->kite_spot_, false, 0, 0, 1, 1);
    this->kite_on_ = false;
  }

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
                (x > 75 && x < 391 && y > 388 && y < 428);              // планеты
        else
          bad = bad || x < 50 || x > 416 || (x > 82 && x < 372 && y > 22 && y < 122);
      } while (bad);
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
  if (this->stars_on_ && !this->bats_on_) {
    for (int i = 0; i < NST; i++)
      this->mark_(this->sts_[i], i < this->nst_, this->stx_[i], this->sty_[i], 3, 3);
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
  if (!this->stars_on_ && this->npl_) {
    this->npl_ = 0;
    this->invalidate_(PL_AREA);
  }
  // Иней: узор собирается при включении и освобождает память при выключении
  if (this->frost_on_ && !this->frost_buf_) {
    this->build_frost_();
    lv_obj_invalidate(this->paint_);
  } else if (!this->frost_on_ && this->frost_buf_) {
    this->free_frost_();
    lv_obj_invalidate(this->paint_);
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
    if (this->fset_ == 2 || this->fset_ == 4) {
      // Сердечки и воздушные шарики медленно всплывают снизу вверх, покачиваясь
      this->fy_[i] -= k * (big ? 0.9f + (i % 3) * 0.15f : 0.55f + (i % 3) * 0.1f);
      this->fph_[i] += k * ((big ? 0.06f : 0.05f) + i * 0.003f);
      this->fx_[i] = wrap_x(this->fx_[i] + k * (cosf(this->fph_[i]) * (big ? 0.9f : 0.6f) + wind * 0.5f));
      if (this->fy_[i] < -30) {
        this->fy_[i] = 490 + rnd_(0, 40);
        this->fx_[i] = rnd_(30, 430);
      }
    } else if (this->fset_ == 1 || this->fset_ == 3 || this->fset_ == 5) {
      // Листья и цветы падают медленнее, раскачиваются шире и сильнее летят по ветру
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
    const int set = this->fset_;
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
  // Звёзды мерцают: яркость меняется по синусу, у каждой свой ритм. Каждая
  // звезда меняется раз в 200 мс — чаще глаз не заметит. Звёзды поделены на
  // три группы, которые меняются по очереди раз в ~67 мс: если менять все
  // 12 разом, этот кадр выходит втрое тяжелее и на нём спотыкается метеор
  if (!this->stars_on_ || this->bats_on_ || now - this->star_ms_ < 60)
    return;
  this->star_ms_ = now;
  this->star_grp_ = (this->star_grp_ + 1) % 3;
  const int hi = this->dim_ ? 0xFF : 0xC8;
  for (int i = this->star_grp_; i < NST; i += 3) {
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
  // Гром: экран вздрагивает на первых долях удара
  const int off = t < 50 ? 3 : (t < 100 ? -3 : (t < 150 ? 2 : 0));
  if (off != this->shake_) {
    this->shake_ = off;
    lv_obj_set_style_translate_x(lv_screen_active(), off, 0);
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
  this->invalidate_(PL_AREA);
}

void WeatherFx::build_frost_() {
  // Морозный узор, как на оконном стекле: плотная кайма у края (внизу
  // толще), перистые кристаллы, ветвящиеся под 60°, — к середине они тают,
  // — мелкие шестилучевые звёздочки и крупа. Всё рисуется сглаженно в маску
  // прозрачности; узор одинаковый при каждом включении — постоянное зерно
  static const int W = 466;
  if (!this->frost_buf_) {
    this->frost_buf_ = static_cast<uint8_t *>(lv_malloc(W * W));
    if (!this->frost_buf_)
      return;
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

  // Кайма у края и изморозь-крупа
  const float p1 = rnd01() * 6.28f, p2 = rnd01() * 6.28f, p3 = rnd01() * 6.28f;
  for (int y = 0; y < W; y++) {
    for (int x = 0; x < W; x++) {
      const float dx = x - 232.5f, dy = y - 232.5f, r2 = dx * dx + dy * dy;
      if (r2 < 192.0f * 192.0f || r2 > 233.5f * 233.5f)
        continue;
      const float edge = 233.0f - sqrtf(r2);
      const float ang = atan2f(dy, dx);
      const float bottom = std::max(0.0f, sinf(ang));
      const float n = 0.5f * sinf(3 * ang + p1) + 0.3f * sinf(7 * ang + p2) + 0.2f * sinf(13 * ang + p3);
      const float thick = 9.0f + 9.0f * bottom + 5.0f * n;
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

void WeatherFx::heat_() {
  if (!this->heat_on_)
    return;
  // Струйки поднимаются на ~60 px за 6 с, колышутся и тают; на смену —
  // новые у самого края
  for (int i = 0; i < NHEAT; i++) {
    this->hage_[i] += this->dt_ms_ / 6000.0f;
    if (this->hage_[i] >= 1.0f) {
      this->hage_[i] = 0;
      this->hx_[i] = rnd_(140, 326);
      this->hy_[i] = 440;
    }
    this->hy_[i] -= this->k_ * 0.5f;
    this->hph_[i] += this->k_ * 0.35f;
    const int x = (int) this->hx_[i] - 15, y = (int) this->hy_[i] - 5;
    if (this->heat_spot_[i].on)
      this->invalidate_(this->heat_spot_[i].a);
    this->mark_(this->heat_spot_[i], true, x, y, 31, 11);
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

void WeatherFx::candles_(uint32_t now) {
  if (!this->cnd_on_ || now - this->cnd_ms_ < 70)
    return;
  this->cnd_ms_ = now;
  // Пламя дрожит: высота, наклон и яркость ореола бродят понемногу
  for (int i = 0; i < NCND; i++) {
    this->cnd_fh_[i] = std::min(16.0f, std::max(10.0f, this->cnd_fh_[i] + rnd_(-15, 16) / 10.0f));
    this->cnd_dx_[i] = std::min(1.6f, std::max(-1.6f, this->cnd_dx_[i] * 0.7f + rnd_(-10, 11) / 10.0f));
    this->cnd_glow_[i] = std::min(1.0f, std::max(0.6f, this->cnd_glow_[i] + rnd_(-12, 13) / 100.0f));
    const int x = CND_X[i], top = CND_Y[i] - CND_H[i];
    lv_area_t a = {x - CND_GLOW - 1, top - 16 - CND_GLOW, x + CND_GLOW + 1, top + 18};
    this->invalidate_(a);
  }
}

void WeatherFx::update_root_() {
  show_(this->root_, this->nd_ || this->nf_ || this->ncl_ || this->storm_ || this->stars_on_ || this->sun_on_ ||
                         this->gar_on_ || this->fw_on_ || this->kite_mode_ || this->rainbow_on_ || this->frost_on_ ||
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
      this->invalidate_(PL_AREA);
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

void WeatherFx::meteor_(uint32_t now) {
  if (!this->stars_on_) {
    this->next_met_ = 0;
    return;
  }
  if (!this->met_on_) {
    // В ночи звездопадов метеоры летят каждые несколько секунд
    const bool shower = this->hol_ == HOL_METEORS;
    if (this->next_met_ == 0)
      this->next_met_ = now + (shower ? rnd_(1500, 4000) : rnd_(15000, 45000));
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
    this->next_met_ = now + (this->hol_ == HOL_METEORS ? rnd_(2000, 6000) : rnd_(30000, 90000));
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
  if (this->moon_spot_.on && !changed && std::fabs(ph - this->moon_phase_) < 0.003)
    return;
  if (this->moon_spot_.on)
    this->invalidate_(this->moon_spot_.a);
  this->moon_ev_ = ev;
  this->moon_red_ = red;
  this->moon_r_ = r_moon;
  this->moon_phase_ = (float) ph;
  const int r = r_moon + 8;
  this->mark_(this->moon_spot_, true, MOON_X - r, MOON_Y - r, 2 * r + 1, 2 * r + 1);
}

void WeatherFx::fireworks_(uint32_t now) {
  if (!this->fw_on_)
    return;
  // Новая вспышка раз в 0,35–0,9 с, если есть свободное место
  if ((int32_t) (now - this->next_burst_) >= 0) {
    this->next_burst_ = now + rnd_(350, 900);
    for (auto &b : this->bursts_) {
      if (b.on)
        continue;
      b.on = true;
      b.t0 = now;
      b.x = rnd_(110, 357);
      b.y = rnd_(60, 175);
      b.c = lv_color_hex(this->fw_pal_ == 1   ? FW_TRICOLOR[rnd_(0, 3)]
                         : this->fw_pal_ == 2 ? FW_RED[rnd_(0, 3)]
                                              : FW_COLORS[rnd_(0, 6)]);
      const float sp = rnd_(7, 12) / 100.0f;  // px/мс
      const float a0 = rnd_(0, 628) / 100.0f;
      for (int k = 0; k < NP; k++) {
        const float a = a0 + k * (2.0f * PI_F / NP) + rnd_(-15, 16) / 100.0f;
        const float v = sp * (0.8f + rnd_(0, 40) / 100.0f);
        b.vx[k] = v * cosf(a);
        b.vy[k] = v * sinf(a);
      }
      break;
    }
  }
  // Искры разлетаются, падают под своим весом и гаснут за 1,3 с
  for (int j = 0; j < NB; j++) {
    Burst &b = this->bursts_[j];
    if (!b.on)
      continue;
    const float t = now - b.t0;
    if (t > 1300) {
      for (auto &sp : b.sp)
        this->mark_(sp, false, 0, 0, 1, 1);
      b.on = false;
      continue;
    }
    this->burst_opa_[j] = (lv_opa_t) (255 * (1.0f - t / 1300.0f));
    for (int k = 0; k < NP; k++) {
      const int x = (int) (b.x + b.vx[k] * t) - 2;
      const int y = (int) (b.y + b.vy[k] * t + 0.00006f * t * t) - 2;
      // Гаснет — перерисовать, даже если искра не сдвинулась
      if (b.sp[k].on)
        this->invalidate_(b.sp[k].a);
      this->mark_(b.sp[k], !this->excluded_(x, y, 5, 5), x, y, 5, 5);
    }
  }
}

void WeatherFx::kite_(uint32_t now, float wind) {
  if (!this->kite_mode_)
    return;
  if (!this->kite_on_) {
    if (this->next_kite_ == 0)
      this->next_kite_ = now + (this->kite_mode_ == 2 ? 1000 : rnd_(20000, 60000));
    if ((int32_t) (now - this->next_kite_) < 0)
      return;
    // Летит по ветру, а без ветра — в случайную сторону
    this->kite_dir_ = wind > 0.05f ? 1.0f : (wind < -0.05f ? -1.0f : (rnd_(0, 2) ? 1.0f : -1.0f));
    this->kite_speed_ = (25.0f + std::fabs(wind) * 25.0f + rnd_(0, 10)) / 1000.0f;  // px/мс
    this->kite_x_ = this->kite_dir_ > 0 ? -40.0f : 506.0f;
    this->kite_t0_ = now;
    this->kite_on_ = true;
  }
  const float t = now - this->kite_t0_;
  this->kite_x_ = (this->kite_dir_ > 0 ? -40.0f : 506.0f) + this->kite_dir_ * this->kite_speed_ * t;
  this->kite_y_ = 72.0f + 16.0f * sinf(t * 0.0013f);
  if (this->kite_x_ < -60.0f || this->kite_x_ > 526.0f) {
    this->mark_(this->kite_spot_, false, 0, 0, 1, 1);
    this->kite_on_ = false;
    this->next_kite_ = now + (this->kite_mode_ == 2 ? rnd_(4000, 8000) : rnd_(180000, 480000));
    return;
  }
  // Хвост тянется назад и вниз и треплется на ветру
  float x0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (int j = 0; j < NTAIL; j++) {
    const float tx = this->kite_x_ - this->kite_dir_ * j * 6.0f;
    const float ty = this->kite_y_ + 24.0f + j * 4.5f + 5.0f * sinf(t * 0.009f + j * 0.9f);
    this->kite_tail_[j].x = (lv_value_precise_t) tx;
    this->kite_tail_[j].y = (lv_value_precise_t) ty;
    x0 = std::min(x0, tx);
    x1 = std::max(x1, tx);
    y1 = std::max(y1, ty);
  }
  x0 = std::min(x0, this->kite_x_ - 15.0f);
  x1 = std::max(x1, this->kite_x_ + 15.0f);
  const int bx = (int) x0 - 8, by = (int) this->kite_y_ - 26;
  // Хвост треплется и при той же рамке — перерисовать в любом случае
  if (this->kite_spot_.on)
    this->invalidate_(this->kite_spot_.a);
  this->mark_(this->kite_spot_, true, bx, by, (int) (x1 - x0) + 17, (int) (y1 - by) + 10);
}

void WeatherFx::bstar_(uint32_t now) {
  // Лучи Вифлеемской звезды чуть вытягиваются и укорачиваются, 8 раз в секунду
  if (!this->bstar_on_ || now - this->bstar_ms_ < 120)
    return;
  this->bstar_ms_ = now;
  this->bstar_ph_ += 0.35f;
  if (this->bstar_spot_.on)
    this->invalidate_(this->bstar_spot_.a);
}

void WeatherFx::rocket_(uint32_t now) {
  if (this->hol_ != HOL_COSMOS)
    return;
  if (!this->rk_on_) {
    if (this->next_rk_ == 0)
      this->next_rk_ = now + 2000;
    if ((int32_t) (now - this->next_rk_) < 0)
      return;
    this->rk_on_ = true;
    this->rk_t0_ = now;
    this->rk_puff_ms_ = now;
    for (auto &a : this->puff_age_)
      a = PUFF_MS;
  }
  const uint32_t t = now - this->rk_t0_;
  // Дым: клубы остаются позади, расплываются и тают
  for (auto &a : this->puff_age_)
    a += this->dt_ms_;
  if (t >= RK_MS) {
    bool smoke = false;
    for (float a : this->puff_age_)
      smoke |= a < PUFF_MS;
    if (!smoke) {
      this->mark_(this->rk_spot_, false, 0, 0, 1, 1);
      this->rk_on_ = false;
      this->next_rk_ = now + rnd_(20000, 45000);
      return;
    }
  }
  // Путь — плавная дуга от левого края над строкой погоды к верху справа
  const float u = std::min(t / (float) RK_MS, 1.0f);
  const float x = -40.0f + 546.0f * u;
  const float y = 205.0f - 185.0f * u - 45.0f * sinf(PI_F * u);
  const float dx = 546.0f, dy = -185.0f - 45.0f * PI_F * cosf(PI_F * u);
  if (t < RK_MS) {
    this->rk_x_ = x;
    this->rk_y_ = y;
    this->rk_a_ = atan2f(dy, dx);
    this->rk_flame_ = 8.0f + rnd_(0, 9);
    if (now - this->rk_puff_ms_ > 110) {
      this->rk_puff_ms_ = now;
      int j = 0;
      for (int i = 1; i < NPUFF; i++)
        if (this->puff_age_[i] > this->puff_age_[j])
          j = i;
      this->puff_x_[j] = x - 22.0f * cosf(this->rk_a_);
      this->puff_y_[j] = y - 22.0f * sinf(this->rk_a_);
      this->puff_age_[j] = 0;
    }
  } else {
    this->rk_x_ = -1000;  // ракета улетела, остаётся тающий дым
  }
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  if (t < RK_MS) {
    x0 = this->rk_x_ - 36;
    y0 = this->rk_y_ - 36;
    x1 = this->rk_x_ + 36;
    y1 = this->rk_y_ + 36;
  }
  for (int j = 0; j < NPUFF; j++) {
    if (this->puff_age_[j] >= PUFF_MS)
      continue;
    x0 = std::min(x0, this->puff_x_[j] - 9);
    y0 = std::min(y0, this->puff_y_[j] - 9);
    x1 = std::max(x1, this->puff_x_[j] + 9);
    y1 = std::max(y1, this->puff_y_[j] + 9);
  }
  if (x0 > x1) {
    this->mark_(this->rk_spot_, false, 0, 0, 1, 1);
    return;
  }
  // Пламя мигает, дым тает — перерисовать, даже если рамка та же
  if (this->rk_spot_.on)
    this->invalidate_(this->rk_spot_.a);
  this->mark_(this->rk_spot_, true, (int) x0, (int) y0, (int) (x1 - x0) + 1, (int) (y1 - y0) + 1);
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
    const int sz = i % 2;
    // Летит волной и дёргается от взмахов
    const float y = this->bat_y_[i] + 14.0f * sinf(this->bat_ph_[i]) + 3.0f * sinf(this->bat_ph_[i] * 7.0f);
    const int x = (int) this->bat_x_[i] + this->bat_ox_[sz], yy = (int) y + this->bat_oy_[sz];
    const int w = this->bat_w_[sz], h = this->bat_h_[sz];
    this->mark_(this->bat_spot_[i], !this->excluded_(x, yy, w, h), x, yy, w, h);
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
                  ((p.dim ? 1 : 0) << 13) | ((p.stars ? 1 : 0) << 14) | ((p.rain_style ? 1 : 0) << 15) |
                  ((p.sun ? 1 : 0) << 16) | ((p.garland ? 1 : 0) << 17) | ((p.fireworks ? 1 : 0) << 18) |
                  ((std::min(std::max(p.kite, 0), 3)) << 19) | ((p.rainbow ? 1 : 0) << 21) |
                  ((p.frost ? 1 : 0) << 22) | ((std::min(std::max(p.holiday, 0), 31)) << 23) |
                  ((p.night ? 1 : 0) << 28) | ((p.fireflies ? 1 : 0) << 29) | ((p.matrix ? 1 : 0) << 30);
  this->fw_pal_ = p.fw_palette;
  if (p.full != this->full_) {
    // Часы ушли или вернулись: небо — заново под весь круг или под «шапку»
    this->full_ = p.full;
    g_box = p.full ? BOX_FULL : BOX_TOP;
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
    this->heat_on_ = p.heat;
    for (int i = 0; i < NHEAT; i++) {
      this->hage_[i] = 1.0f - i / (float) NHEAT;  // струйки вразнобой
      this->hx_[i] = rnd_(140, 326);
      this->hy_[i] = 440;
      if (!p.heat)
        this->mark_(this->heat_spot_[i], false, 0, 0, 1, 1);
    }
    this->update_root_();
  }
  if (p.candles != this->cnd_on_) {
    this->cnd_on_ = p.candles;
    for (int i = 0; i < NCND; i++) {
      this->cnd_fh_[i] = 13;
      this->cnd_dx_[i] = 0;
      this->cnd_glow_[i] = 0.8f;
      const int x = CND_X[i], top = CND_Y[i] - CND_H[i];
      this->mark_(this->cnd_spot_[i], p.candles, x - CND_GLOW - 1, top - 16 - CND_GLOW, 2 * CND_GLOW + 3,
                  CND_Y[i] - top + 16 + CND_GLOW + 2);
    }
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

  const bool any = this->nd_ || this->nf_ || this->ncl_ || this->storm_ || this->stars_on_ || this->sun_on_ ||
                   this->gar_on_ || this->fw_on_ || this->kite_mode_ || this->rainbow_on_ || this->frost_on_ ||
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
  this->garland_(now);
  this->fireworks_(now);
  this->kite_(now, wind);
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

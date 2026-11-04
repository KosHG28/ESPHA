#include "critters.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// lv_image_cache_drop — во внутренних заголовках LVGL
#include <lvgl_private.h>

#include "esphome/components/weather_fx/astro.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "sprites.h"

namespace esphome {
namespace critters {

using namespace weather_fx;

// Геометрия — экранные координаты, центр круга 233,233, радиус 233.
// Спрайты хранятся по клетке на пиксель и увеличиваются при показе: кот и его
// вещи — в 6 раз (кот 156×96), мышка — в 4, остальные гости — в 3
static const int K_CAT = 6, K_MOUSE = 4, K_OTHER = 3;
static const int CAT_W = 26 * K_CAT, CAT_H = 16 * K_CAT;
// Низ кота — у самого края круга: там он крупный, но почти не заслоняет
// надписи. Остальные гости ходят чуть выше
static const int CAT_GROUND = 436;
static const int GROUND = 400;
static const int SKY_Y = 60;            // птица летит по верху
static const float CAT_SPEED = 0.22f;   // px/мс
static const float BLACK_SPEED = 0.26f;  // чёрный кот бежит чуть быстрее
static const float BIRD_SPEED = 0.20f;
static const float SNAIL_SPEED = 0.016f;
static const float SNOWMAN_SPEED = 0.04f;
// Ночью кот спит: «Недолго» — минуту, «Подольше» — две, «Долго» — четыре;
// не на странице часов — не больше минуты
static const uint32_t SLEEP_MS[3] = {60000, 120000, 240000};
static const uint32_t OWL_MS = 20000;     // сова сидит 20 секунд
static const uint32_t PUMPKIN_MS = 120000;  // тыква стоит 2 минуты
static const float BUTTERFLY_SPEED = 0.06f;
static const float HEDGEHOG_SPEED = 0.03f;
static const float MOUSE_SPEED = 0.32f;  // мышь чуть быстрее кота
static const int MOUSE_LEAD = 60;        // на сколько мышь впереди кота на старте
static const int OFF_L = -CAT_W - 10, OFF_R = 476;  // за краем экрана
static const int CENTER_X = 233 - CAT_W / 2;
static const uint32_t MEOW_MS = 1600;  // сколько кот сидит и мяукает после касания
// Лёжа кот ниже: почти не заслоняет надписи над ним
static const int CAT_GROUND_LIE = 448;
static const float WALK_SPEED = 0.075f;  // шагом, px/мс
static const int SPOT_DX = 40;           // на сколько кот отходит от середины
// Как часто гости приходят сами, минуты: «Редко», «Иногда», «Часто», «Очень часто»
static const uint16_t NEXT_MIN[4][2] = {{60, 180}, {30, 60}, {10, 20}, {3, 6}};
// Сколько кот пробегает насквозь, а не заходит, из 100: «Недолго», «Подольше», «Долго»
static const int RUN_PCT[3] = {50, 25, 15};
// Сколько длится занятие, мс (сидит, лежит и дремлет — по-разному, идёт —
// пока не дойдёт): по порядку Act
static const uint16_t ACT_MS[] = {0, 4000, 4800, 3600, 11000, 25000, 3600, 8000, 3600,
                                  0, 2600, 4300, 4800, 4600,  6200,  4200, 4200};
// Сколько кот сидит посередине: просто так, ловя снежинку, глядя в небо, у
// принтера, у миски, у кулича, над блинами, провожая чёрного кота, в жару,
// смущаясь в «Романтике», обидевшись и «почёсываясь»
static const uint32_t SIT_MS[12] = {3000, 4200, 4300, 6000, 5200, 5000, 5200, 4600, 6500, 4200, 7000, 3200};

int Critters::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

static inline float fx_sq_(float v) { return v * v; }

static void tick_cb(lv_timer_t *t) { static_cast<Critters *>(lv_timer_get_user_data(t))->tick(); }

void Sprite::set(const lv_image_dsc_t *s, int k) {
  if (s == this->src && k == this->k)
    return;
  this->src = s;
  this->k = k;
  if (s == nullptr || this->obj == nullptr || k < 1)
    return;
  const int sw = s->header.w, sh = s->header.h;
  // Где на кадре рисунок: прозрачные поля не нужны
  const uint8_t *scol = s->data, *salp = s->data + (size_t) sw * sh * 2;
  int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
  for (int y = 0; y < sh; y++) {
    for (int x = 0; x < sw; x++) {
      if (salp[y * sw + x]) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = y;
      }
    }
  }
  if (x1 < 0)
    x0 = y0 = x1 = y1 = 0;  // пустой — одна прозрачная клетка
  const int cw = x1 - x0 + 1, ch = y1 - y0 + 1, w = cw * k, h = ch * k;
  const size_t need = (size_t) w * h * 3;
  const int n = this->cur ^ 1;
  if (this->cap[n] < need) {
    if (this->buf[n])
      lv_free(this->buf[n]);
    this->buf[n] = static_cast<uint8_t *>(lv_malloc(need));
    this->cap[n] = this->buf[n] ? need : 0;
    if (!this->buf[n]) {
      this->src = nullptr;
      return;
    }
  }
  // RGB565A8: сначала плоскость цвета (2 байта на пиксель), потом прозрачность
  // (байт на пиксель). Строку клеток растягиваем по горизонтали, потом
  // копируем её k раз вниз
  uint8_t *col = this->buf[n], *alp = col + (size_t) w * h * 2;
  for (int y = 0; y < ch; y++) {
    uint8_t *row = col + (size_t) y * k * w * 2;
    uint8_t *arow = alp + (size_t) y * k * w;
    for (int x = 0; x < cw; x++) {
      const int i0 = (y + y0) * sw + x + x0;
      const uint8_t c0 = scol[i0 * 2], c1 = scol[i0 * 2 + 1], a = salp[i0];
      for (int i = 0; i < k; i++) {
        row[(x * k + i) * 2] = c0;
        row[(x * k + i) * 2 + 1] = c1;
        arow[x * k + i] = a;
      }
    }
    for (int r = 1; r < k; r++) {
      memcpy(row + (size_t) r * w * 2, row, (size_t) w * 2);
      memcpy(arow + (size_t) r * w, arow, (size_t) w);
    }
  }
  lv_image_dsc_t &d = this->dsc[n];
  d.header = s->header;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * 2;
  d.data_size = need;
  d.data = this->buf[n];
  // Тот же адрес описания мог попасть в кэш картинок LVGL с прошлым кадром
  lv_image_cache_drop(&d);
  lv_image_set_src(this->obj, &d);
  this->cur = n;
  this->ox = x0 * k;
  this->oy = y0 * k;
}

void Sprite::show(bool v) {
  if (!this->obj)
    return;
  if (v)
    lv_obj_clear_flag(this->obj, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(this->obj, LV_OBJ_FLAG_HIDDEN);
}

void Critters::bind(lv_obj_t *img, lv_obj_t *zzz, lv_obj_t *hat, lv_obj_t *item) {
  this->img_.obj = img;
  this->zzz_ = zzz;
  this->hat_.obj = hat;
  this->item_.obj = item;

  // Первый гость — как обычно, по настройке «Гости приходят»
  this->next_ = millis() + this->interval_();
  // Кадр — в таймере LVGL, в одном проходе с перерисовкой экрана (как у
  // погодного фона): движение ровное, без лишних и пропущенных шагов
  lv_timer_create(tick_cb, 33, this);
}

void Critters::set_festive(bool festive) {
  if (festive && !this->festive_ && this->show_ == SHOW_NONE) {
    // Праздник начался — кот придёт поздравить в ближайшие минуты
    const uint32_t soon = millis() + rnd_(2, 10) * 60000u;
    if ((int32_t) (this->next_ - soon) > 0)
      this->next_ = soon;
  }
  this->festive_ = festive;
}

void Critters::place_(const lv_image_dsc_t *img, int x, int y, int k) {
  // Картинка пересобирается, только если она правда другая
  this->img_.set(img, k);
  this->img_.pos(x, y);
  this->hat_.show(false);
}

enum At { AT_HAT, AT_HEAD, AT_BACK, AT_NECK };

// Что сейчас на коте и куда это надевается (at): в дождь — зонтик, в праздник
// — своя вещь: пилотка, корона, шлем космонавта, ранец, шапка Деда Мороза или
// колпак; в мороз — шарф кончиком назад (какой стороной смотрит кот на кадре
// img — по точке шеи). nullptr — ничего
const lv_image_dsc_t *Critters::prop_(const lv_image_dsc_t *img, int *at) const {
  *at = AT_HAT;
  if (this->weather_ == 1)
    return &spr_cat_umbrella;
  switch (this->holiday_) {
    case HOL_FEB23:
    case HOL_VICTORY:
      return &spr_cat_pilotka;
    case HOL_CAT_DAY:
      return &spr_cat_crown;
    case HOL_COSMOS:
      *at = AT_HEAD;
      return &spr_cat_helmet;
    case HOL_SEPT1:
      *at = AT_BACK;
      return &spr_cat_backpack;
    default:
      break;
  }
  if (this->new_year_ || this->holiday_ == HOL_NEW_YEAR || this->holiday_ == HOL_CHRISTMAS)
    return &spr_cat_santa;
  if (this->birthday_ || this->holiday_ == HOL_BIRTHDAY || this->holiday_ == HOL_MAR8 ||
      this->holiday_ == HOL_CHILDREN || this->holiday_ == HOL_RUSSIA)
    return &spr_cat_hat;
  if (this->climate_ != 2)
    return nullptr;
  bool left = false;
  for (const auto &c : CAT_ANCHORS)
    if (c.img == img)
      left = c.neck_x < 26;
  *at = AT_NECK;
  return left ? &spr_cat_scarf_l : &spr_cat_scarf_r;
}

void Critters::place_cat_(const lv_image_dsc_t *img, int x, int y) {
  this->img_.set(img, K_CAT);
  this->img_.pos(x, y);
  if (!this->hat_.obj)
    return;
  int at;
  const lv_image_dsc_t *prop = this->prop_(img, &at);
  const CatAnchor *a = nullptr;
  if (prop)
    for (const auto &c : CAT_ANCHORS)
      if (c.img == img)
        a = &c;
  if (!a) {
    // Вещи нет — или на этом кадре её некуда надеть (кот валяется на спине)
    this->hat_.show(false);
    return;
  }
  this->hat_.set(prop, K_CAT);
  const int pw = this->hat_.w(), ph = this->hat_.h();
  int px, py;
  if (at == AT_HAT) {
    px = x + a->hat_x * K_CAT / 2 - pw / 2;
    py = y + a->hat_y * K_CAT / 2 - ph;
  } else if (at == AT_HEAD) {
    px = x + a->head_x * K_CAT / 2 - pw / 2;
    py = y + a->head_y * K_CAT / 2 - ph / 2;
  } else if (at == AT_BACK) {
    px = x + a->back_x * K_CAT / 2 - pw / 2;
    py = y + a->back_y * K_CAT / 2 - ph / 2;
  } else {
    const int side = prop == &spr_cat_scarf_l ? 1 : 0;
    px = x + (a->neck_x - SCARF_AT[side][0]) * K_CAT / 2;
    py = y + (a->neck_y - SCARF_AT[side][1]) * K_CAT / 2;
  }
  this->hat_.pos(px, py);
  this->hat_.show(true);
}

void Critters::say_(const char *text) {
  // Надпись над котом; nullptr — убрать. Меняем, только если правда другая
  if (!this->zzz_ || text == this->said_)
    return;
  this->said_ = text;
  if (!text) {
    lv_obj_add_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(this->zzz_, text);
  lv_obj_set_pos(this->zzz_, (int) this->x_ + CAT_W / 2 - 40, (int) this->y_ - 48);
  lv_obj_clear_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
}

void Critters::hush_() {
  // Убрать надпись — и «мяу», и «z z z»
  if (this->zzz_)
    lv_obj_add_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
  this->said_ = nullptr;
  this->zzz_f_ = -1;
}

void Critters::snore_(uint32_t t) {
  // «z», «z z», «z z z» по кругу, чуть поднимаясь
  if (!this->zzz_)
    return;
  static const char *Z[3] = {"z", "z z", "z z z"};
  const int f = (t / 800) % 3;
  if (f == this->zzz_f_)
    return;
  this->zzz_f_ = f;
  this->said_ = nullptr;
  lv_label_set_text(this->zzz_, Z[f]);
  lv_obj_set_pos(this->zzz_, (int) this->x_ + 19 * K_CAT, (int) this->y_ - 20 - f * 5);
  lv_obj_clear_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
}

void Critters::start_drink() {
  this->start(SHOW_CAT_VISIT);
  this->variant_ = VAR_DRINK;
}

void Critters::start_romance() {
  this->start(SHOW_CAT_VISIT);
  this->variant_ = VAR_ROMANCE;
}

void Critters::start_rude() {
  this->start(SHOW_CAT_VISIT);
  this->variant_ = VAR_RUDE;
}

void Critters::bind_kittens(lv_obj_t *parent) {
  for (auto &k : this->kit_) {
    k.obj = lv_image_create(parent);
    lv_obj_add_flag(k.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(k.obj, LV_OBJ_FLAG_CLICKABLE);
  }
}

void Critters::kittens(bool on) {
  this->kit_on_ = on;
  this->kit_t0_ = millis();
  for (int i = 0; i < NKIT; i++) {
    this->kit_f_[i] = -1;
    this->kit_[i].show(on);
  }
  if (on)
    this->kittens_frame_(this->kit_t0_);
}

void Critters::kittens_frame_(uint32_t now) {
  // Рыжий, серый и белый котята сидят рядком, моргают невпопад и чуть
  // подпрыгивают. Рыжий смотрит вправо, остальные — влево, на него
  static const int K_KIT = 4;
  static const int KX[NKIT] = {233 - 165, 233 - 52, 233 + 61}, KY = 233 - 8;
  const uint32_t t = now - this->kit_t0_;
  for (int i = 0; i < NKIT; i++) {
    Sprite &k = this->kit_[i];
    if (!k.obj)
      continue;
    const uint32_t ph = (t + i * 700) % 2300;
    const bool blink = ph < 160;
    const int hop = ((t + i * 400) % 1600) < 180 ? 6 : 0;
    const int f = (blink ? 1 : 0) | (hop ? 2 : 0);
    if (f == this->kit_f_[i])
      continue;
    this->kit_f_[i] = f;
    const lv_image_dsc_t *img;
    if (i == 0)
      img = blink ? &spr_cat_blink_r : &spr_cat_sit_r;
    else if (i == 1)
      img = blink ? &spr_kit_gray_blink : &spr_kit_gray;
    else
      img = blink ? &spr_kit_white_blink : &spr_kit_white;
    k.set(img, K_KIT);
    k.pos(KX[i], KY - hop);
  }
}

void Critters::start_printer_visit() {
  this->start(SHOW_CAT_VISIT);
  this->variant_ = VAR_PRINTER;
}

void Critters::stop_() {
  this->show_ = SHOW_NONE;
  this->img_.show(false);
  if (this->zzz_)
    lv_obj_add_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
  this->hat_.show(false);
  this->item_.show(false);
  this->said_ = nullptr;
  // Следующий — по настройке «Гости приходят», в праздник — хотя бы раз в
  // 30–60 минут
  this->next_ = millis() + this->interval_();
}

uint32_t Critters::interval_() const {
  const uint16_t *m = NEXT_MIN[this->freq_];
  uint32_t ms = rnd_(m[0], m[1] + 1) * 60000u;
  if (this->festive_)
    ms = std::min(ms, (uint32_t) rnd_(30, 61) * 60000u);
  return ms;
}

void Critters::set_prefs(int freq, int stay) {
  freq = std::min(std::max(freq, 0), 3);
  this->stay_ = std::min(std::max(stay, 0), 2);
  if (freq == this->freq_)
    return;
  // Сменили, как часто приходят гости, — следующий по новой настройке
  this->freq_ = freq;
  if (this->show_ == SHOW_NONE)
    this->next_ = millis() + this->interval_();
}

void Critters::start_cat() {
  // Снег — кот садится ловить снежинку (чаще всего), ясная ночь — иногда
  // смотрит на падающую звезду. В праздники — свои сценки. Иначе просто
  // пробегает или заходит посидеть
  const int r = rnd_(0, 10);
  if (this->holiday_ == HOL_FRIDAY13 && r < 7) {
    this->start(SHOW_BLACK_CAT);
  } else if (this->holiday_ == HOL_EASTER && r < 7) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_KULICH;
  } else if (this->holiday_ == HOL_MASLENITSA && r < 7) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_PANCAKES;
  } else if (this->climate_ == 1 && r < 6) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_HOT;
  } else if (this->weather_ == 2 && r < 7) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_CATCH;
  } else if (this->stars_ && this->weather_ == 0 && rnd_(0, 2)) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_STARS;
  } else {
    this->start(rnd_(0, 100) < RUN_PCT[this->stay_] ? SHOW_CAT_RUN : SHOW_CAT_VISIT);
  }
}

void Critters::start_holiday_scene() {
  switch (this->holiday_) {
    case HOL_FRIDAY13:
      this->start(SHOW_BLACK_CAT);
      break;
    case HOL_EASTER:
      this->start(SHOW_CAT_VISIT);
      this->variant_ = VAR_KULICH;
      break;
    case HOL_MASLENITSA:
      this->start(SHOW_CAT_VISIT);
      this->variant_ = VAR_PANCAKES;
      break;
    case HOL_HALLOWEEN:
      this->start(SHOW_PUMPKIN);
      break;
    case HOL_APRIL1:
      this->start(SHOW_CAT_RUN);
      break;
    default:
      // Кот приходит посидеть — видно его праздничную вещь
      this->start(SHOW_CAT_VISIT);
      break;
  }
}

void Critters::poke() {
  if (!this->img_.obj)
    return;
  // Застукали обиженного кота — он делает вид, что просто чесался
  if (this->show_ == SHOW_CAT_VISIT && this->stage_ == 1 && this->variant_ == VAR_RUDE) {
    this->variant_ = VAR_SCRATCH;
    this->stage_t0_ = millis();
    this->said_ = nullptr;
    return;
  }
  const bool cat = this->show_ == SHOW_CAT_RUN || this->show_ == SHOW_CAT_VISIT || this->show_ == SHOW_CAT_SLEEP;
  // Уже мяукает или ещё не выбежал на экран — не замечает
  if (!cat || (this->show_ == SHOW_CAT_VISIT && this->stage_ == 3))
    return;
  if (this->x_ < -CAT_W / 2 || this->x_ > 466 - CAT_W / 2)
    return;
  if (this->show_ == SHOW_CAT_SLEEP)
    this->right_ = rnd_(0, 2);  // проснулся — убежит в случайную сторону
  if (this->show_ == SHOW_CAT_VISIT && this->stage_ == 1) {
    // Лежал или крутился за хвостом — садится как обычно
    if (this->nplan_ && this->plan_[this->iplan_] == ACT_SPIN)
      this->right_ = this->spin_r_;
    this->y_ = this->ground_y_(false) - CAT_H;
  }
  this->show_ = SHOW_CAT_VISIT;
  this->stage_ = 3;
  this->stage_t0_ = millis();
  this->item_.show(false);
  this->said_ = nullptr;
  this->say_("мяу!");
  this->zzz_f_ = -1;
}

void Critters::start(Show show) {
  if (!this->img_.obj)
    return;
  this->show_ = show;
  this->t0_ = millis();
  this->stage_ = 0;
  this->stage_t0_ = this->t0_;
  this->right_ = rnd_(0, 2);
  this->zzz_f_ = -1;
  this->said_ = nullptr;
  this->variant_ = VAR_PLAIN;
  this->meteor_sent_ = false;
  this->item_.show(false);
  // Прежний гость мог уйти посреди «z z z» или «мяу!»
  if (this->zzz_)
    lv_obj_add_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
  switch (show) {
    case SHOW_BIRD:
      this->y_ = SKY_Y + rnd_(0, 30);
      break;
    case SHOW_SNAIL:
      if (this->winter_)
        this->show_ = SHOW_SNOWMAN;
      this->right_ = false;  // улитка нарисована ползущей влево
      this->y_ = this->show_ == SHOW_SNOWMAN ? GROUND - 22 * K_OTHER : GROUND - 12 * K_OTHER;
      break;
    case SHOW_SNOWMAN:
      this->right_ = false;
      this->y_ = GROUND - 22 * K_OTHER;
      break;
    case SHOW_HEDGEHOG:
      this->right_ = false;  // ёжик нарисован идущим влево
      this->y_ = GROUND - 13 * K_OTHER;
      break;
    case SHOW_BUTTERFLY:
      this->y_ = 250 + rnd_(0, 60);
      break;
    case SHOW_OWL:
      // На ветке у левого края круга, чуть выше середины
      this->x_ = 20;
      this->y_ = 170;
      break;
    case SHOW_PUMPKIN:
      this->x_ = 233 - 8 * K_OTHER;
      this->y_ = GROUND - 14 * K_OTHER;
      break;
    case SHOW_BLACK_CAT:
      // Рыжий кот приходит посидеть, а перед ним пробегает чёрный
      this->show_ = SHOW_CAT_VISIT;
      this->variant_ = VAR_BLACK;
      this->y_ = CAT_GROUND - CAT_H;
      break;
    case SHOW_CAT_SLEEP:
      this->x_ = CENTER_X;
      this->y_ = CAT_GROUND - CAT_H;
      break;
    default:
      // Кот и погоня за мышкой
      this->y_ = CAT_GROUND - CAT_H;
      break;
  }
  if (show != SHOW_CAT_SLEEP && show != SHOW_OWL && show != SHOW_PUMPKIN)
    this->x_ = this->right_ ? OFF_L : OFF_R;
  // Надолго кот чаще приходит шагом, не спеша
  this->walk_ = this->stay_ > 0 && rnd_(0, 10) < 6;
  this->came_right_ = this->right_;
  this->nplan_ = this->iplan_ = 0;
  this->img_.show(true);
}

const lv_image_dsc_t *Critters::run_frame_(int f) const {
  static const lv_image_dsc_t *R[4] = {&spr_cat_run_r0, &spr_cat_run_r1, &spr_cat_run_r2, &spr_cat_run_r3};
  static const lv_image_dsc_t *L[4] = {&spr_cat_run_l0, &spr_cat_run_l1, &spr_cat_run_l2, &spr_cat_run_l3};
  // 1 апреля кот бегает задом наперёд
  const bool face_right = this->holiday_ == HOL_APRIL1 ? !this->right_ : this->right_;
  return face_right ? R[f] : L[f];
}

const lv_image_dsc_t *Critters::walk_frame_(int f) const {
  static const lv_image_dsc_t *R[4] = {&spr_cat_walk_r0, &spr_cat_walk_r1, &spr_cat_walk_r2, &spr_cat_walk_r3};
  static const lv_image_dsc_t *L[4] = {&spr_cat_walk_l0, &spr_cat_walk_l1, &spr_cat_walk_l2, &spr_cat_walk_l3};
  const bool face_right = this->holiday_ == HOL_APRIL1 ? !this->right_ : this->right_;
  return face_right ? R[f] : L[f];
}

void Critters::item_at_bowl_(const lv_image_dsc_t *img) {
  // Вещь стоит на земле у морды кота: миска, кулич, блины
  if (!this->item_.obj)
    return;
  this->item_.set(img, K_CAT);
  const int r = this->right_ ? 0 : 1;
  const int bx = (int) this->x_ + BOWL_AT[r][0] * K_CAT / 2, by = (int) this->y_ + BOWL_AT[r][1] * K_CAT / 2;
  this->item_.pos(bx - this->item_.w() / 2, by - this->item_.h());
  this->item_.show(true);
}

void Critters::sit_(uint32_t st) {
  const bool r = this->right_;
  auto pick = [r](const lv_image_dsc_t &right, const lv_image_dsc_t &left) { return r ? &right : &left; };
  const lv_image_dsc_t *sit = pick(spr_cat_sit_r, spr_cat_sit_l), *blink = pick(spr_cat_blink_r, spr_cat_blink_l);
  const lv_image_dsc_t *look = pick(spr_cat_look_r, spr_cat_look_l), *paw = pick(spr_cat_paw_r, spr_cat_paw_l);
  const lv_image_dsc_t *img = sit;
  switch (this->variant_) {
    case VAR_DRINK:
    case VAR_PANCAKES: {
      // Налили воды: у морды миска, кот пригнулся и лакает, потом садится
      // и мурчит. На Масленицу так же ест блины
      this->item_at_bowl_(this->variant_ == VAR_DRINK ? &spr_cat_bowl : &spr_cat_pancakes);
      if (st > 200 && st < 4000) {
        const bool lap = (st / 330) % 2;
        img = r ? (lap ? &spr_cat_drink_r1 : &spr_cat_drink_r0) : (lap ? &spr_cat_drink_l1 : &spr_cat_drink_l0);
      } else if (st > 4600 && st < 4750) {
        img = blink;
      }
      this->say_(st > 4100 ? (this->variant_ == VAR_DRINK ? "мур" : "ням!") : nullptr);
      break;
    }
    case VAR_KULICH: {
      // Пасха: рядом кулич, кот любуется им, моргает и мурчит
      this->item_at_bowl_(&spr_cat_kulich);
      if (st > 600 && st < 1800)
        img = look;
      else if ((st > 2200 && st < 2350) || (st > 3900 && st < 4050))
        img = blink;
      this->say_(st > 2500 && st < 4500 ? "мур" : nullptr);
      break;
    }
    case VAR_BLACK: {
      // Пятница, 13-е: перед котом пробегает чёрный кот — с той стороны,
      // куда рыжий смотрит. Рыжий провожает его взглядом
      const uint32_t T0 = 400;
      if (st > T0 && this->item_.obj) {
        static const lv_image_dsc_t *BLK_R[4] = {&spr_black_run_r0, &spr_black_run_r1, &spr_black_run_r2,
                                              &spr_black_run_r3};
        static const lv_image_dsc_t *BLK_L[4] = {&spr_black_run_l0, &spr_black_run_l1, &spr_black_run_l2,
                                              &spr_black_run_l3};
        // Бежит навстречу взгляду рыжего: смотрит вправо — чёрный бежит справа налево
        const float d = BLACK_SPEED * (st - T0);
        this->bx_ = r ? OFF_R - d : OFF_L + d;
        const int f = (st / 80) % 4;
        const bool on = r ? this->bx_ > OFF_L : this->bx_ < OFF_R;
        if (on) {
          this->item_.set(r ? BLK_L[f] : BLK_R[f], K_CAT);
          this->item_.pos((int) this->bx_, (int) this->y_);
          this->item_.show(true);
        } else {
          this->item_.show(false);
        }
        const bool near = std::fabs(this->bx_ - this->x_) < CAT_W * 1.5f;
        img = near ? look : sit;
        if (!on && st % 1600 < 150)
          img = blink;
        this->say_(near ? "!" : nullptr);
      }
      break;
    }
    case VAR_ROMANCE: {
      // Прибежал, увидел, что происходит, смутился и тактично уходит
      if (st > 300 && st < 1500)
        img = look;
      else if ((st > 1700 && st < 1850) || (st > 2300 && st < 2450))
        img = blink;
      this->say_(st > 500 && st < 2200 ? "ой…" : (st > 2600 ? "…ухожу" : nullptr));
      break;
    }
    case VAR_RUDE: {
      // Сначала молча смотрит, потом поднимает лапу — мозаика «шевелится»
      if (st > 1400 && st < 6000) {
        const bool v = (st / 250) % 2;
        img = r ? (v ? &spr_cat_rude1_r : &spr_cat_rude0_r) : (v ? &spr_cat_rude1_l : &spr_cat_rude0_l);
      } else if (st < 1400) {
        img = blink;
      }
      this->say_(st > 300 && st < 1300 ? "хм." : (st > 1600 && st < 5800 ? "вот." : nullptr));
      break;
    }
    case VAR_SCRATCH: {
      // Чешет за ухом, как ни в чём не бывало
      if (st < 2400)
        img = (st / 180) % 2 ? paw : look;
      else if (st > 2600 && st < 2750)
        img = blink;
      this->say_(st > 200 && st < 2800 ? "я чешусь" : nullptr);
      break;
    }
    case VAR_HOT: {
      // Жара: кот ложится пластом, высунув язык, и вздыхает
      img = r ? &spr_cat_flat_r : &spr_cat_flat_l;
      this->say_(st > 900 && st < 4200 ? "жарко…" : nullptr);
      break;
    }
    case VAR_CATCH: {
      // Снежинка падает, покачиваясь, прямо на лапу; кот следит за ней,
      // поднимает лапу — поймал! — и мяукает
      if (this->item_.obj)
        this->item_.set(&spr_flake, K_MOUSE);
      const int tx = (int) this->x_ + PAW_TIP[r ? 0 : 1][0] * K_CAT / 2;
      const int ty = (int) this->y_ + PAW_TIP[r ? 0 : 1][1] * K_CAT / 2;
      const int fw = this->item_.w(), fh = this->item_.h();
      if (st > 200 && st < 1750 && this->item_.obj) {
        const float k = (st - 200) / 1500.0f;
        const int fx = tx + (int) (14.0f * sinf(k * 9.0f) * (1.0f - k)), fy = ty - (int) (180.0f * (1.0f - k));
        this->item_.pos(fx - fw / 2, fy - fh / 2);
        this->item_.show(true);
      } else {
        this->item_.show(false);
      }
      if (st > 200 && st < 1650)
        img = look;
      else if (st >= 1650 && st < 2600)
        img = paw;
      else if (st > 3200 && st < 3350)
        img = blink;
      this->say_(st > 1800 && st < 3400 ? "мяу!" : nullptr);
      break;
    }
    case VAR_STARS: {
      // Смотрит в небо — там пролетает метеор
      if (st > 700 && !this->meteor_sent_) {
        this->meteor_sent_ = true;
        this->meteor_req_ = true;
      }
      if (st > 700 && st < 3300)
        img = look;
      else if (st > 3500 && st < 3650)
        img = blink;
      this->say_(st > 900 && st < 1900 ? "!" : nullptr);
      break;
    }
    case VAR_PRINTER: {
      // Печать закончилась: кот пришёл посмотреть, долго сидит и мурчит
      if ((st > 1500 && st < 1650) || (st > 4000 && st < 4150))
        img = blink;
      else if (st > 600 && st < 1400)
        img = look;
      this->say_(st > 3000 && st < 5500 ? "мур" : nullptr);
      break;
    }
    default:
      // Сидит 3 секунды и дважды моргает
      if ((st > 900 && st < 1050) || (st > 2000 && st < 2150))
        img = blink;
      break;
  }
  this->place_cat_(img, (int) this->x_, (int) this->y_);
}

// ---- Визит: из чего он складывается ----

void Critters::item_at_(const lv_image_dsc_t *img, int k, int cx, int cy) {
  // Вещь рядом с котом — серединой в точку (cx, cy)
  if (!this->item_.obj)
    return;
  this->item_.set(img, k);
  this->item_.pos(cx - this->item_.w() / 2, cy - this->item_.h() / 2);
  this->item_.show(true);
}

int Critters::ground_y_(bool lying) const {
  // Где земля под котом (низ кадра): у края круга — выше, чтобы кот не
  // обрезался. Лёжа кот ниже и почти не заслоняет надписи над ним
  auto bottom = [](float x) {
    const float d = x - 233.0f;
    return 233.0f + sqrtf(std::max(0.0f, 233.0f * 233.0f - d * d));
  };
  const float b = std::min(bottom(this->x_ + 2 * K_CAT), bottom(this->x_ + 25 * K_CAT)) - 4.0f;
  return (int) std::min((float) (lying ? CAT_GROUND_LIE : CAT_GROUND), b);
}

int Critters::reactions_(Act *out) const {
  // На что на небе кот приходит посмотреть: лепестки, пух, листья, ночью —
  // светлячки и зарницы, гало и радуга
  int n = 0;
  if (this->sky_ & SKY_PETALS)
    out[n++] = ACT_PETAL;
  if (this->sky_ & SKY_FLUFF)
    out[n++] = ACT_FLUFF;
  if ((this->sky_ & SKY_LEAVES) && this->weather_ != 1)
    out[n++] = ACT_LEAF;
  if ((this->sky_ & SKY_FIREFLIES) && this->night_)
    out[n++] = ACT_FIREFLY;
  if ((this->sky_ & SKY_HEAT_LIGHTNING) && this->night_)
    out[n++] = ACT_FLASH;
  if (this->sky_ & (SKY_HALO | SKY_RAINBOW))
    out[n++] = ACT_GAZE;
  return n;
}

Critters::Act Critters::pick_act_(Act prev) const {
  // Чем заняться дальше — по погоде и времени суток, не то же, что сейчас.
  // В жару — только лежать; в дождь (под зонтиком) не играет и не валяется,
  // как и в снег; с праздничной вещью не валяется на спине; ночью больше
  // лежит и дремлет; вздремнуть днём — только если кот гостит долго
  struct W {
    Act a;
    int w;
  } w[20];
  int n = 0;
  auto add = [&](Act a, int k) {
    if (a != prev && n < 20)
      w[n++] = {a, k};
  };
  const bool night = this->night_, rain = this->weather_ == 1, snow = this->weather_ == 2;
  int at;
  const bool prop = this->prop_(&spr_cat_sit_r, &at) != nullptr;
  if (this->climate_ == 1) {
    add(ACT_LOAF, 3);
    add(ACT_YAWN, 1);
    add(ACT_NAP, 2);
  } else {
    add(ACT_SIT, 2);
    add(ACT_GROOM, 3);
    add(ACT_YAWN, night ? 3 : 2);
    add(ACT_LOAF, night ? 4 : 3);
    add(ACT_TAIL, night ? 1 : 2);
    add(ACT_WALK, night ? 1 : 2);
    if ((night || this->stay_ == 2) && !rain && !snow)
      add(ACT_NAP, night ? 3 : 1);
    if (!night && !rain) {
      add(ACT_PLAY, 3);
      add(ACT_SPIN, 1);
      if (!prop && !snow)
        add(ACT_ROLL, 2);
    }
    Act r[6];
    const int nr = this->reactions_(r);
    for (int i = 0; i < nr; i++)
      add(r[i], 2);
  }
  int sum = 0;
  for (int i = 0; i < n; i++)
    sum += w[i].w;
  if (sum <= 0)
    return ACT_SIT;
  int k = rnd_(0, sum);
  for (int i = 0; i < n; i++) {
    if (k < w[i].w)
      return w[i].a;
    k -= w[i].w;
  }
  return ACT_SIT;
}

void Critters::plan_visit_() {
  // Сначала — сценка (миска, кулич, снежинка…) или то, на что кот пришёл
  // посмотреть на небе; потом, если кот гостит подольше, — ещё несколько
  // занятий. Обиделся, смутился или провожает чёрного кота — сразу уходит;
  // не на странице часов — тоже
  this->nplan_ = 0;
  this->iplan_ = 0;
  const Variant v = this->variant_;
  if (v != VAR_PLAIN) {
    this->plan_[this->nplan_++] = ACT_SCENE;
  } else {
    Act r[6];
    const int nr = this->reactions_(r);
    this->plan_[this->nplan_++] = nr && rnd_(0, 100) < 85 ? r[rnd_(0, nr)] : this->pick_act_(ACT_SCENE);
  }
  if (this->page_ != 0 || v == VAR_ROMANCE || v == VAR_RUDE || v == VAR_SCRATCH || v == VAR_BLACK)
    return;
  const int extra = this->stay_ == 0 ? 0 : (this->stay_ == 1 ? rnd_(2, 5) : rnd_(5, 9));
  for (int i = 0; i < extra && this->nplan_ < NACT; i++) {
    const Act a = this->pick_act_(this->plan_[this->nplan_ - 1]);
    this->plan_[this->nplan_++] = a;
  }
}

void Critters::begin_act_(uint32_t now) {
  this->stage_t0_ = now;
  this->act_seed_ = rnd_(0, 1000);
  this->flash_sent_ = false;
  this->item_.show(false);
  this->hush_();
  const Act a = this->plan_[this->iplan_];
  this->act_ms_ = ACT_MS[a];
  if (a == ACT_SIT) {
    this->act_ms_ = rnd_(3000, 5000);
  } else if (a == ACT_LOAF) {
    this->act_ms_ = rnd_(8000, 15000);
  } else if (a == ACT_NAP) {
    this->act_ms_ = this->stay_ == 2 ? rnd_(25000, 45000) : rnd_(15000, 25000);
  } else if (a == ACT_SPIN) {
    this->spin_r_ = this->right_;
  } else if (a == ACT_WALK) {
    // На шаг в сторону или обратно к середине
    const float opts[3] = {(float) (CENTER_X - SPOT_DX), (float) CENTER_X, (float) (CENTER_X + SPOT_DX)};
    int i = rnd_(0, 3);
    if (std::fabs(opts[i] - this->x_) < 10)
      i = (i + 1 + rnd_(0, 2)) % 3;
    this->tx_ = opts[i];
    this->right_ = this->tx_ > this->x_;
  }
}

void Critters::leave_(uint32_t now) {
  // Разворачивается и уходит туда, откуда пришёл
  this->say_(nullptr);
  this->hush_();
  this->item_.show(false);
  this->right_ = !this->came_right_;
  this->y_ = CAT_GROUND - CAT_H;
  this->stage_ = 2;
  this->stage_t0_ = now;
}

bool Critters::act_(Act a, uint32_t st, uint32_t dt) {
  const bool r = this->right_;
  const int ri = r ? 0 : 1;
  const float dir = r ? 1.0f : -1.0f;
  auto pick = [r](const lv_image_dsc_t &right, const lv_image_dsc_t &left) { return r ? &right : &left; };
  // Точка кадра в полуклетках — на экран
  auto px = [this](int hx) { return (int) this->x_ + hx * K_CAT / 2; };
  auto py = [this](int hy) { return (int) this->y_ + hy * K_CAT / 2; };
  const lv_image_dsc_t *sit = pick(spr_cat_sit_r, spr_cat_sit_l), *blink = pick(spr_cat_blink_r, spr_cat_blink_l);
  const lv_image_dsc_t *look = pick(spr_cat_look_r, spr_cat_look_l), *paw = pick(spr_cat_paw_r, spr_cat_paw_l);
  const lv_image_dsc_t *img = sit;
  bool lying = false;
  if (st >= this->act_ms_ && a != ACT_WALK)
    return false;
  switch (a) {
    case ACT_SIT:
      // Сидит, иногда поглядывает вверх, моргает
      if ((this->act_seed_ & 1) && st > 1200 && st < 2300)
        img = look;
      if (st % 1700 > 1550)
        img = blink;
      break;
    case ACT_GROOM: {
      // Лижет лапу, потом трёт ею мордочку
      const lv_image_dsc_t *lick = pick(spr_cat_groom_r0, spr_cat_groom_l0);
      if (st < 2000)
        img = (st / 260) % 2 ? paw : lick;
      else if (st < 3800)
        img = (st / 320) % 2 ? pick(spr_cat_groom_r1, spr_cat_groom_l1) : lick;
      else if (st > 4200 && st < 4350)
        img = blink;
      break;
    }
    case ACT_YAWN:
      // Сладко зевает и потягивается
      if (st > 400 && st < 1600)
        img = pick(spr_cat_yawn_r, spr_cat_yawn_l);
      else if (st >= 1600 && st < 1750)
        img = blink;
      else if (st >= 1750 && st < 3200)
        img = pick(spr_cat_stretch_r, spr_cat_stretch_l);
      break;
    case ACT_LOAF: {
      // Лежит «буханкой», жмурится и мурчит — над головой всплывает сердечко
      lying = true;
      img = st % 2600 > 2420 ? pick(spr_cat_loafb_r, spr_cat_loafb_l) : pick(spr_cat_loaf_r, spr_cat_loaf_l);
      this->say_(st > 2500 && st < 5000 ? "мур" : nullptr);
      if (st > 2500 && st < 4100) {
        const int hx = px(r ? 38 : 14), hy = py(10);
        this->item_at_(&spr_heart, K_MOUSE, hx + (int) (4 * sinf(st * 0.008f)), hy - (int) ((st - 2500) * 0.04f));
      } else {
        this->item_.show(false);
      }
      break;
    }
    case ACT_NAP:
      // Дремлет: «z z z»
      lying = true;
      img = &spr_cat_sleep;
      break;
    case ACT_TAIL:
      // Хвост трубой покачивается
      img = (st / 380) % 2 ? pick(spr_cat_tail_r1, spr_cat_tail_l1) : pick(spr_cat_tail_r0, spr_cat_tail_l0);
      break;
    case ACT_PLAY: {
      // Клубок скатывается по краю круга к коту — кот припал к земле, бьёт
      // лапой; клубок откатывается вверх по краю и скатывается обратно;
      // второй удар — укатился совсем
      const int tipx = px(BAT_TIP[ri][0]), gy = py(BAT_TIP[ri][1]);
      float bx = -1;  // где клубок: на сколько впереди лапы
      if (st < 1400)
        bx = 6 + 114.0f * (1.0f - st / 1400.0f);
      else if (st < 2400)
        bx = 6 + 36.0f * (1.0f - fx_sq_(1.0f - (st - 1400) / 1000.0f));
      else if (st < 2900)
        bx = 42;
      else if (st < 3900)
        bx = 42 - 36.0f * fx_sq_((st - 2900) / 1000.0f);
      else if (st < 5600)
        bx = 6 + 260.0f * (st > 4400 ? st - 4400 : 0) / 1200.0f;
      if (bx >= 0 && this->item_.obj) {
        const lv_image_dsc_t *ball = ((int) (bx / 7) % 2) ? &spr_yarn1 : &spr_yarn0;
        this->item_.set(ball, K_MOUSE);
        // Клубок лежит на краю круга: у края земля выше
        const float bxx = tipx + dir * bx, d = bxx - 233.0f;
        const float edge = 233.0f + sqrtf(std::max(0.0f, 233.0f * 233.0f - d * d)) - 4.0f;
        this->item_at_(ball, K_MOUSE, (int) bxx, (int) std::min((float) gy, edge) - this->item_.h() / 2);
      } else {
        this->item_.show(false);
      }
      const lv_image_dsc_t *crouch =
          (st / 220) % 2 ? pick(spr_cat_crouch_r1, spr_cat_crouch_l1) : pick(spr_cat_crouch_r0, spr_cat_crouch_l0);
      if ((st >= 1400 && st < 1700) || (st >= 4300 && st < 4700))
        img = pick(spr_cat_bat_r, spr_cat_bat_l);
      else if (st < 5000)
        img = crouch;
      else if (st > 7400 && st < 7550)
        img = blink;
      this->say_(st > 5800 && st < 7000 ? "мяу!" : nullptr);
      break;
    }
    case ACT_ROLL:
      // Валяется на спине, болтая лапами
      if (st < 3000) {
        lying = true;
        img = (st / 240) % 2 ? pick(spr_cat_roll_r1, spr_cat_roll_l1) : pick(spr_cat_roll_r0, spr_cat_roll_l0);
      } else if (st > 3200 && st < 3350) {
        img = blink;
      }
      break;
    case ACT_WALK: {
      // Неспешно переходит на другое место и поворачивается к середине
      const float step = WALK_SPEED * dt;
      if (std::fabs(this->tx_ - this->x_) <= step) {
        this->x_ = this->tx_;
        this->right_ = this->x_ <= CENTER_X;
        return false;
      }
      this->x_ += (this->tx_ > this->x_ ? 1.0f : -1.0f) * step;
      img = this->walk_frame_((st / 130) % 4);
      break;
    }
    case ACT_SPIN:
      // Ловит свой хвост — кружится на месте, потом «уф»
      if (st < 2000) {
        this->right_ = (st / 150) % 2 ? !this->spin_r_ : this->spin_r_;
        img = this->run_frame_((st / 75) % 4);
      } else {
        this->right_ = this->spin_r_;
        img = st < 2150 ? (this->right_ ? &spr_cat_blink_r : &spr_cat_blink_l)
                        : (this->right_ ? &spr_cat_sit_r : &spr_cat_sit_l);
      }
      this->say_(st > 2050 && st < 2600 ? "уф" : nullptr);
      break;
    case ACT_PETAL: {
      // Лепесток, кружась, опускается прямо на лапу: кот следит за ним,
      // ловит — мурчит, лепесток соскальзывает на землю
      const int tx = px(PAW_TIP[ri][0]), ty = py(PAW_TIP[ri][1]), gy = py(32) - 8;
      if (st > 200 && st < 1900) {
        const float k = (st - 200) / 1700.0f;
        this->item_at_(&spr_petal, K_MOUSE, tx + (int) (22.0f * sinf(k * 8.0f) * (1.0f - k)),
                       ty - (int) (190.0f * (1.0f - k)));
      } else if (st >= 1900 && st < 2900) {
        this->item_at_(&spr_petal, K_MOUSE, tx, ty - 2);
      } else if (st >= 2900 && st < 3500) {
        const float k = (st - 2900) / 600.0f;
        this->item_at_(&spr_petal, K_MOUSE, tx + (int) (dir * 10 * k), ty + (int) ((gy - ty) * k));
      } else {
        this->item_.show(false);
      }
      if (st > 200 && st < 1850)
        img = look;
      else if (st >= 1850 && st < 2900)
        img = paw;
      else if (st > 3700 && st < 3850)
        img = blink;
      this->say_(st > 2000 && st < 3400 ? "мур" : nullptr);
      break;
    }
    case ACT_FLUFF: {
      // Пушинка подлетает к самому носу — кот косится на неё и… чихает!
      // Пушинку сдувает, кот отмахивается лапой
      const int nx = px(NOSE_AT[ri][0]), ny = py(NOSE_AT[ri][1]);
      if (st > 200 && st < 2200) {
        const float k = (st - 200) / 2000.0f;
        this->item_at_(&spr_fluff, K_MOUSE, nx + (int) (dir * 46 * (1.0f - k) + 10.0f * sinf(k * 6.0f) * (1.0f - k)),
                       ny - (int) (150.0f * (1.0f - k)));
      } else if (st >= 2200 && st < 3400) {
        const float k = (st - 2200) / 1200.0f;
        this->item_at_(&spr_fluff, K_MOUSE, nx + (int) (dir * 170 * k), ny - (int) (130.0f * k * (0.5f + k)));
      } else {
        this->item_.show(false);
      }
      if (st > 200 && st < 2150)
        img = look;
      else if (st >= 2150 && st < 2650)
        img = pick(spr_cat_sneeze_r, spr_cat_sneeze_l);
      else if (st >= 3000 && st < 3700)
        img = (st / 200) % 2 ? paw : sit;
      else if (st > 4300 && st < 4450)
        img = blink;
      this->say_(st > 2250 && st < 3500 ? "апчхи!" : nullptr);
      break;
    }
    case ACT_LEAF: {
      // Лист, кружась, падает перед котом — кот припадает к земле, прыгает,
      // ловит его в воздухе и прижимает лапой: «ам!»
      const int jx = px(JUMP_TIP[ri][0]), jy = py(JUMP_TIP[ri][1]);
      const int bx = px(BAT_TIP[ri][0]), by = py(BAT_TIP[ri][1]) - 8;
      if (st < 2000) {
        const float k = st / 2000.0f;
        this->item_at_(&spr_leaf, K_MOUSE, jx + (int) (26.0f * sinf(k * 7.0f) * (1.0f - k)),
                       jy - (int) (170.0f * (1.0f - k)));
      } else if (st < 2450) {
        this->item_at_(&spr_leaf, K_MOUSE, jx, jy);
      } else if (st < 3500) {
        this->item_at_(&spr_leaf, K_MOUSE, bx, by);
      } else {
        this->item_.show(false);
      }
      if (st < 2000)
        img = (st / 200) % 2 ? pick(spr_cat_crouch_r1, spr_cat_crouch_l1) : pick(spr_cat_crouch_r0, spr_cat_crouch_l0);
      else if (st < 2450)
        img = pick(spr_cat_jump_r, spr_cat_jump_l);
      else if (st < 3500)
        img = pick(spr_cat_bat_r, spr_cat_bat_l);
      else if (st > 3900 && st < 4050)
        img = blink;
      this->say_(st > 2500 && st < 3500 ? "ам!" : nullptr);
      break;
    }
    case ACT_FIREFLY: {
      // Светлячок кружит у головы, мерцая; кот следит за ним, прыгает — не
      // поймал: светлячок улетает вверх. «Ой»
      const int hx = px(r ? 31 : 21), hy = py(9);
      const bool bright = (st / 280) % 3 != 2;
      const lv_image_dsc_t *ff = bright ? &spr_firefly0 : &spr_firefly1;
      if (st < 3900) {
        const float ang = st * 0.0032f;
        this->item_at_(ff, K_MOUSE, hx + (int) (dir * 12 + 58.0f * cosf(ang)),
                       hy - 44 + (int) (22.0f * sinf(ang * 1.3f)));
      } else if (st < 5600) {
        const float ang = 3900 * 0.0032f, t = st - 3900;
        this->item_at_(ff, K_MOUSE, hx + (int) (dir * 12 + 58.0f * cosf(ang) + dir * 0.08f * t),
                       hy - 44 + (int) (22.0f * sinf(ang * 1.3f) - 0.14f * t));
      } else {
        this->item_.show(false);
      }
      if (st < 3800)
        img = (st / 900) % 3 == 2 ? sit : look;
      else if (st < 4250)
        img = pick(spr_cat_jump_r, spr_cat_jump_l);
      else if (st > 5700 && st < 5850)
        img = blink;
      this->say_(st > 4400 && st < 5400 ? "ой" : nullptr);
      break;
    }
    case ACT_FLASH:
      // Насторожился — у горизонта сверкнула зарница: шерсть дыбом, спина
      // дугой; потом успокаивается
      if (st > 600 && !this->flash_sent_) {
        this->flash_sent_ = true;
        this->flash_req_ = true;
      }
      if (st < 650)
        img = look;
      else if (st < 2000)
        img = pick(spr_cat_arch_r, spr_cat_arch_l);
      else if ((st > 2600 && st < 2750) || (st > 3400 && st < 3550))
        img = blink;
      this->say_(st > 700 && st < 1900 ? "!" : (st > 2800 && st < 3700 ? "мяу?" : nullptr));
      break;
    case ACT_GAZE:
      // Любуется небом: гало — «ох…», радуга — «ух ты!»
      if (st > 300 && st < 3300)
        img = look;
      else if (st > 3600 && st < 3750)
        img = blink;
      this->say_(st > 500 && st < 2800 ? ((this->sky_ & SKY_RAINBOW) ? "ух ты!" : "ох…") : nullptr);
      break;
    default:
      return false;
  }
  if (a != ACT_WALK)
    this->y_ = this->ground_y_(lying) - CAT_H;
  else
    this->y_ = this->ground_y_(false) - CAT_H;
  this->place_cat_(img, (int) this->x_, (int) this->y_);
  if (a == ACT_NAP)
    this->snore_(st);
  return true;
}

void Critters::frame(bool can_show, bool night, bool winter) {
  if (!this->img_.obj)
    return;
  this->winter_ = winter;
  const uint32_t now = millis();

  if (this->kit_on_)
    this->kittens_frame_(now);

  if (this->show_ == SHOW_NONE) {
    if ((int32_t) (now - this->next_) < 0)
      return;
    if (this->quiet_) {
      // «Романтика» — гости не беспокоят
      this->next_ = now + 300000u;
      return;
    }
    if (!can_show) {
      // Экран погашен — попробуем через 5 минут
      this->next_ = now + 300000u;
      return;
    }
    Show s;
    int r = rnd_(0, 100);
    if (this->festive_ || this->weather_ == 1)
      r = rnd_(0, 70);  // в праздник и в дождь приходит только кот
    if (night && r < 50) {
      s = SHOW_CAT_SLEEP;
    } else if (r < 80) {
      // Кот — самый частый гость
      this->start_cat();
      return;
    } else if (r < 90) {
      // Ночью вместо птицы — сова, летом днём — иногда бабочка
      const bool summer = this->month_ >= 6 && this->month_ <= 8;
      s = night ? SHOW_OWL : (summer && rnd_(0, 2) ? SHOW_BUTTERFLY : SHOW_BIRD);
    } else {
      // Иногда вместо улитки — погоня за мышью; осенью — ёжик, зимой — снеговик
      const bool autumn = this->month_ >= 9 && this->month_ <= 11;
      s = rnd_(0, 3) == 0 ? SHOW_MOUSE : (autumn ? SHOW_HEDGEHOG : SHOW_SNAIL);
    }
    // На Хэллоуин чаще всего выходит тыква
    if (this->holiday_ == HOL_HALLOWEEN && rnd_(0, 10) < 6)
      s = SHOW_PUMPKIN;
    this->start(s);
    return;
  }

  // Экран выключили совсем — гость уходит
  if (!can_show) {
    this->stop_();
    return;
  }

  static uint32_t last = 0;
  uint32_t dt = last ? now - last : 33;
  if (dt > 100)
    dt = 33;
  last = now;
  const uint32_t t = now - this->t0_;
  const float dir = this->right_ ? 1.0f : -1.0f;
  auto off_screen = [this]() { return this->right_ ? this->x_ > OFF_R : this->x_ < OFF_L; };

  switch (this->show_) {
    case SHOW_CAT_RUN: {
      this->x_ += dir * CAT_SPEED * dt;
      this->place_cat_(this->run_frame_((t / 90) % 4), (int) this->x_, (int) this->y_);
      if (off_screen())
        this->stop_();
      break;
    }
    case SHOW_CAT_VISIT: {
      const uint32_t st = now - this->stage_t0_;
      if (this->stage_ == 0) {
        // Приходит до середины — бегом или шагом
        this->x_ += dir * (this->walk_ ? WALK_SPEED : CAT_SPEED) * dt;
        const lv_image_dsc_t *img = this->walk_ ? this->walk_frame_((t / 130) % 4) : this->run_frame_((t / 90) % 4);
        this->place_cat_(img, (int) this->x_, (int) this->y_);
        if ((this->right_ && this->x_ >= CENTER_X) || (!this->right_ && this->x_ <= CENTER_X)) {
          this->x_ = CENTER_X;
          this->stage_ = 1;
          this->plan_visit_();
          this->begin_act_(now);
        }
      } else if (this->stage_ == 1) {
        // Занят: сценка или очередное занятие. Ушли с часов — закругляется
        const Act a = this->plan_[this->iplan_];
        bool done;
        if (a == ACT_SCENE) {
          this->sit_(st);
          done = st > SIT_MS[this->variant_];
        } else {
          done = !this->act_(a, st, dt);
          if (this->page_ != 0 && st > 1500)
            done = true;
        }
        if (done) {
          this->say_(nullptr);
          this->item_.show(false);
          if (++this->iplan_ >= this->nplan_ || this->page_ != 0)
            this->leave_(now);
          else
            this->begin_act_(now);
        }
      } else if (this->stage_ == 3) {
        // Заметил касание: сидит, смотрит, моргает, над ним «мяу!»
        bool blink = st > 600 && st < 750;
        const lv_image_dsc_t *img = this->right_ ? (blink ? &spr_cat_blink_r : &spr_cat_sit_r)
                                                 : (blink ? &spr_cat_blink_l : &spr_cat_sit_l);
        this->place_cat_(img, (int) this->x_, (int) this->y_);
        if (st > MEOW_MS) {
          // И бежит дальше, куда бежал
          this->say_(nullptr);
          this->walk_ = false;
          this->stage_ = 2;
          this->stage_t0_ = now;
        }
      } else {
        // Уходит — бегом или шагом
        const float v = this->walk_ ? WALK_SPEED * 1.1f : CAT_SPEED * 1.2f;
        this->x_ += (this->right_ ? 1.0f : -1.0f) * v * dt;
        const lv_image_dsc_t *img = this->walk_ ? this->walk_frame_((st / 120) % 4) : this->run_frame_((st / 80) % 4);
        this->place_cat_(img, (int) this->x_, (int) this->y_);
        if (off_screen())
          this->stop_();
      }
      break;
    }
    case SHOW_BIRD: {
      this->x_ += dir * BIRD_SPEED * dt;
      const float yy = this->y_ + 8.0f * sinf(t * 0.004f);
      bool up = (t / 120) % 2;
      const lv_image_dsc_t *img =
          this->right_ ? (up ? &spr_bird_r1 : &spr_bird_r0) : (up ? &spr_bird_l1 : &spr_bird_l0);
      this->place_(img, (int) this->x_, (int) yy, K_OTHER);
      if (off_screen())
        this->stop_();
      break;
    }
    case SHOW_SNAIL:
    case SHOW_SNOWMAN: {
      if (this->show_ == SHOW_SNOWMAN) {
        // Снеговик: переваливается и машет руками
        this->x_ -= SNOWMAN_SPEED * dt;
        bool up = (t / 400) % 2;
        const float hop = up ? -3.0f : 0.0f;
        this->place_(up ? &spr_snowman1 : &spr_snowman0, (int) this->x_, (int) (this->y_ + hop), K_OTHER);
      } else {
        this->x_ -= SNAIL_SPEED * dt;
        bool up = (t / 700) % 2;
        this->place_(up ? &spr_snail_l1 : &spr_snail_l0, (int) this->x_, (int) this->y_, K_OTHER);
      }
      if (this->x_ < OFF_L)
        this->stop_();
      break;
    }
    case SHOW_OWL: {
      // Сидит, моргает раз в пару секунд и один раз ухает
      const bool blink = (t % 2600) > 2400;
      this->place_(blink ? &spr_owl1 : &spr_owl0, (int) this->x_, (int) this->y_, K_OTHER);
      this->say_(t > 3000 && t < 5500 ? "угу" : nullptr);
      if (this->said_ && this->zzz_)
        lv_obj_set_pos(this->zzz_, (int) this->x_ + 40, (int) this->y_ - 30);
      if (t > OWL_MS)
        this->stop_();
      break;
    }
    case SHOW_BUTTERFLY: {
      // Порхает: крылья хлопают, полёт волной и чуть вверх-вниз
      this->x_ += dir * BUTTERFLY_SPEED * dt;
      const float yy = this->y_ + 30.0f * sinf(t * 0.0025f) + 6.0f * sinf(t * 0.013f);
      this->place_((t / 110) % 2 ? &spr_butterfly1 : &spr_butterfly0, (int) this->x_, (int) yy, K_OTHER);
      if (off_screen())
        this->stop_();
      break;
    }
    case SHOW_HEDGEHOG: {
      this->x_ -= HEDGEHOG_SPEED * dt;
      this->place_((t / 260) % 2 ? &spr_hedgehog1 : &spr_hedgehog0, (int) this->x_, (int) this->y_, K_OTHER);
      if (this->x_ < OFF_L)
        this->stop_();
      break;
    }
    case SHOW_MOUSE: {
      // Мышка удирает, кот несётся следом и чуть отстаёт
      const float mx = this->x_ + dir * (MOUSE_LEAD + (MOUSE_SPEED - CAT_SPEED) * t);
      this->x_ += dir * CAT_SPEED * dt;
      if (this->item_.obj) {
        const lv_image_dsc_t *m = this->right_ ? ((t / 70) % 2 ? &spr_mouse_r1 : &spr_mouse_r0)
                                               : ((t / 70) % 2 ? &spr_mouse_l1 : &spr_mouse_l0);
        this->item_.set(m, K_MOUSE);
        this->item_.pos((int) mx + (this->right_ ? CAT_W - 30 : 30 - this->item_.w()), CAT_GROUND - this->item_.h());
        this->item_.show(true);
      }
      this->place_cat_(this->run_frame_((t / 70) % 4), (int) this->x_, (int) this->y_);
      if (off_screen())
        this->stop_();
      break;
    }
    case SHOW_PUMPKIN: {
      // Глаза и рот то горят, то гаснут — как свеча внутри
      const bool lit = ((t / 180) % 7) != 3 && ((t / 180) % 11) != 5;
      this->place_(lit ? &spr_pumpkin1 : &spr_pumpkin0, (int) this->x_, (int) this->y_, K_OTHER);
      if (t > PUMPKIN_MS)
        this->stop_();
      break;
    }
    case SHOW_CAT_SLEEP: {
      this->place_cat_(&spr_cat_sleep, (int) this->x_, (int) this->y_);
      this->snore_(t);
      if (t > (this->page_ ? SLEEP_MS[0] : SLEEP_MS[this->stay_]))
        this->stop_();
      break;
    }
    default:
      this->stop_();
      break;
  }
}

}  // namespace critters
}  // namespace esphome

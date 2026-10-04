#include "critters.h"

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
static const uint32_t SLEEP_MS = 120000;  // кот спит 2 минуты
static const uint32_t OWL_MS = 20000;     // сова сидит 20 секунд
static const uint32_t PUMPKIN_MS = 120000;  // тыква стоит 2 минуты
static const float BUTTERFLY_SPEED = 0.06f;
static const float HEDGEHOG_SPEED = 0.03f;
static const float MOUSE_SPEED = 0.32f;  // мышь чуть быстрее кота
static const int MOUSE_LEAD = 60;        // на сколько мышь впереди кота на старте
static const int OFF_L = -CAT_W - 10, OFF_R = 476;  // за краем экрана
static const int CENTER_X = 233 - CAT_W / 2;
static const uint32_t MEOW_MS = 1600;  // сколько кот сидит и мяукает после касания
// Сколько кот сидит посередине: просто так, ловя снежинку, глядя в небо, у
// принтера, у миски, у кулича, над блинами, провожая чёрного кота
static const uint32_t SIT_MS[8] = {3000, 4200, 4300, 6000, 5200, 5000, 5200, 4600};

int Critters::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

static void tick_cb(lv_timer_t *t) { static_cast<Critters *>(lv_timer_get_user_data(t))->tick(); }

void Sprite::set(const lv_image_dsc_t *s, int k) {
  if (s == this->src && k == this->k)
    return;
  this->src = s;
  this->k = k;
  if (s == nullptr || this->obj == nullptr || k < 1)
    return;
  const int sw = s->header.w, sh = s->header.h, w = sw * k, h = sh * k;
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
  const uint8_t *scol = s->data, *salp = s->data + (size_t) sw * sh * 2;
  for (int y = 0; y < sh; y++) {
    uint8_t *row = col + (size_t) y * k * w * 2;
    uint8_t *arow = alp + (size_t) y * k * w;
    for (int x = 0; x < sw; x++) {
      const uint8_t c0 = scol[(y * sw + x) * 2], c1 = scol[(y * sw + x) * 2 + 1], a = salp[y * sw + x];
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

  // Первый гость — через 20–60 минут после запуска
  this->next_ = millis() + rnd_(20, 60) * 60000u;
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

void Critters::place_cat_(const lv_image_dsc_t *img, int x, int y) {
  this->img_.set(img, K_CAT);
  this->img_.pos(x, y);
  if (!this->hat_.obj)
    return;
  // На коте: в дождь — зонтик, в праздник — своя вещь: пилотка, корона,
  // шлем космонавта, ранец, шапка Деда Мороза или колпак
  enum At { AT_HAT, AT_HEAD, AT_BACK };
  const lv_image_dsc_t *prop = nullptr;
  At at = AT_HAT;
  if (this->weather_ == 1) {
    prop = &spr_cat_umbrella;
  } else {
    switch (this->holiday_) {
      case HOL_FEB23:
      case HOL_VICTORY:
        prop = &spr_cat_pilotka;
        break;
      case HOL_CAT_DAY:
        prop = &spr_cat_crown;
        break;
      case HOL_COSMOS:
        prop = &spr_cat_helmet;
        at = AT_HEAD;
        break;
      case HOL_SEPT1:
        prop = &spr_cat_backpack;
        at = AT_BACK;
        break;
      default:
        if (this->new_year_ || this->holiday_ == HOL_NEW_YEAR || this->holiday_ == HOL_CHRISTMAS)
          prop = &spr_cat_santa;
        else if (this->birthday_ || this->holiday_ == HOL_BIRTHDAY || this->holiday_ == HOL_MAR8 ||
                 this->holiday_ == HOL_CHILDREN || this->holiday_ == HOL_RUSSIA)
          prop = &spr_cat_hat;
        break;
    }
  }
  const CatAnchor *a = nullptr;
  if (prop)
    for (const auto &c : CAT_ANCHORS)
      if (c.img == img)
        a = &c;
  if (!a) {
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
  } else {
    px = x + a->back_x * K_CAT / 2 - pw / 2;
    py = y + a->back_y * K_CAT / 2 - ph / 2;
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

void Critters::start_drink() {
  this->start(SHOW_CAT_VISIT);
  this->variant_ = VAR_DRINK;
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
  // Следующий — через 1–3 часа, в праздник кот заходит чаще: раз в 30–60 минут
  this->next_ = millis() + (this->festive_ ? rnd_(30, 60) : rnd_(60, 180)) * 60000u;
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
  } else if (this->weather_ == 2 && r < 7) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_CATCH;
  } else if (this->stars_ && this->weather_ == 0 && rnd_(0, 2)) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_STARS;
  } else {
    this->start(rnd_(0, 2) ? SHOW_CAT_RUN : SHOW_CAT_VISIT);
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
  const bool cat = this->show_ == SHOW_CAT_RUN || this->show_ == SHOW_CAT_VISIT || this->show_ == SHOW_CAT_SLEEP;
  // Уже мяукает или ещё не выбежал на экран — не замечает
  if (!cat || (this->show_ == SHOW_CAT_VISIT && this->stage_ == 3))
    return;
  if (this->x_ < -CAT_W / 2 || this->x_ > 466 - CAT_W / 2)
    return;
  if (this->show_ == SHOW_CAT_SLEEP)
    this->right_ = rnd_(0, 2);  // проснулся — убежит в случайную сторону
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
  this->img_.show(true);
}

const lv_image_dsc_t *Critters::run_frame_(int f) const {
  static const lv_image_dsc_t *R[4] = {&spr_cat_run_r0, &spr_cat_run_r1, &spr_cat_run_r2, &spr_cat_run_r3};
  static const lv_image_dsc_t *L[4] = {&spr_cat_run_l0, &spr_cat_run_l1, &spr_cat_run_l2, &spr_cat_run_l3};
  // 1 апреля кот бегает задом наперёд
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
        static const lv_image_dsc_t *BR[4] = {&spr_black_run_r0, &spr_black_run_r1, &spr_black_run_r2,
                                              &spr_black_run_r3};
        static const lv_image_dsc_t *BL[4] = {&spr_black_run_l0, &spr_black_run_l1, &spr_black_run_l2,
                                              &spr_black_run_l3};
        // Бежит навстречу взгляду рыжего: смотрит вправо — чёрный бежит справа налево
        const float d = BLACK_SPEED * (st - T0);
        this->bx_ = r ? OFF_R - d : OFF_L + d;
        const int f = (st / 80) % 4;
        const bool on = r ? this->bx_ > OFF_L : this->bx_ < OFF_R;
        if (on) {
          this->item_.set(r ? BL[f] : BR[f], K_CAT);
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

void Critters::frame(bool can_show, bool night, bool winter) {
  if (!this->img_.obj)
    return;
  this->winter_ = winter;
  const uint32_t now = millis();

  if (this->show_ == SHOW_NONE) {
    if ((int32_t) (now - this->next_) < 0)
      return;
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
    } else if (r < 70) {
      this->start_cat();
      return;
    } else if (r < 85) {
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
        // Бежит до середины
        this->x_ += dir * CAT_SPEED * dt;
        this->place_cat_(this->run_frame_((t / 90) % 4), (int) this->x_, (int) this->y_);
        if ((this->right_ && this->x_ >= CENTER_X) || (!this->right_ && this->x_ <= CENTER_X)) {
          this->x_ = CENTER_X;
          this->stage_ = 1;
          this->stage_t0_ = now;
        }
      } else if (this->stage_ == 1) {
        this->sit_(st);
        if (st > SIT_MS[this->variant_]) {
          this->say_(nullptr);
          this->item_.show(false);
          // Разворачивается и убегает обратно
          this->right_ = !this->right_;
          this->stage_ = 2;
          this->stage_t0_ = now;
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
          this->stage_ = 2;
          this->stage_t0_ = now;
        }
      } else {
        this->x_ += (this->right_ ? 1.0f : -1.0f) * CAT_SPEED * 1.2f * dt;
        this->place_cat_(this->run_frame_((st / 80) % 4), (int) this->x_, (int) this->y_);
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
      if (this->zzz_) {
        // «z», «z z», «z z z» по кругу, чуть поднимаясь
        static const char *Z[3] = {"z", "z z", "z z z"};
        int f = (t / 800) % 3;
        if (f != this->zzz_f_) {
          this->zzz_f_ = f;
          lv_label_set_text(this->zzz_, Z[f]);
          lv_obj_set_pos(this->zzz_, (int) this->x_ + 19 * K_CAT, (int) this->y_ - 20 - f * 5);
          lv_obj_clear_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
        }
      }
      if (t > SLEEP_MS)
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

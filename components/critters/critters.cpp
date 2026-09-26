#include "critters.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "sprites.h"

namespace esphome {
namespace critters {

// Геометрия — экранные координаты, центр круга 233,233, радиус 233.
// Спрайты: кот 104×64, птица 48×30, улитка 60×36, снеговик 48×66
static const int GROUND = 400;          // низ спрайтов у нижнего края круга
static const int CAT_W = 104, CAT_H = 64;
static const int SKY_Y = 60;            // птица летит по верху
static const float CAT_SPEED = 0.20f;   // px/мс — ~200 px/с
static const float BIRD_SPEED = 0.20f;
static const float SNAIL_SPEED = 0.016f;
static const float SNOWMAN_SPEED = 0.04f;
static const uint32_t SLEEP_MS = 120000;  // кот спит 2 минуты
static const int OFF_L = -110, OFF_R = 480;  // за краем экрана
static const int CENTER_X = 233 - CAT_W / 2;
static const uint32_t MEOW_MS = 1600;  // сколько кот сидит и мяукает после касания
// Сколько кот сидит посередине: просто так, ловя снежинку, глядя в небо, у принтера
static const uint32_t SIT_MS[4] = {3000, 4200, 4300, 6000};

int Critters::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

void Critters::bind(lv_obj_t *img, lv_obj_t *zzz, lv_obj_t *hat, lv_obj_t *item) {
  this->img_ = img;
  this->zzz_ = zzz;
  this->hat_ = hat;
  this->item_ = item;
  if (item)
    lv_image_set_src(item, &spr_flake);
  // Первый гость — через 20–60 минут после запуска
  this->next_ = millis() + rnd_(20, 60) * 60000u;
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

void Critters::place_(const lv_image_dsc_t *img, int x, int y) {
  // Меняем картинку, только если она правда другая: смена источника
  // перерисовывает объект
  if (img != this->shown_img_) {
    lv_image_set_src(this->img_, img);
    this->shown_img_ = img;
  }
  lv_obj_set_pos(this->img_, x, y);
  if (!this->hat_)
    return;
  // На макушке кота: в дождь — зонтик, в праздник — колпак
  const lv_image_dsc_t *prop = this->weather_ == 1 ? &spr_cat_umbrella : (this->festive_ ? &spr_cat_hat : nullptr);
  const HatAnchor *a = nullptr;
  if (prop)
    for (const auto &h : HAT_ANCHORS)
      if (h.img == img)
        a = &h;
  if (a) {
    if (prop != this->head_src_) {
      lv_image_set_src(this->hat_, prop);
      this->head_src_ = prop;
    }
    lv_obj_set_pos(this->hat_, x + a->x - (int) prop->header.w / 2, y + a->y - (int) prop->header.h);
    lv_obj_clear_flag(this->hat_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(this->hat_, LV_OBJ_FLAG_HIDDEN);
  }
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

void Critters::start_printer_visit() {
  this->start(SHOW_CAT_VISIT);
  this->variant_ = VAR_PRINTER;
}

void Critters::stop_() {
  this->show_ = SHOW_NONE;
  if (this->img_)
    lv_obj_add_flag(this->img_, LV_OBJ_FLAG_HIDDEN);
  if (this->zzz_)
    lv_obj_add_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
  if (this->hat_)
    lv_obj_add_flag(this->hat_, LV_OBJ_FLAG_HIDDEN);
  if (this->item_)
    lv_obj_add_flag(this->item_, LV_OBJ_FLAG_HIDDEN);
  this->said_ = nullptr;
  this->shown_img_ = nullptr;
  // Следующий — через 1–3 часа, в праздник кот заходит чаще: раз в 30–60 минут
  this->next_ = millis() + (this->festive_ ? rnd_(30, 60) : rnd_(60, 180)) * 60000u;
}

void Critters::start_cat() {
  // Снег — кот садится ловить снежинку (чаще всего), ясная ночь — иногда
  // смотрит на падающую звезду, иначе просто пробегает или заходит посидеть
  if (this->weather_ == 2 && rnd_(0, 10) < 7) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_CATCH;
  } else if (this->stars_ && this->weather_ == 0 && rnd_(0, 2)) {
    this->start(SHOW_CAT_VISIT);
    this->variant_ = VAR_STARS;
  } else {
    this->start(rnd_(0, 2) ? SHOW_CAT_RUN : SHOW_CAT_VISIT);
  }
}

void Critters::poke() {
  if (!this->img_)
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
  if (this->item_)
    lv_obj_add_flag(this->item_, LV_OBJ_FLAG_HIDDEN);
  this->said_ = nullptr;
  this->say_("мяу!");
  this->zzz_f_ = -1;
}

void Critters::start(Show show) {
  if (!this->img_)
    return;
  this->show_ = show;
  this->t0_ = millis();
  this->stage_ = 0;
  this->stage_t0_ = this->t0_;
  this->right_ = rnd_(0, 2);
  this->shown_img_ = nullptr;
  this->zzz_f_ = -1;
  this->said_ = nullptr;
  this->variant_ = VAR_PLAIN;
  this->meteor_sent_ = false;
  switch (show) {
    case SHOW_BIRD:
      this->y_ = SKY_Y + rnd_(0, 30);
      break;
    case SHOW_SNAIL:
      this->right_ = false;  // улитка нарисована ползущей влево
      this->y_ = this->winter_ ? GROUND - 66 : GROUND - 36;
      break;
    case SHOW_CAT_SLEEP:
      this->x_ = CENTER_X;
      this->y_ = GROUND - CAT_H;
      break;
    default:
      this->y_ = GROUND - CAT_H;
      break;
  }
  if (show != SHOW_CAT_SLEEP)
    this->x_ = this->right_ ? OFF_L : OFF_R;
  lv_obj_clear_flag(this->img_, LV_OBJ_FLAG_HIDDEN);
}

void Critters::sit_(uint32_t st) {
  const bool r = this->right_;
  auto pick = [r](const lv_image_dsc_t &right, const lv_image_dsc_t &left) { return r ? &right : &left; };
  const lv_image_dsc_t *sit = pick(spr_cat_sit_r, spr_cat_sit_l), *blink = pick(spr_cat_blink_r, spr_cat_blink_l);
  const lv_image_dsc_t *look = pick(spr_cat_look_r, spr_cat_look_l), *paw = pick(spr_cat_paw_r, spr_cat_paw_l);
  const lv_image_dsc_t *img = sit;
  switch (this->variant_) {
    case VAR_CATCH: {
      // Снежинка падает, покачиваясь, прямо на лапу; кот следит за ней,
      // поднимает лапу — поймал! — и мяукает
      const int tx = (int) this->x_ + PAW_TIP[r ? 0 : 1][0], ty = (int) this->y_ + PAW_TIP[r ? 0 : 1][1];
      const int fw = spr_flake.header.w, fh = spr_flake.header.h;
      if (st > 200 && st < 1750 && this->item_) {
        const float k = (st - 200) / 1500.0f;
        const int fx = tx + (int) (10.0f * sinf(k * 9.0f) * (1.0f - k)), fy = ty - (int) (150.0f * (1.0f - k));
        lv_obj_set_pos(this->item_, fx - fw / 2, fy - fh / 2);
        lv_obj_clear_flag(this->item_, LV_OBJ_FLAG_HIDDEN);
      } else if (this->item_) {
        lv_obj_add_flag(this->item_, LV_OBJ_FLAG_HIDDEN);
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
  this->place_(img, (int) this->x_, (int) this->y_);
}

void Critters::frame(bool can_show, bool night, bool winter) {
  if (!this->img_)
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
    } else if (r < 85)
      s = SHOW_BIRD;
    else
      s = SHOW_SNAIL;
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
      int f = (t / 90) % 4;
      static const lv_image_dsc_t *R[4] = {&spr_cat_run_r0, &spr_cat_run_r1, &spr_cat_run_r2, &spr_cat_run_r3};
      static const lv_image_dsc_t *L[4] = {&spr_cat_run_l0, &spr_cat_run_l1, &spr_cat_run_l2, &spr_cat_run_l3};
      this->place_(this->right_ ? R[f] : L[f], (int) this->x_, (int) this->y_);
      if (off_screen())
        this->stop_();
      break;
    }
    case SHOW_CAT_VISIT: {
      static const lv_image_dsc_t *R[4] = {&spr_cat_run_r0, &spr_cat_run_r1, &spr_cat_run_r2, &spr_cat_run_r3};
      static const lv_image_dsc_t *L[4] = {&spr_cat_run_l0, &spr_cat_run_l1, &spr_cat_run_l2, &spr_cat_run_l3};
      const uint32_t st = now - this->stage_t0_;
      if (this->stage_ == 0) {
        // Бежит до середины
        this->x_ += dir * CAT_SPEED * dt;
        int f = (t / 90) % 4;
        this->place_(this->right_ ? R[f] : L[f], (int) this->x_, (int) this->y_);
        if ((this->right_ && this->x_ >= CENTER_X) || (!this->right_ && this->x_ <= CENTER_X)) {
          this->stage_ = 1;
          this->stage_t0_ = now;
        }
      } else if (this->stage_ == 1) {
        this->sit_(st);
        if (st > SIT_MS[this->variant_]) {
          this->say_(nullptr);
          if (this->item_)
            lv_obj_add_flag(this->item_, LV_OBJ_FLAG_HIDDEN);
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
        this->place_(img, (int) this->x_, (int) this->y_);
        if (st > MEOW_MS) {
          // И бежит дальше, куда бежал
          this->say_(nullptr);
          this->stage_ = 2;
          this->stage_t0_ = now;
        }
      } else {
        this->x_ += (this->right_ ? 1.0f : -1.0f) * CAT_SPEED * 1.2f * dt;
        int f = (st / 80) % 4;
        this->place_(this->right_ ? R[f] : L[f], (int) this->x_, (int) this->y_);
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
      this->place_(img, (int) this->x_, (int) yy);
      if (off_screen())
        this->stop_();
      break;
    }
    case SHOW_SNAIL: {
      if (this->winter_) {
        // Снеговик: переваливается и машет руками
        this->x_ -= SNOWMAN_SPEED * dt;
        bool up = (t / 400) % 2;
        const float hop = up ? -3.0f : 0.0f;
        this->place_(up ? &spr_snowman1 : &spr_snowman0, (int) this->x_, (int) (this->y_ + hop));
      } else {
        this->x_ -= SNAIL_SPEED * dt;
        bool up = (t / 700) % 2;
        this->place_(up ? &spr_snail_l1 : &spr_snail_l0, (int) this->x_, (int) this->y_);
      }
      if (this->x_ < OFF_L)
        this->stop_();
      break;
    }
    case SHOW_CAT_SLEEP: {
      this->place_(&spr_cat_sleep, (int) this->x_, (int) this->y_);
      if (this->zzz_) {
        // «z», «z z», «z z z» по кругу, чуть поднимаясь
        static const char *Z[3] = {"z", "z z", "z z z"};
        int f = (t / 800) % 3;
        if (f != this->zzz_f_) {
          this->zzz_f_ = f;
          lv_label_set_text(this->zzz_, Z[f]);
          lv_obj_set_pos(this->zzz_, (int) this->x_ + 70, (int) this->y_ - 34 - f * 4);
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

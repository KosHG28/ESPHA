#include "critters.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "sprites.h"

namespace esphome {
namespace critters {

// Геометрия — экранные координаты, центр круга 233,233, радиус 233
static const int GROUND_Y = 372;   // верх спрайта кота/улитки у нижнего края
static const int SKY_Y = 56;       // птица летит по верху
static const float CAT_SPEED = 0.16f;    // px/мс — ~160 px/с
static const float BIRD_SPEED = 0.18f;
static const float SNAIL_SPEED = 0.014f;
static const float SNOWMAN_SPEED = 0.035f;
static const uint32_t SLEEP_MS = 120000;  // кот спит 2 минуты
static const int OFF_L = -60, OFF_R = 480;  // за краем экрана

int Critters::rnd_(int lo, int hi) { return lo + (int) (random_uint32() % (uint32_t) (hi - lo)); }

void Critters::bind(lv_obj_t *img, lv_obj_t *zzz) {
  this->img_ = img;
  this->zzz_ = zzz;
  // Первый гость — через 20–60 минут после запуска
  this->next_ = millis() + rnd_(20, 60) * 60000u;
}

void Critters::place_(const lv_image_dsc_t *img, int x, int y) {
  // Меняем картинку, только если она правда другая: смена источника
  // перерисовывает объект
  if (img != this->shown_img_) {
    lv_image_set_src(this->img_, img);
    this->shown_img_ = img;
  }
  lv_obj_set_pos(this->img_, x, y);
}

void Critters::stop_() {
  this->show_ = SHOW_NONE;
  if (this->img_)
    lv_obj_add_flag(this->img_, LV_OBJ_FLAG_HIDDEN);
  if (this->zzz_)
    lv_obj_add_flag(this->zzz_, LV_OBJ_FLAG_HIDDEN);
  this->shown_img_ = nullptr;
  // Следующий — через 1–3 часа
  this->next_ = millis() + rnd_(60, 180) * 60000u;
}

void Critters::start_cat() { this->start(rnd_(0, 2) ? SHOW_CAT_RUN : SHOW_CAT_VISIT); }

void Critters::start(Show show) {
  if (!this->img_)
    return;
  this->show_ = show;
  this->t0_ = millis();
  this->stage_ = 0;
  this->stage_t0_ = this->t0_;
  this->right_ = rnd_(0, 2);
  this->shown_img_ = nullptr;
  switch (show) {
    case SHOW_BIRD:
      this->y_ = SKY_Y + rnd_(0, 30);
      break;
    case SHOW_SNAIL:
      this->right_ = false;  // улитка нарисована ползущей влево
      this->y_ = this->winter_ ? GROUND_Y - 14 : GROUND_Y + 8;
      break;
    case SHOW_CAT_SLEEP:
      this->x_ = 233 - 26;
      this->y_ = GROUND_Y;
      break;
    default:
      this->y_ = GROUND_Y;
      break;
  }
  if (show != SHOW_CAT_SLEEP)
    this->x_ = this->right_ ? OFF_L : OFF_R;
  lv_obj_clear_flag(this->img_, LV_OBJ_FLAG_HIDDEN);
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
    if (night && r < 50)
      s = SHOW_CAT_SLEEP;
    else if (r < 70)
      s = rnd_(0, 2) ? SHOW_CAT_RUN : SHOW_CAT_VISIT;
    else if (r < 85)
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
        if ((this->right_ && this->x_ >= 233 - 26) || (!this->right_ && this->x_ <= 233 - 26)) {
          this->stage_ = 1;
          this->stage_t0_ = now;
        }
      } else if (this->stage_ == 1) {
        // Сидит 3 секунды и дважды моргает
        bool blink = (st > 900 && st < 1050) || (st > 2000 && st < 2150);
        const lv_image_dsc_t *img = this->right_ ? (blink ? &spr_cat_blink_r : &spr_cat_sit_r)
                                                 : (blink ? &spr_cat_blink_l : &spr_cat_sit_l);
        this->place_(img, (int) this->x_, (int) this->y_);
        if (st > 3000) {
          // Разворачивается и убегает обратно
          this->right_ = !this->right_;
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
        static int last_f = -1;
        if (f != last_f) {
          last_f = f;
          lv_label_set_text(this->zzz_, Z[f]);
          lv_obj_set_pos(this->zzz_, (int) this->x_ + 34, (int) this->y_ - 22 - f * 3);
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

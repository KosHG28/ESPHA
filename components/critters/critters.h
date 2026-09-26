#pragma once

#include <cstdint>

#include <lvgl.h>

#include "esphome/core/component.h"

namespace esphome {
namespace critters {

/// Кто выходит на экран
enum Show : uint8_t {
  SHOW_NONE = 0,
  SHOW_CAT_RUN,    ///< кот пробегает насквозь
  SHOW_CAT_VISIT,  ///< кот добегает до середины, садится, моргает, убегает обратно
  SHOW_BIRD,       ///< птица пролетает по верху
  SHOW_SNAIL,      ///< улитка медленно ползёт по низу (зимой — снеговик)
  SHOW_CAT_SLEEP,  ///< ночью: кот спит внизу, над ним «z z z»
};

class Critters : public Component {
 public:
  /// Картинка гостя, надпись над ним («z z z», «мяу!»), вещь на голове
  /// (колпак или зонтик) и вещь в воздухе (снежинка) — на верхнем слое LVGL
  /// (packages/critters.yaml)
  void bind(lv_obj_t *img, lv_obj_t *zzz, lv_obj_t *hat, lv_obj_t *item);

  /// Погода для кота: wx 0 — сухо, 1 — дождь (кот под зонтиком), 2 — снег
  /// (ловит снежинку); stars — ясная ночь (провожает метеор взглядом)
  void set_weather(int wx, bool stars) {
    this->weather_ = wx;
    this->stars_ = stars;
  }

  /// Печать закончилась: кот прибегает и заинтересованно смотрит
  void start_printer_visit();

  /// Кот хочет, чтобы пролетел метеор (он смотрит в небо). Флаг сбрасывается
  bool take_meteor() {
    bool r = this->meteor_req_;
    this->meteor_req_ = false;
    return r;
  }

  /// Праздник (Новый год, день рождения): кот приходит чаще и в колпаке
  void set_festive(bool festive);

  /// Вызвать гостя сейчас. SHOW_NONE — случайный, с учётом ночи и зимы
  void start(Show show);
  /// Для кнопки в HA и секретных касаний: случайное поведение кота
  void start_cat();

  /// Кто-то коснулся экрана. Кот на экране замечает это: останавливается,
  /// садится, говорит «мяу» и бежит дальше. Спящий кот просыпается и убегает
  void poke();

  /// Один кадр. can_show — экран включён (не погашен совсем), night — солнце
  /// за горизонтом, winter — декабрь…февраль. Раз в 1–3 часа гость приходит сам
  void frame(bool can_show, bool night, bool winter);

  bool active() const { return this->show_ != SHOW_NONE; }
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  /// Что кот делает, когда сидит посередине
  enum Variant : uint8_t { VAR_PLAIN, VAR_CATCH, VAR_STARS, VAR_PRINTER };

  void stop_();
  void place_(const lv_image_dsc_t *img, int x, int y);
  void say_(const char *text);
  void sit_(uint32_t st);
  static int rnd_(int lo, int hi);

  lv_obj_t *img_{nullptr}, *zzz_{nullptr}, *hat_{nullptr}, *item_{nullptr};
  const lv_image_dsc_t *head_src_{nullptr};
  const char *said_{nullptr};
  bool festive_{false}, stars_{false}, meteor_req_{false}, meteor_sent_{false};
  int weather_{0};
  Variant variant_{VAR_PLAIN};
  const lv_image_dsc_t *shown_img_{nullptr};
  Show show_{SHOW_NONE};
  bool winter_{false};
  bool right_{true};     ///< бежит слева направо
  uint32_t t0_{0};       ///< начало выхода
  uint32_t next_{0};     ///< когда следующий гость сам по себе
  float x_{0}, y_{0};
  int stage_{0};         ///< для «визита»: 0 бежит, 1 сидит, 2 убегает, 3 «мяу»
  uint32_t stage_t0_{0};
  int zzz_f_{-1};        ///< что сейчас написано над котом
};

}  // namespace critters
}  // namespace esphome

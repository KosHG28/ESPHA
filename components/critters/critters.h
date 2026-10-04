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
  SHOW_SNOWMAN,    ///< снеговик проходит по низу (зимой вместо улитки)
  SHOW_OWL,        ///< ночью: сова сидит на ветке у левого края, моргает, «угу»
  SHOW_BUTTERFLY,  ///< летом днём: бабочка порхает по экрану
  SHOW_HEDGEHOG,   ///< осенью: ёжик с листом на спине идёт по низу
  SHOW_PUMPKIN,    ///< Хэллоуин: тыква внизу, глаза мерцают
  SHOW_MOUSE,      ///< мышка пробегает по низу, кот гонится за ней
  SHOW_BLACK_CAT,  ///< пятница, 13-е: перед котом пробегает чёрный кот
};

/// Картинка LVGL, увеличенная «по пикселям»: каждая клетка спрайта — квадрат
/// k×k. Спрайты хранятся в прошивке маленькими, а увеличенный кадр
/// собирается в памяти при смене кадра (раз в 70–330 мс) — сама отрисовка
/// идёт по быстрому пути LVGL, без масштабирования на каждом кадре. Буферов
/// два: LVGL ещё может рисовать старый кадр, когда собирается новый
struct Sprite {
  lv_obj_t *obj{nullptr};
  const lv_image_dsc_t *src{nullptr};
  int k{0};
  lv_image_dsc_t dsc[2]{};
  uint8_t *buf[2]{nullptr, nullptr};
  size_t cap[2]{0, 0};
  int cur{0};

  void set(const lv_image_dsc_t *s, int k);
  int w() const { return this->src ? this->src->header.w * this->k : 0; }
  int h() const { return this->src ? this->src->header.h * this->k : 0; }
  void show(bool v);
  void pos(int x, int y) { lv_obj_set_pos(this->obj, x, y); }
};

class Critters : public Component {
 public:
  /// Картинка гостя, надпись над ним («z z z», «мяу!»), вещь на коте
  /// (шапка, зонтик, шлем, ранец) и вещь рядом (снежинка, миска, мышка,
  /// чёрный кот) — на верхнем слое LVGL (packages/critters.yaml)
  void bind(lv_obj_t *img, lv_obj_t *zzz, lv_obj_t *hat, lv_obj_t *item);

  /// Погода для кота: wx 0 — сухо, 1 — дождь (кот под зонтиком), 2 — снег
  /// (ловит снежинку); stars — ясная ночь (провожает метеор взглядом)
  void set_weather(int wx, bool stars) {
    this->weather_ = wx;
    this->stars_ = stars;
  }

  /// Печать закончилась: кот прибегает и заинтересованно смотрит
  void start_printer_visit();

  /// В поилку налили воды: кот прибегает попить из миски
  void start_drink();

  /// Какой сегодня день — для сезонных гостей
  void set_date(int month, int day) {
    this->month_ = month;
    this->day_ = day;
  }

  /// Кот хочет, чтобы пролетел метеор (он смотрит в небо). Флаг сбрасывается
  bool take_meteor() {
    bool r = this->meteor_req_;
    this->meteor_req_ = false;
    return r;
  }

  /// Праздник: кот приходит чаще и в праздничной шапке
  void set_festive(bool festive);

  /// Какой сегодня праздник (номер из weather_fx/astro.h) и что из семейного:
  /// день рождения — колпак, новогодние дни — шапка Деда Мороза. От праздника
  /// зависят вещь на коте и его сценки
  void set_holiday(int holiday, bool birthday, bool new_year) {
    this->holiday_ = holiday;
    this->birthday_ = birthday;
    this->new_year_ = new_year;
  }

  /// Погода для кота на улице: 0 — обычно, 1 — жара (лежит пластом),
  /// 2 — сильный мороз (в шарфе)
  void set_climate(int climate) { this->climate_ = climate; }

  /// Вызвать гостя сейчас. SHOW_NONE — случайный, с учётом ночи и зимы
  void start(Show show);
  /// Для кнопки в HA и секретных касаний: случайное поведение кота
  void start_cat();
  /// «Показать праздник»: сразу праздничная сценка — чёрный кот, кулич,
  /// блины, тыква, кот задом наперёд или просто кот в праздничной вещи
  void start_holiday_scene();

  /// «Романтика»: кот прибегает, смущается («ой…») и тактично уходит.
  /// quiet — пока режим включён, гости сами не приходят
  void start_romance();
  void set_quiet(bool quiet) { this->quiet_ = quiet; }

  /// «Взрослый юмор»: кота давно не трогали — он обиделся и показывает
  /// лапой неприличный жест под мозаикой. Касание — делает вид, что чесался
  void start_rude();

  /// Розыгрыш «18+»: три котёнка внутри карточки parent (packages/ui.yaml).
  /// kittens(true) — показать и оживить, false — спрятать
  void bind_kittens(lv_obj_t *parent);
  void kittens(bool on);

  /// Кто-то коснулся экрана. Кот на экране замечает это: останавливается,
  /// садится, говорит «мяу» и бежит дальше. Спящий кот просыпается и убегает
  void poke();

  /// Состояние для гостей: can_show — экран включён (не погашен совсем),
  /// night — солнце за горизонтом, winter — декабрь…февраль. Сам кадр
  /// считается в таймере LVGL — один шаг на перерисовку экрана
  void set_state(bool can_show, bool night, bool winter) {
    this->can_show_ = can_show;
    this->night_ = night;
    this->winter_in_ = winter;
  }

  /// Один кадр. Раз в 1–3 часа гость приходит сам
  void frame(bool can_show, bool night, bool winter);
  void tick() { this->frame(this->can_show_, this->night_, this->winter_in_); }

  bool active() const { return this->show_ != SHOW_NONE; }
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  /// Что кот делает, когда сидит посередине
  enum Variant : uint8_t {
    VAR_PLAIN,
    VAR_CATCH,
    VAR_STARS,
    VAR_PRINTER,
    VAR_DRINK,
    VAR_KULICH,    ///< Пасха: рядом кулич
    VAR_PANCAKES,  ///< Масленица: ест блины
    VAR_BLACK,     ///< пятница, 13-е: провожает взглядом чёрного кота
    VAR_HOT,       ///< жара: лежит пластом
    VAR_ROMANCE,   ///< «Романтика»: смущается и уходит
    VAR_RUDE,      ///< обиделся: неприличный жест под мозаикой
    VAR_SCRATCH,   ///< застукали — делает вид, что чесался
  };

  void stop_();
  void place_(const lv_image_dsc_t *img, int x, int y, int k);
  void place_cat_(const lv_image_dsc_t *img, int x, int y);
  void say_(const char *text);
  void sit_(uint32_t st);
  void item_at_bowl_(const lv_image_dsc_t *img);
  const lv_image_dsc_t *run_frame_(int f) const;
  static int rnd_(int lo, int hi);

  void kittens_frame_(uint32_t now);

  Sprite img_, hat_, item_;
  static const int NKIT = 3;
  Sprite kit_[NKIT];
  bool kit_on_{false};
  uint32_t kit_t0_{0};
  int kit_f_[NKIT]{};
  bool quiet_{false};
  lv_obj_t *zzz_{nullptr};
  const char *said_{nullptr};
  bool festive_{false}, stars_{false}, meteor_req_{false}, meteor_sent_{false};
  int weather_{0};
  int month_{0}, day_{0};
  int holiday_{0};
  int climate_{0};
  bool birthday_{false}, new_year_{false};
  bool can_show_{false}, night_{false}, winter_in_{false};
  Variant variant_{VAR_PLAIN};
  Show show_{SHOW_NONE};
  bool winter_{false};
  bool right_{true};     ///< бежит слева направо
  uint32_t t0_{0};       ///< начало выхода
  uint32_t next_{0};     ///< когда следующий гость сам по себе
  float x_{0}, y_{0};
  float bx_{0};          ///< чёрный кот: где он
  int stage_{0};         ///< для «визита»: 0 бежит, 1 сидит, 2 убегает, 3 «мяу»
  uint32_t stage_t0_{0};
  int zzz_f_{-1};        ///< что сейчас написано над котом
};

}  // namespace critters
}  // namespace esphome

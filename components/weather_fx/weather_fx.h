#pragma once

#include <cmath>
#include <cstdint>

#include <lvgl.h>

#include "esphome/core/component.h"

namespace esphome {
namespace weather_fx {

/// Что показывать в этом кадре. Заполняет packages/weather.yaml.
struct Params {
  int mode{0};        ///< осадки: 0 нет, 1 дождь, 2 снег, 3 мокрый снег, 4 град
  int count{0};       ///< сколько частиц, до 16
  int clouds{0};      ///< сколько облаков, до 3
  bool storm{false};  ///< гроза: молнии
  bool stars{false};  ///< ясная ночь: звёзды
  bool dim{false};    ///< экран приглушён — нужна яркая палитра
  bool active{false};  ///< страница часов на экране и экран не выключен
  float wind_speed{NAN};    ///< м/с
  float wind_bearing{NAN};  ///< градусы, откуда дует
  float wind_gust{NAN};     ///< м/с, порывы
};

class WeatherFx : public Component {
 public:
  /// Виджеты из packages/ui.yaml. Порядок детей внутри контейнеров важен:
  /// капли и снежинки — по 16, брызги — 8, облака — 3, звёзды — 12.
  void bind(lv_obj_t *root, lv_obj_t *drops, lv_obj_t *flakes, lv_obj_t *splash, lv_obj_t *clouds,
            lv_obj_t *stars, lv_obj_t *bolt, lv_obj_t *glow);

  /// Один кадр анимации. Вызывается раз в weather_fx_interval.
  void frame(const Params &p);

  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static const int ND = 16;   // капли и градины
  static const int NF = 16;   // снежинки
  static const int NS = 8;    // брызги
  static const int NC = 3;    // облака
  static const int NST = 12;  // звёзды
  static const int NBOLT = 10;

  /// Погода или яркость сменились — настроить виджеты.
  void apply_(const Params &p, bool relayout);
  /// Снос ветром в этом кадре: -1…1, плюс — вправо. С учётом порыва.
  float wind_(const Params &p, uint32_t now);
  void drops_(float wind);
  void splashes_();
  void flakes_(float wind);
  void clouds_();
  void stars_(uint32_t now);
  void lightning_(uint32_t now);
  void end_strike_();

  static int rnd_(int lo, int hi);
  static void show_(lv_obj_t *o, bool v);

  bool bound_{false};
  lv_obj_t *root_{nullptr}, *drops_box_{nullptr}, *flakes_box_{nullptr}, *splash_box_{nullptr};
  lv_obj_t *clouds_box_{nullptr}, *stars_box_{nullptr}, *bolt_{nullptr}, *glow_{nullptr};

  // Текущая настройка
  int applied_{-1};
  int nd_{0}, nf_{0}, ncl_{0};
  bool hail_{false}, storm_{false}, stars_on_{false}, dim_{false};
  // Движение считается по реально прошедшему времени: k_ — во сколько раз
  // этот кадр длиннее опорных 50 мс. Так скорость не зависит от частоты кадров
  uint32_t last_ms_{0};
  float k_{1.0f};
  float dt_ms_{50.0f};
  uint32_t star_ms_{0};

  float dx_[ND]{}, dy_[ND]{};
  float fx_[NF]{}, fy_[NF]{}, fph_[NF]{};
  float sx_[NS]{}, sy_[NS]{}, svx_[NS]{}, svy_[NS]{};
  float slife_[NS]{};  // сколько ещё жить брызгам, мс
  float cx_[NC]{};
  float stph_[NST]{};

  // Порыв ветра: когда начался и сколько длится (0 — порыва нет)
  uint32_t gust_t0_{0}, gust_len_{0}, next_gust_{0};

  // Молния
  lv_point_precise_t bolt_pts_[NBOLT]{};
  uint32_t next_strike_{0}, strike_t0_{0};
  bool striking_{false}, bolt_on_{false};
};

}  // namespace weather_fx
}  // namespace esphome

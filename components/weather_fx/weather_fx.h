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
  int count{0};       ///< сколько частиц: капель до 32, снежинок до 16
  int clouds{0};      ///< сколько облаков, до 3
  bool storm{false};  ///< гроза: молнии
  bool stars{false};  ///< ясная ночь: звёзды
  bool dim{false};    ///< экран приглушён — нужна яркая палитра
  bool active{false};  ///< страница часов на экране и экран не выключен
  float wind_speed{NAN};    ///< м/с
  float wind_bearing{NAN};  ///< градусы, откуда дует
  float wind_gust{NAN};     ///< м/с, порывы
  int rain_style{0};  ///< 0 — капли на стекле, 1 — падающий дождь
};

/// Погодный фон страницы часов.
///
/// Капли, снежинки, брызги и звёзды — не отдельные объекты LVGL, а рисунок
/// одного объекта-«холста»: он сам рисует все частицы в обработчике
/// отрисовки, а изменившиеся участки отмечаются напрямую. Сдвиг обычного
/// объекта LVGL меняет его стиль и заставляет пересчитать раскладку всего
/// контейнера — на 50–60 частицах при 30 кадрах в секунду это заметная
/// работа, которой здесь нет. Облака, молния и Большая Медведица — редкие и
/// крупные, они остаются обычными объектами.
class WeatherFx : public Component {
 public:
  /// root — слой погоды на странице часов; clouds — контейнер с тремя
  /// облаками; bolt и glow — линии молнии; шрифты — для снежинок
  void bind(lv_obj_t *root, lv_obj_t *clouds, lv_obj_t *bolt, lv_obj_t *glow, const lv_font_t *flake_s,
            const lv_font_t *flake_l);

  /// Один кадр анимации. Вызывается раз в weather_fx_interval.
  void frame(const Params &p);

  /// Прямоугольник (экранные координаты), где капли и снежинки не рисуются:
  /// крупные цифры времени. До двух прямоугольников
  void add_exclude(int x1, int y1, int x2, int y2);

  /// Нарисовать частицы, попадающие в перерисовываемый участок
  void paint(lv_layer_t *layer);

  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static const int ND = 32;   // капли и градины
  static const int NF = 16;   // снежинки
  static const int NS = 8;    // брызги
  static const int NC = 3;    // облака
  static const int NST = 12;  // звёзды
  static const int NDIP = 7;
  static const int NDASH = 64;

  /// Что сейчас нарисовано для частицы: прямоугольник относительно холста
  struct Spot {
    lv_area_t a;
    bool on;
  };

  void apply_(const Params &p, bool relayout);
  float wind_(const Params &p, uint32_t now);
  void drops_(float wind);
  void glass_(float k, float dt);
  void splashes_();
  void flakes_(float wind, float ks);
  void clouds_();
  void stars_(uint32_t now);
  void lightning_(uint32_t now);
  void end_strike_();
  void build_dipper_();
  void show_dipper_(bool on, bool dim);

  /// Перенести частицу: отметить к перерисовке старое и новое место
  void mark_(Spot &s, bool on, int x, int y, int w, int h);
  void invalidate_(const lv_area_t &a);
  void hide_all_();
  bool excluded_(int x, int y, int w, int h) const;

  static int rnd_(int lo, int hi);
  static void show_(lv_obj_t *o, bool v);

  bool bound_{false};
  lv_obj_t *root_{nullptr}, *paint_{nullptr}, *clouds_box_{nullptr}, *bolt_{nullptr}, *glow_{nullptr};
  const lv_font_t *font_s_{nullptr}, *font_l_{nullptr};

  // Текущая настройка
  int applied_{-1};
  int nd_{0}, nf_{0}, ncl_{0};
  bool hail_{false}, storm_{false}, stars_on_{false}, dim_{false}, glass_on_{false};

  // Движение считается по реально прошедшему времени: k_ — во сколько раз
  // этот кадр длиннее опорных 50 мс
  uint32_t last_ms_{0};
  float k_{1.0f};
  float dt_ms_{50.0f};
  // Медленные частицы (капли на стекле, мелкие снежинки) двигаются раз в
  // ~66 мс: шаг всё равно меньше пары пикселей, а отправок в экран вдвое меньше
  float slow_ms_{0.0f};
  uint32_t star_ms_{0};

  // Цвета (зависят от погоды и приглушения)
  lv_color_t c_drop_{}, c_tail_{}, c_rim_{}, c_splash_{}, c_flake_s_{}, c_flake_l_{};

  // Частицы и то, что сейчас нарисовано
  float dx_[ND]{}, dy_[ND]{};
  Spot ds_[ND]{};
  float fx_[NF]{}, fy_[NF]{}, fph_[NF]{};
  Spot fs_[NF]{};
  // Прямоугольник знака снежинки относительно точки рисования и его размер
  int fox_[NF]{}, foy_[NF]{}, fw_[NF]{}, fh_[NF]{};
  float sx_[NS]{}, sy_[NS]{}, svx_[NS]{}, svy_[NS]{};
  float slife_[NS]{};  // сколько ещё жить брызгам, мс
  Spot ss_[NS]{};
  int stx_[NST]{}, sty_[NST]{};
  float stph_[NST]{};
  lv_color_t stc_[NST]{};
  Spot sts_[NST]{};
  float cx_[NC]{};

  // Капли на стекле: состояние (0 ждёт, 1 на стекле), сколько прожила и
  // сколько проживёт, мс; когда начнёт сползать (0 — не сползёт)
  uint8_t gst_[ND]{};
  float gt_[ND]{}, glife_[ND]{}, gslide_[ND]{};

  int n_ex_{0};
  int ex_[2][4]{};

  // Порыв ветра: когда начался и сколько длится (0 — порыва нет)
  uint32_t gust_t0_{0}, gust_len_{0}, next_gust_{0};

  // Молния
  lv_point_precise_t bolt_pts_[10]{};
  uint32_t next_strike_{0}, strike_t0_{0};
  bool striking_{false}, bolt_on_{false};

  // Большая Медведица
  lv_obj_t *dip_box_{nullptr};
  lv_obj_t *dip_star_[NDIP]{};
  lv_obj_t *dash_[NDASH]{};
  lv_point_precise_t dash_pts_[NDASH][2]{};
  int ndash_{0};
};

}  // namespace weather_fx
}  // namespace esphome

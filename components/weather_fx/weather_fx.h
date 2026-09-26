#pragma once

#include <cmath>
#include <cstdint>

#include <lvgl.h>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

/// Что показывать в этом кадре. Заполняет packages/weather.yaml.
struct Params {
  int mode{0};  ///< 0 нет, 1 дождь, 2 снег, 3 мокрый снег, 4 град, 5 листопад, 6 сердечки
  int count{0};       ///< сколько частиц: капель до 32, снежинок и листьев до 16
  int clouds{0};      ///< сколько облаков, до 3
  bool storm{false};  ///< гроза: молнии
  bool stars{false};  ///< ясная ночь: звёзды
  bool dim{false};    ///< экран приглушён — нужна яркая палитра
  bool active{false};  ///< страница часов на экране и экран не выключен
  float wind_speed{NAN};    ///< м/с
  float wind_bearing{NAN};  ///< градусы, откуда дует
  float wind_gust{NAN};     ///< м/с, порывы
  int rain_style{0};  ///< 0 — капли на стекле, 1 — падающий дождь
  bool sun{false};      ///< ясный день: солнце с лучами
  bool garland{false};  ///< праздник: гирлянда по краю экрана
  bool fireworks{false};  ///< салют
  bool rainbow{false};    ///< радуга: днём после дождя прояснилось
  int kite{0};            ///< воздушный змей: 0 нет, 1 изредка, 2 часто (проверка)
  // Для созвездия: где и когда смотрим на небо. utc 0 — время неизвестно
  float lat{NAN}, lon{NAN};  ///< градусы, восточная долгота — плюс
  uint32_t utc{0};           ///< секунды Unix
};

/// Погодный фон страницы часов.
///
/// Капли, снежинки, брызги и звёзды — не отдельные объекты LVGL, а рисунок
/// одного объекта-«холста»: он сам рисует все частицы в обработчике
/// отрисовки, а изменившиеся участки отмечаются напрямую. Сдвиг обычного
/// объекта LVGL меняет его стиль и заставляет пересчитать раскладку всего
/// контейнера — на 50–60 частицах при 30 кадрах в секунду это заметная
/// работа, которой здесь нет. Облака и молния — редкие и крупные, они
/// остаются обычными объектами.
///
/// Холстов два. Задний лежит под облаками — на нём небо: звёзды, созвездие,
/// метеоры и солнце, так что облако их закрывает. Передний — над облаками:
/// капли, брызги, снежинки и гирлянда.
class WeatherFx : public Component {
 public:
  /// root — слой погоды на странице часов; clouds — контейнер с тремя
  /// облаками; bolt и glow — линии молнии; шрифты — для снежинок и подписи
  /// созвездия
  void bind(lv_obj_t *root, lv_obj_t *clouds, lv_obj_t *bolt, lv_obj_t *glow, const lv_font_t *flake_s,
            const lv_font_t *flake_l, const lv_font_t *caption);

  /// Что показывать — packages/weather.yaml обновляет это раз в
  /// weather_fx_interval. Сам кадр считается в таймере LVGL (см. bind) —
  /// ровно один шаг на каждую перерисовку экрана
  void set_params(const Params &p) { this->params_ = p; }
  const Params &params_for_timer() const { return this->params_; }

  /// Один кадр анимации
  void frame(const Params &p);

  /// Прямоугольник (экранные координаты), где капли и снежинки не рисуются:
  /// крупные цифры времени. До двух прямоугольников
  void add_exclude(int x1, int y1, int x2, int y2);

  /// Нарисовать то, что попадает в перерисовываемый участок: небо (задний
  /// холст) или осадки и гирлянду (передний)
  void paint_back(lv_layer_t *layer);
  void paint_front(lv_layer_t *layer);

  /// Пустить метеор сейчас (кот смотрит в небо). Только ясной ночью
  void launch_meteor() {
    if (this->stars_on_ && !this->met_on_)
      this->next_met_ = millis();
  }

  /// Какое созвездие сейчас на экране (для отладки), пустая строка — никакое
  const char *constellation() const { return this->con_on_ ? this->con_name_ : ""; }

  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static const int ND = 32;   // капли и градины
  static const int NF = 16;   // снежинки
  static const int NS = 8;    // брызги
  static const int NC = 3;    // облака
  static const int NST = 12;  // звёзды
  static const int NCS = 10;     // звёзд в созвездии, не больше
  static const int NDASH = 128;  // чёрточек пунктира
  static const int NG = 24;      // лампочек гирлянды
  static const int NB = 3;       // вспышек салюта одновременно
  static const int NP = 14;      // искр во вспышке
  static const int NTAIL = 9;    // точек хвоста воздушного змея

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
  void sky_(const Params &p);
  void layout_con_(int idx, const float (*pts)[2], int n);
  void meteor_(uint32_t now);
  void sun_(uint32_t now);
  void garland_(uint32_t now);
  void fireworks_(uint32_t now);
  void kite_(uint32_t now, float wind);
  void moon_(uint32_t utc);

  /// Перенести частицу: отметить к перерисовке старое и новое место
  void mark_(Spot &s, bool on, int x, int y, int w, int h);
  void invalidate_(const lv_area_t &a);
  void hide_all_();
  bool excluded_(int x, int y, int w, int h) const;

  static int rnd_(int lo, int hi);
  static void show_(lv_obj_t *o, bool v);

  bool bound_{false};
  Params params_{};
  lv_timer_t *timer_{nullptr};
  lv_obj_t *root_{nullptr}, *paint_{nullptr}, *back_{nullptr}, *clouds_box_{nullptr}, *bolt_{nullptr},
      *glow_{nullptr};
  const lv_font_t *font_s_{nullptr}, *font_l_{nullptr}, *font_cap_{nullptr};

  // Текущая настройка
  int applied_{-1};
  int nd_{0}, nf_{0}, ncl_{0};
  bool hail_{false}, storm_{false}, stars_on_{false}, dim_{false}, glass_on_{false};
  bool sun_on_{false}, gar_on_{false}, fw_on_{false}, rainbow_on_{false};
  // Луна: фаза 0..1 (0 — новолуние, 0,5 — полнолуние), нарисована ли
  float moon_phase_{-1};
  Spot moon_spot_{};
  int kite_mode_{0};

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
  // Прямоугольник знака относительно точки рисования и его размер: [0] —
  // снежинки, [1] — листья, [2] — сердечки
  int fox_[3][NF]{}, foy_[3][NF]{}, fw_[3][NF]{}, fh_[3][NF]{};
  int fset_{0};  // что падает (или всплывает): 0 снежинки, 1 листья, 2 сердечки
  lv_color_t c_leaf_[4]{}, c_leaf_s_[4]{}, c_heart_[4]{}, c_heart_s_[4]{};

  // Салют: вспышки, у каждой — центр, начало, цвет и разлетающиеся искры
  struct Burst {
    bool on;
    uint32_t t0;
    float x, y;
    lv_color_t c;
    float vx[NP], vy[NP];
    Spot sp[NP];
  };
  Burst bursts_[NB]{};
  uint32_t next_burst_{0};
  lv_opa_t burst_opa_[NB]{};

  // Воздушный змей: летит ли, откуда, когда вылетел; точки хвоста
  bool kite_on_{false};
  uint32_t kite_t0_{0}, next_kite_{0};
  float kite_x_{0}, kite_y_{0}, kite_dir_{1}, kite_speed_{0};
  lv_point_precise_t kite_tail_[NTAIL]{};
  Spot kite_spot_{};
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

  // Созвездие: какое, где звёзды (экранные координаты центра) и их размер,
  // чёрточки пунктира, общий прямоугольник вместе с подписью
  bool con_on_{false}, con_force_{true};
  int con_idx_{-1};
  int64_t con_min_{-1000};  // минута последнего расчёта
  int64_t con_slot_{-1};    // десятиминутка, в которую выбрано созвездие
  const char *con_name_{""};
  int cn_{0};
  int csx_[NCS]{}, csy_[NCS]{}, csz_[NCS]{};
  int ndash_{0};
  lv_point_precise_t dash_[NDASH][2]{};
  lv_area_t con_area_{};
  lv_area_t cap_area_{};

  // Метеор: откуда и куда летит, когда начался; что нарисовано сейчас
  bool met_on_{false};
  uint32_t met_t0_{0}, met_len_{0}, next_met_{0};
  float met_x0_{0}, met_y0_{0}, met_vx_{0}, met_vy_{0};
  float met_hx_{0}, met_hy_{0}, met_tx_{0}, met_ty_{0};
  lv_opa_t met_opa_{0};
  Spot met_spot_{};

  // Солнце: угол поворота лучей
  float sun_ang_{0};
  uint32_t sun_ms_{0};
  Spot sun_spot_{};

  // Гирлянда: шаг перемигивания
  int gar_step_{0};
  uint32_t gar_ms_{0};
  int gx_[NG]{}, gy_[NG]{};
  Spot gs_[NG]{};
};

}  // namespace weather_fx
}  // namespace esphome

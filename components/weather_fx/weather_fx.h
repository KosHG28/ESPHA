#pragma once

#include <cmath>
#include <cstdint>

#include <lvgl.h>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "astro.h"

namespace esphome {
namespace weather_fx {

/// Что показывать в этом кадре. Заполняет packages/weather.yaml.
struct Params {
  /// 0 нет, 1 дождь, 2 снег, 3 мокрый снег, 4 град, 5 листопад, 6 сердечки,
  /// 7 тюльпаны и мимоза, 8 воздушные шарики, 9 кленовые листья
  int mode{0};
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
  bool frost{false};      ///< мороз ниже −15°: иней по краю экрана
  int kite{0};            ///< воздушный змей: 0 нет, 1 изредка, 2 часто (проверка)
  int holiday{0};         ///< праздник (HOL_* из astro.h): звезда, ракета, яйца, мыши…
  bool night{false};      ///< солнце за горизонтом (летучие мыши — только ночью)
  bool fireflies{false};  ///< светлячки: Иван Купала или тёплая летняя ночь
  bool matrix{false};     ///< «матрица»: падающие столбцы нулей и единиц
  int fw_palette{0};      ///< салют: 0 разноцветный, 1 бело-сине-красный, 2 красно-золотой
  int moon_force{0};      ///< проверка: HOL_FULL_MOON / HOL_SUPERMOON / HOL_ECLIPSE, 0 — по небу
  float glow{0};          ///< зарево у горизонта на восходе и закате, 0..1
  bool dawn{false};       ///< зарево утреннее (розовое), иначе вечернее (оранжевое)
  bool heat{false};       ///< жара: марево над нижним краем
  int plane{0};           ///< самолёт ночью: 0 нет, 1 изредка, 2 часто (проверка)
  bool candles{false};    ///< «Романтика»: свечи по нижнему краю
  /// Часы убраны с экрана (режим ожидания): небо на весь круг — крупное
  /// созвездие по центру, звёзды повсюду, осадки идут и через середину
  bool full{false};
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

  /// Склейка участков перерисовки: перед каждым кадром близкие участки
  /// экрана объединяются в один, если лишних пикселей в общем прямоугольнике
  /// не больше px. Каждый участок — отдельный проход отрисовки и отдельная
  /// посылка в экран, и эти накладные расходы на плате дороже, чем
  /// перерисовать несколько тысяч лишних пикселей. 0 — как в LVGL: склеиваются
  /// только перекрывающиеся участки. Действует на весь экран, не только на погоду
  static void set_area_join(int px);

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
  static const int NST = 24;  // звёзды: 12 при часах, 24 на весь экран
  static const int NCS = 10;     // звёзд в созвездии, не больше
  static const int NDASH = 128;  // чёрточек пунктира
  static const int NG = 24;      // лампочек гирлянды
  static const int NB = 3;       // вспышек салюта одновременно
  static const int NP = 14;      // искр во вспышке
  static const int NTAIL = 9;    // точек хвоста воздушного змея
  static const int NSET = 6;     // наборов падающих знаков: снег, листья, сердечки, цветы, шарики, клёны
  static const int NFF = 10;     // светлячков
  static const int NBAT = 4;     // летучих мышей
  static const int NEGG = 3;     // пасхальных яиц
  static const int NMX = 9;      // столбцов «матрицы»
  static const int MX_LEN = 8;   // знаков в столбце
  static const int NPUFF = 6;    // клубов дыма за ракетой

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
  void moon_(uint32_t utc, int force);
  void planets_(double jd, float lst, float sphi, float cphi);
  void build_frost_();
  void free_frost_();
  void frost_sparks_(uint32_t now);
  void heat_();
  void plane_(uint32_t now);
  void candles_(uint32_t now);
  /// Показать слой погоды, если на нём хоть что-то есть
  void update_root_();
  void bstar_(uint32_t now);
  void rocket_(uint32_t now);
  void fireflies_(uint32_t now);
  void bats_();
  void eggs_(uint32_t now);
  void matrix_();
  /// Есть ли что рисовать сверх погоды: праздник, зарево, марево, самолёт
  bool extras_drawn_() const {
    return this->bstar_on_ || this->hol_ == HOL_COSMOS || this->hol_ == HOL_EASTER || this->ff_on_ ||
           this->bats_on_ || this->mx_on_ || this->horizon_ > 0.01f || this->heat_on_ || this->plane_mode_ ||
           this->cnd_on_;
  }

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
  // Планеты над горизонтом: сколько, какие (номер в таблице), нарисованы ли
  int npl_{0};
  int pl_idx_[4]{};
  Spot pl_spot_{};
  // Иней: узор собирается один раз в маску прозрачности во весь экран (A8,
  // 466×466, в PSRAM) и рисуется ледяным цветом одной картинкой. Искорки на
  // кончиках кристаллов поблёскивают. В сильный мороз под цифрами — сосульки
  static const int NFSP = 12;
  bool frost_on_{false};
  uint8_t *frost_buf_{nullptr};
  lv_image_dsc_t frost_dsc_{};
  int nfsp_{0};
  int fsp_x_[NFSP]{}, fsp_y_[NFSP]{};
  float fsp_ph_[NFSP]{};
  lv_opa_t fsp_opa_[NFSP]{};
  Spot fsp_spot_[NFSP]{};
  uint32_t fsp_ms_{0};
  int fsp_grp_{0};
  // Зарево у горизонта: сила 0..1, утро ли
  float horizon_{0};
  bool dawn_{false};
  // Жара: струйки марева поднимаются над нижним краем и тают
  static const int NHEAT = 6;
  bool heat_on_{false};
  float hx_[NHEAT]{}, hy_[NHEAT]{}, hage_[NHEAT]{}, hph_[NHEAT]{};
  Spot heat_spot_[NHEAT]{};
  // Самолёт ночью: огни на крыльях и проблесковый маяк
  int plane_mode_{0};
  bool air_on_{false};
  uint32_t air_t0_{0}, next_air_{0};
  float air_x_{0}, air_y_{0}, air_v_{0};
  Spot air_spot_{};
  // «Романтика»: свечи по нижнему краю, пламя дрожит, вокруг тёплый ореол
  static const int NCND = 5;
  bool cnd_on_{false};
  uint32_t cnd_ms_{0};
  float cnd_fh_[NCND]{}, cnd_dx_[NCND]{}, cnd_glow_[NCND]{};
  Spot cnd_spot_[NCND]{};
  int shake_{0};
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
  int star_grp_{0};

  // Цвета (зависят от погоды и приглушения)
  lv_color_t c_drop_{}, c_tail_{}, c_rim_{}, c_splash_{}, c_flake_s_{}, c_flake_l_{};

  // Частицы и то, что сейчас нарисовано
  float dx_[ND]{}, dy_[ND]{};
  Spot ds_[ND]{};
  float fx_[NF]{}, fy_[NF]{}, fph_[NF]{};
  Spot fs_[NF]{};
  // Прямоугольник знака относительно точки рисования и его размер: [0] —
  // снежинки, [1] — листья, [2] — сердечки, [3] — тюльпаны и мимоза,
  // [4] — шарики, [5] — клёны
  int fox_[NSET][NF]{}, foy_[NSET][NF]{}, fw_[NSET][NF]{}, fh_[NSET][NF]{};
  // Что падает (или всплывает): 0 снежинки, 1 листья, 2 сердечки, 3 цветы,
  // 4 шарики, 5 клёны
  int fset_{0};
  lv_color_t c_leaf_[4]{}, c_leaf_s_[4]{}, c_heart_[4]{}, c_heart_s_[4]{};
  lv_color_t c_set_[NSET][4]{}, c_set_s_[NSET][4]{};

  // Праздник и то, что для него рисуется
  int hol_{0};
  bool night_{false}, bats_on_{false}, ff_on_{false}, mx_on_{false};
  int fw_pal_{0};
  // Вифлеемская звезда: фаза мерцания
  bool bstar_on_{false};
  float bstar_ph_{0};
  uint32_t bstar_ms_{0};
  Spot bstar_spot_{};
  // Ракета: летит ли, когда вылетела, когда следующая; положение, курс,
  // длина пламени и клубы дыма позади
  bool rk_on_{false};
  uint32_t rk_t0_{0}, next_rk_{0}, rk_puff_ms_{0};
  float rk_x_{0}, rk_y_{0}, rk_a_{0}, rk_flame_{0};
  float puff_x_[NPUFF]{}, puff_y_[NPUFF]{}, puff_age_[NPUFF]{};
  Spot rk_spot_{};
  // Светлячки: место «дома», фазы движения и мерцания
  float ffx_[NFF]{}, ffy_[NFF]{}, ffph_[NFF][3]{};
  float ffk_[NFF]{};
  int ffpx_[NFF]{}, ffpy_[NFF]{};
  uint32_t ff_ms_{0};
  Spot ff_spot_[NFF]{};
  // Летучие мыши: где, скорость, фаза взмахов; рамка знака для двух размеров
  float bat_x_[NBAT]{}, bat_y_[NBAT]{}, bat_v_[NBAT]{}, bat_ph_[NBAT]{};
  int bat_ox_[2]{}, bat_oy_[2]{}, bat_w_[2]{}, bat_h_[2]{};
  Spot bat_spot_[NBAT]{};
  // Пасхальные яйца катятся по низу: когда покатились, когда следующие
  bool egg_on_{false};
  uint32_t egg_t0_{0}, next_egg_{0};
  float egg_x_[NEGG]{};
  Spot egg_spot_[NEGG]{};
  // «Матрица»: голова каждого столбца, скорость, знаки
  float mx_y_[NMX]{}, mx_v_[NMX]{};
  uint8_t mx_ch_[NMX][MX_LEN]{};
  Spot mx_spot_[NMX]{};
  int mx_ox_{0}, mx_oy_{0};
  // Луна по небу или по выбору «Показать праздник»: событие, краснота при
  // затмении, радиус
  int moon_ev_{0}, moon_r_{15};
  float moon_red_{0};
  int64_t moon_ev_min_{-1};

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
  bool full_{false};
  int nst_{12};
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

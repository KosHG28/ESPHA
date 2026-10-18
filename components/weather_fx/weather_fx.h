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
  int count{0};       ///< сила осадков: дождь и град до 32, снежинок и листьев до 16
  int clouds{0};      ///< сколько облаков, до 3
  bool storm{false};  ///< гроза: молнии
  bool stars{false};  ///< ясная ночь: звёзды
  bool dim{false};    ///< экран приглушён — нужна яркая палитра
  bool active{false};  ///< страница часов на экране и экран не выключен
  float wind_speed{NAN};    ///< м/с
  float wind_bearing{NAN};  ///< градусы, откуда дует
  float wind_gust{NAN};     ///< м/с, порывы
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
  bool snow_cover{true};  ///< сугроб после снега; false — убрать сразу («Фон погоды: Выкл»)
  /// Часы убраны с экрана (режим ожидания): небо на весь круг — крупное
  /// созвездие по центру, звёзды повсюду, осадки идут и через середину
  bool full{false};
  // Для созвездия: где и когда смотрим на небо. utc 0 — время неизвестно
  float lat{NAN}, lon{NAN};  ///< градусы, восточная долгота — плюс
  uint32_t utc{0};           ///< секунды Unix
};

/// Картинка, которую компонент рисует сам при запуске: маска A8 (цвет
/// задаётся при рисовании) или RGB565A8 (со своими цветами). ax, ay — точка
/// привязки внутри картинки: голова струи дождя, центр капли
struct FxSprite {
  lv_image_dsc_t dsc;
  uint8_t *buf;
  int cap;  // сколько байт выделено
  int ax, ay;
};

/// Набор картинок одного размера — кадры поворота, взмахов крыльев, — которые
/// рисуются, когда впервые понадобились. Описания, пиксели и отметки «уже
/// нарисована» — в куче LVGL (на плате это PSRAM)
struct FxSheet {
  lv_image_dsc_t *dsc;
  uint8_t *buf;
  uint8_t *ready;  // по биту на картинку
  int16_t *crop;   // картинка обрезана по рисунку: где её левый верхний угол
  int n, w, h;
  int ax, ay;  // точка привязки
  bool rgb;
};

/// Погодный фон страницы часов.
///
/// Капли, снежинки, облака, молния и звёзды — не отдельные объекты LVGL, а
/// рисунок самого слоя погоды: он рисует всё в своих обработчиках
/// отрисовки, а изменившиеся участки отмечаются напрямую. Сдвиг обычного
/// объекта LVGL меняет его стиль и заставляет пересчитать раскладку всего
/// контейнера — на 50–60 частицах при 30 кадрах в секунду это заметная
/// работа, которой здесь нет. А каждый объект LVGL обходит на каждом
/// перерисовываемом участке.
///
/// Небо (звёзды, созвездие, метеоры, солнце), затем молнию и облака слой
/// рисует в своей отрисовке, осадки и гирлянду — после неё, поверх.
/// Дождь и град — в rain.cpp, солнце — в sun.cpp, облака и молния — в
/// clouds.cpp.
class WeatherFx : public Component {
 public:
  /// root — слой погоды на странице часов; caption — шрифт подписи
  /// созвездия, планет и «матрицы»
  void bind(lv_obj_t *root, const lv_font_t *caption);

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
  static const int ND = 32;   // сила осадков (Params::count), не больше
  static const int NF = 16;   // снежинки
  static const int NC = 3;    // облака
  static const int NST = 24;  // звёзды: 12 при часах, 24 на весь экран
  static const int NCS = 10;     // звёзд в созвездии, не больше
  static const int NDASH = 128;  // чёрточек пунктира
  static const int NG = 24;      // лампочек гирлянды
  static const int NB = 3;       // вспышек салюта одновременно
  static const int NP = 14;      // искр во вспышке
  static const int NSET = 6;     // наборов падающего: снег, листья, сердечки, цветы, шарики, клёны
  static const int NFF = 10;     // светлячков
  static const int NBAT = 4;     // летучих мышей
  static const int NEGG = 3;     // пасхальных яиц
  static const int NMX = 9;      // столбцов «матрицы»
  static const int MX_LEN = 8;   // знаков в столбце
  static const int NPUFF = 6;    // клубов дыма за ракетой
  static const int NPUFF_R = 7;  // размеров клуба: радиус 4…10 px

  /// Что сейчас нарисовано для частицы: прямоугольник относительно холста
  struct Spot {
    lv_area_t a;
    bool on;
  };

  void apply_(const Params &p, bool relayout);
  float wind_(const Params &p, uint32_t now);
  void flakes_(float wind, float ks);
  void clouds_();
  void clouds_apply_(bool relayout);
  void cloud_prepare_(int i);
  void clouds_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void bolt_build_(int x0, int y0);
  void bolt_light_(uint8_t opa, bool lit, bool sky);
  void sky_flash_paint_(lv_layer_t *layer, const lv_area_t &clip);
  void stars_(uint32_t now);
  void lightning_(uint32_t now);
  void end_strike_();
  void sky_(const Params &p);
  void layout_con_(int idx, const float (*pts)[2], int n);
  void meteor_(uint32_t now);
  void meteor_sprite_();
  void meteor_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void sun_(uint32_t now);
  void sun_apply_();
  void sun_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void garland_(uint32_t now);
  void fireworks_(uint32_t now);
  void kite_(uint32_t now, float wind);
  void kite_stop_();
  void kite_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void moon_(uint32_t utc, int force);
  void planets_(double jd, float lst, float sphi, float cphi);
  void build_frost_();
  void free_frost_();
  void frost_sparks_(uint32_t now);
  void heat_();
  void heat_apply_(bool on);
  void heat_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void plane_(uint32_t now);
  void candles_(uint32_t now);
  /// Показать слой погоды, если на нём хоть что-то есть
  void update_root_();
  void bstar_(uint32_t now);
  void bstar_apply_(bool on);
  void bstar_render_(int i);
  void bstar_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void rocket_(uint32_t now);
  void rocket_render_(int bin);
  void rocket_apply_(bool on);
  void rocket_stop_();
  void rocket_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void icicles_apply_();
  void icicles_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
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
  lv_obj_t *root_{nullptr}, *paint_{nullptr}, *back_{nullptr};
  const lv_font_t *font_cap_{nullptr};

  // Текущая настройка. nd_ — сила дождя или града (0 — его нет)
  int applied_{-1};
  int nd_{0}, nf_{0}, ncl_{0};
  bool hail_{false}, storm_{false}, stars_on_{false}, dim_{false};
  bool sun_on_{false}, gar_on_{false}, fw_on_{false}, rainbow_on_{false};
  // Планеты над горизонтом: сколько, какие (номер в таблице), нарисованы ли
  int npl_{0};
  int pl_idx_[4]{};
  Spot pl_spot_{};
  // Иней: узор собирается один раз в маску прозрачности во весь экран (A8,
  // 466×466, в PSRAM) и рисуется ледяным цветом одной картинкой. Искорки на
  // кончиках кристаллов поблёскивают. В сильный мороз под цифрами — сосульки
  // (extras.cpp): одна картинка и при какой яркости экрана нарисована
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
  FxSprite ice_spr_{};
  bool ice_dim_{false};
  // Зарево у горизонта: сила 0..1, утро ли
  float horizon_{0};
  bool dawn_{false};
  // Жара: струйки марева поднимаются над нижним краем и тают; картинки
  // струйки — по фазе волны (extras.cpp)
  static const int NHEAT = 6;
  bool heat_on_{false};
  float hx_[NHEAT]{}, hy_[NHEAT]{}, hage_[NHEAT]{}, hph_[NHEAT]{};
  Spot heat_spot_[NHEAT]{};
  FxSheet heat_sheet_{};
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
  // Картинки свечей (candles.cpp): ореол, тела свечей, пламя в высотах и
  // наклонах
  FxSprite cnd_glow_spr_{}, cnd_body_[NCND]{};
  FxSheet cnd_flame_{};
  void candles_apply_(bool on);
  void candles_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  int shake_{0};
  // Луна: фаза 0..1 (0 — новолуние, 0,5 — полнолуние), нарисована ли
  float moon_phase_{-1};
  Spot moon_spot_{};
  // Картинка луны (moon.cpp) и при какой яркости экрана нарисована
  FxSprite moon_spr_{};
  bool moon_dim_{false};
  void moon_sprite_();
  void moon_paint_(lv_layer_t *layer, int cx, int cy);
  int kite_mode_{0};

  // Движение считается по реально прошедшему времени: k_ — во сколько раз
  // этот кадр длиннее опорных 50 мс
  uint32_t last_ms_{0};
  float k_{1.0f};
  float dt_ms_{50.0f};
  // Мелкие падающие знаки (сердечки, цветы, шарики) двигаются раз в
  // ~66 мс: шаг всё равно меньше пары пикселей, а отправок в экран вдвое меньше
  float slow_ms_{0.0f};
  // Сколько ещё картинок поворота (листья, сердечки, цветы, шарики) можно
  // нарисовать в этом кадре: каждая — заметная работа, все разом дали бы
  // заминку
  int render_budget_{0};
  uint32_t star_ms_{0};
  int star_grp_{0};

  // Падающие и всплывающие картинки (сердечки, цветы, шарики — signs.cpp):
  // где, фаза покачивания, что сейчас нарисовано и какой кадр
  float fx_[NF]{}, fy_[NF]{}, fph_[NF]{};
  Spot fs_[NF]{};
  uint8_t fidx_[NF]{};
  // Картинки: дальние и ближние, для какого набора, при какой яркости
  FxSheet sign_sheet_[2]{};
  int sign_set_{-1};
  bool sign_dim_{false};
  void signs_apply_();
  void sign_render_(int idx, int sz);
  void sign_mark_(int i);
  void signs_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  // Что падает (или всплывает): 0 снег (snow.cpp), 1 листья (leaves.cpp),
  // 2 сердечки, 3 цветы, 4 шарики, 5 клёны (leaves.cpp)
  int fset_{0};
  lv_color_t c_leaf_[4]{}, c_leaf_s_[4]{}, c_heart_[4]{}, c_heart_s_[4]{};
  lv_color_t c_set_[NSET][4]{}, c_set_s_[NSET][4]{};

  // Праздник и то, что для него рисуется
  int hol_{0};
  bool night_{false}, bats_on_{false}, ff_on_{false}, mx_on_{false};
  int fw_pal_{0};
  // Вифлеемская звезда (extras.cpp): фаза мерцания; кадры «дыхания» лучей,
  // какой сейчас и при какой яркости экрана нарисованы
  bool bstar_on_{false};
  float bstar_ph_{0};
  uint32_t bstar_ms_{0};
  Spot bstar_spot_{};
  FxSheet bstar_sheet_{};
  int bstar_idx_{0};
  bool bstar_dim_{false};
  // Ракета (extras.cpp): летит ли, когда вылетела, когда следующая;
  // положение, курс, длина пламени и клубы дыма позади. Картинки корпуса и
  // трёх длин пламени — под курс rk_bin_·2°, клубы дыма по размеру
  bool rk_on_{false};
  uint32_t rk_t0_{0}, next_rk_{0}, rk_puff_ms_{0};
  float rk_x_{0}, rk_y_{0}, rk_a_{0}, rk_flame_{0};
  float puff_x_[NPUFF]{}, puff_y_[NPUFF]{}, puff_age_[NPUFF]{};
  Spot rk_spot_{};
  int rk_bin_{0};
  FxSprite rk_body_{}, rk_fl_[3]{}, rk_puff_[NPUFF_R]{};
  // Светлячки: место «дома», фазы движения и мерцания
  float ffx_[NFF]{}, ffy_[NFF]{}, ffph_[NFF][3]{};
  float ffk_[NFF]{};
  int ffpx_[NFF]{}, ffpy_[NFF]{};
  uint32_t ff_ms_{0};
  Spot ff_spot_[NFF]{};
  // Летучие мыши (signs.cpp): где, скорость, фаза полёта, кадр взмаха;
  // кадры взмаха — дальней и ближней
  float bat_x_[NBAT]{}, bat_y_[NBAT]{}, bat_v_[NBAT]{}, bat_ph_[NBAT]{};
  uint8_t bat_frame_[NBAT]{};
  Spot bat_spot_[NBAT]{};
  FxSheet bat_sheet_[2]{};
  void bat_render_(int sz, int frame);
  void bat_mark_(int i, int x, int y);
  void bats_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
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

  // Салют (fireworks.cpp). Залп: что сейчас (0 — ничего, 1 — летит ракета,
  // 2 — разлетаются искры), начало этого этапа, сколько летит ракета; где
  // взорвётся и откуда взлетела; цвет залпа и цвет искр сейчас; скорость,
  // направление и яркость каждой искры; ракета, вспышка и дым — где и с
  // какой яркостью
  struct Burst {
    uint8_t st;
    uint32_t t0, len;
    float x, y, rx;
    lv_color_t c, cur;
    float vx[NP], vy[NP];
    uint8_t dir[NP];
    lv_opa_t opa[NP];
    Spot sp[NP];
    Spot rk, fl, sm;
    int rkx, rky;
    lv_opa_t fl_opa, sm_opa;
    uint8_t sm_k;
    uint32_t sm_t0;
    float smx, smy;
  };
  Burst bursts_[NB]{};
  uint32_t next_burst_{0};
  // Картинки: искры по 16 направлениям, голова и хвост ракеты, вспышка,
  // клубы дыма четырёх размеров
  FxSprite fw_spark_[16]{}, fw_dot_{}, fw_trail_{}, fw_flash_{}, fw_smoke_[4]{};
  void fw_sprites_();
  void fireworks_stop_();
  void fw_mark_(Spot &spot, const FxSprite &s, int x, int y, bool on);
  void fireworks_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);

  // Воздушный змей (extras.cpp): летит ли, откуда, когда вылетел; где
  // нарисован ромб и каким наклоном, где хвост и какой его кадр. Картинки —
  // ромб в наклонах и хвост по фазе волны
  bool kite_on_{false};
  uint32_t kite_t0_{0}, next_kite_{0};
  float kite_x_{0}, kite_y_{0}, kite_dir_{1}, kite_speed_{0};
  int kite_cx_{0}, kite_cy_{0}, kite_frame_{-1}, kite_tx_{0}, kite_ty_{0}, kite_tframe_{-1};
  Spot kite_spot_{};
  FxSheet kite_sheet_{}, kite_tail_{};
  // Звёзды (stars.cpp): где (левый верхний угол прежнего квадратика 3×3),
  // фаза мерцания, оттенок, яркая ли с лучиками, непрозрачность сейчас, где
  // нарисованы; картинки — обычная и яркая звезда, звёзды созвездия по размеру
  int stx_[NST]{}, sty_[NST]{};
  float stph_[NST]{};
  uint8_t sttint_[NST]{};
  bool stbig_[NST]{};
  lv_opa_t stopa_[NST]{};
  Spot sts_[NST]{};
  FxSprite st_spr_[2]{}, con_spr_[4]{};
  void star_sprites_();
  void stars_layout_();
  void star_mark_(int i, bool on);
  void stars_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void con_star_paint_(lv_layer_t *layer, int x, int y, int sz);
  // Облака (clouds.cpp): левый край; маски тела и света и сколько их строк
  // уже нарисовано; цветные картинки — обычная и во вспышке молнии; под
  // какие цвета собрана каждая и какие нужны сейчас (0 — ни под какие); где
  // нарисованы; подсвечены ли сейчас молнией
  float cx_[NC]{};
  FxSprite cloud_body_[NC]{}, cloud_light_[NC]{}, cloud_img_[NC]{}, cloud_flash_[NC]{};
  int cloud_rows_[NC]{};
  int cloud_look_[NC]{}, cloud_want_{0};
  Spot cloud_spot_[NC]{};
  bool clouds_lit_{false};

  // ---- Дождь и град (rain.cpp) ----
  using Sprite = FxSprite;
  static const int NRS = 40;   // струй дождя и градин
  static const int NSPL = 32;  // брызг и отскоков
  static const int NSTV = 6;   // картинок струи: 3 слоя глубины × 2 длины

  void rain_apply_(const Params &p, bool relayout);
  void rain_frame_(float wind);
  void rain_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void rain_hide_();
  void streaks_(float wind);
  void respawn_streak_(int i, float tan_a, bool anywhere);
  void streak_sprites_(int q);
  void splash_(float x, float y, int kind, float wind);
  void splashes_();
  void hail_sprite_();
  bool visible_(int x, int y, int w, int h) const;
  int streak_parts_(int i, const FxSprite **spr, lv_area_t *rect, lv_opa_t *opa) const;

  // Струи (пучки струй): голова первой струи, скорость (px за 50 мс), вид:
  // 0–2 — пучки дальнего слоя, 3–5 — среднего, 6–7 — ближние струи (град — 6)
  int nrs_{0};
  float rx_[NRS]{}, ry_[NRS]{}, rv_[NRS]{};
  uint8_t rkind_[NRS]{};
  float rgy_[NRS]{};     // нижний край круга под струёй: ниже её не видно
  bool rland_[NRS]{};    // голова уже коснулась края (брызги были)
  float rfar_k_[NRS]{};  // дальние пучки: сколько кадров копится до шага
  uint8_t rain_tick_{0};
  Spot rs_[NRS]{};
  Sprite streak_[NSTV]{};
  int streak_q_{-1000};    // угол наклона струй, при котором нарисованы картинки
  float tan_a_{0};         // наклон, который сейчас у струй
  // Брызги и отскоки градин: вид (0 капелька, 1 вспышка удара, 2 градина),
  // скорость, сколько осталось жить и сколько жили всего, мс
  float spx_[NSPL]{}, spy_[NSPL]{}, spvx_[NSPL]{}, spvy_[NSPL]{};
  float splife_[NSPL]{}, spmax_[NSPL]{};
  uint8_t spk_[NSPL]{};
  lv_opa_t spopa_[NSPL]{};
  Spot sps_[NSPL]{};
  Sprite dot_{}, flash_{}, hail_spr_{};
  lv_color_t c_streak_[3]{}, c_splash_{};

  // ---- Снег и сугроб (snow.cpp) ----
  static const int NSN = 24;   // снежинок и стаек дальних точек
  static const int NSROT = 8;  // поворотов кристалла в пределах 60°
  void snow_apply_(const Params &p, bool relayout);
  void snow_respawn_(int i, bool anywhere);
  void snow_hide_();
  void snow_frame_(float wind);
  void snow_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  void cover_step_(uint32_t now);
  void cover_update_();
  void cover_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  // Снежинки: где, скорость (px за 50 мс), фаза покачивания, поворот и его
  // скорость, накопленное время дальних (они двигаются через кадр), вид
  // (0 — стайка дальних точек, 1 — хлопок, 2–3 — кристаллы), где нарисованы
  int nsn_{0};
  float snx_[NSN]{}, sny_[NSN]{}, snv_[NSN]{}, snph_[NSN]{}, snrot_[NSN]{}, snw_[NSN]{}, snacc_[NSN]{};
  int16_t snpx_[NSN]{}, snpy_[NSN]{};
  uint8_t snk_[NSN]{};
  uint8_t snow_tick_{0};
  Spot sns_[NSN]{};
  FxSprite sn_dot_{}, sn_flake_{}, sn_cry_[2][NSROT]{};
  lv_color_t c_snow_[3]{}, c_cover_{};
  // Сугроб: высота 0..1, при какой нарисован, скорость роста (минус — тает)
  // в долях за секунду
  float cover_{0}, cover_drawn_{0}, cover_rate_{0};
  uint32_t cover_ms_{0};
  FxSprite cover_spr_{};
  Spot cover_spot_{};

  // ---- Листопад (leaves.cpp) ----
  static const int NLF = 16;  // листьев
  bool leaf_sprites_();
  void leaf_frame_(int idx);
  void leaf_free_();
  void leaves_apply_(const Params &p, bool relayout);
  void leaf_respawn_(int i, bool anywhere);
  void leaves_frame_(float wind);
  void leaves_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
  // Листья: где, скорость падения (px за 50 мс), фаза покачивания, поворот
  // и скорость кружения, фаза и скорость переворота, сколько ещё лежать на
  // краю (мс, 0 — летит), какая картинка и изнанкой ли, непрозрачность, где
  // нарисованы
  int nlf_{0};
  bool leaf_maple_only_{false};
  float lfx_[NLF]{}, lfy_[NLF]{}, lfv_[NLF]{}, lfph_[NLF]{}, lfrot_[NLF]{}, lfspin_[NLF]{};
  float lfflip_[NLF]{}, lfflipw_[NLF]{}, lfrest_[NLF]{};
  uint16_t lfidx_[NLF]{};
  bool lfback_[NLF]{};
  lv_opa_t lfopa_[NLF]{};
  Spot lfs_[NLF]{};
  // Образцы листьев (остаются в памяти), все картинки одним куском памяти,
  // их описания (в PSRAM) и какие уже нарисованы — по биту на картинку
  uint8_t *leaf_master_{nullptr};
  uint8_t *leaf_buf_{nullptr};
  lv_image_dsc_t *leaf_dsc_{nullptr};
  uint8_t leaf_ready_[36]{};

  int n_ex_{0};
  int ex_[2][4]{};

  // Порыв ветра: когда начался и сколько длится (0 — порыва нет)
  uint32_t gust_t0_{0}, gust_len_{0}, next_gust_{0};

  // Молния: когда следующая и когда началась; идёт ли удар, ярко ли сейчас
  // (струи дождя белеют), вспыхнуло ли небо; маски сердцевины и свечения,
  // где они на экране и с какой яркостью рисуются
  uint32_t next_strike_{0}, strike_t0_{0};
  bool striking_{false}, bolt_on_{false}, sky_flash_{false};
  FxSprite bolt_core_{}, bolt_glow_{};
  Spot bolt_spot_{};
  int bolt_x_{0}, bolt_y_{0};
  uint8_t bolt_opa_{0};

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

  // Метеор (extras.cpp): откуда и куда летит, когда начался; что нарисовано
  // сейчас; картинка следа под его курс
  bool met_on_{false};
  uint32_t met_t0_{0}, met_len_{0}, next_met_{0};
  float met_x0_{0}, met_y0_{0}, met_vx_{0}, met_vy_{0};
  float met_hx_{0}, met_hy_{0}, met_tx_{0}, met_ty_{0};
  lv_opa_t met_opa_{0};
  Spot met_spot_{};
  FxSprite met_spr_{};

  // ---- Солнце (sun.cpp) ----
  // Венец: угол поворота и время для дрожания лучей, с; масштаб, под который
  // нарисованы картинки (0 — не нарисованы)
  float sun_ang_{0}, sun_t_{0}, sun_k_{0};
  uint32_t sun_ms_{0};
  // Квадрат вокруг солнца и маска длинных лучей во весь экран
  Spot sun_spot_{}, sun_sky_spot_{};
  FxSprite sun_core_{}, sun_cor_{}, sun_sky_{};
  int sun_sky_rows_{0};  // сколько строк маски длинных лучей уже нарисовано
  void sun_sky_step_();

  // Радуга (rainbow.cpp): картинка, яркость 0..1 (идёт к rainbow_on_) и
  // когда она менялась
  FxSprite rb_spr_{};
  float rb_level_{0};
  uint32_t rb_ms_{0};
  void rainbow_sprite_();
  void rainbow_(uint32_t now);
  void rainbow_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);

  // Гирлянда (garland.cpp): шаг бегущей волны и когда он начался; середины
  // лампочек, где они нарисованы и как горят (0 погасла, 1 полутон,
  // 2 горит); маски лампочек — свечение, патрон, колба, блик — и отрезков
  // провода с их левым верхним углом
  int gar_step_{0};
  uint32_t gar_ms_{0};
  int gx_[NG]{}, gy_[NG]{};
  Spot gs_[NG]{};
  uint8_t glev_[NG]{};
  FxSprite gb_glow_[NG]{}, gb_cap_[NG]{}, gb_body_[NG]{}, gb_spec_[NG]{}, gw_[NG]{};
  int gw_x_[NG]{}, gw_y_[NG]{};
  void garland_sprites_();
  void garland_apply_();
  void garland_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip);
};

}  // namespace weather_fx
}  // namespace esphome

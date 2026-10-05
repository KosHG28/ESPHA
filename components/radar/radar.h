#pragma once

#include <atomic>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <lvgl.h>

#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace radar {

/// Самолёт, как его прислал сервер: положение — в пикселях экрана от центра
/// (x — на восток, y — на юг), в той же проекции, что и карта
struct Plane {
  char id[12]{};
  float x{0}, y{0};      ///< где был в момент данных
  float vx{0}, vy{0};    ///< скорость, px/с
  int16_t hdg{0};        ///< курс, градусы от севера по часовой
  int32_t alt{0};        ///< высота, м
  int16_t spd{0};        ///< путевая скорость, км/ч
  float dist{0};         ///< от дома, км
  uint8_t kind{0};       ///< силуэт: 0 обычный, 1 широкофюзеляжный, 2 винтовой
  char cs[10]{};         ///< позывной
  char rt[12]{};         ///< маршрут «SVO-KUF»
  char ty[6]{};          ///< тип «A320»
  char al[28]{};         ///< авиакомпания
  char num[10]{};        ///< номер рейса «SU1520»
  char tl[24]{};         ///< модель «Airbus A320»
  char on[20]{}, dn[20]{};  ///< аэропорты вылета и прилёта
  uint8_t icon{0};       ///< силуэт: номер в AC_ICON (aircraft_icons.h)
  uint8_t ntr{0};
  int16_t tr[8][2]{};    ///< хвост: прежние точки
  // Показ: откуда плавно доезжает до новых данных и что нарисовано
  float fx{0}, fy{0};    ///< поправка, которая тает за 1,5 с
  float px{0}, py{0};    ///< где нарисован сейчас
  bool label{false};     ///< подпись помещается
  lv_area_t area{};      ///< что занимает на экране
  bool drawn{false};
};

/// Вид радара — настраивается на веб-странице сервера и приходит с /esp
struct View {
  char map[12]{};       ///< стиль карты: dark, sat, osm-dark, osm
  int r{0};             ///< радиус, с которого открывается радар, км
  bool labels{true};    ///< подписи маршрутов
  bool grid{true};      ///< кольца и стороны света
  bool alt_color{true}; ///< цвет по высоте
  bool shapes{true};    ///< силуэты по типу, иначе стрелка
  bool rim{true};       ///< отметки самолётов за кругом
  char hn[24]{};        ///< город в центре
  bool operator==(const View &o) const {
    return strcmp(map, o.map) == 0 && r == o.r && labels == o.labels && grid == o.grid &&
           alt_color == o.alt_color && shapes == o.shapes && rim == o.rim && strcmp(hn, o.hn) == 0;
  }
};

/// Отслеживаемый рейс — /flight с сервера, уже в пикселях экрана: вылет
/// слева, прилёт справа, дуга между ними
struct Track {
  char st[6]{};           ///< ok, nf (не найден), err, none, wait
  char n[10]{}, cs[10]{}, al[28]{}, tl[24]{};
  char oc[5]{}, on[20]{}, dc[5]{}, dn[20]{};
  char ot[6]{}, dep[6]{}, eta[6]{};
  uint8_t icon{5};
  static const int NPT = 49;
  int16_t pt[NPT][2]{};
  int npt{0};
  bool plane{false};
  int16_t px{0}, py{0}, ph{0}, pi{0};
  int32_t alt{0};
  int16_t spd{0}, hdg{0};
  uint32_t ver{0};        ///< версия карты /trackmap.jpg
};

class Radar : public Component {
 public:
  void set_url(const std::string &url);
  /// Номер рейса для страницы «Рейс» — из HA, например SU1520
  void set_flight(const std::string &flight);
  void set_interval(uint32_t ms) { this->interval_ms_ = ms; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  /// Холст поверх карты, строка ошибок, заголовок с радиусом, карточка
  /// самолёта (во весь экран, содержимое создаётся здесь) и шрифты: мелкий,
  /// позывных, средний и крупный — packages/radar.yaml
  void bind(lv_obj_t *view, lv_obj_t *status, lv_obj_t *title, lv_obj_t *card, const lv_font_t *font,
            const lv_font_t *font_cs, const lv_font_t *font_mid, const lv_font_t *font_big);

  /// Страница радара на экране и экран включён — только тогда идут запросы
  void set_active(bool active);
  /// Масштаб: dir < 0 — ближе (меньше радиус), dir > 0 — дальше; шаги — ZOOMS
  void zoom(int dir);
  int radius() const { return this->r_.load(); }
  /// Адрес картинки карты для текущего радиуса и стиля
  std::string map_url();
  /// Радиус сменился или страницу открыли впервые — пора скачать карту
  bool take_map_request() {
    bool r = this->map_req_;
    this->map_req_ = false;
    return r;
  }

  void paint(lv_layer_t *layer);

  /// Страница «Рейс»: холст поверх карты маршрута и шрифты как у карточки
  void bind_track(lv_obj_t *view);
  void set_track_active(bool active);
  bool take_track_map_request() {
    bool r = this->trk_map_req_;
    this->trk_map_req_ = false;
    return r;
  }
  std::string track_map_url();
  void paint_track(lv_layer_t *layer);
  void tick();
  /// Сектор развёртки: шаг поворота, 30 раз в секунду
  void sweep_tick();
  void on_event(lv_event_t *e);

  static const int R_PX = 220;  ///< радиус круга дальности на экране
  static constexpr int ZOOMS[4] = {25, 50, 100, 200};

 protected:
  static void task_fn(void *arg);
  std::string url_copy_();
  void fetch_();
  void fetch_track_();
  void apply_track_(const Track &t);
  void apply_(std::vector<Plane> &fresh, int r);
  void layout_labels_();
  void update_status_();
  void show_card_(int idx);
  void hide_card_();
  void invalidate_plane_(const Plane &p);
  /// Повёрнутый по курсу силуэт (маска A8 50×50) — из кэша или собранный
  const lv_image_dsc_t *sprite_(int icon, int hdg);
  void invalidate_sweep_(float from_deg, float to_deg);
  void show_title_();
  void set_radius_(int r);
  lv_color_t plane_color_(int32_t alt) const;
  void apply_view_(const View &v);
  lv_area_t plane_area_(const Plane &p) const;

  std::string url_;
  uint32_t interval_ms_{8000};
  TaskHandle_t task_{nullptr};
  std::atomic<bool> active_{false};
  std::atomic<int> r_{100};
  bool map_req_{false}, map_shown_{false};
  View view_cfg_;
  bool view_known_{false};

  // Из задачи загрузки — в поток LVGL
  std::mutex mtx_;
  std::vector<Plane> pending_;
  bool has_pending_{false};
  View pending_view_;
  bool has_view_{false};
  int pending_r_{0};
  int fails_{0};
  bool fail_changed_{false};
  uint8_t *body_{nullptr};

  // То, что на экране
  std::vector<Plane> planes_;
  uint32_t data_ms_{0};
  int shown_r_{0};
  bool loaded_{false};
  int selected_{-1};
  char sel_id_[12]{};
  uint32_t card_ms_{0};
  lv_point_t press_{};

  lv_obj_t *view_{nullptr}, *status_{nullptr}, *title_{nullptr}, *card_{nullptr};
  const lv_font_t *font_{nullptr}, *font_cs_{nullptr}, *font_mid_{nullptr}, *font_big_{nullptr};
  uint32_t title_ms_{0};

  // Развёртка: угол переднего края, градусы от востока по часовой
  float sweep_deg_{-90.0f};
  uint32_t sweep_ms_{0};

  // Кэш повёрнутых силуэтов: номер иконки и курс шагом 5°
  struct SprEnt {
    int key{-1};
    uint32_t used{0};
    uint8_t *buf{nullptr};
    lv_image_dsc_t dsc{};
  };
  static const int NSPR = 40;
  SprEnt spr_[NSPR];
  uint32_t spr_clock_{0};

  // Карточка самолёта во весь экран
  lv_obj_t *d_air_{nullptr}, *d_img_{nullptr}, *d_type_{nullptr}, *d_o_{nullptr}, *d_oc_{nullptr}, *d_d_{nullptr},
      *d_dc_{nullptr}, *d_num_{nullptr}, *d_val_[3]{}, *d_cap_[3]{}, *d_cs_{nullptr};
  // Рисунок сбоку в карточке: ширина и наибольшая высота после обрезки
  static const int DET_W = 210, DET_H = 110;
  uint8_t *det_buf_{nullptr};
  lv_image_dsc_t det_dsc_{};
  int det_icon_{-1};

  // Трекер рейса
  std::string flight_;
  std::atomic<bool> trk_active_{false};
  bool has_trk_{false}, trk_map_req_{false}, trk_changed_{false};
  Track trk_pending_, trk_;
  uint32_t trk_shown_ver_{0};
  lv_obj_t *trk_view_{nullptr}, *t_num_{nullptr}, *t_air_{nullptr}, *t_oc_{nullptr}, *t_on_{nullptr},
      *t_dc_{nullptr}, *t_dn_{nullptr}, *t_val_[3]{}, *t_cap_[3]{}, *t_tcap_[2]{}, *t_tval_[2]{}, *t_msg_{nullptr},
      *t_type_{nullptr};
};

}  // namespace radar
}  // namespace esphome

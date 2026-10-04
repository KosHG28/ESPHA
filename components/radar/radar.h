#pragma once

#include <atomic>
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
  uint8_t ntr{0};
  int16_t tr[8][2]{};    ///< хвост: прежние точки
  // Показ: откуда плавно доезжает до новых данных и что нарисовано
  float fx{0}, fy{0};    ///< поправка, которая тает за 1,5 с
  float px{0}, py{0};    ///< где нарисован сейчас
  bool label{false};     ///< подпись помещается
  lv_area_t area{};      ///< что занимает на экране
  bool drawn{false};
};

class Radar : public Component {
 public:
  void set_url(const std::string &url);
  void set_interval(uint32_t ms) { this->interval_ms_ = ms; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  /// Холст поверх карты, строка «ближайший», заголовок с радиусом, карточка
  /// самолёта (заголовок и текст) и шрифт подписей — packages/radar.yaml
  void bind(lv_obj_t *view, lv_obj_t *status, lv_obj_t *title, lv_obj_t *card, lv_obj_t *card_title,
            lv_obj_t *card_body, const lv_font_t *font);

  /// Страница радара на экране и экран включён — только тогда идут запросы
  void set_active(bool active);
  /// Масштаб: dir < 0 — ближе (меньше радиус), dir > 0 — дальше
  void zoom(int dir);
  int radius() const { return ZOOMS[this->zoom_idx_.load()]; }
  /// Адрес картинки карты для текущего радиуса
  std::string map_url();
  /// Радиус сменился или страницу открыли впервые — пора скачать карту
  bool take_map_request() {
    bool r = this->map_req_;
    this->map_req_ = false;
    return r;
  }

  void paint(lv_layer_t *layer);
  void tick();
  void on_event(lv_event_t *e);

  static const int R_PX = 220;  ///< радиус круга дальности на экране
  static constexpr int ZOOMS[4] = {25, 50, 100, 200};

 protected:
  static void task_fn(void *arg);
  std::string url_copy_();
  void fetch_();
  void apply_(std::vector<Plane> &fresh, int r);
  void layout_labels_();
  void update_status_();
  void show_card_(int idx);
  void hide_card_();
  void invalidate_plane_(const Plane &p);
  lv_area_t plane_area_(const Plane &p) const;

  std::string url_;
  uint32_t interval_ms_{8000};
  TaskHandle_t task_{nullptr};
  std::atomic<bool> active_{false};
  std::atomic<int> zoom_idx_{2};
  bool map_req_{false}, map_shown_{false};

  // Из задачи загрузки — в поток LVGL
  std::mutex mtx_;
  std::vector<Plane> pending_;
  bool has_pending_{false};
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

  lv_obj_t *view_{nullptr}, *status_{nullptr}, *title_{nullptr}, *card_{nullptr}, *card_title_{nullptr},
      *card_body_{nullptr};
  const lv_font_t *font_{nullptr};
};

}  // namespace radar
}  // namespace esphome

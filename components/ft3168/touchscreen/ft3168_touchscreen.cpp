#include "ft3168_touchscreen.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ft3168 {

static const char *const TAG = "ft3168.touchscreen";

bool FT3168Touchscreen::try_init_() {
  // Линии RST и INT к чипу на этой плате не разведены, поэтому единственный
  // способ его разбудить — записать power mode, как делает драйвер Waveshare.
  // Без этого чип отвечает на свой адрес, но координат не отдаёт.
  if (!this->write_byte(REG_POWER_MODE, 0x01)) {
    return false;
  }
  if (!this->read_byte(REG_DEVICE_ID, &this->device_id_)) {
    return false;
  }
  ESP_LOGI(TAG, "FT3168 ready, device ID 0x%02X", this->device_id_);
  return true;
}

void FT3168Touchscreen::setup() {
  if (this->display_ != nullptr) {
    if (this->x_raw_max_ == this->x_raw_min_) {
      this->x_raw_max_ = this->display_->get_native_width();
    }
    if (this->y_raw_max_ == this->y_raw_min_) {
      this->y_raw_max_ = this->display_->get_native_height();
    }
  }

  // Панель питается от GPIO42, и на момент настройки тача это питание может
  // ещё отсутствовать — тогда пробуждающая запись уходит в никуда. Поэтому
  // инициализация повторяется на каждом опросе, пока не удастся.
  this->initialized_ = this->try_init_();
  if (!this->initialized_) {
    ESP_LOGW(TAG, "FT3168 not responding yet, will retry while polling");
  }
}

void FT3168Touchscreen::update_touches() {
  if (!this->initialized_) {
    this->initialized_ = this->try_init_();
    if (!this->initialized_) {
      return;
    }
  }

  uint8_t fingers = 0;
  // Регистры читаются по одному байту: пакетное чтение этот чип не отдаёт.
  if (!this->read_byte(REG_TOUCH_NUM, &fingers)) {
    this->read_errors_++;
    this->status_set_warning();
    // Возможно, чип потерял питание и вернулся в спящий режим.
    this->initialized_ = false;
    return;
  }
  this->status_clear_warning();
  this->last_fingers_ = fingers;

  if (fingers == 0 || fingers > 2) {
    return;
  }

  uint8_t xh = 0, xl = 0, yh = 0, yl = 0;
  if (!this->read_byte(REG_XPOS_HIGH, &xh) || !this->read_byte(REG_XPOS_LOW, &xl) ||
      !this->read_byte(REG_YPOS_HIGH, &yh) || !this->read_byte(REG_YPOS_LOW, &yl)) {
    this->read_errors_++;
    this->status_set_warning();
    return;
  }

  this->last_x_ = encode_uint16(xh & 0x0F, xl);
  this->last_y_ = encode_uint16(yh & 0x0F, yl);
  this->touch_count_++;
  ESP_LOGD(TAG, "Touch at %u/%u (%u fingers)", this->last_x_, this->last_y_, fingers);
  this->add_raw_touch_position_(0, this->last_x_, this->last_y_);
}

std::string FT3168Touchscreen::debug_state() const {
  char buf[96];
  if (!this->initialized_) {
    snprintf(buf, sizeof(buf), "нет ответа, ошибок чтения: %u", (unsigned) this->read_errors_);
    return std::string(buf);
  }
  snprintf(buf, sizeof(buf), "id=0x%02X пальцев=%u посл.=%u/%u касаний=%u ошибок=%u",
           this->device_id_, this->last_fingers_, this->last_x_, this->last_y_,
           (unsigned) this->touch_count_, (unsigned) this->read_errors_);
  return std::string(buf);
}

void FT3168Touchscreen::dump_config() {
  ESP_LOGCONFIG(TAG, "FT3168 Touchscreen:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Device ID: 0x%02X", this->device_id_);
  ESP_LOGCONFIG(TAG, "  Raw range: %d..%d / %d..%d", this->x_raw_min_, this->x_raw_max_, this->y_raw_min_,
                this->y_raw_max_);
}

}  // namespace ft3168
}  // namespace esphome

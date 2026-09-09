#include "ft3168_touchscreen.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ft3168 {

static const char *const TAG = "ft3168.touchscreen";

void FT3168Touchscreen::setup() {
  // Линии RST и INT к чипу на этой плате не разведены, поэтому единственный
  // способ его разбудить — записать power mode, как это делает драйвер
  // Waveshare. Без этого чип отвечает на адрес, но координат не отдаёт.
  this->write_byte(REG_POWER_MODE, 0x01);
  delay(20);  // NOLINT

  if (!this->read_byte(REG_DEVICE_ID, &this->device_id_)) {
    // Не повод отключаться: чип часто отвечает уже на рабочих опросах.
    ESP_LOGW(TAG, "Device ID read failed, continuing anyway");
  }

  if (this->display_ != nullptr) {
    if (this->x_raw_max_ == this->x_raw_min_) {
      this->x_raw_max_ = this->display_->get_native_width();
    }
    if (this->y_raw_max_ == this->y_raw_min_) {
      this->y_raw_max_ = this->display_->get_native_height();
    }
  }
}

void FT3168Touchscreen::update_touches() {
  uint8_t fingers = 0;
  // Регистры читаются по одному байту. Пакетное чтение нескольких регистров
  // подряд этот чип не отдаёт — именно на этом не работали готовые драйверы.
  if (!this->read_byte(REG_TOUCH_NUM, &fingers)) {
    this->status_set_warning();
    return;
  }
  this->status_clear_warning();

  if (fingers == 0 || fingers > 2) {
    return;
  }

  uint8_t xh = 0, xl = 0, yh = 0, yl = 0;
  if (!this->read_byte(REG_XPOS_HIGH, &xh) || !this->read_byte(REG_XPOS_LOW, &xl) ||
      !this->read_byte(REG_YPOS_HIGH, &yh) || !this->read_byte(REG_YPOS_LOW, &yl)) {
    this->status_set_warning();
    return;
  }

  uint16_t x = encode_uint16(xh & 0x0F, xl);
  uint16_t y = encode_uint16(yh & 0x0F, yl);
  ESP_LOGV(TAG, "Touch at %u/%u (%u fingers)", x, y, fingers);
  this->add_raw_touch_position_(0, x, y);
}

void FT3168Touchscreen::dump_config() {
  ESP_LOGCONFIG(TAG, "FT3168 Touchscreen:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Device ID: 0x%02X", this->device_id_);
}

}  // namespace ft3168
}  // namespace esphome

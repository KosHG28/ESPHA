#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace ft3168 {

// Регистры FT3168. Карта взята из драйвера Arduino_FT3x68 самой Waveshare
// (поставляется с демо для ESP32-S3-Touch-AMOLED-1.8, там тот же чип).
static const uint8_t REG_TOUCH_NUM = 0x02;
static const uint8_t REG_XPOS_HIGH = 0x03;  // значимы младшие 4 бита
static const uint8_t REG_XPOS_LOW = 0x04;
static const uint8_t REG_YPOS_HIGH = 0x05;
static const uint8_t REG_YPOS_LOW = 0x06;
static const uint8_t REG_DEVICE_ID = 0xA0;   // 0x03 = FT3168
static const uint8_t REG_POWER_MODE = 0xA5;  // 0x01 = monitor mode

class FT3168Touchscreen : public touchscreen::Touchscreen, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;

 protected:
  void update_touches() override;

  uint8_t device_id_{0xFF};
};

}  // namespace ft3168
}  // namespace esphome

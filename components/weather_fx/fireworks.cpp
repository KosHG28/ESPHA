// Салют: в Новый год, в день рождения и по кнопке.
//
// Сначала снизу взлетает ракета — яркая точка с искрящимся хвостом,
// замедляясь к верху. Взрыв — короткая вспышка, искры разлетаются, падают
// под своим весом и гаснут: в первые доли секунды они раскалённо-белые,
// потом цвета залпа, к концу мерцают. Каждая искра — чёрточка вдоль полёта
// (картинки для 16 направлений нарисованы заранее). На месте взрыва
// остаётся и тает облачко дыма.
#include "fx_draw.h"

#include "esphome/core/hal.h"

namespace esphome {
namespace weather_fx {

// Цвета залпов: разноцветный, бело-сине-красный (12 июня), красно-золотой
// (23 февраля)
static const uint32_t FW_COLORS[6] = {0xFF5252, 0xFFD740, 0x69F0AE, 0x40C4FF, 0xE040FB, 0xFFFFFF};
static const uint32_t FW_TRICOLOR[3] = {0xFFFFFF, 0x2962FF, 0xFF1744};
static const uint32_t FW_RED[3] = {0xFF1744, 0xFFC400, 0xFF6D00};
static const int NDIR = 16;               // направлений искры
static constexpr float SPARK_LEN = 6.0f;  // длина чёрточки искры, px
static constexpr int SPARK_H = (int) SPARK_LEN + 2;
static const uint32_t SPARK_MS = 1300;  // сколько живут искры
static const uint32_t FLASH_MS = 170;   // вспышка взрыва
static const uint32_t SMOKE_MS = 2600;  // дым тает
static const float GRAV = 0.00006f;     // падение искр, px/мс²
static const int SMOKE_R[4] = {14, 22, 30, 38};
static const float PI2_F = 6.28318531f;

// Искра: чёрточка с яркой головой (точка привязки) и тающим хвостом
// назад по направлению a. Картинка обрезана по самой чёрточке: участок
// перерисовки искры тогда вчетверо меньше
static void draw_spark(FxSprite &s, float a) {
  const int h = SPARK_H, n = 2 * h + 1;
  uint8_t full[(2 * SPARK_H + 1) * (2 * SPARK_H + 1)] = {};
  const float c = cosf(a), sn = sinf(a);
  int x0 = n, y0 = n, x1 = -1, y1 = -1;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      float acc = 0;
      for (int q = 0; q < 9; q++) {
        const float px = x - h + ((q % 3) - 1) / 3.0f, py = y - h + ((q / 3) - 1) / 3.0f;
        // Вдоль хвоста (назад от головы) и поперёк
        const float along = -(px * c + py * sn), across = fabsf(-px * sn + py * c);
        if (along < -0.9f || along > SPARK_LEN)
          continue;
        const float t = fx_clamp01(along / SPARK_LEN);
        if (across < 0.95f - 0.55f * t)
          acc += 1.0f - 0.85f * t;
      }
      const uint8_t v = (uint8_t) std::min(255.0f, acc / 9.0f * 255.0f * 1.2f);
      full[y * n + x] = v;
      if (v) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
      }
    }
  }
  if (x1 < 0 || !fx_sprite_init(s, x1 - x0 + 1, y1 - y0 + 1, false))
    return;
  s.ax = h - x0;
  s.ay = h - y0;
  for (int y = y0; y <= y1; y++)
    memcpy(s.buf + (y - y0) * (x1 - x0 + 1), full + y * n + x0, x1 - x0 + 1);
  fx_sprite_done(s);
}

// Мягкое пятно радиуса r: f — степень спада к краю, k — неровность края
// (дым клубится)
static void draw_blob(FxSprite &s, float r, float f, float k, uint32_t seed) {
  const int h = (int) ceilf(r) + 1, n = 2 * h + 1;
  if (!fx_sprite_init(s, n, n, false))
    return;
  s.ax = s.ay = h;
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float dx = (float) (x - h), dy = (float) (y - h);
      float d = sqrtf(dx * dx + dy * dy) / r;
      if (k > 0)
        d *= 1.0f + k * sinf(3.0f * fx_atan2(dy, dx) + seed) * sinf(5.0f * fx_atan2(dy, dx) + 2 * seed);
      const float v = 1.0f - fx_clamp01(d);
      if (v > 0)
        s.buf[y * n + x] = (uint8_t) std::min(255.0f, std::max(0.0f, 255.0f * powf(v, f) + fx_dither(x, y) * 4.0f));
    }
  }
  fx_sprite_done(s);
}

// Хвост ракеты: вертикальная полоска вниз от головы, тает и искрит
static void draw_trail(FxSprite &s) {
  const int w = 7, hgt = 32;
  if (!fx_sprite_init(s, w, hgt, false))
    return;
  // Привязка — голова: выше неё есть место под саму яркую точку
  s.ax = w / 2;
  s.ay = 4;
  FxRand rnd(4242);
  for (int y = 0; y < hgt; y++) {
    const float t = (y - 4.0f) / (hgt - 5.0f);
    for (int x = 0; x < w; x++) {
      const float ax = fabsf(x - w / 2.0f + 0.5f);
      float v = fx_clamp01(1.3f - ax / (1.4f - 0.7f * fx_clamp01(t))) * (1.0f - fx_clamp01(t));
      // Искорки, отлетающие от хвоста
      if (t > 0.2f && rnd.f() < 0.07f)
        v = std::max(v, 0.8f * (1.0f - t));
      s.buf[y * w + x] = (uint8_t) (255.0f * fx_clamp01(v));
    }
  }
  fx_sprite_done(s);
}

void WeatherFx::fw_sprites_() {
  if (this->fw_dot_.buf)
    return;
  for (int d = 0; d < NDIR; d++)
    draw_spark(this->fw_spark_[d], d * (PI2_F / NDIR));
  draw_blob(this->fw_dot_, 2.4f, 1.3f, 0, 0);
  draw_blob(this->fw_flash_, 16.0f, 2.2f, 0, 0);
  for (int k = 0; k < 4; k++)
    draw_blob(this->fw_smoke_[k], SMOKE_R[k], 1.4f, 0.12f, 7 + k);
  draw_trail(this->fw_trail_);
}

void WeatherFx::fireworks_stop_() {
  for (auto &b : this->bursts_) {
    for (auto &sp : b.sp)
      this->mark_(sp, false, 0, 0, 1, 1);
    this->mark_(b.rk, false, 0, 0, 1, 1);
    this->mark_(b.fl, false, 0, 0, 1, 1);
    this->mark_(b.sm, false, 0, 0, 1, 1);
    b.st = 0;
    b.sm_t0 = 0;
  }
}

// Отметить участок картинки s с привязкой в (x, y): перерисовать прежнее и
// новое место, даже если оно не сдвинулось (меняется яркость)
void WeatherFx::fw_mark_(Spot &spot, const FxSprite &s, int x, int y, bool on) {
  if (spot.on)
    this->invalidate_(spot.a);
  const lv_area_t a = fx_sprite_area(s, x, y);
  const int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
  this->mark_(spot, on && s.buf && !this->excluded_(a.x1, a.y1, w, h), a.x1, a.y1, w, h);
}

void WeatherFx::fireworks_(uint32_t now) {
  if (!this->fw_on_)
    return;
  this->fw_sprites_();
  // Новая ракета раз в 0,35–0,9 с, если есть свободное место
  if ((int32_t) (now - this->next_burst_) >= 0) {
    this->next_burst_ = now + rnd_(350, 900);
    for (auto &b : this->bursts_) {
      if (b.st)
        continue;
      b.st = 1;
      b.t0 = now;
      b.len = rnd_(550, 800);
      b.x = rnd_(110, 357);
      b.y = rnd_(60, 175);
      b.rx = b.x + rnd_(-30, 31);
      b.c = lv_color_hex(this->fw_pal_ == 1   ? FW_TRICOLOR[rnd_(0, 3)]
                         : this->fw_pal_ == 2 ? FW_RED[rnd_(0, 3)]
                                              : FW_COLORS[rnd_(0, 6)]);
      break;
    }
  }
  for (auto &b : this->bursts_) {
    if (b.st == 1) {
      const uint32_t t = now - b.t0;
      if (t < b.len) {
        // Ракета взлетает, замедляясь к верху, и чуть виляет
        const float e = (float) t / b.len, q = 1.0f - (1.0f - e) * (1.0f - e);
        b.rkx = (int) (b.rx + (b.x - b.rx) * q + 1.5f * sinf(e * 9.0f));
        b.rky = (int) (470.0f + (b.y - 470.0f) * q);
        this->fw_mark_(b.rk, this->fw_trail_, b.rkx, b.rky, true);
        continue;
      }
      // Взрыв: искры во все стороны, вспышка, дым
      this->fw_mark_(b.rk, this->fw_trail_, 0, 0, false);
      b.st = 2;
      b.t0 = now;
      const float sp = rnd_(7, 12) / 100.0f;  // px/мс
      const float a0 = rnd_(0, 628) / 100.0f;
      for (int k = 0; k < NP; k++) {
        const float a = a0 + k * (PI2_F / NP) + rnd_(-15, 16) / 100.0f;
        const float v = sp * (0.8f + rnd_(0, 40) / 100.0f);
        b.vx[k] = v * cosf(a);
        b.vy[k] = v * sinf(a);
      }
      b.sm_t0 = now;
      b.smx = b.x;
      b.smy = b.y;
    }
    if (b.st == 2) {
      const uint32_t t = now - b.t0;
      if (t > SPARK_MS) {
        for (auto &sp : b.sp)
          this->mark_(sp, false, 0, 0, 1, 1);
        b.st = 0;
      } else {
        const float life = (float) t / SPARK_MS, fade = 1.0f - life;
        // Раскалённые искры белые, потом цвета залпа
        b.cur = lv_color_mix(lv_color_white(), b.c, (uint8_t) (255 * (1.0f - fx_smooth(0.0f, 0.16f, life))));
        for (int k = 0; k < NP; k++) {
          const int x = (int) (b.x + b.vx[k] * t), y = (int) (b.y + b.vy[k] * t + GRAV * t * t);
          // Чёрточка — вдоль полёта: скорость с учётом падения
          const float a = atan2f(b.vy[k] + 2.0f * GRAV * t, b.vx[k]);
          b.dir[k] = (uint8_t) (((int) lroundf(a / (PI2_F / NDIR)) % NDIR + NDIR) % NDIR);
          float o = 255.0f * powf(fade, 0.7f);
          // К концу — мерцают
          if (life > 0.62f)
            o *= 0.3f + 0.7f * (rnd_(0, 100) / 100.0f);
          b.opa[k] = (lv_opa_t) o;
          this->fw_mark_(b.sp[k], this->fw_spark_[b.dir[k]], x, y, true);
        }
      }
      // Вспышка взрыва
      if (t < FLASH_MS) {
        const float f = 1.0f - (float) t / FLASH_MS;
        b.fl_opa = (lv_opa_t) (255 * f * f);
        this->fw_mark_(b.fl, this->fw_flash_, (int) b.x, (int) b.y, true);
      } else if (b.fl.on) {
        this->mark_(b.fl, false, 0, 0, 1, 1);
      }
    }
    // Дым: клубится, расширяясь, и тает; меняется раз в ~140 мс
    if (b.sm_t0) {
      const uint32_t ts = now - b.sm_t0;
      if (ts > SMOKE_MS) {
        this->mark_(b.sm, false, 0, 0, 1, 1);
        b.sm_t0 = 0;
      } else {
        const float f = (float) ts / SMOKE_MS;
        const uint8_t k = (uint8_t) std::min(3, (int) (f * 5.0f));
        const lv_opa_t o = (lv_opa_t) (((this->dim_ ? 60.0f : 38.0f) * fx_smooth(0.0f, 0.12f, f) * (1.0f - f)));
        if (k != b.sm_k || std::abs(o - b.sm_opa) >= 4 || !b.sm.on) {
          b.sm_k = k;
          b.sm_opa = o;
          // Дым медленно поднимается
          this->fw_mark_(b.sm, this->fw_smoke_[k], (int) b.smx, (int) (b.smy - 0.006f * ts), true);
        }
      }
    }
  }
}

void WeatherFx::fireworks_paint_(lv_layer_t *layer, const lv_area_t &oc, const lv_area_t &clip) {
  auto screen = [&](const Spot &s) {
    return lv_area_t{s.a.x1 + oc.x1, s.a.y1 + oc.y1, s.a.x2 + oc.x1, s.a.y2 + oc.y1};
  };
  // Серый дым чуть розоватый: тусклый нейтральный серый на RGB565 зеленит
  const lv_color_t smoke = lv_color_hex(this->dim_ ? 0x9A8E9C : 0x6A5C6A);
  // Дым — под всем остальным
  for (const auto &b : this->bursts_) {
    if (b.sm.on && b.sm_opa) {
      const lv_area_t a = screen(b.sm);
      if (fx_hit(a, clip))
        fx_draw(layer, this->fw_smoke_[b.sm_k], a, b.sm_opa, smoke);
    }
  }
  for (const auto &b : this->bursts_) {
    // Ракета: хвост и яркая голова
    if (b.st == 1 && b.rk.on) {
      const lv_area_t a = screen(b.rk);
      if (fx_hit(a, clip)) {
        fx_draw(layer, this->fw_trail_, a, 200, lv_color_hex(0xFFD58A));
        const int x = b.rkx + oc.x1, y = b.rky + oc.y1;
        fx_draw(layer, this->fw_dot_, fx_sprite_area(this->fw_dot_, x, y), LV_OPA_COVER, lv_color_hex(0xFFF4DC));
      }
    }
    if (b.st != 2)
      continue;
    if (b.fl.on) {
      const lv_area_t a = screen(b.fl);
      if (fx_hit(a, clip))
        fx_draw(layer, this->fw_flash_, a, b.fl_opa, lv_color_mix(lv_color_white(), b.c, 150));
    }
    for (int k = 0; k < NP; k++) {
      if (!b.sp[k].on)
        continue;
      const lv_area_t a = screen(b.sp[k]);
      if (fx_hit(a, clip))
        fx_draw(layer, this->fw_spark_[b.dir[k]], a, b.opa[k], b.cur);
    }
  }
}

}  // namespace weather_fx
}  // namespace esphome

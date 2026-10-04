#include "radar.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include "esphome/components/json/json_util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace radar {

static const char *const TAG = "radar";
static const float PI_F = 3.14159265f;
// Ответ сервера: 40 самолётов с хвостами — около 10 КБ, с запасом
static const size_t BODY_MAX = 32 * 1024;
// За сколько мс самолёт плавно доезжает до новых данных
static const float BLEND_MS = 1500.0f;

void Radar::set_url(const std::string &url) {
  std::string u = url;
  while (!u.empty() && (u.back() == '/' || u.back() == ' '))
    u.pop_back();
  while (!u.empty() && u.front() == ' ')
    u.erase(0, 1);
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    if (u == this->url_)
      return;
    this->url_ = u;
    this->fails_ = 0;
    this->fail_changed_ = true;
  }
  // Новый сервер — заново карта, самолёты и его настройки вида
  this->view_known_ = false;
  this->view_cfg_.map[0] = 0;
  this->map_req_ = this->map_shown_;
  if (this->task_)
    xTaskNotifyGive(this->task_);
}

std::string Radar::url_copy_() {
  std::lock_guard<std::mutex> lock(this->mtx_);
  return this->url_;
}

std::string Radar::map_url() {
  std::string u = this->url_copy_() + "/map.jpg?r=" + std::to_string(this->radius());
  if (this->view_cfg_.map[0])
    u += std::string("&s=") + this->view_cfg_.map;
  return u;
}

void Radar::setup() {
  // Буфер ответа — в PSRAM: внутренней памяти на плате немного
  this->body_ = static_cast<uint8_t *>(heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!this->body_)
    this->body_ = static_cast<uint8_t *>(malloc(BODY_MAX));
  // Загрузка — на ядре 0, рядом с Wi-Fi: основной цикл и экран на ядре 1
  xTaskCreatePinnedToCore(task_fn, "radar", 6144, this, 1, &this->task_, 0);
}

void Radar::set_active(bool active) {
  const bool was = this->active_.exchange(active);
  if (active && !was) {
    // Радар открывается с радиуса, заданного на веб-странице
    if (this->view_known_ && this->view_cfg_.r > 0 && this->view_cfg_.r != this->radius())
      this->set_radius_(this->view_cfg_.r);
    if (!this->map_shown_) {
      this->map_shown_ = true;
      this->map_req_ = true;
    }
    if (this->task_)
      xTaskNotifyGive(this->task_);
  }
}

void Radar::zoom(int dir) {
  // Следующий шаг из ZOOMS ближе или дальше текущего радиуса — он может быть
  // и не из списка, если задан на веб-странице
  const int r = this->radius();
  int next = 0;
  for (int z : ZOOMS)
    if (dir < 0 ? z < r : (z > r && !next))
      next = z;
  if (next)
    this->set_radius_(next);
}

void Radar::set_radius_(int r) {
  if (r == this->radius())
    return;
  this->r_ = r;
  this->map_req_ = true;
  this->hide_card_();
  if (this->title_) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Радар · %d км", this->radius());
    lv_label_set_text(this->title_, buf);
  }
  if (this->task_)
    xTaskNotifyGive(this->task_);
}

// ---------------------------------------------------------------------------
// Загрузка — в своей задаче

void Radar::task_fn(void *arg) {
  auto *self = static_cast<Radar *>(arg);
  for (;;) {
    if (!self->active_.load() || self->url_copy_().empty() || !self->body_) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    self->fetch_();
    // Масштаб сменили или страницу открыли заново — задача просыпается раньше
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(self->interval_ms_));
  }
}

void Radar::fetch_() {
  const int r = this->radius();
  const std::string url = this->url_copy_() + "/esp?r=" + std::to_string(r);
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.timeout_ms = 7000;
  size_t len = 0;
  bool ok = false;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c) {
    if (esp_http_client_open(c, 0) == ESP_OK) {
      esp_http_client_fetch_headers(c);
      if (esp_http_client_get_status_code(c) == 200) {
        int n;
        while (len < BODY_MAX && (n = esp_http_client_read(c, (char *) this->body_ + len, BODY_MAX - len)) > 0)
          len += n;
        ok = len > 0 && len < BODY_MAX;
      }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
  std::vector<Plane> fresh;
  if (ok) {
    JsonDocument doc = json::parse_json(this->body_, len);
    JsonArray arr = doc["p"].as<JsonArray>();
    ok = !arr.isNull();
    if (ok) {
      const float k = doc["k"] | 1.0f;  // пикселей на километр в центре
      fresh.reserve(arr.size());
      for (JsonObject o : arr) {
        Plane p;
        strncpy(p.id, o["id"] | "", sizeof(p.id) - 1);
        p.x = o["x"] | 0.0f;
        p.y = o["y"] | 0.0f;
        p.hdg = o["h"] | 0;
        p.alt = o["a"] | 0;
        p.spd = o["s"] | 0;
        p.dist = o["d"] | 0.0f;
        p.kind = o["k"] | 0;
        strncpy(p.cs, o["c"] | "", sizeof(p.cs) - 1);
        strncpy(p.rt, o["rt"] | "", sizeof(p.rt) - 1);
        strncpy(p.ty, o["ty"] | "", sizeof(p.ty) - 1);
        strncpy(p.al, o["al"] | "", sizeof(p.al) - 1);
        // Скорость в пикселях в секунду по курсу
        const float v = p.spd / 3600.0f * k;
        const float a = p.hdg * (PI_F / 180.0f);
        p.vx = v * sinf(a);
        p.vy = -v * cosf(a);
        JsonArray tr = o["tr"].as<JsonArray>();
        for (JsonArray pt : tr) {
          if (p.ntr >= 8)
            break;
          p.tr[p.ntr][0] = pt[0] | 0;
          p.tr[p.ntr][1] = pt[1] | 0;
          p.ntr++;
        }
        fresh.push_back(p);
      }
      JsonObject v = doc["v"].as<JsonObject>();
      if (!v.isNull()) {
        View vw;
        strncpy(vw.map, v["m"] | "", sizeof(vw.map) - 1);
        vw.r = v["r"] | 0;
        vw.labels = (v["l"] | 1) != 0;
        vw.grid = (v["g"] | 1) != 0;
        vw.alt_color = (v["c"] | 1) != 0;
        vw.shapes = (v["s"] | 1) != 0;
        vw.rim = (v["o"] | 1) != 0;
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->pending_view_ = vw;
        this->has_view_ = true;
      }
    }
  }
  std::lock_guard<std::mutex> lock(this->mtx_);
  if (ok) {
    this->pending_ = std::move(fresh);
    this->pending_r_ = r;
    this->has_pending_ = true;
    if (this->fails_) {
      this->fails_ = 0;
      this->fail_changed_ = true;
    }
  } else {
    ESP_LOGW(TAG, "Сервер радара не ответил: %s", url.c_str());
    this->fails_++;
    this->fail_changed_ = true;
  }
}

// ---------------------------------------------------------------------------
// Экран — в потоке LVGL

static void view_event_cb(lv_event_t *e) { static_cast<Radar *>(lv_event_get_user_data(e))->on_event(e); }
static void tick_cb(lv_timer_t *t) { static_cast<Radar *>(lv_timer_get_user_data(t))->tick(); }

void Radar::bind(lv_obj_t *view, lv_obj_t *status, lv_obj_t *title, lv_obj_t *card, lv_obj_t *card_title,
                 lv_obj_t *card_body, const lv_font_t *font) {
  this->view_ = view;
  this->status_ = status;
  this->title_ = title;
  this->card_ = card;
  this->card_title_ = card_title;
  this->card_body_ = card_body;
  this->font_ = font;
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_DRAW_MAIN, this);
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_CLICKED, this);
  if (card)
    lv_obj_add_event_cb(card, view_event_cb, LV_EVENT_CLICKED, this);
  if (title) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Радар · %d км", this->radius());
    lv_label_set_text(title, buf);
  }
  if (status)
    lv_label_set_text(status, this->url_copy_().empty() ? "Сервер радара не задан" : "Загрузка…");
  // Самолёты ползут медленно: 4 шага в секунду хватает с запасом
  lv_timer_create(tick_cb, 250, this);
}

static lv_color_t alt_color_of(int32_t alt) {
  if (alt < 1500)
    return lv_color_hex(0x00E5FF);
  if (alt < 4500)
    return lv_color_hex(0x00FF66);
  if (alt < 8500)
    return lv_color_hex(0xFFEB3B);
  if (alt < 11000)
    return lv_color_hex(0xFFA500);
  return lv_color_hex(0xFF4D4D);
}

lv_area_t Radar::plane_area_(const Plane &p) const {
  // Силуэт, подпись справа, хвост и отметка на ободе — всё, что может быть
  // нарисовано для самолёта
  int x1 = (int) p.px - 13, y1 = (int) p.py - 13, x2 = (int) p.px + 13, y2 = (int) p.py + 13;
  if (p.label) {
    const int w = 9 * (int) std::max(strlen(p.rt), strlen(p.cs)) + 6;
    x2 = std::max(x2, (int) p.px + 10 + w);
    y1 = std::min(y1, (int) p.py - 14);
  }
  for (int i = 0; i < p.ntr; i++) {
    x1 = std::min(x1, (int) p.tr[i][0] - 2);
    y1 = std::min(y1, (int) p.tr[i][1] - 2);
    x2 = std::max(x2, (int) p.tr[i][0] + 2);
    y2 = std::max(y2, (int) p.tr[i][1] + 2);
  }
  // Экранные координаты холста: центр — 233, 233
  lv_area_t a = {x1 + 233, y1 + 233, x2 + 233, y2 + 233};
  const float d = sqrtf(p.px * p.px + p.py * p.py);
  if (d > R_PX) {
    const int ex = 233 + (int) (p.px / d * (R_PX + 2)), ey = 233 + (int) (p.py / d * (R_PX + 2));
    a = {ex - 5, ey - 5, ex + 5, ey + 5};
  }
  return a;
}

void Radar::invalidate_plane_(const Plane &p) {
  if (!this->view_ || !p.drawn)
    return;
  lv_area_t oc;
  lv_obj_get_coords(this->view_, &oc);
  lv_area_t a = {p.area.x1 + oc.x1, p.area.y1 + oc.y1, p.area.x2 + oc.x1, p.area.y2 + oc.y1};
  lv_obj_invalidate_area(this->view_, &a);
}

void Radar::apply_view_(const View &v) {
  if (this->view_known_ && v == this->view_cfg_)
    return;
  const View old = this->view_cfg_;
  const bool first = !this->view_known_;
  this->view_cfg_ = v;
  this->view_known_ = true;
  ESP_LOGI(TAG, "Вид: карта %s, %d км, подписи %d, сетка %d, цвет %d, силуэты %d, обод %d", v.map, v.r, v.labels,
           v.grid, v.alt_color, v.shapes, v.rim);
  // Первый раз карта уже скачана в стиле сервера по умолчанию — он и есть
  // выбранный, качать заново не нужно
  if (!first && strcmp(old.map, v.map) != 0)
    this->map_req_ = this->map_shown_;
  if (v.r > 0 && (first ? v.r != this->radius() : v.r != old.r))
    this->set_radius_(v.r);
  // Подписи и границы самолётов — заново, весь холст — перерисовать
  this->layout_labels_();
  for (auto &p : this->planes_)
    p.area = this->plane_area_(p);
  if (this->selected_ >= 0)
    this->show_card_(this->selected_);
  if (this->view_)
    lv_obj_invalidate(this->view_);
}

lv_color_t Radar::plane_color_(int32_t alt) const {
  // Без цвета по высоте — все янтарные, как на веб-странице
  return this->view_cfg_.alt_color ? alt_color_of(alt) : lv_color_hex(0xFFC107);
}

void Radar::apply_(std::vector<Plane> &fresh, int r) {
  const uint32_t now = millis();
  // Тот же самолёт — плавно доезжает с того места, где нарисован
  for (auto &n : fresh) {
    for (const auto &o : this->planes_) {
      if (strcmp(o.id, n.id) != 0)
        continue;
      if (r == this->shown_r_ && o.drawn) {
        n.fx = o.px - n.x;
        n.fy = o.py - n.y;
      }
      break;
    }
    n.px = n.x + n.fx;
    n.py = n.y + n.fy;
  }
  for (const auto &o : this->planes_)
    this->invalidate_plane_(o);
  this->planes_ = std::move(fresh);
  this->data_ms_ = now;
  this->shown_r_ = r;
  this->loaded_ = true;
  // Выбранный самолёт мог улететь
  this->selected_ = -1;
  for (size_t i = 0; i < this->planes_.size(); i++)
    if (this->sel_id_[0] && strcmp(this->planes_[i].id, this->sel_id_) == 0)
      this->selected_ = (int) i;
  if (this->selected_ < 0)
    this->hide_card_();
  else
    this->show_card_(this->selected_);
  this->layout_labels_();
  for (auto &p : this->planes_) {
    p.area = this->plane_area_(p);
    p.drawn = true;
    this->invalidate_plane_(p);
  }
  this->update_status_();
}

void Radar::layout_labels_() {
  // Подписи — ближним к дому в первую очередь; где наползает на уже
  // подписанный, подписи нет. Самолёты в списке уже по удалённости
  std::vector<lv_area_t> used;
  for (auto &p : this->planes_) {
    p.label = false;
    if (!this->view_cfg_.labels)
      continue;
    if (p.px * p.px + p.py * p.py > (float) (R_PX - 10) * (R_PX - 10))
      continue;
    const char *t = p.rt[0] ? p.rt : p.cs;
    if (!t[0])
      continue;
    const int w = 9 * (int) strlen(t) + 4;
    lv_area_t a = {(int) p.px + 10, (int) p.py - 14, (int) p.px + 10 + w, (int) p.py + 4};
    bool free = true;
    for (const auto &u : used)
      if (a.x1 <= u.x2 && a.x2 >= u.x1 && a.y1 <= u.y2 && a.y2 >= u.y1)
        free = false;
    if (free) {
      p.label = true;
      used.push_back(a);
    }
  }
}

void Radar::update_status_() {
  if (!this->status_)
    return;
  if (this->url_copy_().empty()) {
    lv_label_set_text(this->status_, "Сервер радара не задан");
    return;
  }
  if (this->fails_ >= 2) {
    lv_label_set_text(this->status_, "Нет связи с сервером радара");
    return;
  }
  if (!this->loaded_)
    return;
  const Plane *near = nullptr;
  for (const auto &p : this->planes_)
    if (!near || p.dist < near->dist)
      near = &p;
  if (!near) {
    lv_label_set_text(this->status_, "В небе пусто");
    return;
  }
  char buf[96];
  const char *name = near->ty[0] ? near->ty : (near->cs[0] ? near->cs : "самолёт");
  const int alt = near->alt / 100 * 100;
  if (alt >= 1000)
    snprintf(buf, sizeof(buf), "Ближайший: %s · %d км · %d %03d м", name, (int) lroundf(near->dist), alt / 1000,
             alt % 1000);
  else
    snprintf(buf, sizeof(buf), "Ближайший: %s · %d км · %d м", name, (int) lroundf(near->dist), alt);
  lv_label_set_text(this->status_, buf);
}

void Radar::show_card_(int idx) {
  if (!this->card_ || idx < 0 || idx >= (int) this->planes_.size())
    return;
  const Plane &p = this->planes_[idx];
  strncpy(this->sel_id_, p.id, sizeof(this->sel_id_) - 1);
  char title[40], body[160];
  snprintf(title, sizeof(title), "%s%s%s", p.cs[0] ? p.cs : "—", p.ty[0] ? " · " : "", p.ty);
  char route[24] = "";
  if (p.rt[0]) {
    // «SVO-KUF» → «SVO → KUF»
    const char *dash = strchr(p.rt, '-');
    if (dash)
      snprintf(route, sizeof(route), "%.*s → %s", (int) (dash - p.rt), p.rt, dash + 1);
    else
      snprintf(route, sizeof(route), "%s", p.rt);
  }
  // Высота с пробелом между тысячами: «10 600 м»
  char alt[16];
  const int a = p.alt / 10 * 10;
  if (a >= 1000)
    snprintf(alt, sizeof(alt), "%d %03d м", a / 1000, a % 1000);
  else
    snprintf(alt, sizeof(alt), "%d м", a);
  snprintf(body, sizeof(body), "%s%s%s%s%s · %d км/ч\n%d км от дома", p.al, p.al[0] ? "\n" : "", route,
           route[0] ? "\n" : "", alt, p.spd, (int) lroundf(p.dist));
  lv_label_set_text(this->card_title_, title);
  lv_label_set_text(this->card_body_, body);
  lv_obj_set_style_border_color(this->card_, this->plane_color_(p.alt), 0);
  lv_obj_clear_flag(this->card_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->card_);
  this->card_ms_ = millis();
}

void Radar::hide_card_() {
  if (this->selected_ >= 0 && this->selected_ < (int) this->planes_.size())
    this->invalidate_plane_(this->planes_[this->selected_]);
  this->selected_ = -1;
  this->sel_id_[0] = 0;
  if (this->card_)
    lv_obj_add_flag(this->card_, LV_OBJ_FLAG_HIDDEN);
}

void Radar::on_event(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_DRAW_MAIN) {
    this->paint(lv_event_get_layer(e));
    return;
  }
  lv_indev_t *indev = lv_indev_active();
  if (lv_event_get_current_target(e) == this->card_) {
    this->hide_card_();
    return;
  }
  if (code == LV_EVENT_PRESSED) {
    if (indev)
      lv_indev_get_point(indev, &this->press_);
    return;
  }
  // Касание, а не листание: палец почти не сдвинулся
  lv_point_t pt{};
  if (indev)
    lv_indev_get_point(indev, &pt);
  if (std::abs(pt.x - this->press_.x) > 14 || std::abs(pt.y - this->press_.y) > 14)
    return;
  lv_area_t oc;
  lv_obj_get_coords(this->view_, &oc);
  const float tx = pt.x - oc.x1 - 233, ty = pt.y - oc.y1 - 233;
  int best = -1;
  float bd = 30.0f * 30.0f;
  for (size_t i = 0; i < this->planes_.size(); i++) {
    const Plane &p = this->planes_[i];
    if (p.px * p.px + p.py * p.py > (float) R_PX * R_PX)
      continue;
    const float d = (p.px - tx) * (p.px - tx) + (p.py - ty) * (p.py - ty);
    if (d < bd) {
      bd = d;
      best = (int) i;
    }
  }
  if (best < 0) {
    this->hide_card_();
    return;
  }
  if (this->selected_ >= 0 && this->selected_ < (int) this->planes_.size())
    this->invalidate_plane_(this->planes_[this->selected_]);
  this->selected_ = best;
  this->show_card_(best);
  this->invalidate_plane_(this->planes_[best]);
}

void Radar::tick() {
  if (!this->view_)
    return;
  std::vector<Plane> fresh;
  bool got = false, fail = false, got_view = false;
  int r = 0;
  View view;
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    if (this->has_view_) {
      view = this->pending_view_;
      this->has_view_ = false;
      got_view = true;
    }
    if (this->has_pending_) {
      fresh = std::move(this->pending_);
      this->has_pending_ = false;
      r = this->pending_r_;
      got = true;
    }
    if (this->fail_changed_) {
      this->fail_changed_ = false;
      fail = true;
    }
  }
  if (got_view)
    this->apply_view_(view);
  if (got && r == this->radius())
    this->apply_(fresh, r);
  if (fail)
    this->update_status_();
  // Карточка закрывается сама через 15 секунд
  if (this->selected_ >= 0 && millis() - this->card_ms_ > 15000)
    this->hide_card_();
  if (!this->active_.load() || this->planes_.empty())
    return;

  // Самолёты летят по курсу; поправка к новым данным тает за 1,5 с
  const float t = (millis() - this->data_ms_) / 1000.0f;
  const float blend = std::max(0.0f, 1.0f - t * 1000.0f / BLEND_MS);
  bool moved_any = false;
  for (auto &p : this->planes_) {
    const float nx = p.x + p.vx * t + p.fx * blend, ny = p.y + p.vy * t + p.fy * blend;
    if (std::fabs(nx - p.px) < 0.5f && std::fabs(ny - p.py) < 0.5f)
      continue;
    this->invalidate_plane_(p);
    p.px = nx;
    p.py = ny;
    moved_any = true;
  }
  if (moved_any) {
    this->layout_labels_();
    for (auto &p : this->planes_) {
      const lv_area_t a = this->plane_area_(p);
      if (a.x1 != p.area.x1 || a.y1 != p.area.y1 || a.x2 != p.area.x2 || a.y2 != p.area.y2) {
        this->invalidate_plane_(p);
        p.area = a;
        this->invalidate_plane_(p);
      }
    }
  }
}

// Силуэты: треугольники в осях самолёта (нос — вверх, −y)
struct Tri {
  float x0, y0, x1, y1, x2, y2;
};
static const Tri JET[] = {
    {0, -9, 1.7f, -4, -1.7f, -4},   {-1.7f, -4, 1.7f, -4, 1.7f, 6}, {-1.7f, -4, 1.7f, 6, -1.7f, 6},
    {-1.5f, -2, -9, 3, -1.5f, 2},   {1.5f, -2, 9, 3, 1.5f, 2},      {-1.2f, 4, -4.5f, 8, -1.2f, 6.5f},
    {1.2f, 4, 4.5f, 8, 1.2f, 6.5f},
};
static const Tri HEAVY[] = {
    {0, -11, 2.2f, -5, -2.2f, -5}, {-2.2f, -5, 2.2f, -5, 2.2f, 7}, {-2.2f, -5, 2.2f, 7, -2.2f, 7},
    {-2, -3, -12, 4, -2, 2},       {2, -3, 12, 4, 2, 2},           {-1.5f, 5, -6, 10, -1.5f, 8},
    {1.5f, 5, 6, 10, 1.5f, 8},
};
// Без силуэтов — одна стрелка
static const Tri ARROW[] = {
    {0, -9, 6.5f, 7, 0, 3},
    {0, -9, 0, 3, -6.5f, 7},
};
static const Tri PROP[] = {
    {0, -8, 1.4f, -4, -1.4f, -4}, {-1.4f, -4, 1.4f, -4, 1.4f, 6}, {-1.4f, -4, 1.4f, 6, -1.4f, 6},
    {-8, -3, 8, -3, 8, -0.5f},    {-8, -3, 8, -0.5f, -8, -0.5f},  {-3.5f, 5, 3.5f, 5, 3.5f, 6.5f},
    {-3.5f, 5, 3.5f, 6.5f, -3.5f, 6.5f},
};

void Radar::paint(lv_layer_t *layer) {
  lv_area_t oc;
  lv_obj_get_coords(this->view_, &oc);
  const lv_area_t &clip = layer->_clip_area;
  const int cx = oc.x1 + 233, cy = oc.y1 + 233;
  auto hit = [&clip](const lv_area_t &a) {
    return a.x1 <= clip.x2 && a.x2 >= clip.x1 && a.y1 <= clip.y2 && a.y2 >= clip.y1;
  };

  // Кольца дальности — каждая четверть радиуса — с подписями километров
  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.center = {cx, cy};
  arc.width = 1;
  arc.start_angle = 0;
  arc.end_angle = 360;
  arc.color = lv_color_hex(0x4A5F7A);
  arc.opa = 170;
  lv_draw_label_dsc_t lb;
  lv_draw_label_dsc_init(&lb);
  lb.font = this->font_;
  lb.opa = LV_OPA_COVER;
  lb.flag = LV_TEXT_FLAG_EXPAND;
  const int x1 = clip.x1 - cx, x2 = clip.x2 - cx, y1 = clip.y1 - cy, y2 = clip.y2 - cy;
  const int nx = std::max(x1, std::max(0, -x2)), ny = std::max(y1, std::max(0, -y2));
  const int fx = std::max(std::abs(x1), std::abs(x2)), fy = std::max(std::abs(y1), std::abs(y2));
  const int near2 = nx * nx + ny * ny, far2 = fx * fx + fy * fy;
  const int r_km = this->shown_r_ ? this->shown_r_ : this->radius();
  const bool grid = this->view_cfg_.grid;
  for (int i = 1; i <= 4 && grid; i++) {
    const int rr = R_PX * i / 4;
    if (near2 <= (rr + 2) * (rr + 2) && far2 >= (rr - 2) * (rr - 2)) {
      arc.radius = rr;
      lv_draw_arc(layer, &arc);
    }
    char t[20];
    snprintf(t, sizeof(t), "%d км", r_km * i / 4);
    const int lx = cx + (int) (rr * 0.707f) + 3, ly = cy - (int) (rr * 0.707f) - 16;
    lv_area_t ta = {lx, ly, lx + 60, ly + 16};
    if (hit(ta)) {
      lb.text = t;
      lb.color = lv_color_hex(0x7F93AD);
      lv_draw_label(layer, &lb, &ta);
    }
  }
  // Стороны света
  static const char *const SIDE[4] = {"С", "В", "Ю", "З"};
  static const int SDX[4] = {0, 1, 0, -1}, SDY[4] = {-1, 0, 1, 0};
  for (int i = 0; i < 4 && grid; i++) {
    const int sx = cx + SDX[i] * (R_PX - 14), sy = cy + SDY[i] * (R_PX - 14);
    lv_area_t ta = {sx - 10, sy - 9, sx + 10, sy + 9};
    if (!hit(ta))
      continue;
    lb.text = SIDE[i];
    lb.color = lv_color_hex(i == 0 ? 0xFF8A80 : 0xA9B8CC);
    lb.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(layer, &lb, &ta);
    lb.align = LV_TEXT_ALIGN_AUTO;
  }
  // Дом
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.radius = LV_RADIUS_CIRCLE;
  {
    lv_area_t h = {cx - 6, cy - 6, cx + 6, cy + 6};
    if (hit(h)) {
      fill.color = lv_color_hex(0x5AC8FA);
      fill.opa = 90;
      lv_draw_fill(layer, &fill, &h);
      lv_area_t d = {cx - 3, cy - 3, cx + 3, cy + 3};
      fill.opa = LV_OPA_COVER;
      lv_draw_fill(layer, &fill, &d);
    }
  }

  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.width = 2;
  ln.round_start = 1;
  ln.round_end = 1;
  lv_draw_triangle_dsc_t tr;
  lv_draw_triangle_dsc_init(&tr);
  tr.opa = LV_OPA_COVER;
  for (size_t i = 0; i < this->planes_.size(); i++) {
    const Plane &p = this->planes_[i];
    if (!p.drawn)
      continue;
    lv_area_t pa = {p.area.x1 + oc.x1, p.area.y1 + oc.y1, p.area.x2 + oc.x1, p.area.y2 + oc.y1};
    if (!hit(pa))
      continue;
    const lv_color_t col = this->plane_color_(p.alt);
    const float d = sqrtf(p.px * p.px + p.py * p.py);
    if (d > R_PX) {
      if (!this->view_cfg_.rim)
        continue;
      // За кругом — красная точка на ободе в его сторону
      const int ex = cx + (int) (p.px / d * (R_PX + 2)), ey = cy + (int) (p.py / d * (R_PX + 2));
      lv_area_t m = {ex - 3, ey - 3, ex + 3, ey + 3};
      fill.color = lv_color_hex(0xFF4D4D);
      fill.opa = LV_OPA_COVER;
      lv_draw_fill(layer, &fill, &m);
      continue;
    }
    const float px = cx + p.px, py = cy + p.py;
    // Хвост — откуда прилетел, гаснущим цветом высоты
    if (p.ntr) {
      ln.color = col;
      ln.opa = 110;
      for (int k = 0; k < p.ntr; k++) {
        ln.p1 = {(lv_value_precise_t) (cx + p.tr[k][0]), (lv_value_precise_t) (cy + p.tr[k][1])};
        ln.p2 = k + 1 < p.ntr
                    ? lv_point_precise_t{(lv_value_precise_t) (cx + p.tr[k + 1][0]), (lv_value_precise_t) (cy + p.tr[k + 1][1])}
                    : lv_point_precise_t{(lv_value_precise_t) px, (lv_value_precise_t) py};
        lv_draw_line(layer, &ln);
      }
    }
    // Выбранный — в светлом кольце
    if ((int) i == this->selected_) {
      lv_area_t s = {(int) px - 12, (int) py - 12, (int) px + 12, (int) py + 12};
      fill.color = lv_color_white();
      fill.opa = 60;
      lv_draw_fill(layer, &fill, &s);
      fill.opa = LV_OPA_COVER;
    }
    // Силуэт по курсу
    const Tri *tris = !this->view_cfg_.shapes ? ARROW : (p.kind == 1 ? HEAVY : (p.kind == 2 ? PROP : JET));
    const int ntris = this->view_cfg_.shapes ? 7 : 2;
    const float a = p.hdg * (PI_F / 180.0f), ca = cosf(a), sa = sinf(a);
    tr.color = col;
    for (int k = 0; k < ntris; k++) {
      const Tri &t = tris[k];
      const float xs[3] = {t.x0, t.x1, t.x2}, ys[3] = {t.y0, t.y1, t.y2};
      for (int v = 0; v < 3; v++)
        tr.p[v] = {(lv_value_precise_t) (px + xs[v] * ca - ys[v] * sa), (lv_value_precise_t) (py + xs[v] * sa + ys[v] * ca)};
      lv_draw_triangle(layer, &tr);
    }
    // Подпись: маршрут или позывной
    if (p.label) {
      lb.text = p.rt[0] ? p.rt : p.cs;
      lb.color = lv_color_hex(0xE6EDF5);
      lv_area_t ta = {(int) px + 10, (int) py - 14, (int) px + 10 + 9 * (int) strlen(lb.text) + 4, (int) py + 4};
      lv_draw_label(layer, &lb, &ta);
    }
  }
}

}  // namespace radar
}  // namespace esphome

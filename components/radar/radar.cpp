#include "radar.h"
#include "aircraft_icons.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_http_client.h>
// lv_image_cache_drop — во внутренних заголовках LVGL
#include <lvgl_private.h>

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
    // Открыли страницу — на пару секунд показать радиус
    this->show_title_();
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
  this->show_title_();
  if (this->task_)
    xTaskNotifyGive(this->task_);
}

// ---------------------------------------------------------------------------
// Загрузка — в своей задаче

void Radar::task_fn(void *arg) {
  auto *self = static_cast<Radar *>(arg);
  for (;;) {
    const bool radar = self->active_.load(), track = self->trk_active_.load();
    if ((!radar && !track) || self->url_copy_().empty() || !self->body_) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }
    if (radar)
      self->fetch_();
    else
      self->fetch_track_();
    // Масштаб сменили, рейс поменяли или страницу открыли заново — задача
    // просыпается раньше. Рейс обновляется раз в 15 секунд
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(radar ? self->interval_ms_ : 15000));
  }
}

// GET в буфер body_; длина ответа — в len
static bool http_get(const std::string &url, uint8_t *body, size_t &len) {
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str();
  cfg.timeout_ms = 7000;
  len = 0;
  bool ok = false;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c) {
    if (esp_http_client_open(c, 0) == ESP_OK) {
      esp_http_client_fetch_headers(c);
      if (esp_http_client_get_status_code(c) == 200) {
        int n;
        while (len < BODY_MAX && (n = esp_http_client_read(c, (char *) body + len, BODY_MAX - len)) > 0)
          len += n;
        ok = len > 0 && len < BODY_MAX;
      }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
  return ok;
}

void Radar::set_flight(const std::string &flight) {
  std::string f;
  for (char ch : flight)
    if (isalnum((unsigned char) ch) && f.size() < 10)
      f += (char) toupper((unsigned char) ch);
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    if (f == this->flight_)
      return;
    this->flight_ = f;
  }
  // Другой рейс — прежний с экрана долой, пока не придёт новый
  Track t;
  strcpy(t.st, f.empty() ? "none" : "wait");
  this->apply_track_(t);
  if (this->task_)
    xTaskNotifyGive(this->task_);
}

void Radar::fetch_track_() {
  std::string flight;
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    flight = this->flight_;
  }
  Track t;
  if (flight.empty()) {
    strcpy(t.st, "none");
  } else {
    size_t len = 0;
    const std::string url = this->url_copy_() + "/flight?f=" + flight;
    if (!http_get(url, this->body_, len)) {
      strcpy(t.st, "err");
    } else {
      JsonDocument doc = json::parse_json(this->body_, len);
      strncpy(t.st, doc["st"] | "err", sizeof(t.st) - 1);
      strncpy(t.n, doc["n"] | flight.c_str(), sizeof(t.n) - 1);
      strncpy(t.cs, doc["cs"] | "", sizeof(t.cs) - 1);
      strncpy(t.al, doc["al"] | "", sizeof(t.al) - 1);
      strncpy(t.tl, doc["tl"] | "", sizeof(t.tl) - 1);
      strncpy(t.oc, doc["o"]["c"] | "", sizeof(t.oc) - 1);
      strncpy(t.on, doc["o"]["n"] | "", sizeof(t.on) - 1);
      strncpy(t.dc, doc["d"]["c"] | "", sizeof(t.dc) - 1);
      strncpy(t.dn, doc["d"]["n"] | "", sizeof(t.dn) - 1);
      strncpy(t.ot, doc["ot"] | "", sizeof(t.ot) - 1);
      strncpy(t.dep, doc["dep"] | "", sizeof(t.dep) - 1);
      strncpy(t.eta, doc["eta"] | "", sizeof(t.eta) - 1);
      const int ic = doc["ic"] | 5;
      t.icon = (uint8_t) (ic >= 0 && ic < AC_ICONS ? ic : 5);
      for (JsonArray pt : doc["pt"].as<JsonArray>()) {
        if (t.npt >= Track::NPT)
          break;
        t.pt[t.npt][0] = pt[0] | 0;
        t.pt[t.npt][1] = pt[1] | 0;
        t.npt++;
      }
      JsonObject pl = doc["p"].as<JsonObject>();
      if (!pl.isNull()) {
        t.plane = true;
        t.px = pl["x"] | 0;
        t.py = pl["y"] | 0;
        t.ph = pl["h"] | 0;
        t.pi = pl["i"] | 0;
      }
      t.alt = doc["a"] | 0;
      t.spd = doc["s"] | 0;
      t.hdg = doc["h"] | 0;
      t.ver = doc["v"] | 0u;
    }
  }
  std::lock_guard<std::mutex> lock(this->mtx_);
  this->trk_pending_ = t;
  this->has_trk_ = true;
}

void Radar::fetch_() {
  const int r = this->radius();
  const std::string url = this->url_copy_() + "/esp?r=" + std::to_string(r);
  size_t len = 0;
  bool ok = http_get(url, this->body_, len);
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
        strncpy(p.num, o["n"] | "", sizeof(p.num) - 1);
        strncpy(p.tl, o["tl"] | "", sizeof(p.tl) - 1);
        strncpy(p.on, o["on"] | "", sizeof(p.on) - 1);
        strncpy(p.dn, o["dn"] | "", sizeof(p.dn) - 1);
        const int ic = o["ic"] | 5;  // по умолчанию — узкофюзеляжный
        p.icon = (uint8_t) (ic >= 0 && ic < AC_ICONS ? ic : 5);
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
        strncpy(vw.hn, v["hn"] | "", sizeof(vw.hn) - 1);
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
// Экран — в потоке LVGL.
//
// Вид — по экрану «Local radar» из AirESP32ace (Vadim Malis, MIT): тёмная
// карта, бледно-зелёные кольца, самолёты — зелёными силуэтами своего типа
// по курсу, позывные рядом, в центре — город.
// Касание по самолёту — карточка во весь экран: авиакомпания, рисунок
// самолёта сбоку, модель, откуда и куда, номер рейса, высота, скорость, курс

static const uint32_t C_RING = 0x0B4D1C;     // кольца
static const uint32_t C_PLANE = 0x009428;    // силуэт
static const uint32_t C_PLANE_SEL = 0x3DFF6A;  // выбранный
static const uint32_t C_LABEL = 0x24BD38;    // позывной
static const uint32_t C_TRAIL = 0x0A7625;    // хвост и отметки за кругом
static const uint32_t TITLE_MS = 2500;       // сколько виден радиус после смены

static void view_event_cb(lv_event_t *e) { static_cast<Radar *>(lv_event_get_user_data(e))->on_event(e); }
static void tick_cb(lv_timer_t *t) { static_cast<Radar *>(lv_timer_get_user_data(t))->tick(); }

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_CENTER, x, y);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}

void Radar::bind(lv_obj_t *view, lv_obj_t *status, lv_obj_t *title, lv_obj_t *card, const lv_font_t *font,
                 const lv_font_t *font_cs, const lv_font_t *font_mid, const lv_font_t *font_big) {
  this->view_ = view;
  this->status_ = status;
  this->title_ = title;
  this->card_ = card;
  this->font_ = font;
  this->font_cs_ = font_cs ? font_cs : font;
  this->font_mid_ = font_mid ? font_mid : font;
  this->font_big_ = font_big ? font_big : font;
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_DRAW_MAIN, this);
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_PRESSED, this);
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_CLICKED, this);
  if (card) {
    lv_obj_add_event_cb(card, view_event_cb, LV_EVENT_CLICKED, this);
    // Карточка: сверху вниз — авиакомпания, рисунок, модель, откуда и куда,
    // номер рейса, высота / скорость / курс, позывной
    // Рисунок — первым: подписи ложатся поверх его пустых краёв
    this->d_img_ = lv_image_create(card);
    lv_obj_align(this->d_img_, LV_ALIGN_CENTER, 0, -98);
    this->d_air_ = mk_label(card, this->font_mid_, 0xBFC7CF, 0, -172);
    lv_obj_set_style_image_recolor(this->d_img_, lv_color_hex(0x1FB04A), 0);
    lv_obj_set_style_image_recolor_opa(this->d_img_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(this->d_img_, LV_OBJ_FLAG_CLICKABLE);
    this->d_type_ = mk_label(card, this->font_, 0x8A949E, 0, -28);
    this->d_o_ = mk_label(card, this->font_big_, 0xFFFFFF, -118, 10);
    this->d_oc_ = mk_label(card, this->font_, 0x8A949E, -118, 42);
    this->d_d_ = mk_label(card, this->font_big_, 0xFFFFFF, 118, 10);
    this->d_dc_ = mk_label(card, this->font_, 0x8A949E, 118, 42);
    // Линия маршрута с точкой посередине
    lv_obj_t *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 110, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x0A7625), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 10);
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(C_LABEL), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 10);
    this->d_num_ = mk_label(card, this->font_big_, 0xFFFFFF, 0, 82);
    static const char *const CAPS[3] = {"высота", "скорость", "курс"};
    for (int i = 0; i < 3; i++) {
      this->d_cap_[i] = mk_label(card, this->font_, 0x6E7881, (i - 1) * 118, 124);
      lv_label_set_text(this->d_cap_[i], CAPS[i]);
      this->d_val_[i] = mk_label(card, this->font_mid_, 0xE6EDF5, (i - 1) * 118, 150);
    }
    this->d_cs_ = mk_label(card, this->font_, 0x6E7881, 0, 186);
  }
  if (title)
    this->show_title_();
  if (status) {
    lv_label_set_text(status, this->url_copy_().empty() ? "Сервер радара не задан" : "");
    if (this->url_copy_().empty())
      lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
  }
  // Самолёты ползут медленно: 4 шага в секунду хватает с запасом
  lv_timer_create(tick_cb, 250, this);
}

void Radar::show_title_() {
  if (!this->title_)
    return;
  char buf[32];
  snprintf(buf, sizeof(buf), "%d км", this->radius());
  lv_label_set_text(this->title_, buf);
  lv_obj_clear_flag(this->title_, LV_OBJ_FLAG_HIDDEN);
  this->title_ms_ = millis();
}

// ---- Силуэты ----------------------------------------------------------------

const lv_image_dsc_t *Radar::sprite_(int icon, int hdg) {
  const int step = ((hdg % 360 + 360) % 360 + 2) / 5 % 72;
  const int key = icon * 72 + step;
  this->spr_clock_++;
  SprEnt *slot = nullptr;
  for (auto &e : this->spr_)
    if (e.key == key) {
      e.used = this->spr_clock_;
      return &e.dsc;
    }
  for (auto &e : this->spr_)
    if (!slot || e.used < slot->used)
      slot = &e;
  if (!slot->buf) {
    slot->buf = static_cast<uint8_t *>(heap_caps_malloc(AC_MAP * AC_MAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slot->buf)
      slot->buf = static_cast<uint8_t *>(malloc(AC_MAP * AC_MAP));
    if (!slot->buf)
      return nullptr;
  }
  // Поворот маски по часовой на курс: для каждой точки результата — точка
  // исходника, с усреднением четырёх соседей. Исходник — 4 бита на точку,
  // носом вниз (как в AirESP32ace): поворот на курс + 180°
  const uint8_t *src = AC_ICON[icon].map;
  auto at = [src](int x, int y) -> int {
    if (x < 0 || y < 0 || x >= AC_MAP || y >= AC_MAP)
      return 0;
    const uint8_t b = src[(y * AC_MAP + x) >> 1];
    return ((x & 1) ? (b & 0x0F) : (b >> 4)) * 17;
  };
  const float a = (step * 5.0f + 180.0f) * (PI_F / 180.0f), ca = cosf(a), sa = sinf(a), c = (AC_MAP - 1) / 2.0f;
  for (int y = 0; y < AC_MAP; y++) {
    for (int x = 0; x < AC_MAP; x++) {
      const float dx = x - c, dy = y - c;
      const float sx = c + dx * ca + dy * sa, sy = c - dx * sa + dy * ca;
      const int ix = (int) floorf(sx), iy = (int) floorf(sy);
      const float fx = sx - ix, fy = sy - iy;
      const float v = at(ix, iy) * (1 - fx) * (1 - fy) + at(ix + 1, iy) * fx * (1 - fy) +
                      at(ix, iy + 1) * (1 - fx) * fy + at(ix + 1, iy + 1) * fx * fy;
      slot->buf[y * AC_MAP + x] = (uint8_t) std::min(255.0f, v + 0.5f);
    }
  }
  lv_image_dsc_t &d = slot->dsc;
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_A8;
  d.header.flags = 0;
  d.header.w = AC_MAP;
  d.header.h = AC_MAP;
  d.header.stride = AC_MAP;
  d.data_size = AC_MAP * AC_MAP;
  d.data = slot->buf;
  lv_image_cache_drop(&d);
  slot->key = key;
  slot->used = this->spr_clock_;
  return &d;
}

// ---- Самолёты ---------------------------------------------------------------

static const char *label_of(const Plane &p) { return p.cs[0] ? p.cs : (p.num[0] ? p.num : p.rt); }

lv_area_t Radar::plane_area_(const Plane &p) const {
  // Силуэт 50×50, позывной справа, хвост и отметка на ободе — всё, что
  // может быть нарисовано для самолёта
  int x1 = (int) p.px - 26, y1 = (int) p.py - 26, x2 = (int) p.px + 26, y2 = (int) p.py + 26;
  if (p.label)
    x2 = std::max(x2, (int) p.px + 20 + 11 * (int) strlen(label_of(p)) + 6);
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

lv_color_t Radar::plane_color_(int32_t) const { return lv_color_hex(C_PLANE); }

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
  // Позывные — ближним к дому в первую очередь; где наползает на уже
  // подписанный, подписи нет. Самолёты в списке уже по удалённости
  std::vector<lv_area_t> used;
  for (auto &p : this->planes_) {
    p.label = false;
    if (!this->view_cfg_.labels)
      continue;
    if (p.px * p.px + p.py * p.py > (float) (R_PX - 10) * (R_PX - 10))
      continue;
    const char *t = label_of(p);
    if (!t[0])
      continue;
    const int w = 11 * (int) strlen(t) + 4;
    lv_area_t a = {(int) p.px + 20, (int) p.py - 11, (int) p.px + 20 + w, (int) p.py + 11};
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
  // Строка внизу — только когда что-то не так
  const char *msg = nullptr;
  if (this->url_copy_().empty())
    msg = "Сервер радара не задан";
  else if (this->fails_ >= 2)
    msg = "Нет связи с сервером радара";
  else if (this->loaded_ && this->planes_.empty())
    msg = "В небе пусто";
  if (msg) {
    lv_label_set_text(this->status_, msg);
    lv_obj_clear_flag(this->status_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(this->status_, LV_OBJ_FLAG_HIDDEN);
  }
}

static const char *compass16(int hdg) {
  static const char *const W[16] = {"С", "ССВ", "СВ", "ВСВ", "В", "ВЮВ", "ЮВ", "ЮЮВ",
                                    "Ю", "ЮЮЗ", "ЮЗ", "ЗЮЗ", "З", "ЗСЗ", "СЗ", "ССЗ"};
  return W[((hdg % 360 + 360) % 360 * 16 + 180) / 360 % 16];
}

void Radar::show_card_(int idx) {
  if (!this->card_ || idx < 0 || idx >= (int) this->planes_.size())
    return;
  const Plane &p = this->planes_[idx];
  strncpy(this->sel_id_, p.id, sizeof(this->sel_id_) - 1);
  // Рисунок самолёта сбоку — свой или семейства; распаковка 4 → 8 бит
  int ic = p.icon < AC_ICONS ? p.icon : 5;
  if (!AC_ICON[ic].detail)
    ic = AC_ICON[ic].family;
  if (ic != this->det_icon_ && AC_ICON[ic].detail && this->d_img_) {
    // Рисунок в AirESP32ace хранится повёрнутым на 180° — разворачиваем, как
    // их прошивка, обрезаем пустые строки сверху и снизу (чтобы не наезжал на
    // подписи) и увеличиваем до DET_W в ширину. Масштаб — здесь, с
    // усреднением соседних точек: масштаб LVGL для масок A8 на плате даёт мусор
    const int sw = AC_ICON[ic].dw, sh = AC_ICON[ic].dh;
    const uint8_t *src = AC_ICON[ic].detail;
    auto at = [src, sw, sh](int x, int y) -> float {
      if (x < 0 || y < 0 || x >= sw || y >= sh)
        return 0.0f;
      const int i = y * sw + x;
      const uint8_t b = src[i >> 1];
      return ((i & 1) ? (b & 0x0F) : (b >> 4)) * 17.0f;
    };
    int y0 = sh, y1 = -1;
    for (int y = 0; y < sh; y++)
      for (int x = 0; x < sw; x++)
        if (at(x, y) > 0) {
          y0 = std::min(y0, y);
          y1 = std::max(y1, y);
          break;
        }
    if (y1 < y0)
      y0 = 0, y1 = sh - 1;
    const float k = (float) DET_W / sw;  // точек экрана на точку исходника
    const int w = DET_W, h = (int) ((y1 - y0 + 3) * k);
    if (!this->det_buf_)
      this->det_buf_ = static_cast<uint8_t *>(heap_caps_malloc(DET_W * DET_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (this->det_buf_ && h <= DET_H) {
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          // Точка исходника, уже развёрнутого: x → sw−1−x, y → y1+1−y
          const float fx = sw - 1 - ((x + 0.5f) / k - 0.5f), fy = y1 + 1 - ((y + 0.5f) / k - 0.5f);
          const int ix = (int) floorf(fx), iy = (int) floorf(fy);
          const float ax = fx - ix, ay = fy - iy;
          const float v = at(ix, iy) * (1 - ax) * (1 - ay) + at(ix + 1, iy) * ax * (1 - ay) +
                          at(ix, iy + 1) * (1 - ax) * ay + at(ix + 1, iy + 1) * ax * ay;
          this->det_buf_[y * w + x] = (uint8_t) std::min(255.0f, v + 0.5f);
        }
      }
      lv_image_dsc_t &d = this->det_dsc_;
      d.header.magic = LV_IMAGE_HEADER_MAGIC;
      d.header.cf = LV_COLOR_FORMAT_A8;
      d.header.flags = 0;
      d.header.w = w;
      d.header.h = h;
      d.header.stride = w;
      d.data_size = w * h;
      d.data = this->det_buf_;
      lv_image_cache_drop(&d);
      lv_image_set_src(this->d_img_, &d);
      this->det_icon_ = ic;
    }
  }
  lv_label_set_text(this->d_air_, p.al);
  lv_label_set_text(this->d_type_, p.tl[0] ? p.tl : p.ty);
  // «SVO-KUF» → SVO и KUF по краям
  char o[8] = "", d[8] = "";
  const char *dash = strchr(p.rt, '-');
  if (dash) {
    snprintf(o, sizeof(o), "%.*s", (int) std::min<ptrdiff_t>(dash - p.rt, 7), p.rt);
    snprintf(d, sizeof(d), "%s", dash + 1);
  }
  lv_label_set_text(this->d_o_, o[0] ? o : "—");
  lv_label_set_text(this->d_d_, d[0] ? d : "—");
  lv_label_set_text(this->d_oc_, p.on);
  lv_label_set_text(this->d_dc_, p.dn);
  lv_label_set_text(this->d_num_, p.num[0] ? p.num : (p.cs[0] ? p.cs : "—"));
  // Высота с пробелом между тысячами: «10 600 м»
  char buf[24];
  const int a = p.alt / 10 * 10;
  if (a >= 1000)
    snprintf(buf, sizeof(buf), "%d %03d м", a / 1000, a % 1000);
  else
    snprintf(buf, sizeof(buf), "%d м", a);
  lv_label_set_text(this->d_val_[0], buf);
  snprintf(buf, sizeof(buf), "%d км/ч", p.spd);
  lv_label_set_text(this->d_val_[1], buf);
  snprintf(buf, sizeof(buf), "%s %d°", compass16(p.hdg), (p.hdg % 360 + 360) % 360);
  lv_label_set_text(this->d_val_[2], buf);
  char foot[64];
  snprintf(foot, sizeof(foot), "%s · %d км от дома", p.cs[0] ? p.cs : "—", (int) lroundf(p.dist));
  lv_label_set_text(this->d_cs_, foot);
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
    if (lv_event_get_current_target(e) == this->trk_view_)
      this->paint_track(lv_event_get_layer(e));
    else
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
  {
    Track t;
    bool got_trk = false;
    {
      std::lock_guard<std::mutex> lock(this->mtx_);
      if (this->has_trk_) {
        t = this->trk_pending_;
        this->has_trk_ = false;
        got_trk = true;
      }
    }
    if (got_trk)
      this->apply_track_(t);
  }
  if (got && r == this->radius())
    this->apply_(fresh, r);
  if (fail)
    this->update_status_();
  // Карточка закрывается сама через 15 секунд
  if (this->selected_ >= 0 && millis() - this->card_ms_ > 15000)
    this->hide_card_();
  // Радиус виден пару секунд после открытия страницы и смены масштаба
  if (this->title_ && this->title_ms_ && millis() - this->title_ms_ > TITLE_MS) {
    this->title_ms_ = 0;
    lv_obj_add_flag(this->title_, LV_OBJ_FLAG_HIDDEN);
  }
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

// Без силуэтов — одна стрелка
struct Tri {
  float x0, y0, x1, y1, x2, y2;
};
static const Tri ARROW[] = {
    {0, -9, 6.5f, 7, 0, 3},
    {0, -9, 0, 3, -6.5f, 7},
};

void Radar::paint(lv_layer_t *layer) {
  lv_area_t oc;
  lv_obj_get_coords(this->view_, &oc);
  const lv_area_t &clip = layer->_clip_area;
  const int cx = oc.x1 + 233, cy = oc.y1 + 233;
  auto hit = [&clip](const lv_area_t &a) {
    return a.x1 <= clip.x2 && a.x2 >= clip.x1 && a.y1 <= clip.y2 && a.y2 >= clip.y1;
  };

  // Кольца дальности — треть, две трети и весь радиус
  if (this->view_cfg_.grid) {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center = {cx, cy};
    arc.width = 1;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.color = lv_color_hex(C_RING);
    arc.opa = 220;
    const int x1 = clip.x1 - cx, x2 = clip.x2 - cx, y1 = clip.y1 - cy, y2 = clip.y2 - cy;
    const int nx = std::max(x1, std::max(0, -x2)), ny = std::max(y1, std::max(0, -y2));
    const int fx = std::max(std::abs(x1), std::abs(x2)), fy = std::max(std::abs(y1), std::abs(y2));
    const int near2 = nx * nx + ny * ny, far2 = fx * fx + fy * fy;
    for (int i = 1; i <= 3; i++) {
      const int rr = R_PX * i / 3;
      if (near2 <= (rr + 2) * (rr + 2) && far2 >= (rr - 2) * (rr - 2)) {
        arc.radius = rr;
        lv_draw_arc(layer, &arc);
      }
    }
  }

  // Дом: точка и город под ней
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.radius = LV_RADIUS_CIRCLE;
  {
    lv_area_t h = {cx - 3, cy - 3, cx + 3, cy + 3};
    if (hit(h)) {
      fill.color = lv_color_hex(0xD8DEE4);
      fill.opa = LV_OPA_COVER;
      lv_draw_fill(layer, &fill, &h);
    }
    if (this->view_cfg_.hn[0]) {
      lv_area_t ta = {cx - 110, cy + 8, cx + 110, cy + 36};
      if (hit(ta)) {
        lv_draw_label_dsc_t lb;
        lv_draw_label_dsc_init(&lb);
        lb.font = this->font_mid_;
        lb.color = lv_color_hex(0xE6E6E6);
        lb.align = LV_TEXT_ALIGN_CENTER;
        lb.text = this->view_cfg_.hn;
        lv_draw_label(layer, &lb, &ta);
      }
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
  lv_draw_label_dsc_t lb;
  lv_draw_label_dsc_init(&lb);
  lb.font = this->font_cs_;
  lb.color = lv_color_hex(C_LABEL);
  lb.flag = LV_TEXT_FLAG_EXPAND;
  for (size_t i = 0; i < this->planes_.size(); i++) {
    const Plane &p = this->planes_[i];
    if (!p.drawn)
      continue;
    lv_area_t pa = {p.area.x1 + oc.x1, p.area.y1 + oc.y1, p.area.x2 + oc.x1, p.area.y2 + oc.y1};
    if (!hit(pa))
      continue;
    const bool sel = (int) i == this->selected_;
    const lv_color_t col = lv_color_hex(sel ? C_PLANE_SEL : C_PLANE);
    const float d = sqrtf(p.px * p.px + p.py * p.py);
    if (d > R_PX) {
      // За кругом — точка на ободе в его сторону
      if (!this->view_cfg_.rim)
        continue;
      const int ex = cx + (int) (p.px / d * (R_PX + 2)), ey = cy + (int) (p.py / d * (R_PX + 2));
      lv_area_t m = {ex - 3, ey - 3, ex + 3, ey + 3};
      fill.color = lv_color_hex(C_TRAIL);
      fill.opa = LV_OPA_COVER;
      lv_draw_fill(layer, &fill, &m);
      continue;
    }
    const float px = cx + p.px, py = cy + p.py;
    // Хвост — откуда прилетел, тускло-зелёный
    if (p.ntr) {
      ln.color = lv_color_hex(C_TRAIL);
      ln.opa = 120;
      for (int k = 0; k < p.ntr; k++) {
        ln.p1 = {(lv_value_precise_t) (cx + p.tr[k][0]), (lv_value_precise_t) (cy + p.tr[k][1])};
        ln.p2 = k + 1 < p.ntr
                    ? lv_point_precise_t{(lv_value_precise_t) (cx + p.tr[k + 1][0]), (lv_value_precise_t) (cy + p.tr[k + 1][1])}
                    : lv_point_precise_t{(lv_value_precise_t) px, (lv_value_precise_t) py};
        lv_draw_line(layer, &ln);
      }
    }
    // Силуэт своего типа по курсу; без силуэтов — стрелка
    const lv_image_dsc_t *spr = this->view_cfg_.shapes ? this->sprite_(p.icon, p.hdg) : nullptr;
    if (spr) {
      lv_draw_image_dsc_t im;
      lv_draw_image_dsc_init(&im);
      im.src = spr;
      im.recolor = col;
      im.opa = LV_OPA_COVER;
      lv_area_t ia = {(int32_t) px - AC_MAP / 2, (int32_t) py - AC_MAP / 2, (int32_t) px - AC_MAP / 2 + AC_MAP - 1,
                      (int32_t) py - AC_MAP / 2 + AC_MAP - 1};
      im.image_area = ia;
      lv_draw_image(layer, &im, &ia);
    } else {
      const float a = p.hdg * (PI_F / 180.0f), ca = cosf(a), sa = sinf(a);
      tr.color = col;
      for (const Tri &t : ARROW) {
        const float xs[3] = {t.x0, t.x1, t.x2}, ys[3] = {t.y0, t.y1, t.y2};
        for (int v = 0; v < 3; v++)
          tr.p[v] = {(lv_value_precise_t) (px + xs[v] * ca - ys[v] * sa), (lv_value_precise_t) (py + xs[v] * sa + ys[v] * ca)};
        lv_draw_triangle(layer, &tr);
      }
    }
    // Позывной справа
    if (p.label) {
      lb.text = label_of(p);
      lv_area_t ta = {(int) px + 20, (int) py - 11, (int) px + 20 + 11 * (int) strlen(lb.text) + 4, (int) py + 11};
      lv_draw_label(layer, &lb, &ta);
    }
  }
}

// ---------------------------------------------------------------------------
// Трекер рейса — по экрану «Flight tracker» из AirESP32ace: вылет слева,
// прилёт справа, дуга маршрута, пройденная часть сплошная, остаток пунктиром,
// самолёт на дуге. Сверху номер рейса, снизу коды аэропортов с городами,
// высота, скорость, курс и время

void Radar::bind_track(lv_obj_t *view) {
  this->trk_view_ = view;
  lv_obj_add_event_cb(view, view_event_cb, LV_EVENT_DRAW_MAIN, this);
  this->t_num_ = mk_label(view, this->font_big_, 0xFFFFFF, 0, -168);
  this->t_air_ = mk_label(view, this->font_, 0x8A949E, 0, -134);
  this->t_type_ = mk_label(view, this->font_, 0x6E7881, 0, -116);
  this->t_oc_ = mk_label(view, this->font_big_, 0xFFFFFF, -120, 58);
  this->t_on_ = mk_label(view, this->font_, 0x8A949E, -120, 88);
  this->t_dc_ = mk_label(view, this->font_big_, 0xFFFFFF, 120, 58);
  this->t_dn_ = mk_label(view, this->font_, 0x8A949E, 120, 88);
  static const char *const CAPS[3] = {"высота", "скорость", "курс"};
  for (int i = 0; i < 3; i++) {
    this->t_cap_[i] = mk_label(view, this->font_, 0x6E7881, (i - 1) * 110, 118);
    lv_label_set_text(this->t_cap_[i], CAPS[i]);
    this->t_val_[i] = mk_label(view, this->font_mid_, 0xE6EDF5, (i - 1) * 110, 142);
  }
  for (int i = 0; i < 2; i++) {
    this->t_tcap_[i] = mk_label(view, this->font_, 0x6E7881, i ? 60 : -60, 170);
    this->t_tval_[i] = mk_label(view, this->font_mid_, 0xE6EDF5, i ? 60 : -60, 192);
  }
  this->t_msg_ = mk_label(view, this->font_mid_, 0x8A949E, 0, 0);
  lv_obj_set_width(this->t_msg_, 330);
  lv_label_set_long_mode(this->t_msg_, LV_LABEL_LONG_WRAP);
  Track t;
  strcpy(t.st, "none");
  this->apply_track_(t);
}

void Radar::set_track_active(bool active) {
  const bool was = this->trk_active_.exchange(active);
  if (active && !was) {
    // Открыли страницу — карта маршрута (если уже известна) и свежие данные
    if (this->trk_.ver)
      this->trk_map_req_ = true;
    if (this->task_)
      xTaskNotifyGive(this->task_);
  }
}

std::string Radar::track_map_url() {
  return this->url_copy_() + "/trackmap.jpg?v=" + std::to_string(this->trk_.ver);
}

void Radar::apply_track_(const Track &t) {
  this->trk_ = t;
  if (!this->trk_view_)
    return;
  const bool ok = strcmp(t.st, "ok") == 0;
  lv_obj_t *const info[] = {this->t_air_, this->t_type_, this->t_oc_, this->t_on_, this->t_dc_, this->t_dn_,
                            this->t_cap_[0], this->t_cap_[1], this->t_cap_[2], this->t_val_[0], this->t_val_[1],
                            this->t_val_[2], this->t_tcap_[0], this->t_tcap_[1], this->t_tval_[0], this->t_tval_[1]};
  for (lv_obj_t *o : info) {
    if (ok)
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text(this->t_num_, t.n[0] ? t.n : "Рейс");
  if (!ok) {
    const char *msg = strcmp(t.st, "none") == 0  ? "Впишите номер рейса в Home Assistant — «Рейс для отслеживания»"
                      : strcmp(t.st, "wait") == 0 ? "Ищу рейс…"
                      : strcmp(t.st, "nf") == 0   ? "Рейс не найден или ещё не в воздухе"
                                                  : "Нет связи с сервером радара";
    lv_label_set_text(this->t_msg_, msg);
    lv_obj_clear_flag(this->t_msg_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(this->trk_view_);
    return;
  }
  lv_obj_add_flag(this->t_msg_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(this->t_air_, t.al);
  lv_label_set_text(this->t_type_, t.tl);
  lv_label_set_text(this->t_oc_, t.oc[0] ? t.oc : "—");
  lv_label_set_text(this->t_on_, t.on);
  lv_label_set_text(this->t_dc_, t.dc[0] ? t.dc : "—");
  lv_label_set_text(this->t_dn_, t.dn);
  char buf[32];
  if (t.plane) {
    const int a = t.alt / 10 * 10;
    if (a >= 1000)
      snprintf(buf, sizeof(buf), "%d %03d м", a / 1000, a % 1000);
    else
      snprintf(buf, sizeof(buf), "%d м", a);
    lv_label_set_text(this->t_val_[0], buf);
    snprintf(buf, sizeof(buf), "%d км/ч", t.spd);
    lv_label_set_text(this->t_val_[1], buf);
    snprintf(buf, sizeof(buf), "%s %d°", compass16(t.hdg), (t.hdg % 360 + 360) % 360);
    lv_label_set_text(this->t_val_[2], buf);
  } else {
    for (auto *v : this->t_val_)
      lv_label_set_text(v, "—");
  }
  snprintf(buf, sizeof(buf), "время %s", t.oc);
  lv_label_set_text(this->t_tcap_[0], buf);
  lv_label_set_text(this->t_tval_[0], t.ot[0] ? t.ot : "—");
  snprintf(buf, sizeof(buf), "прилёт %s", t.dc);
  lv_label_set_text(this->t_tcap_[1], buf);
  lv_label_set_text(this->t_tval_[1], t.eta[0] ? t.eta : "—");
  if (t.ver && t.ver != this->trk_shown_ver_) {
    this->trk_shown_ver_ = t.ver;
    this->trk_map_req_ = true;
  }
  lv_obj_invalidate(this->trk_view_);
}

void Radar::paint_track(lv_layer_t *layer) {
  const Track &t = this->trk_;
  if (strcmp(t.st, "ok") != 0 || t.npt < 2)
    return;
  lv_area_t oc;
  lv_obj_get_coords(this->trk_view_, &oc);
  auto P = [&oc](int x, int y) {
    return lv_point_precise_t{(lv_value_precise_t) (oc.x1 + x), (lv_value_precise_t) (oc.y1 + y)};
  };
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.color = lv_color_hex(0x1FB04A);
  ln.round_start = 1;
  ln.round_end = 1;
  // Пройдено — сплошной линией до самолёта, дальше — пунктиром
  const int split = t.plane ? std::max(0, std::min<int>(t.pi, t.npt - 1)) : 0;
  ln.width = 4;
  ln.opa = LV_OPA_COVER;
  for (int i = 0; i < split; i++) {
    ln.p1 = P(t.pt[i][0], t.pt[i][1]);
    ln.p2 = P(t.pt[i + 1][0], t.pt[i + 1][1]);
    lv_draw_line(layer, &ln);
  }
  ln.width = 2;
  ln.opa = 170;
  for (int i = split; i + 1 < t.npt; i += 2) {
    ln.p1 = P(t.pt[i][0], t.pt[i][1]);
    ln.p2 = P(t.pt[i + 1][0], t.pt[i + 1][1]);
    lv_draw_line(layer, &ln);
  }
  // Аэропорты — точками на концах
  lv_draw_fill_dsc_t fill;
  lv_draw_fill_dsc_init(&fill);
  fill.radius = LV_RADIUS_CIRCLE;
  fill.color = lv_color_hex(0x1FB04A);
  fill.opa = LV_OPA_COVER;
  for (int k : {0, t.npt - 1}) {
    lv_area_t a = {oc.x1 + t.pt[k][0] - 5, oc.y1 + t.pt[k][1] - 5, oc.x1 + t.pt[k][0] + 5, oc.y1 + t.pt[k][1] + 5};
    lv_draw_fill(layer, &fill, &a);
  }
  // Самолёт — силуэт своего типа по дуге
  if (t.plane) {
    const lv_image_dsc_t *spr = this->sprite_(t.icon, t.ph);
    if (spr) {
      lv_draw_image_dsc_t im;
      lv_draw_image_dsc_init(&im);
      im.src = spr;
      im.recolor = lv_color_hex(0x3DFF6A);
      im.opa = LV_OPA_COVER;
      lv_area_t ia = {oc.x1 + t.px - AC_MAP / 2, oc.y1 + t.py - AC_MAP / 2, oc.x1 + t.px - AC_MAP / 2 + AC_MAP - 1,
                      oc.y1 + t.py - AC_MAP / 2 + AC_MAP - 1};
      im.image_area = ia;
      lv_draw_image(layer, &im, &ia);
    }
  }
}

}  // namespace radar
}  // namespace esphome

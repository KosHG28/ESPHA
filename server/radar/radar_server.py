#!/usr/bin/env python3
"""Сервер радара самолётов для ESPHA.

Берёт самолёты из FlightRadar24 (библиотека FlightRadarAPI,
https://github.com/JeanExtreme002/FlightRadarAPI) вокруг дома и отдаёт:

  /            веб-страница радара с картой (для браузера)
  /data        самолёты и осадки для веб-страницы
  /esp?r=100   самолёты для платы: уже в пикселях экрана 466×466, в той же
               проекции, что и карта, ближние 40, с хвостами
  /map.jpg?r=100  тёмная карта 466×466 с городами — фон страницы радара
  /health      проверка, что сервер жив

  /tile/<стиль>/z/x/y  тайлы карты для веб-страницы — через сервер и его кэш

Настройки — переменные окружения (см. docker-compose.yml): CENTER_LAT,
CENTER_LON, PORT, CACHE_DIR, MAP_STYLE, MAP_BRIGHTNESS, CITIES_FILE.
"""

import json
import math
import os
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import urllib.error
import urllib.parse
from urllib.parse import parse_qs, urlparse

try:  # FlightRadarAPI 1.4+ — модуль FlightRadarAPI
    from FlightRadarAPI import FlightRadar24API
except ImportError:  # старые версии — модуль FlightRadar24
    from FlightRadar24 import FlightRadar24API

from PIL import Image, ImageDraw, ImageEnhance, ImageFont, ImageOps

def _coord(name, default):
    """Координаты дома — только из окружения (в Dockge — из .env стека), в
    репозитории их нет. Не заданы — центр Москвы и предупреждение в журнале"""
    try:
        return float(os.environ.get(name, "").replace(",", "."))
    except ValueError:
        print(f"{name} не задана — радар в центре Москвы. Впишите координаты дома в .env стека")
        return default


CENTER_LAT = _coord("CENTER_LAT", 55.7558)
CENTER_LON = _coord("CENTER_LON", 37.6173)
PORT = int(os.environ.get("PORT", "8080"))
CACHE_DIR = os.environ.get("CACHE_DIR", "/data")
MAP_BRIGHTNESS = float(os.environ.get("MAP_BRIGHTNESS", "1.15"))
CITIES_FILE = os.environ.get("CITIES_FILE", "")  # свой список городов; иначе — из OpenStreetMap
BUNDLED_CITIES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cities.json")
OVERPASS_URL = os.environ.get("OVERPASS_URL", "https://overpass-api.de/api/interpreter")
MAP_STYLE = os.environ.get("MAP_STYLE", "dark")
TILE_URL = os.environ.get("TILE_URL", "")  # свой источник тайлов {z}/{x}/{y} вместо MAP_STYLE
FONT_FILE = os.environ.get("FONT_FILE", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")

# Экран платы: 466×466, круг дальности — 220 px от центра
W = 466
R_PX = 220
# Самолёты запрашиваются один раз на самый большой радиус, а дальше
# отбираются под каждый запрос
FETCH_RADIUS_KM = 300
FLIGHTS_TTL = 6.0
MAX_ESP_PLANES = 40
MAX_TRAIL = 12
ESP_TRAIL = 6

fr_api = FlightRadar24API()

# Вид радара — общий для веб-страницы и платы. Меняется на веб-странице,
# хранится в CACHE_DIR/config.json, плата получает его с каждым /esp
THEME_STYLE = {"night": "dark", "sat": "sat", "osm-dark": "osm-dark", "day": "osm"}
CONFIG = {
    "radius": 100,  # с какого радиуса открывается радар, км
    "theme": {v: k for k, v in THEME_STYLE.items()}.get(MAP_STYLE, "night"),
    "showLabels": True,  # подписи маршрутов у самолётов
    "showCompass": True,  # кольца дальности и стороны света
    "colorByAlt": True,  # цвет по высоте, иначе все одним цветом
    "silhouettes": True,  # силуэт по типу самолёта, иначе стрелка
    "showRim": True,  # красные отметки самолётов за кругом
    "maxPlanes": 40,  # сколько ближних самолётов показывать
}
CONFIG_FILE = os.path.join(CACHE_DIR, "config.json")


def load_config():
    try:
        with open(CONFIG_FILE, encoding="utf-8") as f:
            update_config(json.load(f), save=False)
    except FileNotFoundError:
        pass
    except Exception as e:  # noqa: BLE001
        print(f"Настройки {CONFIG_FILE}: {e}")


def update_config(new, save=True):
    """Только известные ключи и только разумные значения"""
    for key, val in new.items():
        if key == "radius":
            try:
                CONFIG["radius"] = max(10, min(400, int(val)))
            except (TypeError, ValueError):
                pass
        elif key == "maxPlanes":
            try:
                CONFIG["maxPlanes"] = max(1, min(MAX_ESP_PLANES, int(val)))
            except (TypeError, ValueError):
                pass
        elif key == "theme":
            if val in THEME_STYLE:
                CONFIG["theme"] = val
        elif key in CONFIG:
            CONFIG[key] = bool(val)
    if save:
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            with open(CONFIG_FILE, "w", encoding="utf-8") as f:
                json.dump(CONFIG, f, ensure_ascii=False, indent=1)
        except OSError as e:
            print(f"Не удалось сохранить настройки: {e}")


def board_style():
    """Стиль карты платы: свой TILE_URL или выбранный на веб-странице"""
    return "custom" if TILE_URL else THEME_STYLE.get(CONFIG["theme"], "dark")

# Авиакомпании по коду ИКАО — названия для карточки самолёта на плате
AIRLINES = {
    "AFL": "Аэрофлот", "SDM": "Россия", "SBI": "S7 Airlines", "UTA": "ЮТэйр", "PBD": "Победа",
    "SVR": "Уральские авиалинии", "NWS": "Nordwind", "AZV": "Азимут", "RSY": "РусЛайн", "KAR": "ИрАэро",
    "YKS": "Якутия", "TYA": "NordStar", "RWZ": "Red Wings", "AUL": "Smartavia", "DRU": "Алроса",
    "SHU": "Аврора", "AGU": "Ангара", "SSF": "Северсталь", "KTK": "Azur Air", "IKT": "ИрАэро",
    "PLG": "Pegas Fly", "ORB": "Оренбуржье", "GZP": "Газпром авиа", "VAS": "ATRAN", "ABW": "AirBridgeCargo",
    "THY": "Turkish Airlines", "UAE": "Emirates", "QTR": "Qatar Airways", "ETD": "Etihad",
    "FDB": "flydubai", "ABY": "Air Arabia", "CCA": "Air China", "CES": "China Eastern",
    "CSN": "China Southern", "CHH": "Hainan Airlines", "BRU": "Belavia", "UZB": "Uzbekistan Airways",
    "ASL": "Air Serbia", "AIC": "Air India", "KZR": "Air Astana", "AZE": "AZAL", "TAJ": "Somon Air",
}

# Широкофюзеляжные и винтовые — для силуэта на плате
HEAVY = ("B74", "A38", "B77", "B78", "A33", "A34", "A35", "IL9", "IL7", "A12", "AN1", "B76")
PROP = ("C1", "C2", "AT4", "AT7", "DH8", "DHC", "AN2", "AN3", "L41", "SF3", "PC1", "BE", "IL18", "I114")


# ---------------------------------------------------------------------------
# Города для подписей на карте. Порядок в списке — кого подписывать первым.
# Откуда берутся, по порядку:
#   1. CITIES_FILE или data/cities.json — свой список;
#   2. города и посёлки вокруг дома из OpenStreetMap (Overpass API, без ключа):
#      качаются один раз в фоне и лежат в data/;
#   3. пока их нет — крупные города России из cities.json в образе.

CITIES = []
CITIES_SIG = ""


def _set_cities(cities, source):
    global CITIES, CITIES_SIG
    CITIES = cities
    CITIES_SIG = f"{source}{len(cities)}"
    print(f"Города: {len(cities)} ({source})")


def _read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def _osm_cities():
    path = os.path.join(CACHE_DIR, f"cities_osm_{CENTER_LAT:.2f}_{CENTER_LON:.2f}.json")
    if os.path.exists(path):
        return _read_json(path)
    query = (f'[out:json][timeout:120];node["place"~"^(city|town)$"]'
             f'(around:450000,{CENTER_LAT},{CENTER_LON});out body qt;')
    req = urllib.request.Request(OVERPASS_URL, data=urllib.parse.urlencode({"data": query}).encode(),
                                 headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=150) as resp:
        elements = json.loads(resp.read().decode("utf-8")).get("elements", [])
    cities = []
    for e in elements:
        tags = e.get("tags", {})
        name = tags.get("name:ru") or tags.get("name")
        if not name or "lat" not in e:
            continue
        digits = "".join(ch for ch in tags.get("population", "") if ch.isdigit())
        pop = int(digits) if digits else (50000 if tags.get("place") == "city" else 5000)
        cities.append({"name": name, "lat": round(e["lat"], 4), "lon": round(e["lon"], 4), "pop": pop})
    # Крупные — первыми: им подпись достаётся раньше мелких
    cities.sort(key=lambda c: -c["pop"])
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cities, f, ensure_ascii=False)
    return cities


def load_cities():
    own = CITIES_FILE or os.path.join(CACHE_DIR, "cities.json")
    if os.path.exists(own):
        try:
            _set_cities(_read_json(own), "свои ")
            return
        except Exception as e:  # noqa: BLE001
            print(f"Список городов {own}: {e}")
    try:
        _set_cities(_read_json(BUNDLED_CITIES), "крупные ")
    except Exception as e:  # noqa: BLE001
        print(f"Нет списка городов {BUNDLED_CITIES}: {e}")

    def fetch():
        for attempt in range(5):
            try:
                _set_cities(_osm_cities(), "OSM ")
                return
            except Exception as e:  # noqa: BLE001
                print(f"Города из OpenStreetMap: {e}")
                time.sleep(60 * (attempt + 1))

    threading.Thread(target=fetch, daemon=True).start()


# ---------------------------------------------------------------------------
# Проекция: веб-Меркатор, как у тайлов карты. Масштаб подбирается так, чтобы
# радиус r км занимал R_PX пикселей экрана в центре


def merc(lat, lon, z):
    n = 256 * 2 ** z
    x = (lon + 180.0) / 360.0 * n
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n
    return x, y


def scale_for(r_km):
    mpp = r_km * 1000.0 / R_PX  # метров на пиксель экрана
    ground = 156543.03392 * math.cos(math.radians(CENTER_LAT))
    z = max(1, min(17, math.ceil(math.log2(ground / mpp))))
    return z, (ground / 2 ** z) / mpp  # экранных пикселей на пиксель тайла


def project(lat, lon, r_km):
    """Пиксели экрана от центра: x — на восток, y — на юг"""
    z, s = scale_for(r_km)
    cx, cy = merc(CENTER_LAT, CENTER_LON, z)
    x, y = merc(lat, lon, z)
    return (x - cx) * s, (y - cy) * s


def dist_km(lat, lon):
    p1, p2 = math.radians(CENTER_LAT), math.radians(lat)
    dp, dl = p2 - p1, math.radians(lon - CENTER_LON)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 6371.0 * 2 * math.asin(math.sqrt(a))


# ---------------------------------------------------------------------------
# Самолёты: один запрос к FlightRadar24 раз в FLIGHTS_TTL секунд, сколько бы
# клиентов ни спрашивало

FLIGHTS = {"list": [], "t": 0.0}
FLIGHT_TRAILS = {}
_flights_lock = threading.Lock()


def get_dynamic_bounds(radius_km):
    fetch_radius = radius_km * 1.5
    lat_diff = fetch_radius / 111.0
    lon_diff = fetch_radius / (111.0 * math.cos(math.radians(CENTER_LAT)))
    return f"{CENTER_LAT + lat_diff:.2f},{CENTER_LAT - lat_diff:.2f},{CENTER_LON - lon_diff:.2f},{CENTER_LON + lon_diff:.2f}"


def num(v, default=0):
    return v if isinstance(v, (int, float)) else default


def text(v):
    return v if isinstance(v, str) and v != "N/A" else ""


def get_flights():
    with _flights_lock:
        if time.time() - FLIGHTS["t"] < FLIGHTS_TTL:
            return FLIGHTS["list"]
        try:
            flights = fr_api.get_flights(bounds=get_dynamic_bounds(FETCH_RADIUS_KM))
        except Exception as e:  # noqa: BLE001
            print(f"Ошибка FlightRadar24: {e}")
            FLIGHTS["t"] = time.time() - FLIGHTS_TTL + 3  # повторить через 3 с
            return FLIGHTS["list"]
        current = set()
        out = []
        for f in flights:
            if getattr(f, "on_ground", 0):
                continue
            lat, lon = num(f.latitude, None), num(f.longitude, None)
            if lat is None or lon is None:
                continue
            fid = f.id
            current.add(fid)
            trail = FLIGHT_TRAILS.setdefault(fid, [])
            if not trail or trail[-1] != (lat, lon):
                trail.append((lat, lon))
            del trail[:-MAX_TRAIL]
            orig, dest = text(f.origin_airport_iata), text(f.destination_airport_iata)
            out.append({
                "id": fid, "lat": lat, "lon": lon, "hdg": num(f.heading),
                "route": f"{orig}-{dest}" if orig and dest else text(f.callsign),
                "callsign": text(f.callsign), "icon": text(f.airline_icao),
                "alt": num(f.altitude), "speed": num(f.ground_speed), "type": text(f.aircraft_code),
                "trail": list(trail),
            })
        for fid in list(FLIGHT_TRAILS):
            if fid not in current:
                del FLIGHT_TRAILS[fid]
        FLIGHTS["list"] = out
        FLIGHTS["t"] = time.time()
        return out


def esp_payload(r_km):
    k = R_PX / r_km  # пикселей экрана на километр в центре
    planes = []
    for f in get_flights():
        d = dist_km(f["lat"], f["lon"])
        if d > r_km * 1.6:
            continue
        x, y = project(f["lat"], f["lon"], r_km)
        ty = f["type"]
        kind = 1 if ty.startswith(HEAVY) else (2 if ty.startswith(PROP) else 0)
        trail = [project(lat, lon, r_km) for lat, lon in f["trail"][:-1][-ESP_TRAIL:]]
        rt = f["route"] if f["route"] != f["callsign"] else ""
        planes.append({
            "id": str(f["id"])[:11], "x": round(x, 1), "y": round(y, 1), "h": int(f["hdg"]),
            "a": int(f["alt"] * 0.3048), "s": int(f["speed"] * 1.852), "d": round(d, 1), "k": kind,
            "c": f["callsign"][:9], "rt": rt[:11], "ty": ty[:5],
            "al": AIRLINES.get(f["icon"], f["icon"])[:27],
            "tr": [[int(round(a)), int(round(b))] for a, b in trail],
        })
    planes.sort(key=lambda p: p["d"])
    view = {
        "m": board_style(), "r": CONFIG["radius"], "l": int(CONFIG["showLabels"]),
        "g": int(CONFIG["showCompass"]), "c": int(CONFIG["colorByAlt"]), "s": int(CONFIG["silhouettes"]),
        "o": int(CONFIG["showRim"]),
    }
    return {"r": r_km, "k": round(k, 4), "t": int(time.time()), "v": view, "p": planes[:CONFIG["maxPlanes"]]}


# ---------------------------------------------------------------------------
# Карта: тайлы без ключей API, склеенные в картинку 466×466 под нужный
# радиус, и свои подписи городов по-русски. Тайлы и готовые карты
# кэшируются на диске.
#
# Стили (MAP_STYLE):
#   dark      тёмно-серая Esri без подписей — по умолчанию
#   sat       спутник Esri, притемнённый
#   osm-dark  OpenStreetMap, перевёрнутая в тёмные тона
#   osm       обычная светлая OpenStreetMap
# Если тайлы стиля не качаются, карта собирается из osm-dark.

ESRI = "https://server.arcgisonline.com/ArcGIS/rest/services/"
OSM = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
TILE_SOURCES = {
    "dark": ESRI + "Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}",
    "dark-ref": ESRI + "Canvas/World_Dark_Gray_Reference/MapServer/tile/{z}/{y}/{x}",
    "sat": ESRI + "World_Imagery/MapServer/tile/{z}/{y}/{x}",
    "sat-ref": ESRI + "Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}",
    "osm": OSM,
    "osm-dark": OSM,
}
if TILE_URL:
    TILE_SOURCES["custom"] = TILE_URL
    MAP_STYLE = "custom"
# Яркость стиля на плате: спутник притемняем, чтобы самолёты были видны
STYLE_BRIGHTNESS = {"sat": 0.6}
# OpenStreetMap просит, чтобы программа представлялась
USER_AGENT = "ESPHA-radar/1.0 (+https://github.com/KosHG28/ESPHA)"

_map_lock = threading.Lock()
_maps = {}


def _osm_dark(data):
    """Светлую OSM — в тёмные синеватые тона: суша тёмная, вода чуть светлее"""
    g = ImageOps.invert(ImageOps.grayscale(Image.open(BytesIO(data)).convert("RGB")))
    img = ImageOps.colorize(g, black=(8, 11, 18), white=(175, 195, 220), mid=(40, 52, 70))
    out = BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


def tile_bytes(style, z, x, y):
    """Тайл как есть (PNG или JPEG) — из кэша или из сети"""
    path = os.path.join(CACHE_DIR, "tiles", style, str(z), str(x), str(y))
    if os.path.exists(path):
        with open(path, "rb") as f:
            return f.read()
    url = TILE_SOURCES[style].format(s="abc"[(x + y) % 3], z=z, x=x, y=y)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = resp.read()
    Image.open(BytesIO(data)).verify()  # не картинка — ошибка, в кэш не кладём
    if style == "osm-dark":
        data = _osm_dark(data)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return data


LOGO_URL = os.environ.get("LOGO_URL", "https://raw.githubusercontent.com/sexym0nk3y/airline-logos/master/logos/{icao}.png")
LOGO_MISS_TTL = 7 * 86400  # логотипа нет в наборе — спросить снова через неделю
_logo_lock = threading.Lock()


def logo_bytes(icao):
    """Логотип авиакомпании по коду ИКАО — из data/logos или из набора на
    GitHub. None — такого логотипа нет"""
    icao = icao.upper()
    if not (2 <= len(icao) <= 4 and icao.isascii() and icao.isalnum()):
        return None
    path = os.path.join(CACHE_DIR, "logos", f"{icao}.png")
    miss = path + ".none"
    with _logo_lock:
        if os.path.exists(path):
            with open(path, "rb") as f:
                return f.read()
        if os.path.exists(miss) and time.time() - os.path.getmtime(miss) < LOGO_MISS_TTL:
            return None
    req = urllib.request.Request(LOGO_URL.format(icao=icao), headers={"User-Agent": USER_AGENT})
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = resp.read()
        Image.open(BytesIO(data)).verify()
    except urllib.error.HTTPError as e:
        if e.code == 404:  # в наборе нет — запомнить, чтобы не спрашивать каждый раз
            open(miss, "wb").close()
        return None
    except Exception as e:  # noqa: BLE001  нет сети — попробуем в следующий раз
        print(f"Логотип {icao}: {e}")
        return None
    with _logo_lock:
        with open(path, "wb") as f:
            f.write(data)
    return data


def tile(style, z, x, y):
    return Image.open(BytesIO(tile_bytes(style, z, x, y))).convert("RGB")


def render_map(r_km, style):
    """Карта и как она собрана: "ok" — из своего стиля, "fallback" — из
    osm-dark, потому что свой не качается, "holes" — не все тайлы на месте"""
    img, ok = render_base(r_km, style)
    if ok:
        return label_cities(img, r_km), "ok"
    if style != "osm-dark":
        print(f"Карта {r_km} км: тайлы {style} не скачались, пробую osm-dark")
        img2, ok2 = render_base(r_km, "osm-dark")
        if ok2:
            return label_cities(img2, r_km), "fallback"
    return label_cities(img, r_km), "holes"


def render_base(r_km, style):
    z, s = scale_for(r_km)
    cx, cy = merc(CENTER_LAT, CENTER_LON, z)
    span = W / s  # сколько пикселей тайлов на экран
    x0, y0 = cx - span / 2, cy - span / 2
    tx0, ty0 = int(x0 // 256), int(y0 // 256)
    tx1, ty1 = int((x0 + span) // 256), int((y0 + span) // 256)
    canvas = Image.new("RGB", ((tx1 - tx0 + 1) * 256, (ty1 - ty0 + 1) * 256), (18, 20, 24))
    ok = True
    for tx in range(tx0, tx1 + 1):
        for ty in range(ty0, ty1 + 1):
            try:
                canvas.paste(tile(style, z, tx, ty), ((tx - tx0) * 256, (ty - ty0) * 256))
            except Exception as e:  # noqa: BLE001
                ok = False
                print(f"Тайл {style} {z}/{tx}/{ty}: {e}")
    left, top = x0 - tx0 * 256, y0 - ty0 * 256
    img = canvas.crop((int(left), int(top), int(left + span), int(top + span))).resize((W, W), Image.LANCZOS)
    k = MAP_BRIGHTNESS * STYLE_BRIGHTNESS.get(style, 1.0)
    if k != 1.0:
        img = ImageEnhance.Brightness(img).enhance(k)
    return img, ok


def label_cities(img, r_km):
    # Города: точка и название; ближние к центру — в первую очередь, без наложений
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(FONT_FILE, 15)
    except OSError:
        font = ImageFont.load_default()
    used = []
    for c in CITIES:
        x, y = project(c["lat"], c["lon"], r_km)
        if x * x + y * y > (R_PX + 6) ** 2 or x * x + y * y < 14 ** 2:
            continue  # за кругом или это сам дом
        px, py = W / 2 + x, W / 2 + y
        tw = draw.textlength(c["name"], font=font)
        box = (px + 6, py - 9, px + 8 + tw, py + 9)
        if any(box[0] < u[2] and box[2] > u[0] and box[1] < u[3] and box[3] > u[1] for u in used):
            continue
        used.append(box)
        draw.ellipse((px - 3, py - 3, px + 3, py + 3), fill=(200, 212, 224))
        draw.text((px + 7, py - 9), c["name"], font=font, fill=(150, 170, 196), stroke_width=2,
                  stroke_fill=(0, 0, 0))
    out = BytesIO()
    img.save(out, "JPEG", quality=85)
    return out.getvalue()


def get_map(r_km, style=None):
    style = style if style in TILE_SOURCES and not style.endswith("-ref") else board_style()
    key = (style, r_km, CITIES_SIG)
    with _map_lock:
        if key in _maps:
            return _maps[key]
        path = os.path.join(CACHE_DIR, "maps", f"{style}_{CENTER_LAT:.4f}_{CENTER_LON:.4f}_{CITIES_SIG}_{r_km}.jpg")
        if os.path.exists(path):
            with open(path, "rb") as f:
                _maps[key] = f.read()
            return _maps[key]
        data, how = render_map(r_km, style)
        if how == "fallback":  # до перезапуска; на диск — только из своего стиля
            _maps[key] = data
        elif how == "ok":  # с дырами не запоминаем — в следующий раз соберём заново
            _maps[key] = data
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        return data


HTML_PAGE = """
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Радар ESPHA</title>
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
    
    <style>
        :root {
            --bg-body: #080a0c; --bg-card: #15171B; --text-main: #E0E0E0; --text-muted: #667080; --accent: #8899AA;
            --radar-bg: #090b0e;
        }
        body.day-mode {
            --bg-body: #E0E4E8; --bg-card: #FFFFFF; --text-main: #222222; --text-muted: #808590; --accent: #003366;
            --radar-bg: #EAECEE;
        }
        body { font-family: -apple-system, sans-serif; background: var(--bg-body); color: var(--text-main); margin: 0; padding: 20px; display: flex; flex-wrap: wrap; justify-content: center; gap: 40px; align-items: flex-start; transition: 0.3s;}
        .card { background: var(--bg-card); padding: 25px; border-radius: 12px; width: 100%; max-width: 320px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); transition: 0.3s;}
        h2 { margin-top: 0; color: var(--accent); font-size: 18px; letter-spacing: 1px;}
        .group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: var(--text-muted); font-size: 13px;}
        input[type="range"] { width: 100%; accent-color: var(--accent); }
        select { width: 100%; padding: 8px; background: transparent; color: var(--text-main); border: 1px solid var(--text-muted); border-radius: 6px; }
        .switch { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; font-size: 13px; font-weight: bold;}
        input[type="checkbox"] { width: 18px; height: 18px; accent-color: var(--accent); cursor: pointer;}
        .val { float: right; color: var(--accent); }
        
        .radar-wrapper { 
            position: relative; 
            width: 400px; 
            height: 400px; 
            border-radius: 50%; 
            box-shadow: 0 0 40px rgba(0,0,0,0.6); 
            border: 5px solid #222; 
            overflow: hidden; 
            background: var(--radar-bg);
            transition: background 0.3s, border-color 0.3s;
        }
        body.day-mode .radar-wrapper { border-color: #A0A5B0; box-shadow: 0 0 40px rgba(0,0,0,0.15); }
        
        .carto-layer { opacity: 0.95; }
        
        #map { position: absolute; top: 0; left: 0; width: 100%; height: 100%; z-index: 1; }
        #radar { position: absolute; top: 0; left: 0; z-index: 2; pointer-events: none; }
        
        hr { border: 0; border-top: 1px solid var(--text-muted); opacity: 0.2; margin: 15px 0; }
        .stats-box { background: rgba(255,255,255,0.02); border: 1px solid rgba(255,255,255,0.05); padding: 10px; border-radius: 8px; font-family: monospace; font-size: 12px; margin-top: 20px;}
        body.day-mode .stats-box { background: rgba(0,0,0,0.02); border-color: rgba(0,0,0,0.05); }
    </style>
</head>
<body id="body">
    <div class="card">
        <h2>ATC Terminal</h2>
        
        <div class="group">
            <label>Масштаб: <span id="radVal" class="val">100 км</span></label>
            <input id="radius" type="range" min="20" max="300" step="5" value="100" oninput="updateVal('radVal', this.value + ' км'); sendConfig()">
        </div>

        <div class="group">
            <label>Базовый навигационный слой:</label>
            <select id="theme" onchange="sendConfig()">
                <option value="night">Тёмная (Esri)</option>
                <option value="sat">Спутник (Esri)</option>
                <option value="osm-dark">Тёмная (OpenStreetMap)</option>
                <option value="day">Светлая (OpenStreetMap)</option>
            </select>
        </div>
        <hr>
        <div class="switch"><label>Метки и маршруты</label><input type="checkbox" id="showLabels" checked onchange="sendConfig()"></div>
        <div class="switch"><label>Сетка и Компас</label><input type="checkbox" id="showCompass" checked onchange="sendConfig()"></div>
        <div class="switch"><label style="color:var(--accent)">Цвет по высоте</label><input type="checkbox" id="colorByAlt" checked onchange="sendConfig()"></div>
        <div class="switch"><label style="color:var(--accent)">Форма силуэтов</label><input type="checkbox" id="silhouettes" checked onchange="sendConfig()"></div>
        <div class="switch"><label>Отметки за кругом</label><input type="checkbox" id="showRim" checked onchange="sendConfig()"></div>
        <div class="group" style="margin-top: 12px;">
            <label>Сколько самолётов показывать:</label>
            <select id="maxPlanes" onchange="sendConfig()">
                <option value="5">5 ближних</option>
                <option value="10">10 ближних</option>
                <option value="20">20 ближних</option>
                <option value="40">40 ближних</option>
            </select>
        </div>
        <p style="font-size: 12px; color: var(--text-muted); margin: 12px 0 0;">Всё это — и на часах: они подхватят за 8 секунд.</p>
        
        <div class="stats-box">
            <b>Tactical Air Data:</b><br>
            Targets Tracked: <span id="statCount" style="color:var(--accent)">0</span><br>
            Max Recorded Speed: <span id="statSpeed" style="color:var(--accent)">0 km/h</span>
        </div>
    </div>

    <div class="radar-wrapper">
        <div id="map"></div>
        <canvas id="radar" width="400" height="400"></canvas>
    </div>

    <script>
        const CENTER_LAT = __CENTER_LAT__;
        const CENTER_LON = __CENTER_LON__;
        const canvas = document.getElementById('radar');
        const ctx = canvas.getContext('2d');
        
        // Настройки — с сервера: они же у платы
        let localConfig = __CONFIG__;
        let planesState = {}; 
        let lastFrameTime = performance.now(); 
        let uniqueFlights = new Set();
        let maxSpeedLog = 0;

        const map = L.map('map', {
            center: [CENTER_LAT, CENTER_LON],
            zoomSnap: 0, 
            zoomControl: false, dragging: false, scrollWheelZoom: false, doubleClickZoom: false, boxZoom: false, keyboard: false,
            attributionControl: false
        });

        // Тайлы без ключей API — через сервер радара, он их кэширует.
        // Тема → [подложка, подписи поверх неё или null]
        const TILE_STYLES = {
            'night': ['dark', 'dark-ref'],
            'sat': ['sat', 'sat-ref'],
            'osm-dark': ['osm-dark', null],
            'day': ['osm', null]
        };
        const tileUrl = (st) => '/tile/' + st + '/{z}/{x}/{y}';
        const mapLayer = L.tileLayer(tileUrl('dark'), { maxZoom: 14, className: 'carto-layer' }).addTo(map);
        const refLayer = L.tileLayer(tileUrl('dark-ref'), { maxZoom: 14 }).addTo(map);
        function applyTiles(theme) {
            const st = TILE_STYLES[theme] || TILE_STYLES['night'];
            mapLayer.setUrl(tileUrl(st[0]));
            if (st[1]) { refLayer.setUrl(tileUrl(st[1])); if (!map.hasLayer(refLayer)) refLayer.addTo(map); }
            else if (map.hasLayer(refLayer)) map.removeLayer(refLayer);
        }

        function updateMapBounds() {
            const rKm = localConfig.radius * (200.0 / 190.0); 
            const latDiff = rKm / 111.0;
            const lonDiff = rKm / (111.0 * Math.cos(CENTER_LAT * Math.PI / 180.0));
            map.fitBounds([
                [CENTER_LAT - latDiff, CENTER_LON - lonDiff],
                [CENTER_LAT + latDiff, CENTER_LON + lonDiff]
            ], {animate: false});
        }
        updateMapBounds();

        const iconCache = {};
        function getIcon(icao) {
            if (!icao) return null;
            if (!iconCache[icao]) {
                const img = new Image();
                img.src = `/logo/${encodeURIComponent(icao)}.png`;  // через сервер и его кэш
                iconCache[icao] = img;
            }
            return iconCache[icao];
        }

        function updateVal(id, val) { document.getElementById(id).innerText = val; }

        function sendConfig() {
            localConfig = {
                radius: parseInt(document.getElementById('radius').value),
                theme: document.getElementById('theme').value,
                showLabels: document.getElementById('showLabels').checked,
                showCompass: document.getElementById('showCompass').checked,
                colorByAlt: document.getElementById('colorByAlt').checked,
                silhouettes: document.getElementById('silhouettes').checked,
                showRim: document.getElementById('showRim').checked,
                maxPlanes: parseInt(document.getElementById('maxPlanes').value)
            };
            
            document.getElementById('body').className = localConfig.theme === 'day' ? 'day-mode' : '';
            
            // Бесшовная смена слоя без разрушения карты
            applyTiles(localConfig.theme);
            
            updateMapBounds();
            fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(localConfig) })
                .then(fetchRadarData);
        }
        window.sendConfig = sendConfig; 

        // Элементы страницы — по сохранённым настройкам
        (function initControls() {
            document.getElementById('radius').value = localConfig.radius;
            updateVal('radVal', localConfig.radius + ' км');
            document.getElementById('theme').value = localConfig.theme;
            ['showLabels', 'showCompass', 'colorByAlt', 'silhouettes', 'showRim'].forEach(
                k => document.getElementById(k).checked = !!localConfig[k]);
            const mp = document.getElementById('maxPlanes');
            if (![...mp.options].some(o => +o.value === localConfig.maxPlanes)) mp.add(new Option(localConfig.maxPlanes + ' ближних', localConfig.maxPlanes));
            mp.value = localConfig.maxPlanes;
            document.getElementById('body').className = localConfig.theme === 'day' ? 'day-mode' : '';
            applyTiles(localConfig.theme);
        })();

        function getAltColor(alt, isEnabled) {
            if (!isEnabled || !alt) return (localConfig.theme === 'day' ? '#003366' : '#FFC107');
            if (alt < 5000) return '#00FFFF';  
            if (alt < 15000) return '#00FF00'; 
            if (alt < 28000) return '#FFFF00'; 
            if (alt < 36000) return '#FFA500'; 
            return '#FF4444';                  
        }

        function fetchRadarData() {
            fetch('/data').then(r=>r.json()).then(data => {
                let currentIds = new Set();
                
                data.planes.forEach(p => {
                    uniqueFlights.add(p.id);
                    if (p.speed && p.speed > maxSpeedLog) maxSpeedLog = p.speed;
                    currentIds.add(p.id);
                    
                    if (!planesState[p.id]) {
                        const speedKmh = (p.speed || 400) * 1.852;
                        const distKmPerMs = speedKmh / 3600000;
                        const hdgRad = (p.hdg * Math.PI) / 180.0;
                        
                        const dLatKm = Math.cos(hdgRad) * distKmPerMs;
                        const dLonKm = Math.sin(hdgRad) * distKmPerMs;
                        
                        const vLat = dLatKm / 111.0;
                        const vLon = dLonKm / (111.0 * Math.cos(CENTER_LAT * Math.PI/180.0));
                        
                        planesState[p.id] = { 
                            currLat: p.lat, currLon: p.lon, 
                            vLat: vLat, vLon: vLon, 
                            hdg: p.hdg, data: p 
                        };
                    } else {
                        // Далеко разошёлся с данными (больше ~5 км) — сразу на место
                        // и дальше по курсу и скорости
                        if (Math.abs(p.lat - planesState[p.id].currLat) > 0.05 || Math.abs(p.lon - planesState[p.id].currLon) > 0.08) {
                            const speedKmh = (p.speed || 400) * 1.852;
                            const hdgRad = (p.hdg * Math.PI) / 180.0;
                            planesState[p.id].currLat = p.lat;
                            planesState[p.id].currLon = p.lon;
                            planesState[p.id].vLat = Math.cos(hdgRad) * speedKmh / 3600000 / 111.0;
                            planesState[p.id].vLon = Math.sin(hdgRad) * speedKmh / 3600000 / (111.0 * Math.cos(CENTER_LAT * Math.PI/180.0));
                        } else {
                            planesState[p.id].vLat = (p.lat - planesState[p.id].currLat) / 8000;
                            planesState[p.id].vLon = (p.lon - planesState[p.id].currLon) / 8000;
                        }
                        
                        let diff = p.hdg - planesState[p.id].hdg;
                        diff = ((diff + 180) % 360) - 180;
                        planesState[p.id].targetHdg = planesState[p.id].hdg + diff;
                        planesState[p.id].data = p; 
                    }
                });
                
                for (let id in planesState) { if (!currentIds.has(id)) delete planesState[id]; }
                document.getElementById('statCount').innerText = uniqueFlights.size;
                document.getElementById('statSpeed').innerText = Math.round(maxSpeedLog) + " km/h";
            });
        }

        function renderLoop() {
            const cx = 200, cy = 200, maxR = 190; 
            const isDay = localConfig.theme === 'day';
            
            const now = performance.now();
            // Вкладку сворачивали — кадров не было; не двигаем самолёты на всё
            // пропущенное время разом, иначе они улетают от своих хвостов
            const dt = Math.min(now - lastFrameTime, 100);
            lastFrameTime = now;
            
            const gridHex = isDay ? 'rgba(120, 130, 140, 0.35)' : 'rgba(100, 120, 140, 0.3)';
            const textHex = isDay ? '#444444' : '#8899AA';

            ctx.clearRect(0, 0, 400, 400);

            // 1. СЕТКА И КОМПАС
            if(localConfig.showCompass) {
                ctx.strokeStyle = gridHex; 
                ctx.lineWidth = 1; 
                ctx.setLineDash([4, 4]); 
                
                for(let i = 1; i <= 4; i++) { ctx.beginPath(); ctx.arc(cx, cy, (maxR/4)*i, 0, Math.PI*2); ctx.stroke(); }
                ctx.beginPath(); ctx.moveTo(cx, cy-maxR); ctx.lineTo(cx, cy+maxR); ctx.stroke();
                ctx.beginPath(); ctx.moveTo(cx-maxR, cy); ctx.lineTo(cx+maxR, cy); ctx.stroke();
                ctx.setLineDash([]); 

                ctx.fillStyle = textHex; 
                ctx.font = 'bold 11px monospace'; 
                ctx.textAlign = 'center'; 
                ctx.textBaseline = 'middle';
                const pad = 12;
                ctx.fillText('N', cx, cy - maxR + pad); ctx.fillText('S', cx, cy + maxR - pad);
                ctx.fillText('E', cx + maxR - pad, cy); ctx.fillText('W', cx - maxR + pad, cy);
            }

            // 3. САМОЛЕТЫ
            let drawnBoxes = []; 

            for (let id in planesState) {
                let p = planesState[id];
                
                p.currLat += p.vLat * dt;
                p.currLon += p.vLon * dt;
                if(p.targetHdg !== undefined) p.hdg += (p.targetHdg - p.hdg) * 0.05;

                const pt = map.latLngToContainerPoint([p.currLat, p.currLon]);
                const px = pt.x; const py = pt.y;

                const dist = Math.sqrt(Math.pow(px - cx, 2) + Math.pow(py - cy, 2));
                const pColor = getAltColor(p.data.alt, localConfig.colorByAlt);

                if (dist <= maxR) {
                    // Хвосты трасс
                    if (p.data.trail && p.data.trail.length > 1) {
                        ctx.save();
                        ctx.beginPath(); ctx.arc(cx, cy, maxR, 0, Math.PI*2); ctx.clip();
                        ctx.globalAlpha = 0.5;
                        ctx.strokeStyle = pColor; 
                        ctx.lineWidth = 2;
                        ctx.beginPath();
                        p.data.trail.forEach((ptLatLon, index) => {
                            const tPos = map.latLngToContainerPoint([ptLatLon[0], ptLatLon[1]]);
                            if (index === 0) ctx.moveTo(tPos.x, tPos.y); else ctx.lineTo(tPos.x, tPos.y);
                        });
                        ctx.lineTo(px, py);
                        ctx.stroke();
                        ctx.restore();
                    }

                    // Силуэт
                    ctx.save();
                    ctx.translate(px, py);
                    ctx.rotate(p.hdg * Math.PI / 180.0);
                    ctx.fillStyle = pColor;
                    ctx.shadowColor = 'rgba(0,0,0,0.4)'; ctx.shadowBlur = 4;
                    
                    ctx.beginPath();
                    if (!localConfig.silhouettes || !p.data.type) {
                        ctx.moveTo(0, -6); ctx.lineTo(4, 5); ctx.lineTo(-4, 5);
                    } else if (p.data.type.startsWith('B74') || p.data.type.startsWith('A38') || p.data.type.startsWith('B77') || p.data.type.startsWith('IL9')) {
                        ctx.moveTo(0, -8); ctx.lineTo(2, -4); ctx.lineTo(8, 2); ctx.lineTo(2, 2); ctx.lineTo(0, 6); ctx.lineTo(-2, 2); ctx.lineTo(-8, 2); ctx.lineTo(-2, -4);
                    } else if (p.data.type.startsWith('C') || p.data.type.startsWith('AT') || p.data.type.startsWith('DH') || p.data.type.startsWith('AN')) {
                        ctx.moveTo(0, -6); ctx.lineTo(1, -2); ctx.lineTo(6, -2); ctx.lineTo(6, 0); ctx.lineTo(1, 0); ctx.lineTo(0, 5); ctx.lineTo(-1, 0); ctx.lineTo(-6, 0); ctx.lineTo(-6, -2); ctx.lineTo(-1, -2);
                    } else {
                        ctx.moveTo(0, -7); ctx.lineTo(1.5, -3); ctx.lineTo(6, 3); ctx.lineTo(1.5, 3); ctx.lineTo(0, 6); ctx.lineTo(-1.5, 3); ctx.lineTo(-6, 3); ctx.lineTo(-1.5, -3);
                    }
                    ctx.fill();
                    ctx.restore();

                    // Текст (Anti-Collision блок)
                    if(localConfig.showLabels) {
                        let textY = py - 4;
                        let textX = px + 10;
                        
                        let overlap = false;
                        for(let b of drawnBoxes) {
                            if (Math.abs(b.x - textX) < 40 && Math.abs(b.y - textY) < 20) { overlap = true; break; }
                        }

                        if (!overlap) {
                            if (p.data.icon) {
                                const img = getIcon(p.data.icon);
                                if (img && img.complete && img.naturalWidth > 0) {
                                    ctx.drawImage(img, textX, textY - 8, 32, 16);
                                    textY += 14; 
                                }
                            }
                            ctx.fillStyle = isDay ? '#111111' : '#FFFFFF';
                            ctx.shadowColor = isDay ? 'rgba(255,255,255,0.9)' : 'rgba(0,0,0,0.9)'; 
                            ctx.shadowBlur = 3;
                            ctx.font = 'bold 11px monospace'; 
                            ctx.textAlign = 'left';
                            ctx.fillText(p.data.route || p.data.type || "N/A", textX, textY);
                            ctx.shadowBlur = 0; 
                            
                            drawnBoxes.push({x: textX, y: textY});
                        }
                    }
                } else if (localConfig.showRim) {
                    // Edge Tracking (Красные точки целей на периметре кольца)
                    const edgeX = cx + ((px - cx) / dist) * (maxR - 2); 
                    const edgeY = cy + ((py - cy) / dist) * (maxR - 2);
                    
                    ctx.fillStyle = '#FF4444';
                    ctx.beginPath(); 
                    ctx.arc(edgeX, edgeY, 3, 0, Math.PI*2); 
                    ctx.fill();
                }
            }

            requestAnimationFrame(renderLoop); 
        }

        setInterval(fetchRadarData, 8000); 
        // Вкладку развернули — сразу свежие данные
        document.addEventListener('visibilitychange', () => {
            if (!document.hidden) { lastFrameTime = performance.now(); fetchRadarData(); }
        });
        fetchRadarData();
        requestAnimationFrame(renderLoop); 
    </script>
</body>
</html>
"""


class RadarHandler(BaseHTTPRequestHandler):
    def reply(self, code, ctype, body, cache=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if cache:
            self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        try:
            r = max(10, min(400, int(q.get("r", ["100"])[0])))
        except ValueError:
            r = 100
        if url.path in ("/", "/settings"):
            page = (HTML_PAGE.replace("__CENTER_LAT__", str(CENTER_LAT)).replace("__CENTER_LON__", str(CENTER_LON))
                    .replace("__CONFIG__", json.dumps(CONFIG)))
            self.reply(200, "text/html; charset=utf-8", page.encode("utf-8"))
        elif url.path == "/esp":
            self.reply(200, "application/json", json.dumps(esp_payload(r), ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        elif url.path == "/map.jpg":
            try:
                self.reply(200, "image/jpeg", get_map(r, q.get("s", [None])[0]), "max-age=86400")
            except Exception as e:  # noqa: BLE001
                print(f"Ошибка карты: {e}")
                self.reply(500, "text/plain", b"map error")
        elif url.path == "/data":
            # Как на плате: ближние maxPlanes в радиусе ×1,6
            flights = [(dist_km(f["lat"], f["lon"]), f) for f in get_flights()]
            flights = sorted((x for x in flights if x[0] <= CONFIG["radius"] * 1.6), key=lambda x: x[0])
            planes = []
            for _, f in flights[:CONFIG["maxPlanes"]]:
                p = dict(f)
                p["trail"] = [list(t) for t in f["trail"]]
                planes.append(p)
            body = {"config": CONFIG, "planes": planes}
            self.reply(200, "application/json", json.dumps(body).encode("utf-8"))
        elif url.path.startswith("/tile/"):
            # /tile/<стиль>/z/x/y — тайлы для веб-страницы через кэш сервера
            parts = url.path.split("/")
            try:
                style, z, x, y = parts[2], int(parts[3]), int(parts[4]), int(parts[5])
                if style not in TILE_SOURCES or not 0 <= z <= 14:
                    raise ValueError(style)
                data = tile_bytes(style, z, x, y)
                ctype = "image/jpeg" if data[:2] == b"\xff\xd8" else "image/png"
                self.reply(200, ctype, data, "max-age=604800")
            except (ValueError, IndexError):
                self.reply(404, "text/plain", b"not found")
            except Exception as e:  # noqa: BLE001
                print(f"Тайл {url.path}: {e}")
                self.reply(502, "text/plain", b"tile error")
        elif url.path.startswith("/logo/") and url.path.endswith(".png"):
            data = logo_bytes(url.path[6:-4])
            if data:
                self.reply(200, "image/png", data, "max-age=604800")
            else:
                self.reply(404, "text/plain", b"no logo", "max-age=86400")
        elif url.path == "/health":
            self.reply(200, "text/plain", b"ok")
        else:
            self.reply(404, "text/plain", b"not found")

    def do_POST(self):
        if self.path == "/api/config":
            try:
                update_config(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            except (ValueError, TypeError, AttributeError):
                self.reply(400, "text/plain", b"bad config")
                return
            self.reply(200, "application/json", b'{"status":"ok"}')
        else:
            self.reply(404, "text/plain", b"not found")

    def log_message(self, fmt, *args):  # без строки в журнале на каждый запрос
        pass


if __name__ == "__main__":
    load_config()
    load_cities()
    print(f"Радар: центр {CENTER_LAT}, {CENTER_LON}, порт {PORT}, городов {len(CITIES)}, карта {board_style()}")
    ThreadingHTTPServer(("0.0.0.0", PORT), RadarHandler).serve_forever()

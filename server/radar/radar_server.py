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

Настройки — переменные окружения (см. docker-compose.yml): CENTER_LAT,
CENTER_LON, PORT, CACHE_DIR, MAP_BRIGHTNESS, CITIES_FILE.
"""

import json
import math
import os
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
from urllib.parse import parse_qs, urlparse

try:  # FlightRadarAPI 1.4+ — модуль FlightRadarAPI
    from FlightRadarAPI import FlightRadar24API
except ImportError:  # старые версии — модуль FlightRadar24
    from FlightRadar24 import FlightRadar24API

from PIL import Image, ImageDraw, ImageEnhance, ImageFont

CENTER_LAT = float(os.environ.get("CENTER_LAT", "55.7558"))
CENTER_LON = float(os.environ.get("CENTER_LON", "37.6173"))
PORT = int(os.environ.get("PORT", "8080"))
CACHE_DIR = os.environ.get("CACHE_DIR", "/data")
MAP_BRIGHTNESS = float(os.environ.get("MAP_BRIGHTNESS", "1.15"))
CITIES_FILE = os.environ.get("CITIES_FILE", os.path.join(os.path.dirname(os.path.abspath(__file__)), "cities.json"))
TILE_URL = os.environ.get("TILE_URL", "https://{s}.basemaps.cartocdn.com/dark_nolabels/{z}/{x}/{y}.png")
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

CONFIG = {
    "radius": 200,
    "theme": "night",
    "showLabels": True,
    "showCompass": True,
    "colorByAlt": True,
    "silhouettes": True,
    "showWeather": True,
}

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


def load_cities():
    try:
        with open(CITIES_FILE, encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:  # noqa: BLE001
        print(f"Нет списка городов {CITIES_FILE}: {e}")
        return []


CITIES = load_cities()

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
    return {"r": r_km, "k": round(k, 4), "t": int(time.time()), "p": planes[:MAX_ESP_PLANES]}


# ---------------------------------------------------------------------------
# Карта: тёмные тайлы CartoDB без подписей, склеенные в картинку 466×466 под
# нужный радиус, и свои подписи городов по-русски. Тайлы и готовые карты
# кэшируются на диске

_map_lock = threading.Lock()
_maps = {}


def tile(z, x, y):
    path = os.path.join(CACHE_DIR, "tiles", str(z), str(x), f"{y}.png")
    if os.path.exists(path):
        return Image.open(path).convert("RGB")
    url = TILE_URL.format(s="abcd"[(x + y) % 4], z=z, x=x, y=y)
    req = urllib.request.Request(url, headers={"User-Agent": "ESPHA-radar/1.0"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = resp.read()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return Image.open(BytesIO(data)).convert("RGB")


def render_map(r_km):
    z, s = scale_for(r_km)
    cx, cy = merc(CENTER_LAT, CENTER_LON, z)
    span = W / s  # сколько пикселей тайлов на экран
    x0, y0 = cx - span / 2, cy - span / 2
    tx0, ty0 = int(x0 // 256), int(y0 // 256)
    tx1, ty1 = int((x0 + span) // 256), int((y0 + span) // 256)
    canvas = Image.new("RGB", ((tx1 - tx0 + 1) * 256, (ty1 - ty0 + 1) * 256))
    for tx in range(tx0, tx1 + 1):
        for ty in range(ty0, ty1 + 1):
            try:
                canvas.paste(tile(z, tx, ty), ((tx - tx0) * 256, (ty - ty0) * 256))
            except Exception as e:  # noqa: BLE001
                print(f"Тайл {z}/{tx}/{ty}: {e}")
    left, top = x0 - tx0 * 256, y0 - ty0 * 256
    img = canvas.crop((int(left), int(top), int(left + span), int(top + span))).resize((W, W), Image.LANCZOS)
    if MAP_BRIGHTNESS != 1.0:
        img = ImageEnhance.Brightness(img).enhance(MAP_BRIGHTNESS)

    # Города: точка и название; ближние к центру — в первую очередь, без наложений
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(FONT_FILE, 15)
    except OSError:
        font = ImageFont.load_default()
    used = []
    for c in sorted(CITIES, key=lambda c: dist_km(c["lat"], c["lon"])):
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


def get_map(r_km):
    with _map_lock:
        if r_km not in _maps:
            path = os.path.join(CACHE_DIR, "maps", f"{CENTER_LAT:.4f}_{CENTER_LON:.4f}_{r_km}.jpg")
            if os.path.exists(path):
                with open(path, "rb") as f:
                    _maps[r_km] = f.read()
            else:
                _maps[r_km] = render_map(r_km)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(_maps[r_km])
        return _maps[r_km]


# ---------------------------------------------------------------------------
# Осадки для веб-страницы (как было)

WEATHER_CACHE = {"data": [], "last_update": 0}
WEATHER_CITIES = [c for c in CITIES if c.get("weather")] or [{"lat": CENTER_LAT, "lon": CENTER_LON}]


def get_real_weather():
    now = time.time()
    if now - WEATHER_CACHE["last_update"] < 300 and WEATHER_CACHE["data"]:
        return WEATHER_CACHE["data"]
    try:
        lats = ",".join(str(c["lat"]) for c in WEATHER_CITIES)
        lons = ",".join(str(c["lon"]) for c in WEATHER_CITIES)
        url = f"https://api.open-meteo.com/v1/forecast?latitude={lats}&longitude={lons}&current=precipitation,cloudcover"
        req = urllib.request.Request(url, headers={"User-Agent": "RadarTerminal/1.0"})
        with urllib.request.urlopen(req, timeout=15) as response:
            data = json.loads(response.read().decode())
            results = data if isinstance(data, list) else [data]
            points = []
            for i, loc in enumerate(results[: len(WEATHER_CITIES)]):
                points.append({
                    "lat": WEATHER_CITIES[i]["lat"], "lon": WEATHER_CITIES[i]["lon"],
                    "rain": loc.get("current", {}).get("precipitation", 0),
                    "clouds": loc.get("current", {}).get("cloudcover", 0),
                })
            WEATHER_CACHE["data"] = points
            WEATHER_CACHE["last_update"] = now
            return points
    except Exception as e:  # noqa: BLE001
        print(f"Ошибка метеослужбы: {e}")
        return WEATHER_CACHE["data"]


HTML_PAGE = """
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>Радар PRO: CartoDB Display</title>
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
        <h2>ATC Terminal: CartoDB</h2>
        
        <div class="group">
            <label>Масштаб: <span id="radVal" class="val">200 км</span></label>
            <input id="radius" type="range" min="50" max="300" step="10" value="200" oninput="updateVal('radVal', this.value + ' км'); sendConfig()">
        </div>

        <div class="group">
            <label>Базовый навигационный слой:</label>
            <select id="theme" onchange="sendConfig()">
                <option value="night">CartoDB Dark Matter</option>
                <option value="day">CartoDB Voyager (Color)</option>
            </select>
        </div>
        <hr>
        <div class="switch"><label>Метки и маршруты</label><input type="checkbox" id="showLabels" checked onchange="sendConfig()"></div>
        <div class="switch"><label>Сетка и Компас</label><input type="checkbox" id="showCompass" checked onchange="sendConfig()"></div>
        <div class="switch"><label style="color:var(--accent)">Цвет по высоте</label><input type="checkbox" id="colorByAlt" checked onchange="sendConfig()"></div>
        <div class="switch"><label style="color:var(--accent)">Форма силуэтов</label><input type="checkbox" id="silhouettes" checked onchange="sendConfig()"></div>
        <div class="switch"><label style="color:var(--accent)">Осадки (Метео)</label><input type="checkbox" id="showWeather" checked onchange="sendConfig()"></div>
        
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
        
        let localConfig = { radius: 200, theme: 'night', showLabels: true, showCompass: true, colorByAlt: true, silhouettes: true, showWeather: true };
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

        // Инициализируем единственный слой, который гарантирует стабильность загрузки тайлов
        const mapLayer = L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', { 
            maxZoom: 20, className: 'carto-layer'
        }).addTo(map);

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
                img.src = `https://raw.githubusercontent.com/sexym0nk3y/airline-logos/master/logos/${icao}.png`;
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
                showWeather: document.getElementById('showWeather').checked
            };
            
            document.getElementById('body').className = localConfig.theme === 'day' ? 'day-mode' : '';
            
            // Бесшовная смена URL слоя без разрушения карты
            const newUrl = localConfig.theme === 'day' 
                ? 'https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png'
                : 'https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png';
            mapLayer.setUrl(newUrl);
            
            updateMapBounds();
            fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(localConfig) });
        }
        window.sendConfig = sendConfig; 

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
                        planesState[p.id].vLat = (p.lat - planesState[p.id].currLat) / 8000;
                        planesState[p.id].vLon = (p.lon - planesState[p.id].currLon) / 8000;
                        
                        let diff = p.hdg - planesState[p.id].hdg;
                        diff = ((diff + 180) % 360) - 180;
                        planesState[p.id].targetHdg = planesState[p.id].hdg + diff;
                        planesState[p.id].data = p; 
                    }
                });
                
                for (let id in planesState) { if (!currentIds.has(id)) delete planesState[id]; }
                document.getElementById('statCount').innerText = uniqueFlights.size;
                document.getElementById('statSpeed').innerText = Math.round(maxSpeedLog) + " km/h";
                window.weatherCache = data.weather; 
            });
        }

        function renderLoop() {
            const cx = 200, cy = 200, maxR = 190; 
            const isDay = localConfig.theme === 'day';
            
            const now = performance.now();
            const dt = now - lastFrameTime;
            lastFrameTime = now;
            
            const gridHex = isDay ? 'rgba(120, 130, 140, 0.35)' : 'rgba(100, 120, 140, 0.3)';
            const textHex = isDay ? '#444444' : '#8899AA';

            ctx.clearRect(0, 0, 400, 400);

            // 1. ПОГОДНЫЕ СЕКТОРЫ
            if (localConfig.showWeather && window.weatherCache) {
                window.weatherCache.forEach(w => {
                    if (w.rain > 0 || w.clouds > 40) {
                        const pt = map.latLngToContainerPoint([w.lat, w.lon]);
                        ctx.beginPath();
                        
                        let color = 'rgba(100, 110, 130, 0.12)'; 
                        let radius = 35;
                        
                        if (w.rain > 2.0) {
                            color = 'rgba(230, 50, 50, 0.2)';     
                            radius = 55;
                        } else if (w.rain > 0) {
                            color = 'rgba(230, 130, 0, 0.15)';   
                            radius = 45;
                        }
                        
                        ctx.fillStyle = color;
                        ctx.arc(pt.x, pt.y, radius, 0, Math.PI * 2);
                        ctx.fill();
                    }
                });
            }

            // 2. СЕТКА И КОМПАС
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
                } else {
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
            page = HTML_PAGE.replace("__CENTER_LAT__", str(CENTER_LAT)).replace("__CENTER_LON__", str(CENTER_LON))
            self.reply(200, "text/html; charset=utf-8", page.encode("utf-8"))
        elif url.path == "/esp":
            self.reply(200, "application/json", json.dumps(esp_payload(r), ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
        elif url.path == "/map.jpg":
            try:
                self.reply(200, "image/jpeg", get_map(r), "max-age=86400")
            except Exception as e:  # noqa: BLE001
                print(f"Ошибка карты: {e}")
                self.reply(500, "text/plain", b"map error")
        elif url.path == "/data":
            planes = []
            for f in get_flights():
                p = dict(f)
                p["trail"] = [list(t) for t in f["trail"]]
                planes.append(p)
            body = {"config": CONFIG, "planes": planes, "weather": get_real_weather()}
            self.reply(200, "application/json", json.dumps(body).encode("utf-8"))
        elif url.path == "/health":
            self.reply(200, "text/plain", b"ok")
        else:
            self.reply(404, "text/plain", b"not found")

    def do_POST(self):
        if self.path == "/api/config":
            CONFIG.update(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
            self.reply(200, "application/json", b'{"status":"ok"}')
        else:
            self.reply(404, "text/plain", b"not found")

    def log_message(self, fmt, *args):  # без строки в журнале на каждый запрос
        pass


if __name__ == "__main__":
    print(f"Радар: центр {CENTER_LAT}, {CENTER_LON}, порт {PORT}, городов {len(CITIES)}")
    ThreadingHTTPServer(("0.0.0.0", PORT), RadarHandler).serve_forever()

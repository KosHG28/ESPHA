#!/usr/bin/env python3
"""Иконки самолётов для радара — из проекта AirESP32ace (Vadim Malis,
https://github.com/vmalis/AirESP32ace, лицензия MIT).

Запуск: python3 tools/make_aircraft_icons.py ПУТЬ_К_AirESP32ace

Берёт из firmware/airspace-instrument/main/aircraft_icons.c маски A8:
  - для карты — силуэт сверху, носом вверх, 50×50 — все типы;
  - для карточки самолёта — рисунок сбоку — только для семейств и самых
    частых в России типов (рисунки крупные, все сразу не влезут во флеш).
Упаковывает по 4 бита на точку (вдвое меньше) и пишет:
  - components/radar/aircraft_icons.h — маски и таблица иконок;
  - server/radar/aircraft_types.json — код типа ИКАО → номер иконки и
    название модели (из assets/aircraft_icons/type_registry.json).
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_H = ROOT / "components" / "radar" / "aircraft_icons.h"
OUT_JSON = ROOT / "server" / "radar" / "aircraft_types.json"

# Общие семейства — запасные иконки для типов, которых нет в таблице
FAMILIES = ["light_piston", "single_prop", "turboprop", "regional_jet", "business_jet", "narrowbody_twinjet",
            "widebody_twinjet", "four_engine_widebody", "helicopter"]
FAMILY_LABELS = {"light_piston": "Лёгкий самолёт", "single_prop": "Одномоторный", "turboprop": "Турбовинтовой",
                 "regional_jet": "Региональный", "business_jet": "Бизнес-джет",
                 "narrowbody_twinjet": "Узкофюзеляжный", "widebody_twinjet": "Широкофюзеляжный",
                 "four_engine_widebody": "Четырёхмоторный", "helicopter": "Вертолёт"}
# Рисунки сбоку — для этих типов (остальные берут рисунок своего семейства)
DETAIL = {"airbus_a220", "airbus_a319", "airbus_a320", "airbus_a321", "airbus_a330", "airbus_a350", "boeing_737",
          "boeing_747", "boeing_757", "boeing_767", "boeing_777", "boeing_787", "sukhoi_superjet", "irkut_mc21",
          "embraer_ejet", "bombardier_crj", "atr_72", "dash_8", "antonov_an24", "antonov_an148", "ilyushin_il76",
          "ilyushin_il96", "tupolev_tu204", "let_l410", "mil_mi8"}


def pack4(data):
    """A8 → A4: старшая тетрада — чётная точка"""
    out = bytearray()
    for i in range(0, len(data), 2):
        a = data[i] >> 4
        b = (data[i + 1] >> 4) if i + 1 < len(data) else 0
        out.append((a << 4) | b)
    return bytes(out)


def c_bytes(name, data):
    rows = []
    for i in range(0, len(data), 32):
        rows.append("    " + ",".join(f"0x{v:02x}" for v in data[i:i + 32]) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(rows) + "\n};\n"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src_root = Path(sys.argv[1])
    src = (src_root / "firmware/airspace-instrument/main/aircraft_icons.c").read_text()
    registry = json.loads((src_root / "assets/aircraft_icons/type_registry.json").read_text())

    arrays = {}
    for m in re.finditer(r"static const uint8_t airspace_aircraft_icon_(\w+)_data\[\] = \{(.*?)\};", src, re.S):
        arrays[m.group(1)] = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(2)))
    dims = {}
    for m in re.finditer(r"const lv_image_dsc_t airspace_aircraft_icon_(\w+) = \{.*?\.w = (\d+),\s*\.h = (\d+)", src, re.S):
        dims[m.group(1)] = (int(m.group(2)), int(m.group(3)))

    # Порядок иконок: сначала семейства, потом модели по таблице типов
    names = FAMILIES + [e["cName"] for e in registry]
    labels = [FAMILY_LABELS[f] for f in FAMILIES] + [e["label"] for e in registry]
    fam_of = list(range(len(FAMILIES))) + [FAMILIES.index(e["fallback"]) for e in registry]

    body = []
    table = []
    total = 0
    for i, n in enumerate(names):
        m = arrays[f"{n}_map"]
        w, h = dims[f"{n}_map"]
        assert (w, h) == (50, 50), n
        packed = pack4(m)
        total += len(packed)
        body.append(c_bytes(f"ac_map_{n}", packed))
        det = n if (n in DETAIL or i < len(FAMILIES)) else None
        if det:
            dw, dh = dims[f"{det}_detail"]
            dp = pack4(arrays[f"{det}_detail"])
            total += len(dp)
            body.append(c_bytes(f"ac_det_{det}", dp))
            table.append(f"    {{ac_map_{n}, ac_det_{det}, {dw}, {dh}, {fam_of[i]}}},  // {labels[i]}")
        else:
            table.append(f"    {{ac_map_{n}, nullptr, 0, 0, {fam_of[i]}}},  // {labels[i]}")

    OUT_H.write_text(
        "// Сгенерировано tools/make_aircraft_icons.py — не править вручную.\n"
        "// Иконки самолётов из проекта AirESP32ace © 2026 Vadim Malis, лицензия MIT\n"
        "// (https://github.com/vmalis/AirESP32ace). Маски по 4 бита на точку,\n"
        "// старшая тетрада — левая точка; на экране распаковываются в A8.\n"
        "#pragma once\n\n#include <cstdint>\n\nnamespace esphome {\nnamespace radar {\n\n"
        + "\n".join(body)
        + "\nstruct AcIcon {\n  const uint8_t *map;     ///< сверху, носом вверх, 50×50\n"
          "  const uint8_t *detail;  ///< сбоку; nullptr — взять у семейства\n"
          "  uint8_t dw, dh;\n  uint8_t family;          ///< номер иконки семейства\n};\n"
          f"static const int AC_MAP = 50;\nstatic const int AC_ICONS = {len(names)};\n"
          "static const AcIcon AC_ICON[] = {\n" + "\n".join(table) + "\n};\n"
        "\n}  // namespace radar\n}  // namespace esphome\n"
    )

    codes = {}
    for i, e in enumerate(registry):
        for c in e["codes"]:
            codes[c] = [len(FAMILIES) + i, e["label"]]
    OUT_JSON.write_text(json.dumps({
        "source": "AirESP32ace © 2026 Vadim Malis, MIT — https://github.com/vmalis/AirESP32ace",
        "families": {f: [i, FAMILY_LABELS[f]] for i, f in enumerate(FAMILIES)},
        "codes": codes,
    }, ensure_ascii=False, indent=0) + "\n")
    print(f"{len(names)} icons, {total // 1024} KB packed -> {OUT_H}; {len(codes)} type codes -> {OUT_JSON}")


if __name__ == "__main__":
    main()

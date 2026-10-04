#!/usr/bin/env python3
"""Рисует планеты для ночного неба (Венера, Юпитер, Марс, Сатурн) и
записывает их в components/weather_fx/planets.h как изображения LVGL
(RGB565A8). Запуск: python3 tools/make_planets.py [папка_для_png]

В отличие от гостей (tools/make_sprites.py) планеты не пиксель-арт: каждая
точка считается по 16 подвыборкам, края сглажены, у дисков — затемнение к
краю, как у настоящих. Картинки сразу нужного размера, без увеличения.
Показываются, когда часы убраны и небо на весь экран (weather_fx, full).
"""
import math
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_sprites import c_array  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "components" / "weather_fx" / "planets.h"
SS = 4  # подвыборок на сторону
K = 1.35  # во сколько раз крупнее базовых размеров ниже


def render(w, h, fn):
    """fn(x, y) → (r, g, b, a) в долях 0..1 или None; x, y — в пикселях"""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    c = fn(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS)
                    if c is None:
                        continue
                    r += c[0] * c[3]
                    g += c[1] * c[3]
                    b += c[2] * c[3]
                    a += c[3]
            n = SS * SS
            if a > 0:
                px[x, y] = (round(r / a * 255), round(g / a * 255), round(b / a * 255), round(a / n * 255))
    return im


def mix(c0, c1, t):
    return tuple(c0[i] + (c1[i] - c0[i]) * t for i in range(3))


def disc(cx, cy, rad, color_at, light=(-0.45, -0.55)):
    """Шар: цвет поверхности color_at(u, v) (u, v — от −1 до 1 по диску),
    освещён слева сверху, к краю темнее"""
    def fn(x, y):
        u, v = (x - cx) / rad, (y - cy) / rad
        d2 = u * u + v * v
        if d2 > 1.0:
            return None
        z = math.sqrt(1.0 - d2)
        shade = 0.35 + 0.65 * max(0.0, -light[0] * u - light[1] * v + 0.62 * z)
        shade = min(1.15, shade)
        c = color_at(u, v)
        return (min(1.0, c[0] * shade), min(1.0, c[1] * shade), min(1.0, c[2] * shade), 1.0)
    return fn


def glow(cx, cy, r0, r1, color, peak):
    """Мягкое сияние вокруг шара — от r0 до r1, гаснет к краю"""
    def fn(x, y):
        d = math.hypot(x - cx, y - cy)
        if d < r0 or d > r1:
            return None
        t = 1.0 - (d - r0) / (r1 - r0)
        return (*color, peak * t * t)
    return fn


def layers(*fns):
    """Сложить слои: верхний — последний"""
    def fn(x, y):
        out = None
        for f in fns:
            c = f(x, y)
            if c is None:
                continue
            if out is None:
                out = c
                continue
            a = c[3] + out[3] * (1 - c[3])
            if a <= 0:
                continue
            out = tuple((c[i] * c[3] + out[i] * out[3] * (1 - c[3])) / a for i in range(3)) + (a,)
        return out
    return fn


def venus():
    # Самая яркая: молочно-жёлтый шар в сиянии
    size = round(30 * K)
    c = size / 2
    body = disc(c, c, 9.5 * K, lambda u, v: mix((1.0, 0.97, 0.86), (0.96, 0.86, 0.62), (v + 1) / 2))
    return render(size, size, layers(glow(c, c, 8.5 * K, 15 * K, (1.0, 0.93, 0.75), 0.55), body))


def jupiter():
    # Полосатый гигант с Большим красным пятном
    size = round(32 * K)
    c = size / 2
    bands = [(-0.62, -0.42), (-0.18, 0.02), (0.28, 0.44)]

    def surf(u, v):
        col = (0.93, 0.85, 0.70)
        for b0, b1 in bands:
            if b0 <= v <= b1:
                col = (0.72, 0.52, 0.36)
        if -0.42 < v < -0.18:
            col = (0.97, 0.91, 0.80)
        # Пятно
        if ((u - 0.32) / 0.22) ** 2 + ((v - 0.13) / 0.11) ** 2 < 1:
            col = (0.80, 0.40, 0.28)
        return col
    return render(size, size, disc(c, c, 14.5 * K, surf))


def mars():
    # Красная планета: тёмные пятна и белая шапка у полюса
    size = round(22 * K)
    c = size / 2

    def surf(u, v):
        col = (0.86, 0.40, 0.22)
        if v < -0.72:
            return (0.98, 0.96, 0.94)
        if ((u + 0.25) / 0.45) ** 2 + ((v - 0.05) / 0.2) ** 2 < 1 or ((u - 0.35) / 0.25) ** 2 + ((v - 0.45) / 0.18) ** 2 < 1:
            col = (0.62, 0.26, 0.16)
        return col
    return render(size, size, disc(c, c, 10 * K, surf))


def saturn():
    # Шар с полосами и наклонными кольцами: задняя половина кольца за шаром,
    # передняя — перед ним
    w, h = round(54 * K), round(32 * K)
    cx, cy = w / 2, h / 2
    tilt = math.radians(-14)
    ca, sa = math.cos(tilt), math.sin(tilt)
    body = disc(cx, cy, 10 * K, lambda u, v: (0.93, 0.83, 0.58) if abs(v - 0.15) > 0.12 else (0.80, 0.66, 0.42))

    def ring(front):
        def fn(x, y):
            dx, dy = x - cx, y - cy
            rx, ry = dx * ca + dy * sa, -dx * sa + dy * ca
            e = (rx / (25.5 * K)) ** 2 + (ry / (7.2 * K)) ** 2
            if e > 1.0:
                return None
            ei = (rx / (15.5 * K)) ** 2 + (ry / (4.4 * K)) ** 2
            if ei < 1.0:
                return None
            if (ry > 0) != front:
                return None
            # Щель Кассини
            k = math.sqrt(e)
            if 0.80 < k < 0.85:
                return None
            col = (0.85, 0.76, 0.56) if k < 0.80 else (0.72, 0.64, 0.48)
            return (*col, 0.95)
        return fn
    return render(w, h, layers(ring(False), body, ring(True)))


def main():
    sp = {"venus": venus(), "jupiter": jupiter(), "mars": mars(), "saturn": saturn()}
    if len(sys.argv) > 1:
        dst = Path(sys.argv[1])
        dst.mkdir(parents=True, exist_ok=True)
        for k, v in sp.items():
            v.save(dst / f"planet_{k}.png")
    body = "\n".join(c_array(f"pl_{k}", v) for k, v in sp.items())
    OUT.write_text(
        "// Сгенерировано tools/make_planets.py — не править вручную.\n"
        "#pragma once\n\n#include <lvgl.h>\n\nnamespace esphome {\nnamespace weather_fx {\n\n"
        + body + "\n}  // namespace weather_fx\n}  // namespace esphome\n"
    )
    print(f"{len(sp)} planets -> {OUT}")


if __name__ == "__main__":
    main()

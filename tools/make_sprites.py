#!/usr/bin/env python3
"""Рисует пиксельные спрайты «гостей» экрана (кот, птица, улитка, снеговик,
праздничный колпак, зонтик, снежинка) и записывает их в components/critters/sprites.h как
изображения LVGL (ARGB8888). Запуск: python3 tools/make_sprites.py [папка_для_png]

Колпак — отдельная картинка: в праздники она едет поверх кота. Для каждого
кадра кота записывается точка на макушке между ушами (HAT_ANCHORS), куда
ставится середина нижнего края колпака.

Спрайты рисуются примитивами без сглаживания в маленькой сетке и
увеличиваются в SCALE раз «по пикселям» — получается пиксель-арт.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

# Во сколько раз увеличивать: кот крупный, остальные гости поменьше
SCALE_CAT = 4
SCALE_OTHER = 3
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "components" / "critters" / "sprites.h"

# Рыжий кот
FUR = (245, 161, 66, 255)
FUR_D = (196, 106, 30, 255)
EYE = (30, 24, 20, 255)
NOSE = (255, 140, 160, 255)
BELLY = (255, 214, 160, 255)


def canvas(w, h):
    return Image.new("RGBA", (w, h), (0, 0, 0, 0))


def cat_run(phase):
    """Кот бежит вправо. phase 0..3 — положение лап."""
    im = canvas(26, 16)
    d = ImageDraw.Draw(im)
    bob = [0, -1, 0, 1][phase]
    # хвост — дуга назад и вверх
    tail = [[(0, 4), (1, 5), (2, 6), (3, 7)], [(0, 7), (1, 7), (2, 7), (3, 7)],
            [(0, 3), (1, 4), (2, 5), (3, 6)], [(0, 6), (1, 6), (2, 7), (3, 7)]][phase]
    for x, y in tail:
        d.rectangle([x, y + bob, x, y + 1 + bob], fill=FUR)
    # туловище
    d.ellipse([3, 5 + bob, 18, 11 + bob], fill=FUR)
    d.line([6, 6 + bob, 6, 8 + bob], fill=FUR_D)
    d.line([10, 6 + bob, 10, 8 + bob], fill=FUR_D)
    d.line([14, 6 + bob, 14, 8 + bob], fill=FUR_D)
    d.line([7, 10 + bob, 15, 10 + bob], fill=BELLY)
    # голова
    d.ellipse([16, 2 + bob, 24, 9 + bob], fill=FUR)
    d.polygon([(17, 3 + bob), (18, 0 + bob), (20, 3 + bob)], fill=FUR)
    d.polygon([(21, 3 + bob), (23, 0 + bob), (24, 4 + bob)], fill=FUR)
    d.point((22, 5 + bob), fill=EYE)
    d.point((24, 6 + bob), fill=NOSE)
    # лапы: четыре фазы бега
    legs = {
        0: [(5, 3, 1), (8, 2, 0), (14, 2, 1), (17, 3, 0)],
        1: [(6, 1, 0), (8, 0, 0), (15, 1, 0), (16, 0, 0)],
        2: [(4, 2, 0), (9, 3, 1), (13, 3, 0), (17, 2, 1)],
        3: [(6, 1, 0), (7, 0, 0), (15, 0, 0), (16, 1, 0)],
    }[phase]
    for x, dx, _ in legs:
        y0 = 10 + bob
        d.line([x, y0, x + dx - 1 if dx > 1 else x, 14], fill=FUR_D if x < 11 else FUR)
        d.point((x + (dx - 1 if dx > 1 else 0), 15), fill=FUR_D)
    return im


def cat_sit(blink=False, look=False, paw=False):
    """Кот сидит. look — смотрит вверх (глаза выше), paw — поднял лапу к морде."""
    im = canvas(26, 16)
    d = ImageDraw.Draw(im)
    # хвост обвивает лапы
    d.line([3, 15, 12, 15], fill=FUR_D)
    d.line([2, 14, 2, 12], fill=FUR_D)
    # туловище
    d.ellipse([6, 6, 17, 15], fill=FUR)
    d.line([9, 8, 9, 11], fill=FUR_D)
    d.line([12, 9, 12, 12], fill=FUR_D)
    d.ellipse([12, 10, 16, 15], fill=BELLY)
    # голова
    d.ellipse([11, 1, 20, 8], fill=FUR)
    d.polygon([(11, 3), (12, 0), (14, 2)], fill=FUR)
    d.polygon([(17, 2), (19, 0), (20, 3)], fill=FUR)
    ey = 3 if look else 4
    if blink:
        d.line([13, ey, 14, ey], fill=EYE)
        d.line([17, ey, 18, ey], fill=EYE)
    else:
        d.point((14, ey), fill=EYE)
        d.point((17, ey), fill=EYE)
    d.point((15, 6), fill=NOSE)
    d.point((16, 6), fill=NOSE)
    # передние лапы: одна поднята к морде — ловит снежинку
    d.line([13, 13, 13, 15], fill=FUR)
    if paw:
        d.line([17, 12, 20, 8], fill=FUR)
        d.line([18, 12, 21, 8], fill=FUR)
        d.rectangle([20, 6, 22, 8], fill=FUR)
        d.point((21, 6), fill=NOSE)
    else:
        d.line([16, 13, 16, 15], fill=FUR)
    return im


# Кончик поднятой лапы в спрайте cat_paw_r (клетки сетки): сюда садится снежинка
PAW_TIP = (21.5, 6.5)


def umbrella():
    """Зонтик: голубой купол с тёмными спицами и ручка-крючок."""
    im = canvas(18, 14)
    d = ImageDraw.Draw(im)
    d.pieslice([0, 1, 17, 13], 180, 360, fill=(66, 165, 245, 255))
    rib = (25, 118, 210, 255)
    d.line([9, 1, 4, 7], fill=rib)
    d.line([9, 1, 13, 7], fill=rib)
    d.line([0, 7, 17, 7], fill=rib)
    d.point((9, 0), fill=(40, 40, 50, 255))
    d.line([9, 8, 9, 12], fill=(90, 70, 60, 255))
    d.point((8, 13), fill=(90, 70, 60, 255))
    d.point((7, 12), fill=(90, 70, 60, 255))
    return im


def snowflake():
    """Снежинка, которую ловит кот."""
    im = canvas(7, 7)
    d = ImageDraw.Draw(im)
    c = (235, 245, 255, 255)
    d.line([3, 0, 3, 6], fill=c)
    d.line([0, 3, 6, 3], fill=c)
    d.line([1, 1, 5, 5], fill=c)
    d.line([1, 5, 5, 1], fill=c)
    return im


def cat_sleep():
    im = canvas(26, 16)
    d = ImageDraw.Draw(im)
    d.ellipse([2, 6, 22, 15], fill=FUR)
    d.arc([4, 8, 20, 15], 200, 340, fill=FUR_D)
    # голова лежит на лапах
    d.ellipse([15, 5, 23, 12], fill=FUR)
    d.polygon([(16, 7), (17, 3), (19, 6)], fill=FUR)
    d.polygon([(20, 6), (22, 3), (23, 8)], fill=FUR)
    d.line([18, 9, 19, 9], fill=EYE)
    d.line([21, 9, 22, 9], fill=EYE)
    # хвост вокруг
    d.line([3, 15, 16, 15], fill=FUR_D)
    return im


def bird(up):
    im = canvas(16, 10)
    d = ImageDraw.Draw(im)
    body = (120, 170, 230, 255)
    wing = (80, 130, 200, 255)
    d.ellipse([3, 4, 12, 8], fill=body)
    d.ellipse([10, 3, 14, 6], fill=body)
    d.point((12, 4), fill=EYE)
    d.polygon([(15, 5), (14, 4), (14, 6)], fill=(255, 190, 60, 255))
    d.polygon([(0, 5), (3, 5), (3, 7)], fill=wing)
    if up:
        d.polygon([(5, 5), (8, 0), (10, 5)], fill=wing)
    else:
        d.polygon([(5, 6), (8, 10), (10, 6)], fill=wing)
    return im


def snail(up):
    im = canvas(20, 12)
    d = ImageDraw.Draw(im)
    body = (190, 200, 150, 255)
    shell = (180, 120, 70, 255)
    shell_d = (130, 80, 45, 255)
    d.rectangle([2, 9, 18, 11], fill=body)
    d.ellipse([15, 6, 19, 11], fill=body)
    stalk = 2 if up else 3
    d.line([17, 6, 17, stalk], fill=body)
    d.line([19, 6, 19, stalk + 1], fill=body)
    d.point((17, stalk - 1), fill=EYE)
    d.point((19, stalk), fill=EYE)
    d.ellipse([3, 1, 14, 10], fill=shell)
    d.arc([5, 3, 12, 9], 0, 300, fill=shell_d)
    d.arc([7, 5, 10, 8], 0, 300, fill=shell_d)
    return im


def snowman(arms_up):
    im = canvas(16, 22)
    d = ImageDraw.Draw(im)
    snow = (235, 242, 250, 255)
    d.ellipse([2, 11, 13, 21], fill=snow)
    d.ellipse([4, 5, 11, 12], fill=snow)
    d.rectangle([5, 1, 10, 4], fill=(40, 40, 50, 255))
    d.line([4, 5, 11, 5], fill=(40, 40, 50, 255))
    d.point((6, 7), fill=EYE)
    d.point((9, 7), fill=EYE)
    d.line([8, 9, 10, 9], fill=(255, 140, 40, 255))
    d.point((7, 14), fill=EYE)
    d.point((7, 17), fill=EYE)
    arm = (140, 95, 60, 255)
    if arms_up:
        d.line([3, 13, 0, 9], fill=arm)
        d.line([12, 13, 15, 9], fill=arm)
    else:
        d.line([3, 14, 0, 16], fill=arm)
        d.line([12, 14, 15, 16], fill=arm)
    return im


def party_hat():
    """Праздничный колпак: розовый конус с жёлтыми полосками и помпоном."""
    im = canvas(8, 10)
    d = ImageDraw.Draw(im)
    d.polygon([(4, 2), (1, 9), (7, 9)], fill=(236, 64, 122, 255))
    d.line([3, 5, 5, 5], fill=(255, 213, 79, 255))
    d.line([2, 7, 6, 7], fill=(255, 213, 79, 255))
    d.ellipse([3, 0, 5, 2], fill=(255, 255, 255, 255))
    return im


# Макушка кота в каждом кадре (в клетках исходной сетки 26×16): середина между
# кончиками ушей и верх головы. Для отражённых кадров x = 26 − x
def hat_anchors():
    a = {}
    for p in range(4):
        bob = [0, -1, 0, 1][p]
        a[f"cat_run_r{p}"] = (21.0, 3 + bob)
        a[f"cat_run_l{p}"] = (26 - 21.0, 3 + bob)
    for name in ("cat_sit", "cat_blink", "cat_look", "cat_paw"):
        a[f"{name}_r"] = (16.0, 2)
        a[f"{name}_l"] = (26 - 16.0, 2)
    a["cat_sleep"] = (20.0, 6)
    return a


def scaled(im, k):
    return im.resize((im.width * k, im.height * k), Image.NEAREST)


def mirrored(im):
    return im.transpose(Image.FLIP_LEFT_RIGHT)


def sprites():
    out = {}
    for p in range(4):
        out[f"cat_run_r{p}"] = cat_run(p)
        out[f"cat_run_l{p}"] = mirrored(cat_run(p))
    out["cat_sit_r"] = cat_sit()
    out["cat_blink_r"] = cat_sit(True)
    out["cat_sit_l"] = mirrored(cat_sit())
    out["cat_blink_l"] = mirrored(cat_sit(True))
    out["cat_look_r"] = cat_sit(look=True)
    out["cat_look_l"] = mirrored(cat_sit(look=True))
    out["cat_paw_r"] = cat_sit(paw=True)
    out["cat_paw_l"] = mirrored(cat_sit(paw=True))
    out["cat_sleep"] = cat_sleep()
    for up in (0, 1):
        out[f"bird_r{up}"] = bird(up)
        out[f"bird_l{up}"] = mirrored(bird(up))
        out[f"snail_l{up}"] = mirrored(snail(up))
        out[f"snowman{up}"] = snowman(up)
    out["cat_hat"] = party_hat()
    out["cat_umbrella"] = umbrella()
    out["flake"] = snowflake()
    return {k: scaled(v, SCALE_CAT if k.startswith("cat") else SCALE_OTHER) for k, v in out.items()}


def c_array(name, im):
    # LVGL ARGB8888: в памяти байты B, G, R, A
    data = bytearray()
    px = im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata()
    for r, g, b, a in px:
        data += bytes((b, g, r, a))
    rows = []
    for i in range(0, len(data), 24):
        rows.append("  " + ", ".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
    return (
        f"static const uint8_t {name}_px[] = {{\n" + "\n".join(rows) + "\n};\n"
        f"static const lv_image_dsc_t {name} = {{\n"
        f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .flags = 0,\n"
        f"               .w = {im.width}, .h = {im.height}, .stride = {im.width * 4}, .reserved_2 = 0}},\n"
        f"    .data_size = sizeof({name}_px),\n"
        f"    .data = {name}_px,\n"
        f"    .reserved = nullptr,\n"
        f"}};\n"
    )


def main():
    sp = sprites()
    if len(sys.argv) > 1:
        dst = Path(sys.argv[1])
        dst.mkdir(parents=True, exist_ok=True)
        for k, v in sp.items():
            v.save(dst / f"{k}.png")
    body = "\n".join(c_array(f"spr_{k}", v) for k, v in sp.items())
    rows = [f"    {{&spr_{k}, {round(x * SCALE_CAT)}, {round(y * SCALE_CAT)}}},"
            for k, (x, y) in hat_anchors().items()]
    body += (
        "\n// Куда ставить колпак: середина его нижнего края, в пикселях спрайта\n"
        "struct HatAnchor {\n  const lv_image_dsc_t *img;\n  int16_t x, y;\n};\n"
        "static const HatAnchor HAT_ANCHORS[] = {\n" + "\n".join(rows) + "\n};\n"
        "\n// Кончик поднятой лапы: [0] — кот смотрит вправо, [1] — влево\n"
        f"static const int16_t PAW_TIP[2][2] = {{{{{round(PAW_TIP[0] * SCALE_CAT)}, {round(PAW_TIP[1] * SCALE_CAT)}}}, "
        f"{{{round((26 - PAW_TIP[0]) * SCALE_CAT)}, {round(PAW_TIP[1] * SCALE_CAT)}}}}};\n"
    )
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(
        "// Сгенерировано tools/make_sprites.py — не править вручную.\n"
        "#pragma once\n\n#include <lvgl.h>\n\nnamespace esphome {\nnamespace critters {\n\n"
        + body + "\n}  // namespace critters\n}  // namespace esphome\n"
    )
    print(f"{len(sp)} sprites -> {OUT}")


if __name__ == "__main__":
    main()

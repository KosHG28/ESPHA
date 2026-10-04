#!/usr/bin/env python3
"""Рисует пиксельные спрайты «гостей» экрана (кот, птица, улитка, снеговик,
сова, бабочка, ёжик, тыква, мышка, чёрный кот) и праздничные вещи кота
(колпак, шапка Деда Мороза, пилотка, корона, шлем космонавта, ранец, зонтик,
миска, кулич, блины, снежинка) и записывает их в components/critters/sprites.h
как изображения LVGL (RGB565A8). Запуск: python3 tools/make_sprites.py [папка_для_png]

Спрайты хранятся в исходной маленькой сетке — каждая клетка один пиксель.
Увеличивает их компонент critters при показе, «по пикселям»: кот в 6 раз,
мышка в 4, остальные гости в 3. Так прошивка не растёт от размера кота.

Для каждого кадра кота записываются три точки (CAT_ANCHORS, в полуклетках):
макушка между ушами — туда ставится середина нижнего края шапки, середина
головы — для шлема космонавта, и спина — для ранца.

Спрайты рисуются примитивами без сглаживания — получается пиксель-арт.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

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


def cat_drink(lap):
    """Кот пригнулся и лакает из миски справа. lap — язык высунут."""
    im = canvas(26, 16)
    d = ImageDraw.Draw(im)
    # хвост лежит сзади
    d.line([0, 13, 4, 11], fill=FUR_D)
    # туловище пригнуто
    d.ellipse([3, 7, 18, 14], fill=FUR)
    d.line([7, 8, 7, 10], fill=FUR_D)
    d.line([11, 8, 11, 10], fill=FUR_D)
    d.line([7, 13, 15, 13], fill=BELLY)
    # голова опущена к миске
    hy = 1 if lap else 0
    d.ellipse([16, 7 + hy, 23, 13 + hy], fill=FUR)
    d.polygon([(16, 8 + hy), (17, 5 + hy), (19, 8 + hy)], fill=FUR)
    d.polygon([(20, 8 + hy), (22, 5 + hy), (23, 9 + hy)], fill=FUR)
    d.line([20, 10 + hy, 21, 10 + hy], fill=EYE)  # глаза прикрыты
    d.point((23, 11 + hy), fill=NOSE)
    if lap:
        d.point((24, 13), fill=(255, 120, 150, 255))  # язык
    # лапы
    d.line([6, 13, 6, 15], fill=FUR_D)
    d.line([15, 13, 15, 15], fill=FUR)
    return im


def cat_flat():
    """Жара: кот лежит пластом, лапы врозь, язык наружу, над головой капля пота."""
    im = canvas(26, 16)
    d = ImageDraw.Draw(im)
    # хвост вытянут по земле
    d.line([0, 15, 4, 14], fill=FUR_D)
    # туловище распластано
    d.ellipse([3, 10, 19, 15], fill=FUR)
    d.line([7, 11, 7, 12], fill=FUR_D)
    d.line([11, 11, 11, 12], fill=FUR_D)
    d.line([15, 11, 15, 12], fill=FUR_D)
    # лапы в стороны
    d.line([2, 15, 5, 13], fill=FUR_D)
    d.line([17, 15, 20, 13], fill=FUR)
    # голова лежит на земле
    d.ellipse([17, 8, 25, 14], fill=FUR)
    d.polygon([(18, 10), (19, 6), (21, 9)], fill=FUR)
    d.polygon([(22, 9), (24, 6), (25, 10)], fill=FUR)
    d.line([20, 11, 21, 11], fill=EYE)
    d.line([23, 11, 24, 11], fill=EYE)
    d.point((25, 12), fill=NOSE)
    d.point((24, 14), fill=(255, 120, 150, 255))  # язык
    d.point((24, 15), fill=(255, 120, 150, 255))
    # капля пота
    d.point((16, 6), fill=(130, 200, 255, 255))
    d.line([15, 7, 17, 7], fill=(130, 200, 255, 255))
    d.point((16, 8), fill=(90, 170, 240, 255))
    return im


def scarf():
    """Шарф в мороз: красный в белую полоску, кончик свисает назад."""
    im = canvas(9, 7)
    d = ImageDraw.Draw(im)
    red = (215, 40, 50, 255)
    white = (245, 245, 245, 255)
    d.rectangle([1, 1, 8, 3], fill=red)
    for x in (3, 6):
        d.line([x, 1, x, 3], fill=white)
    # кончик свисает назад (влево для кота, смотрящего вправо)
    d.rectangle([0, 3, 2, 6], fill=red)
    d.line([0, 5, 2, 5], fill=white)
    d.point((0, 6), fill=white)
    d.point((2, 6), fill=white)
    return im


# Середина шарфа на шее — точка в спрайте шарфа (клетки)
SCARF_AT = (4.5, 2.0)


def mouse(step):
    """Серая мышка бежит вправо: хвост, ушко, глаз, лапки."""
    im = canvas(14, 7)
    d = ImageDraw.Draw(im)
    grey = (170, 170, 180, 255)
    dark = (120, 120, 130, 255)
    d.line([0, 3, 3, 4], fill=(230, 160, 170, 255))  # хвост
    d.ellipse([3, 2, 11, 6], fill=grey)
    d.polygon([(10, 3), (13, 4), (10, 5)], fill=grey)  # мордочка
    d.ellipse([8, 1, 10, 3], fill=(240, 170, 180, 255))  # ушко
    d.point((11, 3), fill=EYE)
    d.point((13, 4), fill=(240, 150, 160, 255))
    feet = [(5, 6), (9, 6)] if step else [(4, 6), (10, 6)]
    for x, y in feet:
        d.point((x, y), fill=dark)
    return im


def bowl():
    """Миска с водой."""
    im = canvas(9, 5)
    d = ImageDraw.Draw(im)
    d.polygon([(0, 1), (8, 1), (7, 4), (1, 4)], fill=(66, 165, 245, 255))
    d.line([1, 1, 7, 1], fill=(179, 229, 252, 255))
    d.line([2, 4, 6, 4], fill=(25, 118, 210, 255))
    return im


# Миска стоит у морды пьющего кота: середина её низа, в клетках сетки кота
BOWL_AT = (25.0, 16.0)


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


def santa_hat():
    """Шапка Деда Мороза: красный колпак свисает набок, белая опушка и помпон."""
    im = canvas(12, 10)
    d = ImageDraw.Draw(im)
    red = (220, 38, 38, 255)
    red_d = (170, 20, 30, 255)
    d.polygon([(1, 8), (10, 8), (8, 3), (5, 1), (3, 3)], fill=red)
    d.polygon([(5, 1), (8, 0), (10, 2), (8, 3)], fill=red_d)
    d.ellipse([9, 1, 11, 3], fill=(255, 255, 255, 255))
    d.rectangle([0, 7, 11, 9], fill=(245, 245, 245, 255))
    d.line([1, 9, 10, 9], fill=(215, 220, 230, 255))
    return im


def pilotka():
    """Пилотка защитника: зелёная «лодочка» с красной звёздочкой."""
    im = canvas(12, 5)
    d = ImageDraw.Draw(im)
    khaki = (104, 120, 58, 255)
    dark = (74, 88, 40, 255)
    d.polygon([(0, 4), (1, 1), (5, 0), (11, 1), (11, 4)], fill=khaki)
    d.line([1, 3, 11, 3], fill=dark)
    d.point((7, 1), fill=(230, 40, 40, 255))
    d.point((6, 2), fill=(230, 40, 40, 255))
    d.point((8, 2), fill=(230, 40, 40, 255))
    d.point((7, 2), fill=(255, 80, 60, 255))
    return im


def crown():
    """Золотая корона с камнями — в День кошек."""
    im = canvas(10, 7)
    d = ImageDraw.Draw(im)
    gold = (255, 196, 30, 255)
    gold_d = (205, 145, 10, 255)
    d.polygon([(0, 6), (0, 1), (2, 3), (5, 0), (7, 3), (9, 1), (9, 6)], fill=gold)
    d.line([0, 6, 9, 6], fill=gold_d)
    d.point((5, 0), fill=(255, 240, 160, 255))
    d.point((2, 4), fill=(230, 40, 60, 255))
    d.point((5, 4), fill=(40, 120, 255, 255))
    d.point((7, 4), fill=(230, 40, 60, 255))
    return im


def helmet():
    """Шлем космонавта: прозрачный стеклянный пузырь со светлым ободком и бликом."""
    im = canvas(13, 12)
    d = ImageDraw.Draw(im)
    d.ellipse([0, 0, 12, 11], fill=(150, 210, 255, 48), outline=(225, 240, 255, 230))
    d.line([3, 2, 5, 1], fill=(255, 255, 255, 220))
    d.point((2, 3), fill=(255, 255, 255, 200))
    # воротник
    d.rectangle([2, 10, 10, 11], fill=(200, 205, 215, 255))
    return im


def backpack():
    """Школьный ранец: красный, с синей крышкой и застёжкой."""
    im = canvas(7, 7)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 1, 6, 6], fill=(220, 50, 50, 255))
    d.rectangle([0, 1, 6, 3], fill=(40, 90, 200, 255))
    d.point((3, 3), fill=(255, 210, 60, 255))
    d.line([1, 0, 5, 0], fill=(120, 60, 40, 255))
    return im


def kulich():
    """Пасхальный кулич: высокий, румяный, белая глазурь стекает, посыпка."""
    im = canvas(9, 12)
    d = ImageDraw.Draw(im)
    crust = (190, 112, 40, 255)
    crust_d = (150, 82, 25, 255)
    d.rectangle([1, 4, 7, 11], fill=crust)
    d.line([1, 11, 7, 11], fill=crust_d)
    d.line([7, 5, 7, 11], fill=crust_d)
    d.ellipse([0, 0, 8, 5], fill=(250, 250, 245, 255))
    d.line([1, 5, 1, 6], fill=(250, 250, 245, 255))
    d.line([4, 5, 4, 7], fill=(250, 250, 245, 255))
    d.line([7, 5, 7, 6], fill=(250, 250, 245, 255))
    for x, y, c in ((2, 2, (255, 64, 129, 255)), (4, 1, (64, 196, 255, 255)), (6, 2, (255, 214, 0, 255)),
                    (3, 3, (105, 240, 174, 255)), (5, 3, (255, 64, 129, 255))):
        d.point((x, y), fill=c)
    return im


def pancakes():
    """Стопка блинов на тарелке, сверху кусочек масла."""
    im = canvas(12, 6)
    d = ImageDraw.Draw(im)
    d.ellipse([0, 3, 11, 5], fill=(235, 240, 245, 255))
    gold = (242, 184, 75, 255)
    edge = (208, 138, 46, 255)
    for k, y in enumerate((3, 2, 1)):
        d.rectangle([1 + (k % 2), y, 10 - (k % 2), y], fill=gold if k % 2 == 0 else edge)
    d.rectangle([2, 0, 9, 0], fill=gold)
    d.rectangle([5, 0, 6, 0], fill=(255, 236, 140, 255))
    return im


# Чёрный кот для пятницы, 13-го: тот же бегущий кот, перекрашенный
BLACK = {FUR: (46, 46, 56, 255), FUR_D: (24, 24, 30, 255), BELLY: (70, 70, 82, 255),
         EYE: (255, 214, 0, 255), NOSE: (150, 100, 120, 255)}


def recolor(im, table):
    out = im.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            c = px[x, y]
            if c in table:
                px[x, y] = table[c]
    return out


# Точки на коте в каждом кадре (в клетках сетки 26×16): макушка между ушами
# (низ шапки), середина головы (шлем), спина (ранец) и шея (шарф). Для
# отражённых кадров x = 26 − x
def cat_anchors():
    a = {}

    m = lambda q: (26 - q[0], q[1])

    def both(name, *pts):
        a[name + "_r"] = pts
        a[name + "_l"] = tuple(m(p) for p in pts)

    for p in range(4):
        bob = [0, -1, 0, 1][p]
        pts = ((21.0, 3 + bob), (20.5, 5.5 + bob), (10.0, 5.5 + bob), (17.5, 7.5 + bob))
        a[f"cat_run_r{p}"] = pts
        a[f"cat_run_l{p}"] = tuple(m(q) for q in pts)
    for name in ("cat_sit", "cat_blink", "cat_look", "cat_paw"):
        both(name, (16.0, 2), (15.5, 4.5), (8.0, 9.5), (15.0, 8.5))
    for f in (0, 1):
        hy = 1 if f else 0
        pts = ((20.0, 6 + hy), (19.5, 10 + hy), (10.0, 8.0), (17.0, 11.0 + hy))
        a[f"cat_drink_r{f}"] = pts
        a[f"cat_drink_l{f}"] = tuple(m(q) for q in pts)
    both("cat_flat", (21.5, 7), (21.0, 11.0), (11.0, 10.0), (18.5, 12.5))
    a["cat_sleep"] = ((20.0, 6), (19.0, 8.5), (10.0, 7.0), (16.5, 10.0))
    return a


def owl(blink):
    """Сова сидит на ветке: большие жёлтые глаза, ушки-перья."""
    im = canvas(14, 18)
    d = ImageDraw.Draw(im)
    body = (140, 110, 80, 255)
    dark = (95, 72, 50, 255)
    belly = (205, 180, 140, 255)
    d.ellipse([1, 3, 12, 16], fill=body)
    d.polygon([(1, 5), (2, 0), (5, 3)], fill=body)
    d.polygon([(12, 5), (11, 0), (8, 3)], fill=body)
    d.ellipse([4, 9, 9, 16], fill=belly)
    for y in (11, 13):
        d.point((5, y), fill=dark)
        d.point((8, y), fill=dark)
    if blink:
        d.line([2, 6, 5, 6], fill=dark)
        d.line([8, 6, 11, 6], fill=dark)
    else:
        d.ellipse([2, 4, 6, 8], fill=(255, 214, 79, 255))
        d.ellipse([7, 4, 11, 8], fill=(255, 214, 79, 255))
        d.point((4, 6), fill=EYE)
        d.point((9, 6), fill=EYE)
    d.polygon([(6, 8), (7, 8), (6, 10)], fill=(255, 150, 40, 255))
    # ветка и лапки
    d.line([0, 17, 13, 17], fill=(110, 80, 50, 255))
    d.point((5, 16), fill=(255, 150, 40, 255))
    d.point((8, 16), fill=(255, 150, 40, 255))
    return im


def butterfly(open_):
    """Бабочка: крылья раскрыты или сложены."""
    im = canvas(14, 10)
    d = ImageDraw.Draw(im)
    w1 = (255, 145, 0, 255)
    w2 = (156, 39, 176, 255)
    if open_:
        d.ellipse([0, 0, 6, 5], fill=w1)
        d.ellipse([7, 0, 13, 5], fill=w1)
        d.ellipse([1, 5, 6, 9], fill=w2)
        d.ellipse([7, 5, 12, 9], fill=w2)
        d.point((3, 2), fill=(255, 235, 150, 255))
        d.point((10, 2), fill=(255, 235, 150, 255))
    else:
        d.ellipse([4, 0, 6, 6], fill=w1)
        d.ellipse([7, 0, 9, 6], fill=w1)
        d.ellipse([4, 5, 6, 8], fill=w2)
        d.ellipse([7, 5, 9, 8], fill=w2)
    d.line([6, 2, 6, 8], fill=(40, 30, 30, 255))
    d.line([7, 2, 7, 8], fill=(40, 30, 30, 255))
    d.point((5, 0), fill=(40, 30, 30, 255))
    d.point((8, 0), fill=(40, 30, 30, 255))
    return im


def hedgehog(step):
    """Ёжик идёт влево, на иголках — осенний лист."""
    im = canvas(20, 13)
    d = ImageDraw.Draw(im)
    spikes = (110, 85, 65, 255)
    tip = (70, 55, 45, 255)
    face = (205, 170, 130, 255)
    d.ellipse([4, 3, 19, 11], fill=spikes)
    for x in range(5, 19, 2):
        d.point((x, 3), fill=tip)
        d.point((x + 1, 2), fill=tip)
    d.line([19, 6, 19, 9], fill=tip)
    d.polygon([(5, 6), (0, 9), (5, 11)], fill=face)
    d.point((0, 9), fill=EYE)
    d.point((3, 7), fill=EYE)
    # лист на спине
    d.polygon([(10, 1), (14, 0), (15, 3), (11, 3)], fill=(230, 110, 30, 255))
    d.line([11, 3, 14, 0], fill=(180, 70, 20, 255))
    feet = [(7, 12), (15, 12)] if step else [(8, 12), (14, 12)]
    for x, y in feet:
        d.point((x, y), fill=face)
        d.point((x + 1, y), fill=face)
    return im


def pumpkin(lit):
    """Тыква на Хэллоуин: светящиеся или тёмные глаза и рот."""
    im = canvas(16, 14)
    d = ImageDraw.Draw(im)
    orange = (245, 124, 0, 255)
    ridge = (200, 90, 0, 255)
    glow = (255, 230, 90, 255) if lit else (120, 50, 0, 255)
    d.ellipse([0, 2, 15, 13], fill=orange)
    d.line([5, 3, 4, 12], fill=ridge)
    d.line([10, 3, 11, 12], fill=ridge)
    d.rectangle([7, 0, 8, 2], fill=(90, 140, 40, 255))
    d.polygon([(3, 5), (6, 5), (4, 8)], fill=glow)
    d.polygon([(9, 5), (12, 5), (11, 8)], fill=glow)
    d.polygon([(3, 9), (12, 9), (10, 11), (5, 11)], fill=glow)
    d.point((7, 10), fill=orange)
    return im


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
    out["cat_flat_r"] = cat_flat()
    out["cat_flat_l"] = mirrored(cat_flat())
    for f in (0, 1):
        out[f"cat_drink_r{f}"] = cat_drink(f == 1)
        out[f"cat_drink_l{f}"] = mirrored(cat_drink(f == 1))
    for p in range(4):
        out[f"black_run_r{p}"] = recolor(cat_run(p), BLACK)
        out[f"black_run_l{p}"] = mirrored(recolor(cat_run(p), BLACK))
    for up in (0, 1):
        out[f"bird_r{up}"] = bird(up)
        out[f"bird_l{up}"] = mirrored(bird(up))
        out[f"snail_l{up}"] = mirrored(snail(up))
        out[f"snowman{up}"] = snowman(up)
    for f in (0, 1):
        out[f"owl{f}"] = owl(f)
        out[f"butterfly{f}"] = butterfly(f == 0)
        out[f"hedgehog{f}"] = hedgehog(f)
        out[f"pumpkin{f}"] = pumpkin(f == 1)
    # Вещи кота — увеличиваются вместе с котом
    out["cat_hat"] = party_hat()
    out["cat_santa"] = santa_hat()
    out["cat_pilotka"] = pilotka()
    out["cat_crown"] = crown()
    out["cat_helmet"] = helmet()
    out["cat_backpack"] = backpack()
    out["cat_umbrella"] = umbrella()
    out["cat_scarf_r"] = scarf()
    out["cat_scarf_l"] = mirrored(scarf())
    out["cat_bowl"] = bowl()
    out["cat_kulich"] = kulich()
    out["cat_pancakes"] = pancakes()
    out["flake"] = snowflake()
    for f in (0, 1):
        out[f"mouse_r{f}"] = mouse(f)
        out[f"mouse_l{f}"] = mirrored(mouse(f))
    return out


def c_array(name, im):
    # LVGL RGB565A8: сначала цвета — RGB565, младший байт первым, — потом
    # отдельной плоскостью прозрачность, по байту на пиксель. На 25 % меньше
    # ARGB8888, и LVGL смешивает такую картинку с экраном RGB565 напрямую,
    # без перевода цвета каждого пикселя из 32 бит
    px = list(im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata())
    color = bytearray()
    alpha = bytearray()
    for r, g, b, a in px:
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        color += bytes((v & 0xFF, v >> 8))
        alpha.append(a)
    data = color + alpha
    rows = []
    for i in range(0, len(data), 24):
        rows.append("  " + ", ".join(f"0x{v:02x}" for v in data[i:i + 24]) + ",")
    return (
        f"static const uint8_t {name}_px[] = {{\n" + "\n".join(rows) + "\n};\n"
        f"static const lv_image_dsc_t {name} = {{\n"
        f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .flags = 0,\n"
        f"               .w = {im.width}, .h = {im.height}, .stride = {im.width * 2}, .reserved_2 = 0}},\n"
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
    h2 = lambda v: round(v * 2)
    rows = [f"    {{&spr_{k}, {h2(hat[0])}, {h2(hat[1])}, {h2(head[0])}, {h2(head[1])}, {h2(back[0])}, {h2(back[1])}, "
            f"{h2(neck[0])}, {h2(neck[1])}}},"
            for k, (hat, head, back, neck) in cat_anchors().items()]
    body += (
        "\n// Точки на коте, в полуклетках сетки спрайта: макушка (низ шапки),\n"
        "// середина головы (шлем), спина (ранец) и шея (шарф)\n"
        "struct CatAnchor {\n  const lv_image_dsc_t *img;\n"
        "  int16_t hat_x, hat_y, head_x, head_y, back_x, back_y, neck_x, neck_y;\n};\n"
        "static const CatAnchor CAT_ANCHORS[] = {\n" + "\n".join(rows) + "\n};\n"
        "\n// Кончик поднятой лапы, в полуклетках: [0] — кот смотрит вправо, [1] — влево\n"
        f"static const int16_t PAW_TIP[2][2] = {{{{{h2(PAW_TIP[0])}, {h2(PAW_TIP[1])}}}, "
        f"{{{h2(26 - PAW_TIP[0])}, {h2(PAW_TIP[1])}}}}};\n"
        "\n// Где стоит миска (кулич, блины) у морды кота: середина её низа, в\n"
        "// полуклетках; [0] — вправо, [1] — влево\n"
        f"static const int16_t BOWL_AT[2][2] = {{{{{h2(BOWL_AT[0])}, {h2(BOWL_AT[1])}}}, "
        f"{{{h2(26 - BOWL_AT[0])}, {h2(BOWL_AT[1])}}}}};\n"
        "\n// Середина шарфа в его спрайте, в полуклетках; [0] — вправо, [1] — влево\n"
        f"static const int16_t SCARF_AT[2][2] = {{{{{h2(SCARF_AT[0])}, {h2(SCARF_AT[1])}}}, "
        f"{{{h2(9 - SCARF_AT[0])}, {h2(SCARF_AT[1])}}}}};\n"
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

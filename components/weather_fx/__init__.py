"""Погодный фон для страницы часов ESPHA: дождь, снег, град, облака, звёзды,
молнии, ветер с порывами.

Компонент только считает анимацию и двигает виджеты LVGL. Сами виджеты
объявлены в packages/ui.yaml и передаются ему вызовом bind(); что показывать,
решает packages/weather.yaml и каждый кадр передаёт это в frame().
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@koshg28"]
DEPENDENCIES = ["lvgl"]

weather_fx_ns = cg.esphome_ns.namespace("weather_fx")
WeatherFx = weather_fx_ns.class_("WeatherFx", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(WeatherFx)}).extend(
    cv.COMPONENT_SCHEMA
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

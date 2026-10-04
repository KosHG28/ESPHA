"""Радар самолётов для страницы «Радар» ESPHA.

Данные о самолётах отдаёт свой сервер в Docker (server/radar): плата раз в
несколько секунд забирает с него JSON (/esp?r=радиус) в отдельной задаче
FreeRTOS, чтобы сеть не тормозила экран. Положения уже переведены сервером
в пиксели экрана — в той же проекции, что и картинка карты (/map.jpg), которую
скачивает online_image (packages/radar.yaml).

Компонент рисует поверх карты кольца дальности, стороны света, дом,
самолёты с хвостами и подписями, отметки за пределами круга, а касанием по
самолёту открывает карточку с подробностями.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_URL

CODEOWNERS = ["@koshg28"]
DEPENDENCIES = ["lvgl"]
AUTO_LOAD = ["json"]

CONF_FETCH_INTERVAL = "fetch_interval"

radar_ns = cg.esphome_ns.namespace("radar")
Radar = radar_ns.class_("Radar", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Radar),
        cv.Required(CONF_URL): cv.string,
        cv.Optional(
            CONF_FETCH_INTERVAL, default="8s"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_interval(config[CONF_FETCH_INTERVAL]))

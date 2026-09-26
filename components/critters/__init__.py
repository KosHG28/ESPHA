"""«Гости» экрана — пасхалки: изредка по экрану пробегает кот, пролетает
птица, проползает улитка (зимой проходит снеговик), ночью спит кот.

Компонент только ведёт анимацию: картинку и надпись «z z z» для него
объявляет packages/critters.yaml и передаёт вызовом bind(), а раз в кадр
вызывает frame(). Спрайты — components/critters/sprites.h, их рисует
tools/make_sprites.py.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@koshg28"]
DEPENDENCIES = ["lvgl"]

critters_ns = cg.esphome_ns.namespace("critters")
Critters = critters_ns.class_("Critters", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(Critters)}).extend(
    cv.COMPONENT_SCHEMA
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

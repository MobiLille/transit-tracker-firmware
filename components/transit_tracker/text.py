import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text
from esphome.const import CONF_INITIAL_VALUE, CONF_MAX_LENGTH

from . import transit_tracker_ns

LongText = transit_tracker_ns.class_("LongText", text.Text, cg.Component)

# Optimistic text restored from flash, like `platform: template` with `restore_value: true`,
# but able to hold more than 255 bytes (abbreviations, route styles, schedule...).
# The value previously saved by a template text with the same id is migrated on first boot.
CONFIG_SCHEMA = text.text_schema(LongText).extend(
    {
        cv.Optional(CONF_MAX_LENGTH, default=1024): cv.int_range(min=1, max=4000),
        cv.Optional(CONF_INITIAL_VALUE, default=""): cv.string_strict,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_define("USE_TRANSIT_TRACKER_LONG_TEXT")
    var = await text.new_text(config, min_length=0, max_length=config[CONF_MAX_LENGTH])
    await cg.register_component(var, config)
    if config[CONF_INITIAL_VALUE]:
        cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))

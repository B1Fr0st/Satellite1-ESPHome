import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import speaker
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network"]
CODEOWNERS = ["@9r9r"]

wyoming_announce_ns = cg.esphome_ns.namespace("wyoming_announce")
WyomingAnnounce = wyoming_announce_ns.class_("WyomingAnnounce", cg.Component)

CONF_SPEAKER = "speaker"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WyomingAnnounce),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_PORT, default=10301): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spkr = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spkr))
    cg.add(var.set_port(config[CONF_PORT]))

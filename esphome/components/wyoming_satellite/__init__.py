import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import microphone, speaker
from esphome.const import CONF_ID, CONF_HOST, CONF_PORT, CONF_TRIGGER_ID

DEPENDENCIES = ["network"]
CODEOWNERS = ["@9r9r"]

wyoming_satellite_ns = cg.esphome_ns.namespace("wyoming_satellite")
WyomingSatellite = wyoming_satellite_ns.class_("WyomingSatellite", cg.Component)

StartPipelineAction = wyoming_satellite_ns.class_(
    "StartPipelineAction", automation.Action
)

IdleTrigger = wyoming_satellite_ns.class_("IdleTrigger", automation.Trigger.template())
ListeningTrigger = wyoming_satellite_ns.class_(
    "ListeningTrigger", automation.Trigger.template()
)
ThinkingTrigger = wyoming_satellite_ns.class_(
    "ThinkingTrigger", automation.Trigger.template()
)
ReplyingTrigger = wyoming_satellite_ns.class_(
    "ReplyingTrigger", automation.Trigger.template()
)
ErrorTrigger = wyoming_satellite_ns.class_(
    "ErrorTrigger", automation.Trigger.template()
)

CONF_MICROPHONE = "microphone"
CONF_SPEAKER = "speaker"
CONF_ON_IDLE = "on_idle"
CONF_ON_LISTENING = "on_listening"
CONF_ON_THINKING = "on_thinking"
CONF_ON_REPLYING = "on_replying"
CONF_ON_ERROR = "on_error"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WyomingSatellite),
        cv.Required(CONF_HOST): cv.string,
        cv.Required(CONF_PORT): cv.port,
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_ON_IDLE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(IdleTrigger)}
        ),
        cv.Optional(CONF_ON_LISTENING): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ListeningTrigger)}
        ),
        cv.Optional(CONF_ON_THINKING): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ThinkingTrigger)}
        ),
        cv.Optional(CONF_ON_REPLYING): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReplyingTrigger)}
        ),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ErrorTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


@automation.register_action(
    "wyoming_satellite.start_pipeline",
    StartPipelineAction,
    cv.maybe_simple_value(
        {cv.GenerateID(): cv.use_id(WyomingSatellite)},
        key=CONF_ID,
    ),
)
async def start_pipeline_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    spkr = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spkr))

    for conf in config.get(CONF_ON_IDLE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_LISTENING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_THINKING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_REPLYING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

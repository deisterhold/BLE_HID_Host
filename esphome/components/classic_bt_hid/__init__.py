import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID

DOMAIN = "classic_bt_hid"
CONF_MAC_ADDRESS = "mac_address"
CONF_ON_CONNECT = "on_connect"
CONF_ON_DISCONNECT = "on_disconnect"
CONF_ON_CONNECTION_ERROR = "on_connection_error"
CONF_ON_INPUT = "on_input"

classic_bt_hid_ns = cg.esphome_ns.namespace(DOMAIN)
ClassicBTHID = classic_bt_hid_ns.class_("ClassicBTHID", cg.Component)

INPUT_ARGS = [
    (cg.std_string, "address"),
    (cg.uint8, "protocol_mode"),
    (cg.std_vector.template(cg.uint8), "report"),
]
ADDRESS_ARGS = [(cg.std_string, "address")]
CONNECTION_ERROR_ARGS = [(cg.std_string, "address"), (cg.uint8, "status")]

DEPENDENCIES = []
CONFLICTS_WITH = ["esp32_ble", "esp32_ble_tracker", "esp32_ble_server", "ble_client", "bluetooth_proxy"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ClassicBTHID),
        cv.Optional(CONF_MAC_ADDRESS): cv.string,
        cv.Optional(CONF_ON_CONNECT): automation.validate_automation({}),
        cv.Optional(CONF_ON_DISCONNECT): automation.validate_automation({}),
        cv.Optional(CONF_ON_CONNECTION_ERROR): automation.validate_automation({}),
        cv.Optional(CONF_ON_INPUT): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_MAC_ADDRESS in config:
        cg.add(var.set_mac_address(config[CONF_MAC_ADDRESS]))

    callback_automations = (
        (CONF_ON_CONNECT, "add_on_connect_callback", ADDRESS_ARGS),
        (CONF_ON_DISCONNECT, "add_on_disconnect_callback", ADDRESS_ARGS),
        (CONF_ON_CONNECTION_ERROR, "add_on_connection_error_callback", CONNECTION_ERROR_ARGS),
        (CONF_ON_INPUT, "add_on_input_callback", INPUT_ARGS),
    )
    for config_key, callback_method, args in callback_automations:
        for automation_config in config.get(config_key, []):
            await automation.build_callback_automation(
                var, callback_method, args, automation_config
            )
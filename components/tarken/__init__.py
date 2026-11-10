"""Tarken WM window opener over BLE (on-demand connect, poll + commands)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    ble_client,
    esp32_ble_tracker,
    number,
    sensor,
    text_sensor,
)
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_TEMPERATURE,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CARBON_DIOXIDE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_PARTS_PER_MILLION,
    UNIT_PERCENT,
    UNIT_SECOND,
)

CODEOWNERS = []
DEPENDENCIES = ["ble_client", "esp32_ble_tracker"]
AUTO_LOAD = ["json", "sha256", "sensor", "binary_sensor", "text_sensor"]
MULTI_CONF = False  # preference keys are fixed per device

tarken_ns = cg.esphome_ns.namespace("tarken")
Tarken = tarken_ns.class_("Tarken", cg.Component, ble_client.BLEClientNode)

CONF_IDLE_TIMEOUT = "idle_timeout"
CONF_SESSION_TIMEOUT = "session_timeout"
CONF_COMMAND_DELAY = "command_delay"
CONF_POLL_INTERVAL = "poll_interval"
CONF_POLL_INTERVAL_NUMBER = "poll_interval_number"
CONF_SEND_PING = "send_ping"
CONF_BASE64 = "base64"
CONF_PAIR_UID = "pair_uid"


def validate_pair_uid(value):
    value = cv.string(value)
    words = value.split()
    if len(words) != 4:
        raise cv.Invalid(
            "pair_uid needs 4 hex words, e.g. '00112233 44556677 8899AABB CCDDEEFF'"
        )
    out = []
    for w in words:
        try:
            n = int(w, 16)
        except ValueError as err:
            raise cv.Invalid(f"pair_uid: '{w}' is not hex") from err
        if not 0 <= n <= 0xFFFFFFFF:
            raise cv.Invalid(f"pair_uid: '{w}' does not fit in 32 bits")
        out.append(n)
    return out


CONF_MAX_CONNECTED_TIME = "max_connected_time"
CONF_COMMAND_HOLD_TIME = "command_hold_time"
CONF_STATUS_INTERVAL = "status_interval"

CONF_CO2 = "co2"
CONF_UPTIME = "uptime"
CONF_TVOC = "tvoc"
CONF_WINDOW_RAW = "window_raw"
CONF_ACTIVE = "active"
CONF_CONNECTED = "connected"
CONF_FULLY_OPEN_MODE = "fully_open_mode"
CONF_SPINDLE = "spindle"
CONF_BUTTONS = "buttons"
CONF_FIRMWARE = "firmware"
CONF_WINDOW_STATE = "window_state"
CONF_AUTO = "auto"
CONF_TIMER = "timer"
CONF_LAST_MESSAGE = "last_message"
CONF_DISCOVERED_MAC = "discovered_mac"

ICON_WINDOW = "mdi:window-open-variant"

SENSORS = {
    CONF_TEMPERATURE: sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_HUMIDITY: sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_HUMIDITY,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_CO2: sensor.sensor_schema(
        unit_of_measurement=UNIT_PARTS_PER_MILLION,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_CARBON_DIOXIDE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_TVOC: sensor.sensor_schema(
        icon="mdi:air-filter",
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_BATTERY_LEVEL: sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_BATTERY,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    CONF_UPTIME: sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_TOTAL_INCREASING,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    CONF_WINDOW_RAW: sensor.sensor_schema(
        icon=ICON_WINDOW,
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    CONF_ACTIVE: sensor.sensor_schema(
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}

BINARY_SENSORS = {
    CONF_CONNECTED: binary_sensor.binary_sensor_schema(
        device_class=DEVICE_CLASS_CONNECTIVITY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
    CONF_FULLY_OPEN_MODE: binary_sensor.binary_sensor_schema(icon=ICON_WINDOW),
    CONF_SPINDLE: binary_sensor.binary_sensor_schema(
        icon="mdi:screw-lag", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_BUTTONS: binary_sensor.binary_sensor_schema(
        icon="mdi:gesture-tap-button", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}

TEXT_SENSORS = {
    CONF_FIRMWARE: text_sensor.text_sensor_schema(
        icon="mdi:chip", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_WINDOW_STATE: text_sensor.text_sensor_schema(icon=ICON_WINDOW),
    CONF_AUTO: text_sensor.text_sensor_schema(
        icon="mdi:auto-mode", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_TIMER: text_sensor.text_sensor_schema(
        icon="mdi:timer-outline", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_LAST_MESSAGE: text_sensor.text_sensor_schema(
        icon="mdi:message-text", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
    CONF_DISCOVERED_MAC: text_sensor.text_sensor_schema(
        icon="mdi:bluetooth", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
    ),
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Tarken),
            cv.Optional(
                CONF_IDLE_TIMEOUT, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_SESSION_TIMEOUT, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_MAX_CONNECTED_TIME, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_COMMAND_HOLD_TIME, default="120s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_STATUS_INTERVAL, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_COMMAND_DELAY, default="500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_POLL_INTERVAL, default="300s"
            ): cv.positive_time_period_seconds,
            cv.Optional(CONF_POLL_INTERVAL_NUMBER): cv.use_id(number.Number),
            cv.Optional(CONF_SEND_PING, default=False): cv.boolean,
            # Send commands Base64-encoded instead of plain JSON.
            cv.Optional(CONF_BASE64, default=False): cv.boolean,
            # Key from {"pair":1} in the pairing window: "uid0 uid1 uid2 uid3" (hex).
            cv.Optional(CONF_PAIR_UID): validate_pair_uid,
            # Optional fixed MAC. Without it the Tarken is discovered by name/service.
            cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
        }
    )
    .extend({cv.Optional(k): v for k, v in SENSORS.items()})
    .extend({cv.Optional(k): v for k, v in BINARY_SENSORS.items()})
    .extend({cv.Optional(k): v for k, v in TEXT_SENSORS.items()})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)
    if CONF_MAC_ADDRESS in config:
        cg.add(var.set_fixed_address(config[CONF_MAC_ADDRESS].as_hex))

    cg.add(var.set_idle_timeout(config[CONF_IDLE_TIMEOUT]))
    cg.add(var.set_session_timeout(config[CONF_SESSION_TIMEOUT]))
    cg.add(var.set_command_delay(config[CONF_COMMAND_DELAY]))
    cg.add(var.set_max_connected_time(config[CONF_MAX_CONNECTED_TIME]))
    cg.add(var.set_command_hold_time(config[CONF_COMMAND_HOLD_TIME]))
    cg.add(var.set_status_interval(config[CONF_STATUS_INTERVAL]))
    cg.add(var.set_default_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_send_ping(config[CONF_SEND_PING]))
    cg.add(var.set_base64(config[CONF_BASE64]))
    if CONF_PAIR_UID in config:
        cg.add(var.set_pair_uid(*config[CONF_PAIR_UID]))
    if CONF_POLL_INTERVAL_NUMBER in config:
        n = await cg.get_variable(config[CONF_POLL_INTERVAL_NUMBER])
        cg.add(var.set_poll_interval_number(n))

    setters = {
        CONF_TEMPERATURE: "set_temperature_sensor",
        CONF_HUMIDITY: "set_humidity_sensor",
        CONF_CO2: "set_co2_sensor",
        CONF_TVOC: "set_tvoc_sensor",
        CONF_BATTERY_LEVEL: "set_battery_sensor",
        CONF_UPTIME: "set_uptime_sensor",
        CONF_WINDOW_RAW: "set_window_raw_sensor",
        CONF_ACTIVE: "set_active_sensor",
    }
    for key, setter in setters.items():
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    bsetters = {
        CONF_CONNECTED: "set_connected_binary_sensor",
        CONF_FULLY_OPEN_MODE: "set_fully_open_mode_binary_sensor",
        CONF_SPINDLE: "set_spindle_binary_sensor",
        CONF_BUTTONS: "set_buttons_binary_sensor",
    }
    for key, setter in bsetters.items():
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(s))

    tsetters = {
        CONF_FIRMWARE: "set_firmware_text_sensor",
        CONF_WINDOW_STATE: "set_window_state_text_sensor",
        CONF_AUTO: "set_auto_text_sensor",
        CONF_TIMER: "set_timer_text_sensor",
        CONF_LAST_MESSAGE: "set_last_message_text_sensor",
        CONF_DISCOVERED_MAC: "set_mac_text_sensor",
    }
    for key, setter in tsetters.items():
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(s))

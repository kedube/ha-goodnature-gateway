"""
ESPHome external component: Goodnature trap BLE gateway.

One `goodnature_ble:` block creates the hub, a shared GATT connection and
`max_traps` trap slots. Each slot can become a Home Assistant sub-device
("Goodnature Trap 1", "Goodnature Trap 2", ...) with a fixed set of
entities. `max_traps` is the compile-time ceiling; only the slots that hold
a trap (at least one) are registered with Home Assistant, decided at boot
from the bindings in flash. Traps are discovered automatically from their
BLE advertisements and bound to the first free slot; the gateway restarts
when that slot was not registered yet so Home Assistant gains its device.

    goodnature_ble:
      time_id: sntp_time
      max_traps: 8
      poll_interval: 15min
      lure_life_days: 180
      co2_capacity: 24
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.components import (
    binary_sensor,
    button,
    esp32_ble,
    esp32_ble_client,
    esp32_ble_tracker,
    event,
    number,
    sensor,
    switch,
    text_sensor,
    time,
)
import esphome.config_validation as cv
from esphome import core
from esphome.const import (
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_DEVICE_ID,
    CONF_DISABLED_BY_DEFAULT,
    CONF_ENTITY_CATEGORY,
    CONF_ICON,
    CONF_ID,
    CONF_MODE,
    CONF_NAME,
    CONF_RESTORE_MODE,
    CONF_STATE_CLASS,
    CONF_TIME_ID,
    CONF_UNIT_OF_MEASUREMENT,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_OCCUPANCY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TIMESTAMP,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_PERCENT,
)
from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.core.config import Device
from esphome.helpers import fnv1a_32bit_hash

CODEOWNERS = ["@kedube"]
DEPENDENCIES = ["esp32_ble_tracker"]
AUTO_LOAD = [
    "esp32_ble_client",
    "binary_sensor",
    "button",
    "event",
    "number",
    "sensor",
    "switch",
    "text_sensor",
]

goodnature_ns = cg.esphome_ns.namespace("goodnature_ble")
GoodnatureHub = goodnature_ns.class_(
    "GoodnatureHub", cg.Component, esp32_ble_tracker.ESPBTDeviceListener
)
GoodnatureConnection = goodnature_ns.class_(
    "GoodnatureConnection", esp32_ble_client.BLEClientBase
)
GoodnatureTrap = goodnature_ns.class_("GoodnatureTrap")
TrapButton = goodnature_ns.class_("TrapButton", button.Button)
ForgetAllButton = goodnature_ns.class_("ForgetAllButton", button.Button)
DiscoverySwitch = goodnature_ns.class_("DiscoverySwitch", switch.Switch)
DebugSwitch = goodnature_ns.class_("DebugSwitch", switch.Switch)
LureLifeNumber = goodnature_ns.class_("LureLifeNumber", number.Number)
TrapButtonKind = goodnature_ns.enum("TrapButtonKind", is_class=True)

CONF_CONNECTION_ID = "connection_id"
CONF_MAX_TRAPS = "max_traps"
CONF_SLOT_NAME_PREFIX = "slot_name_prefix"
CONF_POLL_INTERVAL = "poll_interval"
CONF_LURE_LIFE_DAYS = "lure_life_days"
CONF_CO2_CAPACITY = "co2_capacity"
CONF_CO2_LOW_THRESHOLD = "co2_low_threshold"
CONF_OFFLINE_TIMEOUT_A24 = "offline_timeout_a24"
CONF_OFFLINE_TIMEOUT_C20 = "offline_timeout_c20"
CONF_WRITE_TIME_ON_CONNECT = "write_time_on_connect"
CONF_DISCOVERY = "discovery"
CONF_HUB_ENTITIES = "hub_entities"
CONF_AUTO_ACKNOWLEDGE = "auto_acknowledge"
CONF_A24_BATTERY_EMPTY_RAW = "a24_battery_empty_raw"
CONF_A24_BATTERY_FULL_RAW = "a24_battery_full_raw"
CONF_A24_BATTERY_LOW_PERCENT = "a24_battery_low_percent"

UNIT_DAYS = "d"
UNIT_SHOTS = "shots"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(GoodnatureHub),
            cv.GenerateID(CONF_CONNECTION_ID): cv.declare_id(GoodnatureConnection),
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_MAX_TRAPS, default=8): cv.int_range(min=1, max=32),
            cv.Optional(CONF_SLOT_NAME_PREFIX, default="Goodnature Trap"): cv.All(
                cv.string_strict, cv.Length(min=1, max=40)
            ),
            cv.Optional(
                CONF_POLL_INTERVAL, default="15min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LURE_LIFE_DAYS, default=180): cv.int_range(
                min=1, max=730
            ),
            cv.Optional(CONF_CO2_CAPACITY, default=24): cv.int_range(min=1, max=1000),
            cv.Optional(CONF_CO2_LOW_THRESHOLD, default=4): cv.int_range(
                min=0, max=1000
            ),
            cv.Optional(
                CONF_OFFLINE_TIMEOUT_A24, default="24h"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_OFFLINE_TIMEOUT_C20, default="15min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_WRITE_TIME_ON_CONNECT, default=True): cv.boolean,
            cv.Optional(CONF_DISCOVERY, default=True): cv.boolean,
            cv.Optional(CONF_HUB_ENTITIES, default=True): cv.boolean,
            cv.Optional(CONF_AUTO_ACKNOWLEDGE, default=False): cv.boolean,
            cv.Optional(CONF_A24_BATTERY_EMPTY_RAW, default=0): cv.int_range(
                min=0, max=65535
            ),
            cv.Optional(CONF_A24_BATTERY_FULL_RAW, default=0): cv.int_range(
                min=0, max=65535
            ),
            cv.Optional(CONF_A24_BATTERY_LOW_PERCENT, default=15): cv.int_range(
                min=0, max=100
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    esp32_ble.consume_connection_slots(1, "goodnature_ble"),
)


# ----------------------------------------------------------------------------
# Sub-devices and slot entities.
#
# Every slot's Device and entities are created and fully configured at compile
# time (names, object IDs, device links), but they are NOT pushed into the
# App's device and entity tables from main.cpp the way `esphome: devices:` and
# the platform new_X() factories would do. GoodnatureHub::setup() registers
# them for the first N slots, where N is the highest slot with a trap bound
# in flash, so Home Assistant only sees the slots in use. The tables are
# still sized for every slot, so N can grow up to max_traps without a reflash.
# ----------------------------------------------------------------------------


def _slot_device_id(hub_id: core.ID, n: int) -> core.ID:
    return core.ID(f"{hub_id.id}_trap{n}_device", is_declaration=True, type=Device)


async def _new_slot_entity(cfg, platform: str, setup_core, **kwargs):
    """Create an entity the way the platform's new_X() factory does, minus the
    App.register_X() call.

    setup_core is the platform's setup_X_core_(); it emits configure_entity_()
    (name, object ID, flags) and the sub-device link. register_platform_component
    keeps ESPHOME_ENTITY_*_COUNT large enough for every slot.
    """
    var = cg.new_Pvariable(cfg[CONF_ID])
    CORE.register_platform_component(platform, var)
    await setup_core(var, cfg, **kwargs)
    return var


@coroutine_with_priority(CoroPriority.FINAL)
async def _reserve_device_table(extra: int) -> None:
    """Grow the App's static device table for the slot devices the hub registers
    at runtime. The core sizes it for `esphome: devices:` only, and StaticVector
    silently drops anything pushed beyond its capacity."""
    current = 0
    for define in list(CORE.defines):
        if define.name == "ESPHOME_DEVICE_COUNT":
            current = int(str(define.value))
            CORE.defines.discard(define)
    cg.add_define("USE_DEVICES")
    cg.add_define("ESPHOME_DEVICE_COUNT", current + extra)


# ----------------------------------------------------------------------------
# Entity descriptors per slot: (suffix, name, setter, kwargs)
# ----------------------------------------------------------------------------

TRAP_SENSORS = [
    (
        "strikes",
        "Strikes",
        "set_strikes_sensor",
        {
            CONF_ICON: "mdi:counter",
            CONF_STATE_CLASS: STATE_CLASS_TOTAL_INCREASING,
            CONF_ACCURACY_DECIMALS: 0,
        },
    ),
    (
        "battery",
        "Battery",
        "set_battery_sensor",
        {
            CONF_DEVICE_CLASS: DEVICE_CLASS_BATTERY,
            CONF_UNIT_OF_MEASUREMENT: UNIT_PERCENT,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
            CONF_ACCURACY_DECIMALS: 0,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
        },
    ),
    (
        "battery_voltage_raw",
        "Battery Voltage Raw",
        "set_battery_voltage_sensor",
        {
            CONF_ICON: "mdi:battery-unknown",
            CONF_ACCURACY_DECIMALS: 0,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
            CONF_DISABLED_BY_DEFAULT: True,
        },
    ),
    (
        "rssi",
        "Signal Strength",
        "set_rssi_sensor",
        {
            CONF_DEVICE_CLASS: DEVICE_CLASS_SIGNAL_STRENGTH,
            CONF_UNIT_OF_MEASUREMENT: UNIT_DECIBEL_MILLIWATT,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
            CONF_ACCURACY_DECIMALS: 0,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
        },
    ),
    (
        "last_seen",
        "Last Seen",
        "set_last_seen_sensor",
        {
            CONF_DEVICE_CLASS: DEVICE_CLASS_TIMESTAMP,
            CONF_ACCURACY_DECIMALS: 0,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
        },
    ),
    (
        "last_strike",
        "Last Strike",
        "set_last_strike_sensor",
        {
            CONF_DEVICE_CLASS: DEVICE_CLASS_TIMESTAMP,
            CONF_ACCURACY_DECIMALS: 0,
        },
    ),
    (
        "lure_age",
        "Lure Age",
        "set_lure_age_sensor",
        {
            CONF_ICON: "mdi:cookie-clock",
            CONF_UNIT_OF_MEASUREMENT: UNIT_DAYS,
            CONF_DEVICE_CLASS: DEVICE_CLASS_DURATION,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
            CONF_ACCURACY_DECIMALS: 1,
        },
    ),
    (
        "lure_remaining",
        "Lure Remaining",
        "set_lure_remaining_sensor",
        {
            CONF_ICON: "mdi:cookie-check",
            CONF_UNIT_OF_MEASUREMENT: UNIT_DAYS,
            CONF_DEVICE_CLASS: DEVICE_CLASS_DURATION,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
            CONF_ACCURACY_DECIMALS: 1,
        },
    ),
    (
        "co2_remaining",
        "CO2 Shots Remaining",
        "set_co2_remaining_sensor",
        {
            CONF_ICON: "mdi:gas-cylinder",
            CONF_UNIT_OF_MEASUREMENT: UNIT_SHOTS,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
            CONF_ACCURACY_DECIMALS: 0,
        },
    ),
]

TRAP_BINARY_SENSORS = [
    (
        "kill_alert",
        "Kill Alert",
        "set_kill_alert_binary_sensor",
        {CONF_DEVICE_CLASS: DEVICE_CLASS_OCCUPANCY, CONF_ICON: "mdi:rodent"},
    ),
    (
        "battery_low",
        "Battery Low",
        "set_battery_low_binary_sensor",
        {CONF_DEVICE_CLASS: DEVICE_CLASS_BATTERY},
    ),
    (
        "charging",
        "Charging",
        "set_charging_binary_sensor",
        {
            CONF_DEVICE_CLASS: DEVICE_CLASS_BATTERY_CHARGING,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
        },
    ),
    (
        "active",
        "Armed",
        "set_active_binary_sensor",
        {CONF_ICON: "mdi:target", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    (
        "lure_due",
        "Lure Due",
        "set_lure_due_binary_sensor",
        {CONF_DEVICE_CLASS: DEVICE_CLASS_PROBLEM, CONF_ICON: "mdi:cookie-alert"},
    ),
    (
        "co2_low",
        "CO2 Low",
        "set_co2_low_binary_sensor",
        {CONF_DEVICE_CLASS: DEVICE_CLASS_PROBLEM, CONF_ICON: "mdi:gas-cylinder"},
    ),
    (
        "online",
        "Online",
        "set_online_binary_sensor",
        {
            CONF_DEVICE_CLASS: DEVICE_CLASS_CONNECTIVITY,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
        },
    ),
]

TRAP_TEXT_SENSORS = [
    (
        "model",
        "Model",
        "set_model_text_sensor",
        {CONF_ICON: "mdi:information-outline", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    (
        "serial",
        "Serial Number",
        "set_serial_text_sensor",
        {CONF_ICON: "mdi:identifier", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    (
        "firmware",
        "Firmware",
        "set_firmware_text_sensor",
        {CONF_ICON: "mdi:chip", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    (
        "mac",
        "MAC Address",
        "set_mac_text_sensor",
        {CONF_ICON: "mdi:bluetooth", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    (
        "status",
        "Status",
        "set_status_text_sensor",
        {CONF_ICON: "mdi:list-status", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    # Raw captures, only populated while the gateway's Debug Mode switch is on.
    (
        "last_advertisement",
        "Last Advertisement",
        "set_last_advertisement_text_sensor",
        {
            CONF_ICON: "mdi:bluetooth-audio",
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
            CONF_DISABLED_BY_DEFAULT: True,
        },
    ),
    (
        "last_frame",
        "Last Frame",
        "set_last_frame_text_sensor",
        {
            CONF_ICON: "mdi:code-brackets",
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC,
            CONF_DISABLED_BY_DEFAULT: True,
        },
    ),
]

TRAP_BUTTONS = [
    # (suffix, name, enum member, kwargs)
    (
        "poll",
        "Poll Now",
        "POLL",
        {CONF_ICON: "mdi:refresh", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    (
        "lure_replaced",
        "Lure Replaced",
        "LURE_REPLACED",
        {CONF_ICON: "mdi:cookie-refresh", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG},
    ),
    (
        "co2_replaced",
        "CO2 Canister Replaced",
        "CO2_REPLACED",
        {CONF_ICON: "mdi:gas-cylinder", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG},
    ),
    (
        "reset_alert",
        "Clear Kill Alert",
        "RESET_ALERT",
        {
            CONF_ICON: "mdi:notification-clear-all",
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG,
        },
    ),
    (
        "test_fire",
        "Test Fire",
        "TEST_FIRE",
        {CONF_ICON: "mdi:flash", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG},
    ),
    (
        "forget",
        "Forget Trap",
        "FORGET",
        {CONF_ICON: "mdi:delete-forever", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG},
    ),
    (
        "log_debug",
        "Log Debug Info",
        "LOG_DEBUG",
        {CONF_ICON: "mdi:text-box-search", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
]

STRIKE_EVENT_TYPES = ["strike", "test_fire"]


def _entity_id(hub_id: core.ID, n: int, suffix: str, type_) -> core.ID:
    return core.ID(f"{hub_id.id}_trap{n}_{suffix}", is_declaration=True, type=type_)


def _hub_entity_id(hub_id: core.ID, suffix: str, type_) -> core.ID:
    return core.ID(f"{hub_id.id}_{suffix}", is_declaration=True, type=type_)


def _validated(schema, entity_id: core.ID, name: str, kwargs: dict, device_id=None):
    """Build a minimal entity config and run it through the platform schema so
    defaults (disabled_by_default, internal, ...) are filled in the way the
    new_X factories expect."""
    cfg = {CONF_ID: entity_id, CONF_NAME: name}
    if device_id is not None:
        cfg[CONF_DEVICE_ID] = device_id
    cfg.update(kwargs)
    return schema(cfg)


async def _build_trap_entities(hub_id: core.ID, n: int, trap) -> None:
    device_id = _slot_device_id(hub_id, n)

    for suffix, name, setter, kwargs in TRAP_SENSORS:
        cfg = _validated(
            sensor.sensor_schema(),
            _entity_id(hub_id, n, suffix, sensor.Sensor),
            name,
            kwargs,
            device_id,
        )
        var = await _new_slot_entity(cfg, "sensor", sensor.setup_sensor_core_)
        cg.add(getattr(trap, setter)(var))

    for suffix, name, setter, kwargs in TRAP_BINARY_SENSORS:
        cfg = _validated(
            binary_sensor.binary_sensor_schema(),
            _entity_id(hub_id, n, suffix, binary_sensor.BinarySensor),
            name,
            kwargs,
            device_id,
        )
        var = await _new_slot_entity(
            cfg, "binary_sensor", binary_sensor.setup_binary_sensor_core_
        )
        cg.add(getattr(trap, setter)(var))

    for suffix, name, setter, kwargs in TRAP_TEXT_SENSORS:
        cfg = _validated(
            text_sensor.text_sensor_schema(),
            _entity_id(hub_id, n, suffix, text_sensor.TextSensor),
            name,
            kwargs,
            device_id,
        )
        var = await _new_slot_entity(
            cfg, "text_sensor", text_sensor.setup_text_sensor_core_
        )
        cg.add(getattr(trap, setter)(var))

    for suffix, name, kind, kwargs in TRAP_BUTTONS:
        cfg = _validated(
            button.button_schema(TrapButton),
            _entity_id(hub_id, n, suffix, TrapButton),
            name,
            kwargs,
            device_id,
        )
        var = await _new_slot_entity(cfg, "button", button.setup_button_core_)
        cg.add(var.set_parent(trap))
        cg.add(var.set_kind(getattr(TrapButtonKind, kind)))
        cg.add(trap.add_button(var, getattr(TrapButtonKind, kind)))

    cfg = _validated(
        number.number_schema(LureLifeNumber),
        _entity_id(hub_id, n, "lure_life", LureLifeNumber),
        "Lure Life",
        {
            CONF_ICON: "mdi:calendar-clock",
            CONF_UNIT_OF_MEASUREMENT: UNIT_DAYS,
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG,
            CONF_MODE: "BOX",
        },
        device_id,
    )
    var = await _new_slot_entity(
        cfg, "number", number.setup_number_core_, min_value=1, max_value=730, step=1
    )
    cg.add(var.set_parent(trap))
    cg.add(trap.set_lure_life_number(var))

    cfg = _validated(
        event.event_schema(),
        _entity_id(hub_id, n, "strike_event", event.Event),
        "Strike",
        {CONF_ICON: "mdi:rodent"},
        device_id,
    )
    var = await _new_slot_entity(
        cfg, "event", event.setup_event_core_, event_types=STRIKE_EVENT_TYPES
    )
    cg.add(trap.set_strike_event(var))


async def _build_hub_entities(hub_id: core.ID, hub, max_traps: int) -> None:
    cfg = _validated(
        sensor.sensor_schema(),
        _hub_entity_id(hub_id, "traps_discovered", sensor.Sensor),
        "Traps Discovered",
        {
            CONF_ICON: "mdi:rodent",
            CONF_ACCURACY_DECIMALS: 0,
            CONF_STATE_CLASS: STATE_CLASS_MEASUREMENT,
        },
    )
    cg.add(hub.set_traps_count_sensor(await sensor.new_sensor(cfg)))

    cfg = _validated(
        text_sensor.text_sensor_schema(),
        _hub_entity_id(hub_id, "last_discovered", text_sensor.TextSensor),
        "Last Discovered Trap",
        {CONF_ICON: "mdi:bluetooth-connect", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    )
    cg.add(hub.set_last_discovered_text_sensor(await text_sensor.new_text_sensor(cfg)))

    cfg = _validated(
        switch.switch_schema(DiscoverySwitch),
        _hub_entity_id(hub_id, "discovery", DiscoverySwitch),
        "Trap Discovery",
        {
            CONF_ICON: "mdi:radar",
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG,
            CONF_RESTORE_MODE: "RESTORE_DEFAULT_ON",
        },
    )
    sw = await switch.new_switch(cfg)
    cg.add(sw.set_parent(hub))
    cg.add(hub.set_discovery_switch(sw))

    cfg = _validated(
        switch.switch_schema(DebugSwitch),
        _hub_entity_id(hub_id, "debug", DebugSwitch),
        "Debug Mode",
        {
            CONF_ICON: "mdi:bug",
            CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG,
            CONF_RESTORE_MODE: "RESTORE_DEFAULT_OFF",
        },
    )
    sw = await switch.new_switch(cfg)
    cg.add(sw.set_parent(hub))
    cg.add(hub.set_debug_switch(sw))

    cfg = _validated(
        button.button_schema(ForgetAllButton),
        _hub_entity_id(hub_id, "forget_all", ForgetAllButton),
        "Forget All Traps",
        {CONF_ICON: "mdi:delete-sweep", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_CONFIG},
    )
    btn = await button.new_button(cfg)
    cg.add(btn.set_parent(hub))



async def to_code(config):
    hub_id: core.ID = config[CONF_ID]
    hub = cg.new_Pvariable(hub_id)
    await cg.register_component(hub, config)
    await esp32_ble_tracker.register_ble_device(hub, config)

    conn = cg.new_Pvariable(config[CONF_CONNECTION_ID])
    await cg.register_component(conn, {})
    await esp32_ble_tracker.register_client(conn, config)
    cg.add(hub.set_connection(conn))

    if CONF_TIME_ID in config:
        time_var = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(hub.set_time(time_var))

    cg.add(hub.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(hub.set_lure_life_days(config[CONF_LURE_LIFE_DAYS]))
    cg.add(hub.set_co2_capacity(config[CONF_CO2_CAPACITY]))
    cg.add(hub.set_co2_low_threshold(config[CONF_CO2_LOW_THRESHOLD]))
    cg.add(hub.set_offline_timeout_a24(config[CONF_OFFLINE_TIMEOUT_A24]))
    cg.add(hub.set_offline_timeout_c20(config[CONF_OFFLINE_TIMEOUT_C20]))
    cg.add(hub.set_write_time_on_connect(config[CONF_WRITE_TIME_ON_CONNECT]))
    cg.add(hub.set_discovery_default(config[CONF_DISCOVERY]))
    cg.add(hub.set_auto_acknowledge(config[CONF_AUTO_ACKNOWLEDGE]))
    cg.add(
        hub.set_a24_battery_calibration(
            config[CONF_A24_BATTERY_EMPTY_RAW],
            config[CONF_A24_BATTERY_FULL_RAW],
            config[CONF_A24_BATTERY_LOW_PERCENT],
        )
    )

    max_traps: int = config[CONF_MAX_TRAPS]
    prefix: str = config[CONF_SLOT_NAME_PREFIX]

    if config[CONF_HUB_ENTITIES]:
        await _build_hub_entities(hub_id, hub, max_traps)

    for n in range(1, max_traps + 1):
        trap = cg.new_Pvariable(
            core.ID(f"{hub_id.id}_trap{n}", is_declaration=True, type=GoodnatureTrap)
        )
        cg.add(trap.set_index(n - 1))
        cg.add(hub.add_trap(trap))

        # The slot's sub-device. Created here rather than through
        # `esphome: devices:` so that main.cpp does not register it; the hub
        # does that at boot for active slots only.
        dev_id = _slot_device_id(hub_id, n)
        dev = cg.new_Pvariable(dev_id)
        cg.add(dev.set_device_id(fnv1a_32bit_hash(dev_id.id)))
        cg.add(dev.set_name(f"{prefix} {n}"))
        cg.add(trap.set_device(dev))

        await _build_trap_entities(hub_id, n, trap)

    CORE.add_job(_reserve_device_table, max_traps)

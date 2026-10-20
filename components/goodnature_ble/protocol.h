#pragma once

// Goodnature trap BLE protocol: pure C++ with no ESPHome or ESP-IDF
// dependencies so it can be unit tested on the host (see tests/).
//
// Protocol knowledge is taken from the ha-goodnature Home Assistant
// integration (custom_components/goodnature_ble/protocol.py and
// coordinator.py), which was reverse engineered from the Goodnature app
// and BLE captures.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esphome::goodnature_ble::protocol {

// ---------------------------------------------------------------------------
// UUIDs
// ---------------------------------------------------------------------------

// All Goodnature custom UUIDs are 0000XXXX-1212-efde-1523-785fef13d123.
std::string gn_uuid_str(uint16_t short_id);

// Bluetooth SIG company identifier seen in Goodnature manufacturer data
// (Nordic Semiconductor, whose SoC the traps use).
constexpr uint16_t MFG_ID_NORDIC = 0x0059;

// Device information area (A24)
constexpr uint16_t SVC_DEVICE = 0xDE11;
constexpr uint16_t CHR_SERIAL = 0xDE12;         // string
constexpr uint16_t CHR_CONTROL = 0xDE13;        // u8, writable
constexpr uint16_t CHR_DEVICE_STATE = 0xDE14;   // raw u16
constexpr uint16_t CHR_FIRMWARE = 0xDE15;       // string
constexpr uint16_t CHR_DEVICE_CONFIG = 0xDE16;  // raw

// Kill channel (A24)
constexpr uint16_t SVC_KILL = 0xD00D;
constexpr uint16_t CHR_KILL_DISPLAYED = 0xD20D;  // u16
constexpr uint16_t CHR_KILL_PAYLOAD = 0xD30D;    // strike record
constexpr uint16_t CHR_KILL_STATE = 0xD50D;      // raw u16
constexpr uint16_t CHR_KILL_READ = 0xD60D;       // u16, writable

// Event channel (A24)
constexpr uint16_t CHR_EVENT_DISPLAYED = 0xD2ED;  // u16
constexpr uint16_t CHR_EVENT_PAYLOAD = 0xDEED;    // raw
constexpr uint16_t CHR_EVENT_READ = 0xD3ED;       // u16, writable

// Error channel (A24). Names from the Goodnature app, which never reads
// them; the displayed / read counters are assumed to work like the kill pair.
constexpr uint16_t CHR_ERROR_DISPLAYED = 0xE771;  // counter
constexpr uint16_t CHR_ERROR_PAYLOAD = 0xE772;    // raw
constexpr uint16_t CHR_ERROR_READ = 0xE773;       // counter

// Battery / telemetry (A24)
constexpr uint16_t SVC_BATTERY = 0xFADE;
constexpr uint16_t CHR_BATTERY_VOLTAGE = 0xFAD1;     // raw u16
constexpr uint16_t CHR_BATTERY_RESISTANCE = 0xFAD2;  // raw u16
constexpr uint16_t CHR_BATTERY_UNKNOWN = 0xFAD3;     // raw u16

// Time (A24)
constexpr uint16_t SVC_TIME = 0xF1AE;
constexpr uint16_t CHR_TIME = 0xF1AF;  // u32 LE minutes since epoch, writable

// Unknown service root seen on C20 profiles
constexpr uint16_t SVC_UNKNOWN_E010 = 0xE010;

// Legacy 16-bit service UUIDs that appear in advertisements
constexpr uint16_t SVC16_LEGACY_1234 = 0x1234;
constexpr uint16_t SVC16_LEGACY_600D = 0x600D;

// Nordic UART Service (C20)
constexpr const char *NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";  // write
constexpr const char *NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";  // notify
// Memfault diagnostics service (C20); only used for model detection
constexpr const char *MEMFAULT_SERVICE_UUID = "54220000-f6a5-4007-a371-722f4ebd8436";

// A24 control byte values written to CHR_CONTROL
constexpr uint8_t A24_CONTROL_ACK = 0x02;  // the app's end of sync; not sent, see push_a24_poll_ops_
constexpr uint8_t A24_CONTROL_TEST_FIRE = 0x05;  // app setup, on movement; does not fire an A24

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
//
// A24: the CO2-powered rat/stoat trap. Sleeps, advertises in bursts, custom
//      GATT services (DE11 / D00D / FADE).
// C20: the newer profile with a Nordic UART service. The reference
//      integration calls it "C20"; it is what the rechargeable Goodnature
//      Mouse Trap speaks, and the only product seen using it so far.

enum class Model : uint8_t { UNKNOWN = 0, A24 = 1, C20 = 2 };
const char *model_name(Model model);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::optional<uint8_t> parse_u8(const uint8_t *data, size_t len);
std::optional<uint16_t> parse_u16_le(const uint8_t *data, size_t len);
std::optional<uint32_t> parse_u32_le(const uint8_t *data, size_t len, size_t offset = 0);
// Decode a characteristic to text, stripping trailing NULs. Falls back to
// hex when the bytes are not printable UTF-8/ASCII.
std::string decode_text(const uint8_t *data, size_t len);
std::string decode_a24_serial(const uint8_t *data, size_t len);
std::string decode_a24_firmware(const uint8_t *data, size_t len);
std::string to_hex(const uint8_t *data, size_t len);
void put_u16_le(std::vector<uint8_t> &out, uint16_t v);
void put_u32_le(std::vector<uint8_t> &out, uint32_t v);

// ---------------------------------------------------------------------------
// C20 advertisement (9-byte manufacturer payload)
// ---------------------------------------------------------------------------
//
// Layout per the reference integration: [0..3] serial, [4] device type,
// [5] flags, [6..7] strike count, [8] battery. On the Mouse Trap (firmware
// 0.3.730) byte 8 is the battery *state* enumeration (4 = NORMAL), not a
// percentage: the percentage only comes from the UART battery-level
// response. Byte 5 only partly fits the documented bit layout: 0x02 (kill
// alert) and 0x10 (charging) agreed with the UART poll, but activated traps
// send 0x04, never 0x01. The other flag bits below are unverified; `flags`
// carries the raw byte.

struct C20Advertisement {
  uint32_t serial_number_raw;
  uint8_t device_type;
  uint8_t flags;  // raw byte 5
  bool activated;
  bool strikes_available;
  bool low_battery;
  bool charging;
  bool usb_connected;
  bool discoverable;
  uint16_t strike_count;
  uint8_t battery_state_raw;  // raw byte 8, see C20BatteryState

  // Serial as the app shows it: 8 upper-case hex digits.
  std::string serial_number() const;
};

std::optional<C20Advertisement> parse_c20_advertisement(const uint8_t *data, size_t len);

// ---------------------------------------------------------------------------
// A24 strike record (CHR_KILL_PAYLOAD)
// ---------------------------------------------------------------------------

// Layout per the Goodnature app: [0] peak level, [1..4] the trap's ID,
// [5] temperature, [6..9] minutes since the epoch, [10..11] record number.
// The record returned is the one CHR_KILL_DISPLAYED was last set to.
struct A24Strike {
  uint8_t peak_level;  // scale unknown
  int8_t temperature;  // air temperature at the strike; the app stores it as is (°C)
  uint32_t minutes_since_epoch;
  uint16_t strike_id;

  uint32_t epoch_seconds() const { return this->minutes_since_epoch * 60U; }
};

// Accepts either the raw 12+ byte record or its ASCII-hex representation.
std::optional<A24Strike> parse_d30d(const uint8_t *data, size_t len);

// A24 error record (CHR_ERROR_PAYLOAD, 17 bytes), as observed: [0..1] record
// number, [2..5] minutes since the epoch, [6..9] the trap's ID, [10..13] a
// u32 (13 or an nRF52 RAM address), [14..15] u16 and [16] u8, which may be a
// source line and file. The app never reads it. The record returned is the
// one CHR_ERROR_DISPLAYED was last set to.
struct A24ErrorRecord {
  uint16_t record;
  uint32_t minutes_since_epoch;
  uint32_t value;
  uint16_t line;
  uint8_t file;

  // Logged when the trap hangs up on the closing 0x02 before the central
  // disconnects; every record seen on two traps was this one.
  bool session_end() const { return this->line == 2357 && this->file == 1; }
};

std::optional<A24ErrorRecord> parse_e772(const uint8_t *data, size_t len);

// A24 advertisement: exactly six 16-bit service UUIDs (layout from the app).
// [0], [1] serial; [2] flags (low byte) and device type (high byte);
// [3] 0x1234; [4] battery voltage (high byte) and internal resistance (low
// byte); [5] kill count (high byte) and unread kills (low byte).
struct A24Advertisement {
  uint8_t flags;
  uint8_t device_type;
  uint8_t voltage_raw;          // see a24_battery_volts(); 0 = not measured
  uint8_t internal_resistance;  // raw, units unknown; see a24_battery_health()
  uint8_t kills;                // low byte of the kill counter
  uint8_t unread_kills;         // kills not yet acknowledged on the trap

  bool strikes_available() const { return this->flags & 0x01; }
  bool activated() const { return this->flags & 0x02; }
  bool moving() const { return this->flags & 0x04; }
  bool low_battery() const { return this->flags & 0x08; }
  bool new_event() const { return this->flags & 0x20; }
  bool new_error() const { return this->flags & 0x40; }
};
std::optional<A24Advertisement> parse_a24_advertisement(const uint16_t *uuids, size_t count);

// A24 battery, as the app reads it from the advertisement (CHR_BATTERY_VOLTAGE
// and CHR_BATTERY_RESISTANCE carry the same raw bytes).
float a24_battery_volts(uint8_t raw);  // raw / 255 * 3.6 V
enum class A24BatteryHealth : uint8_t { FULL, MEDIUM, LOW, CRITICAL };
A24BatteryHealth a24_battery_health(uint8_t internal_resistance);
const char *a24_battery_health_str(A24BatteryHealth health);

// ---------------------------------------------------------------------------
// C20 Nordic UART framing
// ---------------------------------------------------------------------------
//
// Frame: 0xB0 <escaped body> 0xB1
// Body:  <type> <subtype> <payload...> <crc16 LE>
// Escape: 0xB0/0xB1 inside the body are sent as 0xB2 (byte ^ 0x04). The
//         reference app code only escapes the delimiters; we also escape a
//         literal 0xB2 the same way, since a decoder that treats 0xB2 as an
//         escape marker cannot otherwise carry that byte value.
// CRC:   CRC16-CCITT (poly 0x1021, init 0xFFFF) over type+subtype+payload.

constexpr uint8_t C20_FRAME_START = 0xB0;
constexpr uint8_t C20_FRAME_END = 0xB1;
constexpr uint8_t C20_FRAME_ESC = 0xB2;

constexpr uint8_t C20_TYPE_SET_TIME = 0x04;
constexpr uint8_t C20_TYPE_FIRMWARE = 0x08;
constexpr uint8_t C20_TYPE_DEVICE_STATE = 0x10;
constexpr uint8_t C20_TYPE_BATTERY_LEVEL = 0x11;
constexpr uint8_t C20_TYPE_SET_COMMAND = 0x12;
constexpr uint8_t C20_TYPE_KILL_HISTORY = 0x14;
constexpr uint8_t C20_TYPE_TRIGGER_EVENT = 0x30;
constexpr uint8_t C20_TYPE_STRIKER_EVENT = 0x31;
constexpr uint8_t C20_TYPE_LOGS = 0x40;
constexpr uint8_t C20_TYPE_LOG_EVENT = 0x45;
constexpr uint8_t C20_TYPE_INPUT_STATES = 0x62;
constexpr uint8_t C20_TYPE_BOOTLOADER_VERSION = 0xF8;

constexpr uint8_t C20_SUBTYPE_REQUEST = 0x00;
constexpr uint8_t C20_SUBTYPE_RESPONSE = 0x01;
constexpr uint8_t C20_SUBTYPE_SET = 0x02;

constexpr uint32_t C20_COMMAND_FIRE = 1049702590UL;   // 0x3E9130BE
constexpr uint32_t C20_COMMAND_CLEAR = 1682499629UL;  // 0x6448E82D
// The app (3.6.1) sends WAKE first on every connection, and again to a trap
// that needs battery recovery.
constexpr uint32_t C20_COMMAND_WAKE = 1384542436UL;  // 0x528670E4
// Defined in the app but never sent by it. Firmware 0.3.730 acknowledged
// DEACTIVATE and stayed ACTIVATED, so the gateway does not use either.
constexpr uint32_t C20_COMMAND_ACTIVATE = 3912210963UL;   // 0xE92F9A13
constexpr uint32_t C20_COMMAND_DEACTIVATE = 721180884UL;  // 0x2AFC58D4

constexpr uint32_t C20_LOGS_FLAG_TRIGGER_EVENTS = 1UL << 0;
constexpr uint32_t C20_LOGS_FLAG_STRIKER_EVENTS = 1UL << 1;

uint16_t crc16_ccitt(const uint8_t *data, size_t len);

struct C20Frame {
  uint8_t type;
  uint8_t subtype;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> c20_encode(uint8_t type, uint8_t subtype, const uint8_t *payload = nullptr, size_t len = 0);
inline std::vector<uint8_t> c20_encode(uint8_t type, uint8_t subtype, const std::vector<uint8_t> &payload) {
  return c20_encode(type, subtype, payload.data(), payload.size());
}
// Decodes one complete frame (start byte through end byte). Returns
// nullopt for framing or CRC errors.
std::optional<C20Frame> c20_decode(const uint8_t *data, size_t len);

// Reassembles frames from a byte stream that may split or concatenate
// frames across BLE notifications. Bytes outside a frame are discarded.
class C20FrameAssembler {
 public:
  // Feed bytes; returns every complete frame found.
  std::vector<C20Frame> feed(const uint8_t *data, size_t len);
  void reset() {
    this->buffer_.clear();
    this->in_frame_ = false;
  }

 protected:
  std::vector<uint8_t> buffer_;
  bool in_frame_{false};
};

// C20 payloads ----------------------------------------------------------------

enum class C20DeviceState : uint8_t { DEACTIVATED = 0, ACTIVATED = 1, ERROR = 2, NO_DATA = 3 };
enum class C20KillState : uint8_t { CLEARED = 0, DETECTED = 1, NO_DATA = 2 };
enum class C20TrayState : uint8_t { OPEN = 0, CLOSED = 1, UNKNOWN = 2, NO_DATA = 3 };
enum class C20BatteryState : uint8_t { STARTUP = 0, NOT_CONNECTED = 1, CRITICAL = 2, LOW = 3, NORMAL = 4, NO_DATA = 5 };
enum class C20ChargeState : uint8_t { NOT_CHARGING = 0, CHARGING = 1, NO_DATA = 2 };
enum class C20UsbState : uint8_t { DISCONNECTED = 0, CONNECTED = 1, NO_DATA = 2 };
// App 3.6.1 names: SENSOR, UNUSED, APP, BACKDRIVE_TEST, UNKNOWN. Only
// TRIGGER (SENSOR) is a kill; USER (APP) is a test fire.
enum class C20StrikerSource : uint8_t { TRIGGER = 0, UNUSED = 1, USER = 2, BACKDRIVE_TEST = 3, NO_DATA = 4 };

const char *c20_device_state_str(C20DeviceState v);
const char *c20_kill_state_str(C20KillState v);
const char *c20_battery_state_str(C20BatteryState v);
const char *c20_striker_source_str(C20StrikerSource v);

struct C20DeviceStateResponse {
  uint32_t device_time;
  C20DeviceState device_state;
  C20KillState kill_state;
  C20TrayState tray_state;
  C20BatteryState battery_state;
  C20ChargeState charge_state;
  C20UsbState usb_state;
  uint32_t strike_count;
};
std::optional<C20DeviceStateResponse> parse_c20_device_state(const uint8_t *data, size_t len);

struct C20BatteryLevelResponse {
  uint32_t device_time;
  uint8_t battery_percent;
};
std::optional<C20BatteryLevelResponse> parse_c20_battery_level(const uint8_t *data, size_t len);

struct C20StrikerEvent {
  uint32_t event_time;
  uint32_t strike_count;
  C20StrikerSource source;
  uint32_t trigger_number;
  uint32_t fire_time_ms;
  uint32_t rewind_time_ms;
  std::optional<int16_t> peak_fire_ma;    // firmware 0.1.276 and later
  std::optional<int16_t> peak_rewind_ma;  // firmware 0.1.276 and later
  std::optional<uint32_t> backdrive_time_ms;
};
std::optional<C20StrikerEvent> parse_c20_striker_event(const uint8_t *data, size_t len);

// One infrared sensor's readings in a trigger event. Units unknown.
struct C20TriggerSensor {
  uint32_t ambient;
  uint32_t on_reading;
  float baseline;
  uint32_t threshold;
  uint32_t on_time;
};
// Replayed from the log with C20_LOGS_FLAG_TRIGGER_EVENTS. The readings are
// present from firmware 0.1.276.
struct C20TriggerEvent {
  uint32_t event_time;
  uint32_t count;
  std::optional<C20TriggerSensor> inner;
  std::optional<C20TriggerSensor> outer;
  std::optional<uint32_t> alert_time;
};
std::optional<C20TriggerEvent> parse_c20_trigger_event(const uint8_t *data, size_t len);

// IEEE 11073 32-bit FLOAT, as used for the trigger baselines: a signed
// 24-bit mantissa in the low bits times 10 to a signed 8-bit exponent.
float decode_ieee11073_float(uint32_t raw);

// Firmware response payload is a NUL-terminated string. The bootloader
// version response uses the same format.
std::string parse_c20_firmware(const uint8_t *data, size_t len);

// The trap's last ten kill times, epoch seconds, 0 for an empty slot.
// Firmware after 0.2.550; the app counts kills from it.
constexpr size_t C20_KILL_HISTORY_SIZE = 10;
using C20KillHistory = std::array<uint32_t, C20_KILL_HISTORY_SIZE>;
std::optional<C20KillHistory> parse_c20_kill_history(const uint8_t *data, size_t len);

// Raw input pins and measurements (firmware 0.3.730, app 3.6.1).
struct C20InputStates {
  uint8_t charge_pin;
  uint8_t usb_pin;
  uint8_t button_pin;
  uint8_t battery_error_pin;
  int16_t battery_mv;      // the firmware logs VBAT in mV
  int16_t motor_feedback;  // units unknown
};
std::optional<C20InputStates> parse_c20_input_states(const uint8_t *data, size_t len);

// Compares dotted versions ("0.3.730", an optional leading 'v') number by
// number: <0, 0 or >0, or nullopt when either is not a version.
std::optional<int> compare_versions(const std::string &a, const std::string &b);

// Request builders
std::vector<uint8_t> c20_build_set_time(uint32_t epoch_seconds);
std::vector<uint8_t> c20_build_request(uint8_t type);
std::vector<uint8_t> c20_build_set_command(uint32_t command);
std::vector<uint8_t> c20_build_logs_request(uint32_t start_epoch, uint32_t end_epoch, uint32_t flags);

}  // namespace esphome::goodnature_ble::protocol

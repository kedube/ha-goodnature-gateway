#pragma once

// Goodnature trap BLE protocol: pure C++ with no ESPHome or ESP-IDF
// dependencies so it can be unit tested on the host (see tests/).
//
// Protocol knowledge is taken from the ha-goodnature Home Assistant
// integration (custom_components/goodnature_ble/protocol.py and
// coordinator.py), which was reverse engineered from the Goodnature app
// and BLE captures.

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
constexpr uint8_t A24_CONTROL_ACK = 0x02;
constexpr uint8_t A24_CONTROL_TEST_FIRE = 0x05;

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
// response. Byte 5 read 0x04 on activated, closed, not-charging traps, which
// does not fit the documented bit layout, so the flag bits below are kept
// for reference but treated as unverified; `flags` carries the raw byte.

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

struct A24Strike {
  uint8_t flags;
  uint32_t minutes_since_epoch;
  uint16_t strike_id;

  uint32_t epoch_seconds() const { return this->minutes_since_epoch * 60U; }
};

// Accepts either the raw 12+ byte record or its ASCII-hex representation.
std::optional<A24Strike> parse_d30d(const uint8_t *data, size_t len);

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
constexpr uint8_t C20_TYPE_STRIKER_EVENT = 0x31;
constexpr uint8_t C20_TYPE_LOGS = 0x40;
constexpr uint8_t C20_TYPE_LOG_EVENT = 0x45;

constexpr uint8_t C20_SUBTYPE_REQUEST = 0x00;
constexpr uint8_t C20_SUBTYPE_RESPONSE = 0x01;
constexpr uint8_t C20_SUBTYPE_SET = 0x02;

constexpr uint32_t C20_COMMAND_FIRE = 1049702590UL;   // 0x3E9130BE
constexpr uint32_t C20_COMMAND_CLEAR = 1682499629UL;  // 0x644A8C2D

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
enum class C20StrikerSource : uint8_t { TRIGGER = 0, UNUSED = 1, USER = 2, NO_DATA = 3 };

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
  std::optional<uint32_t> backdrive_time_ms;
};
std::optional<C20StrikerEvent> parse_c20_striker_event(const uint8_t *data, size_t len);

// Firmware response payload is a NUL-terminated string.
std::string parse_c20_firmware(const uint8_t *data, size_t len);

// Request builders
std::vector<uint8_t> c20_build_set_time(uint32_t epoch_seconds);
std::vector<uint8_t> c20_build_request(uint8_t type);
std::vector<uint8_t> c20_build_set_command(uint32_t command);
std::vector<uint8_t> c20_build_logs_request(uint32_t start_epoch, uint32_t end_epoch, uint32_t flags);

}  // namespace esphome::goodnature_ble::protocol

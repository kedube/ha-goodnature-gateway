#include "protocol.h"

#include <cstdio>
#include <cstring>

namespace esphome::goodnature_ble::protocol {

// ---------------------------------------------------------------------------
// UUIDs / model
// ---------------------------------------------------------------------------

std::string gn_uuid_str(uint16_t short_id) {
  char buf[40];
  snprintf(buf, sizeof(buf), "0000%04x-1212-efde-1523-785fef13d123", short_id);
  return std::string(buf);
}

const char *model_name(Model model) {
  switch (model) {
    case Model::A24:
      return "A24";
    case Model::C20:
      return "Mouse Trap";
    default:
      return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::optional<uint8_t> parse_u8(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 1)
    return std::nullopt;
  return data[0];
}

std::optional<uint16_t> parse_u16_le(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 2)
    return std::nullopt;
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

std::optional<uint32_t> parse_u32_le(const uint8_t *data, size_t len, size_t offset) {
  if (data == nullptr || len < offset + 4)
    return std::nullopt;
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::string to_hex(const uint8_t *data, size_t len) {
  static const char DIGITS[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(DIGITS[data[i] >> 4]);
    out.push_back(DIGITS[data[i] & 0x0F]);
  }
  return out;
}

std::string decode_text(const uint8_t *data, size_t len) {
  if (data == nullptr)
    return "";
  while (len > 0 && data[len - 1] == 0)
    len--;
  if (len == 0)
    return "";
  // Printable ASCII (allow tabs/spaces); otherwise fall back to hex.
  bool printable = true;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c < 0x20 || c > 0x7E) {
      printable = false;
      break;
    }
  }
  if (!printable)
    return to_hex(data, len);
  std::string s(reinterpret_cast<const char *>(data), len);
  // trim
  size_t start = 0;
  while (start < s.size() && s[start] == ' ')
    start++;
  size_t end = s.size();
  while (end > start && s[end - 1] == ' ')
    end--;
  return s.substr(start, end - start);
}

void put_u16_le(std::vector<uint8_t> &out, uint16_t v) {
  out.push_back(v & 0xFF);
  out.push_back((v >> 8) & 0xFF);
}

void put_u32_le(std::vector<uint8_t> &out, uint32_t v) {
  out.push_back(v & 0xFF);
  out.push_back((v >> 8) & 0xFF);
  out.push_back((v >> 16) & 0xFF);
  out.push_back((v >> 24) & 0xFF);
}

// ---------------------------------------------------------------------------
// C20 advertisement
// ---------------------------------------------------------------------------

std::string C20Advertisement::serial_number() const {
  char buf[12];
  snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(this->serial_number_raw));
  return std::string(buf);
}

std::optional<C20Advertisement> parse_c20_advertisement(const uint8_t *data, size_t len) {
  if (data == nullptr || len != 9)
    return std::nullopt;
  C20Advertisement adv{};
  adv.serial_number_raw = *parse_u32_le(data, len, 0);
  adv.device_type = data[4];
  uint8_t flags = data[5];
  adv.flags = flags;
  adv.activated = flags & 0x01;
  adv.strikes_available = flags & 0x02;
  adv.low_battery = flags & 0x08;
  adv.charging = flags & 0x10;
  adv.usb_connected = flags & 0x20;
  adv.discoverable = flags & 0x40;
  adv.strike_count = *parse_u16_le(data + 6, 2);
  adv.battery_state_raw = data[8];
  return adv;
}

// ---------------------------------------------------------------------------
// A24 strike record
// ---------------------------------------------------------------------------

static int hex_nibble(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::optional<A24Strike> parse_d30d(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0)
    return std::nullopt;

  // Some firmware reports the record as an ASCII hex string.
  std::vector<uint8_t> decoded;
  const uint8_t *bytes = data;
  size_t nbytes = len;
  if (len % 2 == 0) {
    bool all_hex = true;
    for (size_t i = 0; i < len; i++) {
      if (hex_nibble(data[i]) < 0) {
        all_hex = false;
        break;
      }
    }
    if (all_hex) {
      decoded.reserve(len / 2);
      for (size_t i = 0; i < len; i += 2)
        decoded.push_back(static_cast<uint8_t>((hex_nibble(data[i]) << 4) | hex_nibble(data[i + 1])));
      bytes = decoded.data();
      nbytes = decoded.size();
    }
  }

  if (nbytes < 12)
    return std::nullopt;
  A24Strike strike{};
  strike.flags = bytes[5];
  strike.minutes_since_epoch = *parse_u32_le(bytes, nbytes, 6);
  strike.strike_id = *parse_u16_le(bytes + 10, 2);
  return strike;
}

// ---------------------------------------------------------------------------
// C20 framing
// ---------------------------------------------------------------------------

uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    uint16_t work = static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; bit++) {
      bool set = ((crc ^ work) & 0x8000) != 0;
      crc = static_cast<uint16_t>(crc << 1);
      if (set)
        crc ^= 0x1021;
      work = static_cast<uint16_t>(work << 1);
    }
  }
  return crc;
}

std::vector<uint8_t> c20_encode(uint8_t type, uint8_t subtype, const uint8_t *payload, size_t len) {
  std::vector<uint8_t> body;
  body.reserve(len + 4);
  body.push_back(type);
  body.push_back(subtype);
  if (payload != nullptr && len > 0)
    body.insert(body.end(), payload, payload + len);
  uint16_t crc = crc16_ccitt(body.data(), body.size());
  put_u16_le(body, crc);

  std::vector<uint8_t> frame;
  frame.reserve(body.size() + 4);
  frame.push_back(C20_FRAME_START);
  for (uint8_t b : body) {
    if (b == C20_FRAME_START || b == C20_FRAME_END || b == C20_FRAME_ESC) {
      frame.push_back(C20_FRAME_ESC);
      frame.push_back(b ^ 0x04);
    } else {
      frame.push_back(b);
    }
  }
  frame.push_back(C20_FRAME_END);
  return frame;
}

std::optional<C20Frame> c20_decode(const uint8_t *data, size_t len) {
  // Minimum: start, type, subtype, crc(2), end
  if (data == nullptr || len < 6 || data[0] != C20_FRAME_START || data[len - 1] != C20_FRAME_END)
    return std::nullopt;

  std::vector<uint8_t> body;
  body.reserve(len);
  for (size_t i = 1; i < len - 1; i++) {
    uint8_t b = data[i];
    if (b == C20_FRAME_ESC) {
      i++;
      if (i >= len - 1)
        return std::nullopt;
      body.push_back(data[i] ^ 0x04);
    } else {
      body.push_back(b);
    }
  }
  if (body.size() < 4)
    return std::nullopt;

  size_t content_len = body.size() - 2;
  uint16_t expected = *parse_u16_le(body.data() + content_len, 2);
  uint16_t actual = crc16_ccitt(body.data(), content_len);
  if (expected != actual)
    return std::nullopt;

  C20Frame frame;
  frame.type = body[0];
  frame.subtype = body[1];
  frame.payload.assign(body.begin() + 2, body.begin() + content_len);
  return frame;
}

std::vector<C20Frame> C20FrameAssembler::feed(const uint8_t *data, size_t len) {
  std::vector<C20Frame> frames;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    if (!this->in_frame_) {
      if (b == C20_FRAME_START) {
        this->in_frame_ = true;
        this->buffer_.clear();
        this->buffer_.push_back(b);
      }
      continue;
    }
    this->buffer_.push_back(b);
    if (b == C20_FRAME_END) {
      // An escaped 0xB1 arrives as 0xB2 0xB5, so a literal 0xB1 always ends a frame.
      auto frame = c20_decode(this->buffer_.data(), this->buffer_.size());
      if (frame.has_value())
        frames.push_back(std::move(*frame));
      this->in_frame_ = false;
      this->buffer_.clear();
    } else if (b == C20_FRAME_START) {
      // Unexpected restart: drop the partial frame and begin again.
      this->buffer_.clear();
      this->buffer_.push_back(b);
    } else if (this->buffer_.size() > 512) {
      // Runaway; resync.
      this->in_frame_ = false;
      this->buffer_.clear();
    }
  }
  return frames;
}

// ---------------------------------------------------------------------------
// C20 payloads
// ---------------------------------------------------------------------------

template<typename E> static E enum_at(uint8_t idx, E max_no_data) {
  if (idx >= static_cast<uint8_t>(max_no_data))
    return max_no_data;
  return static_cast<E>(idx);
}

const char *c20_device_state_str(C20DeviceState v) {
  switch (v) {
    case C20DeviceState::DEACTIVATED:
      return "DEACTIVATED";
    case C20DeviceState::ACTIVATED:
      return "ACTIVATED";
    case C20DeviceState::ERROR:
      return "ERROR";
    default:
      return "NO_DATA";
  }
}

const char *c20_kill_state_str(C20KillState v) {
  switch (v) {
    case C20KillState::CLEARED:
      return "CLEARED";
    case C20KillState::DETECTED:
      return "DETECTED";
    default:
      return "NO_DATA";
  }
}

const char *c20_battery_state_str(C20BatteryState v) {
  switch (v) {
    case C20BatteryState::STARTUP:
      return "STARTUP";
    case C20BatteryState::NOT_CONNECTED:
      return "NOT_CONNECTED";
    case C20BatteryState::CRITICAL:
      return "CRITICAL";
    case C20BatteryState::LOW:
      return "LOW";
    case C20BatteryState::NORMAL:
      return "NORMAL";
    default:
      return "NO_DATA";
  }
}

const char *c20_striker_source_str(C20StrikerSource v) {
  switch (v) {
    case C20StrikerSource::TRIGGER:
      return "TRIGGER";
    case C20StrikerSource::UNUSED:
      return "UNUSED";
    case C20StrikerSource::USER:
      return "USER";
    default:
      return "NO_DATA";
  }
}

std::optional<C20DeviceStateResponse> parse_c20_device_state(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 14)
    return std::nullopt;
  C20DeviceStateResponse r{};
  r.device_time = *parse_u32_le(data, len, 0);
  r.device_state = enum_at<C20DeviceState>(data[4], C20DeviceState::NO_DATA);
  r.kill_state = enum_at<C20KillState>(data[5], C20KillState::NO_DATA);
  r.tray_state = enum_at<C20TrayState>(data[6], C20TrayState::NO_DATA);
  r.battery_state = enum_at<C20BatteryState>(data[7], C20BatteryState::NO_DATA);
  r.charge_state = enum_at<C20ChargeState>(data[8], C20ChargeState::NO_DATA);
  r.usb_state = enum_at<C20UsbState>(data[9], C20UsbState::NO_DATA);
  r.strike_count = *parse_u32_le(data, len, 10);
  return r;
}

std::optional<C20BatteryLevelResponse> parse_c20_battery_level(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 5)
    return std::nullopt;
  C20BatteryLevelResponse r{};
  r.device_time = *parse_u32_le(data, len, 0);
  r.battery_percent = data[4];
  return r;
}

std::optional<C20StrikerEvent> parse_c20_striker_event(const uint8_t *data, size_t len) {
  if (data == nullptr || len < 21)
    return std::nullopt;
  C20StrikerEvent e{};
  e.event_time = *parse_u32_le(data, len, 0);
  e.strike_count = *parse_u32_le(data, len, 4);
  e.source = enum_at<C20StrikerSource>(data[8], C20StrikerSource::NO_DATA);
  e.trigger_number = *parse_u32_le(data, len, 9);
  e.fire_time_ms = *parse_u32_le(data, len, 13);
  e.rewind_time_ms = *parse_u32_le(data, len, 17);
  if (len >= 29)
    e.backdrive_time_ms = *parse_u32_le(data, len, 25);
  return e;
}

std::string parse_c20_firmware(const uint8_t *data, size_t len) {
  if (data == nullptr)
    return "";
  size_t n = 0;
  while (n < len && data[n] != 0)
    n++;
  return decode_text(data, n);
}

// ---------------------------------------------------------------------------
// C20 request builders
// ---------------------------------------------------------------------------

std::vector<uint8_t> c20_build_set_time(uint32_t epoch_seconds) {
  std::vector<uint8_t> payload;
  put_u32_le(payload, epoch_seconds);
  return c20_encode(C20_TYPE_SET_TIME, C20_SUBTYPE_SET, payload);
}

std::vector<uint8_t> c20_build_request(uint8_t type) { return c20_encode(type, C20_SUBTYPE_REQUEST); }

std::vector<uint8_t> c20_build_set_command(uint32_t command) {
  std::vector<uint8_t> payload;
  put_u32_le(payload, command);
  return c20_encode(C20_TYPE_SET_COMMAND, C20_SUBTYPE_SET, payload);
}

std::vector<uint8_t> c20_build_logs_request(uint32_t start_epoch, uint32_t end_epoch, uint32_t flags) {
  std::vector<uint8_t> payload;
  put_u32_le(payload, start_epoch);
  put_u32_le(payload, end_epoch);
  put_u32_le(payload, flags);
  return c20_encode(C20_TYPE_LOGS, C20_SUBTYPE_REQUEST, payload);
}

}  // namespace esphome::goodnature_ble::protocol

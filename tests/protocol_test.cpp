// Host-side tests for the Goodnature protocol layer.
// Build and run with tests/run_tests.sh (only needs a C++17 compiler).

#include "../components/goodnature_ble/protocol.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace esphome::goodnature_ble::protocol;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    checks++;                                                                       \
    if (!(cond)) {                                                                  \
      failures++;                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                               \
  } while (0)

static void test_uuid() {
  CHECK(gn_uuid_str(0xD00D) == "0000d00d-1212-efde-1523-785fef13d123");
  CHECK(gn_uuid_str(0xDE11) == "0000de11-1212-efde-1523-785fef13d123");
  CHECK(std::string(model_name(Model::A24)) == "A24 Smart Trap");
  CHECK(std::string(model_name(Model::C20)) == "Mouse Trap");
  CHECK(std::string(model_name(Model::UNKNOWN)) == "Unknown");
}

static void test_helpers() {
  uint8_t u16[] = {0x34, 0x12};
  CHECK(parse_u16_le(u16, 2).value() == 0x1234);
  CHECK(!parse_u16_le(u16, 1).has_value());
  uint8_t u32[] = {0x78, 0x56, 0x34, 0x12};
  CHECK(parse_u32_le(u32, 4).value() == 0x12345678UL);
  CHECK(!parse_u32_le(u32, 4, 1).has_value());

  uint8_t txt[] = {'1', '.', '2', '.', '3', 0, 0};
  CHECK(decode_text(txt, sizeof(txt)) == "1.2.3");
  uint8_t bin[] = {0x01, 0x02, 0xFF};
  CHECK(decode_text(bin, sizeof(bin)) == "0102ff");
  CHECK(decode_text(nullptr, 0) == "");
  uint8_t a24_serial[] = {0x44, 0x23, 0x26, 0x97};
  CHECK(decode_a24_serial(a24_serial, sizeof(a24_serial)) == "97262344");
  uint8_t a24_fw[] = {0x01, 0x03};
  CHECK(decode_a24_firmware(a24_fw, sizeof(a24_fw)) == "1.3.0");
  uint8_t a24_fw_nul[] = {0x01, 0x03, 0x00};
  CHECK(decode_a24_firmware(a24_fw_nul, sizeof(a24_fw_nul)) == "1.3.0");
  uint8_t a24_fw_text[] = {'0', '1', '0', '3'};
  CHECK(decode_a24_firmware(a24_fw_text, sizeof(a24_fw_text)) == "1.3.0");
}

static void test_c20_advertisement() {
  // serial 0x0A0B0C0D LE, device type 7, flags: activated|strikes|charging|usb,
  // strike count 0x0102, battery 87%
  uint8_t adv[] = {0x0D, 0x0C, 0x0B, 0x0A, 0x07, 0x01 | 0x02 | 0x10 | 0x20, 0x02, 0x01, 87};
  auto parsed = parse_c20_advertisement(adv, sizeof(adv));
  CHECK(parsed.has_value());
  CHECK(parsed->serial_number() == "0A0B0C0D");
  CHECK(parsed->device_type == 7);
  CHECK(parsed->activated);
  CHECK(parsed->strikes_available);
  CHECK(!parsed->low_battery);
  CHECK(parsed->charging);
  CHECK(parsed->usb_connected);
  CHECK(!parsed->discoverable);
  CHECK(parsed->strike_count == 0x0102);
  CHECK(parsed->flags == (0x01 | 0x02 | 0x10 | 0x20));
  CHECK(parsed->battery_state_raw == 87);
  CHECK(!parse_c20_advertisement(adv, 8).has_value());

  // Real Mouse Trap advertisement (firmware 0.3.730): serial B2D07F47, 14
  // strikes, byte 5 = 0x04, byte 8 = 4 = battery state NORMAL while the
  // trap's UART battery level said 92%.
  uint8_t mouse[] = {0x47, 0x7F, 0xD0, 0xB2, 0x00, 0x04, 0x0E, 0x00, 0x04};
  auto m = parse_c20_advertisement(mouse, sizeof(mouse));
  CHECK(m.has_value());
  CHECK(m->serial_number() == "B2D07F47");
  CHECK(m->device_type == 0);
  CHECK(m->flags == 0x04);
  CHECK(m->strike_count == 14);
  CHECK(m->battery_state_raw == static_cast<uint8_t>(C20BatteryState::NORMAL));
}

static void test_d30d() {
  // 12 bytes: [0] peak level, [1..4] trap ID, [5] temperature (signed),
  // [6..9] minutes LE, [10..11] strike id LE
  uint32_t minutes = 1758400000UL / 60;  // some time in Sept 2025
  uint8_t rec[12] = {0x82, 0x44, 0x23, 0x26, 0x97, 0x12, 0, 0, 0, 0, 0x2A, 0x00};
  rec[6] = minutes & 0xFF;
  rec[7] = (minutes >> 8) & 0xFF;
  rec[8] = (minutes >> 16) & 0xFF;
  rec[9] = (minutes >> 24) & 0xFF;
  auto s = parse_d30d(rec, sizeof(rec));
  CHECK(s.has_value());
  CHECK(s->peak_level == 0x82);
  CHECK(s->temperature == 18);
  rec[5] = 0xFB;  // -5 degrees
  CHECK(parse_d30d(rec, sizeof(rec))->temperature == -5);
  rec[5] = 0x12;
  CHECK(s->minutes_since_epoch == minutes);
  CHECK(s->strike_id == 42);
  CHECK(s->epoch_seconds() == minutes * 60);

  // ASCII-hex representation of the same record
  std::string hex = to_hex(rec, sizeof(rec));
  auto s2 = parse_d30d(reinterpret_cast<const uint8_t *>(hex.data()), hex.size());
  CHECK(s2.has_value());
  CHECK(s2->strike_id == 42);
  CHECK(s2->minutes_since_epoch == minutes);

  CHECK(!parse_d30d(rec, 11).has_value());
}

static void test_a24_advertisement() {
  // The two payloads captured from one A24 (docs/ble-protocol.md), as the six
  // little-endian 16-bit service UUIDs: 44 23 26 97 26 00 34 12 FD D8 00 01
  const uint16_t woke[6] = {0x2344, 0x9726, 0x0026, 0x1234, 0xD8FD, 0x0100};
  auto a = parse_a24_advertisement(woke, 6);
  CHECK(a.has_value());
  CHECK(a->flags == 0x26 && a->device_type == 0);
  CHECK(!a->strikes_available() && a->activated() && a->moving() && !a->low_battery());
  CHECK(a->new_event() && !a->new_error());
  CHECK(a->voltage_raw == 0xD8 && a->internal_resistance == 0xFD);
  CHECK(a->kills == 1 && a->unread_kills == 0);
  const uint16_t idle[6] = {0x2344, 0x9726, 0x0026, 0x1234, 0x0000, 0x0000};
  auto b = parse_a24_advertisement(idle, 6);
  CHECK(b.has_value() && b->voltage_raw == 0 && b->kills == 0);
  CHECK(!parse_a24_advertisement(woke, 5).has_value());
  const uint16_t other[6] = {0x2344, 0x9726, 0x0026, 0x600D, 0xD8FD, 0x0100};
  CHECK(!parse_a24_advertisement(other, 6).has_value());

  // 216 / 255 * 3.6 = 3.05 V; internal resistance thresholds 56 / 200 / 230
  CHECK(std::fabs(a24_battery_volts(216) - 3.0494f) < 1e-3f);
  CHECK(a24_battery_health(55) == A24BatteryHealth::FULL);
  CHECK(a24_battery_health(56) == A24BatteryHealth::MEDIUM);
  CHECK(a24_battery_health(200) == A24BatteryHealth::LOW);
  CHECK(a24_battery_health(230) == A24BatteryHealth::CRITICAL);
  CHECK(std::string(a24_battery_health_str(A24BatteryHealth::MEDIUM)) == "Medium");

  // Striker sources from app 3.6.1: 3 is a backdrive self-test, not a kill
  std::vector<uint8_t> se;
  put_u32_le(se, 1);
  put_u32_le(se, 0);
  se.push_back(3);
  for (int i = 0; i < 3; i++)
    put_u32_le(se, 0);
  CHECK(parse_c20_striker_event(se.data(), se.size())->source == C20StrikerSource::BACKDRIVE_TEST);
  se[8] = 9;
  CHECK(parse_c20_striker_event(se.data(), se.size())->source == C20StrikerSource::NO_DATA);
}

static void test_crc_and_framing() {
  // Known CRC16-CCITT (0xFFFF init) of "123456789" is 0x29B1
  const char *msg = "123456789";
  CHECK(crc16_ccitt(reinterpret_cast<const uint8_t *>(msg), 9) == 0x29B1);

  // Round-trip a simple request
  auto req = c20_build_request(C20_TYPE_DEVICE_STATE);
  CHECK(req.front() == C20_FRAME_START);
  CHECK(req.back() == C20_FRAME_END);
  auto dec = c20_decode(req.data(), req.size());
  CHECK(dec.has_value());
  CHECK(dec->type == C20_TYPE_DEVICE_STATE);
  CHECK(dec->subtype == C20_SUBTYPE_REQUEST);
  CHECK(dec->payload.empty());

  // Set command FIRE payload is 0x3E9130BE little-endian
  auto fire = c20_build_set_command(C20_COMMAND_FIRE);
  auto fire_dec = c20_decode(fire.data(), fire.size());
  CHECK(fire_dec.has_value());
  CHECK(fire_dec->type == C20_TYPE_SET_COMMAND);
  CHECK(fire_dec->subtype == C20_SUBTYPE_SET);
  CHECK(fire_dec->payload.size() == 4);
  CHECK(fire_dec->payload[0] == 0xBE && fire_dec->payload[1] == 0x30 && fire_dec->payload[2] == 0x91 &&
        fire_dec->payload[3] == 0x3E);

  // Escaping: payload containing frame delimiters must round-trip
  std::vector<uint8_t> tricky = {0xB0, 0xB1, 0xB2, 0x00, 0xB1};
  auto enc = c20_encode(0x7F, 0x01, tricky);
  // No raw delimiters inside the frame body
  for (size_t i = 1; i + 1 < enc.size(); i++)
    CHECK(enc[i] != C20_FRAME_START && enc[i] != C20_FRAME_END);
  auto tricky_dec = c20_decode(enc.data(), enc.size());
  CHECK(tricky_dec.has_value());
  CHECK(tricky_dec->payload == tricky);

  // Corrupt CRC is rejected
  auto bad = req;
  bad[2] ^= 0xFF;
  CHECK(!c20_decode(bad.data(), bad.size()).has_value());
  CHECK(!c20_decode(req.data(), 3).has_value());

  // Logs request layout: start, end, flags
  auto logs = c20_build_logs_request(100, 200, C20_LOGS_FLAG_STRIKER_EVENTS);
  auto logs_dec = c20_decode(logs.data(), logs.size());
  CHECK(logs_dec.has_value());
  CHECK(logs_dec->payload.size() == 12);
  CHECK(parse_u32_le(logs_dec->payload.data(), 12, 0).value() == 100);
  CHECK(parse_u32_le(logs_dec->payload.data(), 12, 4).value() == 200);
  CHECK(parse_u32_le(logs_dec->payload.data(), 12, 8).value() == 2);

  // Set time uses subtype 0x02
  auto st = c20_build_set_time(1700000000UL);
  auto st_dec = c20_decode(st.data(), st.size());
  CHECK(st_dec.has_value());
  CHECK(st_dec->type == C20_TYPE_SET_TIME && st_dec->subtype == C20_SUBTYPE_SET);
  CHECK(parse_u32_le(st_dec->payload.data(), 4).value() == 1700000000UL);
}

static void test_assembler() {
  auto a = c20_build_request(C20_TYPE_FIRMWARE);
  auto b = c20_build_set_command(C20_COMMAND_CLEAR);
  std::vector<uint8_t> stream = {0x11, 0x22};  // junk before
  stream.insert(stream.end(), a.begin(), a.end());
  stream.insert(stream.end(), b.begin(), b.end());

  C20FrameAssembler asm_;
  // Feed in two arbitrary chunks that split frame b
  size_t split = a.size() + 5;
  auto f1 = asm_.feed(stream.data(), split);
  CHECK(f1.size() == 1);
  CHECK(f1[0].type == C20_TYPE_FIRMWARE);
  auto f2 = asm_.feed(stream.data() + split, stream.size() - split);
  CHECK(f2.size() == 1);
  CHECK(f2[0].type == C20_TYPE_SET_COMMAND);
  CHECK(parse_u32_le(f2[0].payload.data(), 4).value() == C20_COMMAND_CLEAR);

  // Byte-at-a-time feed
  C20FrameAssembler one;
  size_t got = 0;
  for (uint8_t byte : stream) {
    got += one.feed(&byte, 1).size();
  }
  CHECK(got == 2);
}

static void test_c20_payloads() {
  // DeviceStateResponse: time(4) state kill tray batt charge usb strikes(4)
  std::vector<uint8_t> ds;
  put_u32_le(ds, 1700000000UL);
  ds.push_back(1);  // ACTIVATED
  ds.push_back(1);  // DETECTED
  ds.push_back(1);  // CLOSED
  ds.push_back(3);  // LOW
  ds.push_back(0);  // NOT_CHARGING
  ds.push_back(1);  // CONNECTED
  put_u32_le(ds, 17);
  auto r = parse_c20_device_state(ds.data(), ds.size());
  CHECK(r.has_value());
  CHECK(r->device_time == 1700000000UL);
  CHECK(r->device_state == C20DeviceState::ACTIVATED);
  CHECK(r->kill_state == C20KillState::DETECTED);
  CHECK(r->tray_state == C20TrayState::CLOSED);
  CHECK(r->battery_state == C20BatteryState::LOW);
  CHECK(r->charge_state == C20ChargeState::NOT_CHARGING);
  CHECK(r->usb_state == C20UsbState::CONNECTED);
  CHECK(r->strike_count == 17);
  CHECK(!parse_c20_device_state(ds.data(), 13).has_value());
  // Out-of-range enum values clamp to NO_DATA
  ds[4] = 99;
  CHECK(parse_c20_device_state(ds.data(), ds.size())->device_state == C20DeviceState::NO_DATA);

  std::vector<uint8_t> bl;
  put_u32_le(bl, 5);
  bl.push_back(64);
  auto b = parse_c20_battery_level(bl.data(), bl.size());
  CHECK(b.has_value() && b->battery_percent == 64);

  std::vector<uint8_t> se;
  put_u32_le(se, 1700000100UL);  // event time
  put_u32_le(se, 18);            // strike count
  se.push_back(2);               // USER
  put_u32_le(se, 3);             // trigger number
  put_u32_le(se, 120);           // fire ms
  put_u32_le(se, 900);           // rewind ms
  auto ev = parse_c20_striker_event(se.data(), se.size());
  CHECK(ev.has_value());
  CHECK(ev->event_time == 1700000100UL);
  CHECK(ev->strike_count == 18);
  CHECK(ev->source == C20StrikerSource::USER);
  CHECK(ev->trigger_number == 3);
  CHECK(ev->fire_time_ms == 120);
  CHECK(ev->rewind_time_ms == 900);
  CHECK(!ev->backdrive_time_ms.has_value());
  CHECK(!ev->peak_fire_ma.has_value());
  se.push_back(0xE8);   // peak fire 1000 mA [21..22]
  se.push_back(0x03);
  se.push_back(0x9C);   // peak rewind -100 mA [23..24]
  se.push_back(0xFF);
  put_u32_le(se, 333);  // backdrive [25..28]
  auto ev2 = parse_c20_striker_event(se.data(), se.size());
  CHECK(ev2.has_value() && ev2->backdrive_time_ms.value() == 333);
  CHECK(ev2->peak_fire_ma.value() == 1000);
  CHECK(ev2->peak_rewind_ma.value() == -100);

  uint8_t fw[] = {'3', '.', '2', '.', '0', 0, 'x', 'y'};
  CHECK(parse_c20_firmware(fw, sizeof(fw)) == "3.2.0");

  // KillHistoryResponse: ten u32 kill times, 0 = empty slot
  std::vector<uint8_t> kh;
  put_u32_le(kh, 1752930863UL);
  put_u32_le(kh, 1792506000UL);
  for (int i = 0; i < 8; i++)
    put_u32_le(kh, 0);
  auto h = parse_c20_kill_history(kh.data(), kh.size());
  CHECK(h.has_value());
  CHECK((*h)[0] == 1752930863UL);
  CHECK((*h)[1] == 1792506000UL);
  CHECK((*h)[9] == 0);
  CHECK(!parse_c20_kill_history(kh.data(), 39).has_value());

  // InputStatesResponse: four pin bytes, battery mV and motor feedback (i16 LE)
  uint8_t is[] = {1, 1, 0, 0, 0x6C, 0x0F, 0xFE, 0xFF};
  auto in = parse_c20_input_states(is, sizeof(is));
  CHECK(in.has_value());
  CHECK(in->charge_pin == 1 && in->usb_pin == 1 && in->button_pin == 0 && in->battery_error_pin == 0);
  CHECK(in->battery_mv == 3948);
  CHECK(in->motor_feedback == -2);
  CHECK(!parse_c20_input_states(is, 7).has_value());

  // IEEE 11073 FLOAT: mantissa 1234, exponent -2 -> 12.34; negative mantissa
  CHECK(std::fabs(decode_ieee11073_float(0xFE0004D2UL) - 12.34f) < 1e-4f);
  CHECK(std::fabs(decode_ieee11073_float(0x00FFFFFFUL) + 1.0f) < 1e-6f);
  CHECK(decode_ieee11073_float(0x02000003UL) == 300.0f);

  // TriggerEvent: time, count, then inner and outer sensors and alert time
  std::vector<uint8_t> te;
  put_u32_le(te, 1792506000UL);
  put_u32_le(te, 4);
  CHECK(parse_c20_trigger_event(te.data(), te.size()).has_value());
  CHECK(!parse_c20_trigger_event(te.data(), te.size())->inner.has_value());
  for (uint32_t v : {100UL, 900UL, 0xFF0003E8UL, 300UL, 45UL,     // inner: baseline 100.0
                     110UL, 800UL, 0x000000C8UL, 250UL, 40UL, 7UL}) // outer: baseline 200
    put_u32_le(te, v);
  auto trig = parse_c20_trigger_event(te.data(), te.size());
  CHECK(trig.has_value() && trig->count == 4);
  CHECK(trig->inner->ambient == 100 && trig->inner->on_reading == 900 && trig->inner->threshold == 300);
  CHECK(std::fabs(trig->inner->baseline - 100.0f) < 1e-3f);
  CHECK(trig->outer->on_time == 40 && std::fabs(trig->outer->baseline - 200.0f) < 1e-3f);
  CHECK(trig->alert_time.value() == 7);
  CHECK(!parse_c20_trigger_event(te.data(), 7).has_value());
}

static void test_versions() {
  CHECK(compare_versions("0.3.730", "0.2.550").value() > 0);
  CHECK(compare_versions("0.2.550", "0.2.550").value() == 0);
  CHECK(compare_versions("0.2.549", "0.2.550").value() < 0);
  CHECK(compare_versions("0.10.0", "0.9.9").value() > 0);  // numeric, not lexical
  CHECK(compare_versions("v2.0.0", "1.0.0").value() > 0);
  CHECK(compare_versions("1.0", "1.0.0").value() == 0);
  CHECK(!compare_versions("", "1.0.0").has_value());
  CHECK(!compare_versions("0.3.x", "1.0.0").has_value());
  CHECK(!compare_versions("1..0", "1.0.0").has_value());
  CHECK(!compare_versions("1.0.", "1.0.0").has_value());

  // New request frames carry no payload; WAKE is a SetCommand like FIRE.
  for (uint8_t type : {C20_TYPE_KILL_HISTORY, C20_TYPE_INPUT_STATES, C20_TYPE_BOOTLOADER_VERSION}) {
    auto req = c20_build_request(type);
    auto dec = c20_decode(req.data(), req.size());
    CHECK(dec.has_value() && dec->type == type && dec->subtype == C20_SUBTYPE_REQUEST && dec->payload.empty());
  }
  auto wake = c20_build_set_command(C20_COMMAND_WAKE);
  auto wake_dec = c20_decode(wake.data(), wake.size());
  CHECK(wake_dec.has_value() && wake_dec->type == C20_TYPE_SET_COMMAND);
  CHECK(parse_u32_le(wake_dec->payload.data(), wake_dec->payload.size()).value() == 0x528670E4UL);
  CHECK(C20_COMMAND_CLEAR == 0x6448E82DUL);
}

int main() {
  test_uuid();
  test_helpers();
  test_c20_advertisement();
  test_d30d();
  test_a24_advertisement();
  test_crc_and_framing();
  test_assembler();
  test_c20_payloads();
  test_versions();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}

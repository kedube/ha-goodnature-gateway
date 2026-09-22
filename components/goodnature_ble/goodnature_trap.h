#pragma once

#ifdef USE_ESP32

#include "protocol.h"

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/device.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/event/event.h"
#include "esphome/components/number/number.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esphome::goodnature_ble {

class GoodnatureHub;

// Kinds of per-slot buttons (TrapButton in entities.h). Declared here so the
// trap can leave model-specific ones unregistered.
enum class TrapButtonKind : uint8_t {
  POLL = 0,
  TEST_FIRE,
  RESET_ALERT,
  LURE_REPLACED,
  CO2_REPLACED,
  FORGET,
  LOG_DEBUG,
};

// Work the gateway can do over a GATT connection to a trap.
enum class Job : uint8_t {
  NONE = 0,
  POLL,         // read counters, battery, identity
  TEST_FIRE,    // A24: DE13=0x05 flow. C20: SetCommand(FIRE)
  RESET_ALERT,  // A24: ack read pointers + DE13=0x02. C20: SetCommand(CLEAR)
};

// Persisted per slot so a trap keeps its device, counters and consumable
// timers across gateway reboots. Kept trivially copyable for NVS.
struct TrapPrefs {
  uint64_t address;         // 0 = slot unassigned
  uint8_t model;            // protocol::Model
  uint8_t addr_type;        // esp_ble_addr_type_t
  uint16_t lure_life_days;  // 0 = use hub default
  uint32_t serial_raw;      // C20 serial from the advertisement, 0 = unknown
  uint32_t lure_replaced;   // epoch seconds, 0 = not yet started
  uint32_t co2_replaced;    // epoch seconds, 0 = not yet started
  uint32_t strikes_at_co2;  // strike counter value when the canister was replaced
  uint32_t strikes;         // last known strike counter
  uint32_t last_strike;     // epoch seconds of last known strike
  uint32_t last_seen;       // epoch seconds the trap was last heard
} __attribute__((packed));

// Readings and log-scan progress kept in flash beside TrapPrefs so they
// survive a gateway restart. Kept separate so TrapPrefs keeps its layout
// (ESPHome rejects a stored record whose size changed, which would forget
// every trap on upgrade).
struct TrapCache {
  uint8_t version;          // CACHE_VERSION
  uint8_t battery_percent;  // 0xFF = unknown
  uint8_t flags;            // CACHE_* bits below
  uint8_t reserved;
  uint32_t log_scanned_to;  // C20: striker log counted up to this epoch (0 = never)
  uint32_t last_full_scan;  // C20: epoch of the last replay of the whole log
  uint32_t test_fires;      // C20: USER striker events seen in the whole log
} __attribute__((packed));

static constexpr uint8_t CACHE_VERSION = 1;
static constexpr uint8_t CACHE_ACTIVE_KNOWN = 0x01;
static constexpr uint8_t CACHE_ACTIVE = 0x02;
static constexpr uint8_t CACHE_CHARGING_KNOWN = 0x04;
static constexpr uint8_t CACHE_CHARGING = 0x08;

// Earliest start accepted for a Mouse Trap log window. A start of 0 made the
// trap stream its events but never send the terminator, so full replays
// begin in mid 2017, long before any Mouse Trap existed.
static constexpr uint32_t C20_HISTORY_START_EPOCH = 1500000000UL;

// One trap slot. Slots are created at compile time (max_traps) and bound
// to a physical trap at runtime when the hub discovers one. Each slot is
// a Home Assistant sub-device with its own entities.
class GoodnatureTrap {
 public:
  void set_hub(GoodnatureHub *hub) { this->hub_ = hub; }
  GoodnatureHub *hub() const { return this->hub_; }
  void set_index(uint8_t index) { this->index_ = index; }
  uint8_t index() const { return this->index_; }

  // The slot's Home Assistant sub-device and the buttons that live on it.
  // Codegen configures every slot's device and entities but does not
  // register them with the App; register_with_app() does that for slots the
  // hub decides are active (the gateway's "Slots" number).
  void set_device(Device *device) { this->device_ = device; }
  Device *device() const { return this->device_; }
  void add_button(button::Button *b, TrapButtonKind kind) { this->buttons_.push_back({b, kind}); }
  bool is_active() const { return this->slot_active_; }
  // True when this slot's entities were registered for a different model
  // than the trap it now holds; only a restart can change the entity set.
  bool needs_reregister() const {
    return this->slot_active_ && this->is_bound() && this->registered_model_ != this->model();
  }
  void set_active(bool active) { this->slot_active_ = active; }
  // Registers the slot's device and entities with the App. When the slot is
  // already bound (prefs loaded), entities that do not apply to that trap
  // model are left out, so Home Assistant never sees CO2 entities on a Mouse
  // Trap. A slot that binds after boot keeps the full set until a restart.
  void register_with_app();

  // ---- entity wiring (called from generated code) ------------------------
  void set_strikes_sensor(sensor::Sensor *s) { this->strikes_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { this->battery_sensor_ = s; }
  void set_battery_voltage_sensor(sensor::Sensor *s) { this->battery_voltage_sensor_ = s; }
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_sensor_ = s; }
  void set_last_seen_sensor(sensor::Sensor *s) { this->last_seen_sensor_ = s; }
  void set_last_strike_sensor(sensor::Sensor *s) { this->last_strike_sensor_ = s; }
  void set_lure_age_sensor(sensor::Sensor *s) { this->lure_age_sensor_ = s; }
  void set_lure_remaining_sensor(sensor::Sensor *s) { this->lure_remaining_sensor_ = s; }
  void set_co2_remaining_sensor(sensor::Sensor *s) { this->co2_remaining_sensor_ = s; }

  void set_kill_alert_binary_sensor(binary_sensor::BinarySensor *s) { this->kill_alert_bs_ = s; }
  void set_battery_low_binary_sensor(binary_sensor::BinarySensor *s) { this->battery_low_bs_ = s; }
  void set_charging_binary_sensor(binary_sensor::BinarySensor *s) { this->charging_bs_ = s; }
  void set_active_binary_sensor(binary_sensor::BinarySensor *s) { this->active_bs_ = s; }
  void set_lure_due_binary_sensor(binary_sensor::BinarySensor *s) { this->lure_due_bs_ = s; }
  void set_co2_low_binary_sensor(binary_sensor::BinarySensor *s) { this->co2_low_bs_ = s; }
  void set_online_binary_sensor(binary_sensor::BinarySensor *s) { this->online_bs_ = s; }

  void set_model_text_sensor(text_sensor::TextSensor *s) { this->model_ts_ = s; }
  void set_serial_text_sensor(text_sensor::TextSensor *s) { this->serial_ts_ = s; }
  void set_firmware_text_sensor(text_sensor::TextSensor *s) { this->firmware_ts_ = s; }
  void set_mac_text_sensor(text_sensor::TextSensor *s) { this->mac_ts_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_ts_ = s; }
  void set_last_advertisement_text_sensor(text_sensor::TextSensor *s) { this->last_adv_ts_ = s; }
  void set_last_frame_text_sensor(text_sensor::TextSensor *s) { this->last_frame_ts_ = s; }

  void set_lure_life_number(number::Number *n) { this->lure_life_number_ = n; }
  void set_strike_event(event::Event *e) { this->strike_event_ = e; }

  // ---- lifecycle ----------------------------------------------------------
  void init_prefs();  // load the binding from flash; before the hub picks active slots
  void setup();       // restore state and publish it (active slots only)

  bool is_bound() const { return this->prefs_.address != 0; }
  uint64_t address() const { return this->prefs_.address; }
  esp_ble_addr_type_t address_type() const { return static_cast<esp_ble_addr_type_t>(this->prefs_.addr_type); }
  protocol::Model model() const { return static_cast<protocol::Model>(this->prefs_.model); }
  uint32_t serial_raw() const { return this->prefs_.serial_raw; }
  const char *address_str() const { return this->address_str_; }

  void bind(uint64_t address, esp_ble_addr_type_t addr_type, protocol::Model model, uint32_t serial_raw);
  // The same trap (matched by C20 serial) showed up under a new address.
  void rebind_address(uint64_t address, esp_ble_addr_type_t addr_type);
  void unbind();

  // ---- advertisement path (passive) --------------------------------------
  void on_advertisement(const esp32_ble_tracker::ESPBTDevice &device, protocol::Model detected_model,
                        const protocol::C20Advertisement *c20_adv);

  // ---- connection path (active) ------------------------------------------
  // Job requests from buttons. The hub connects on the next advertisement.
  void request_job(Job job);
  Job pending_job() const { return this->pending_job_; }
  // Next poll replays the whole striker log instead of only what is new.
  void request_full_rescan() { this->full_rescan_requested_ = true; }
  // Start of the striker-log window for a poll ending at now_epoch, and
  // whether that window is a full replay. Called by the connection when it
  // builds the logs request; the answer is applied in apply_history_().
  uint32_t log_window_start(uint32_t now_epoch);
  void clear_pending_job() { this->pending_job_ = Job::NONE; }

  // True when the hub should connect to this trap next time it is heard.
  bool wants_connection(uint32_t now_ms) const;

  // Results delivered by GoodnatureConnection.
  void apply_a24_characteristic(uint16_t short_id, const uint8_t *data, size_t len);
  void apply_c20_frame(const protocol::C20Frame &frame);
  void on_job_started(Job job);
  void on_job_finished(Job job, bool ok, const char *reason);

  // Debug capture: the connection hands over a printable summary of each
  // GATT read or UART frame while debug mode is on.
  void publish_raw_frame(const std::string &summary);
  bool gatt_dumped() const { return this->gatt_dumped_; }
  void set_gatt_dumped() { this->gatt_dumped_ = true; }

  // Seconds since the epoch of the last poll, 0 if never (for C20 log windows).
  uint32_t last_poll_epoch() const { return this->last_poll_epoch_; }
  uint32_t last_seen_ms() const { return this->last_seen_ms_; }

  // ---- consumables ----------------------------------------------------------
  void mark_lure_replaced();
  void mark_co2_replaced();
  void set_lure_life_days(uint16_t days);
  uint16_t lure_life_days() const;

  // Periodic recompute of derived values (lure timers, online state).
  void update_periodic();

  // Publish everything we know (used after boot and after binding).
  void publish_all();

  // Dump the full slot state to the log at INFO ("Log Debug Info" button).
  void log_debug_info() const;

 protected:
  friend class GoodnatureHub;

  void load_prefs_();
  // flush = also commit NVS now instead of waiting for the periodic sync.
  void save_prefs_(bool flush = false);
  void reset_runtime_state_();
  void set_status_(const std::string &status);
  void set_strikes_(uint32_t strikes, uint32_t strike_epoch, const char *source, const char *event_type = "strike");
  void apply_a24_battery_raw_(uint16_t raw);
  void publish_identity_();
  void publish_counters_();
  void publish_battery_();
  void publish_consumables_();
  void publish_online_();
  void publish_last_seen_();
  void note_strike_epoch_(uint32_t epoch);
  void apply_history_();  // Mouse Trap: turn the replayed log into Strikes / Last Strike

  template<typename T> static void publish_if_changed(T *entity, std::optional<bool> &cache, bool value) {
    if (entity == nullptr)
      return;
    if (!cache.has_value() || *cache != value) {
      cache = value;
      entity->publish_state(value);
    }
  }

  GoodnatureHub *hub_{nullptr};
  uint8_t index_{0};
  bool slot_active_{true};  // registered with Home Assistant (not the trap's armed state)
  char address_str_[18]{};
  Device *device_{nullptr};
  struct SlotButton {
    button::Button *button;
    TrapButtonKind kind;
  };
  std::vector<SlotButton> buttons_;
  // Model the entity set was registered for at boot (UNKNOWN = full set).
  protocol::Model registered_model_{protocol::Model::UNKNOWN};

  ESPPreferenceObject pref_;
  TrapPrefs prefs_{};
  ESPPreferenceObject cache_pref_;
  TrapCache cache_{};
  void load_cache_();
  void save_cache_();
  void restore_from_cache_();

  // Runtime state ------------------------------------------------------------
  uint32_t last_seen_ms_{0};
  uint32_t last_seen_epoch_{0};
  uint32_t last_passive_publish_ms_{0};
  uint32_t last_adv_publish_ms_{0};
  int rssi_{0};
  std::string serial_;
  std::string firmware_;
  std::string status_;

  std::optional<uint16_t> kill_displayed_;
  std::optional<uint16_t> kill_read_;
  std::optional<bool> kill_alert_;
  std::optional<bool> active_;
  std::optional<bool> battery_low_;
  std::optional<bool> charging_;
  std::optional<uint8_t> battery_percent_;
  std::optional<uint16_t> battery_voltage_raw_;
  bool have_device_counter_{false};
  bool have_adv_counter_{false};  // C20: the advertisement carries the total strike count
  bool gatt_dumped_{false};
  // Raw C20 advertisement bytes whose meaning is not fully known; kept for
  // Log Debug Info and logged when they change so they can be decoded.
  uint8_t adv_flags_raw_{0xFF};
  uint8_t adv_battery_state_raw_{0xFF};
  uint16_t adv_word6_raw_{0xFFFF};  // bytes 6..7; documented as strike count, does not match the app

  // Mouse Trap strike accounting. The trap's striker-event log is the only
  // source that matched the app (1 TRIGGER event = 1 kill), so every poll
  // replays the whole log and these accumulate during the job.
  uint32_t hist_trigger_count_{0};
  uint32_t hist_user_count_{0};
  uint32_t hist_last_trigger_epoch_{0};
  bool hist_complete_{false};      // the LogsResponse terminator arrived
  bool replaying_history_{false};  // decoding a nested log event, not a live one
  bool hist_full_{false};          // this replay covers the whole log, not just new events
  uint32_t hist_window_end_{0};    // epoch the current log window ends at
  bool full_rescan_requested_{false};
  uint32_t last_adv_change_poll_ms_{0};

  // A24 passive burst inference (used until the trap counter is read)
  uint32_t burst_started_ms_{0};
  uint32_t last_packet_ms_{0};
  uint8_t packets_in_burst_{0};
  bool activation_counted_{false};
  uint32_t last_activation_ms_{0};
  uint32_t estimated_activations_{0};
  bool activation_needs_poll_{false};

  // Connection bookkeeping
  Job pending_job_{Job::NONE};
  Job active_job_{Job::NONE};
  uint32_t last_poll_ms_{0};
  uint32_t last_poll_epoch_{0};
  uint32_t last_attempt_ms_{0};
  uint8_t consecutive_failures_{0};
  bool ever_polled_{false};

  // Published-state caches (to avoid re-sending unchanged binary/text state)
  std::optional<bool> kill_alert_pub_, battery_low_pub_, charging_pub_, active_pub_, lure_due_pub_, co2_low_pub_,
      online_pub_;
  std::optional<std::string> model_pub_, serial_pub_, firmware_pub_, mac_pub_, status_pub_;
  std::optional<float> strikes_pub_, battery_pub_, co2_pub_, lure_age_pub_, lure_remaining_pub_, last_strike_pub_,
      last_seen_pub_, voltage_pub_;

  // Entities -----------------------------------------------------------------
  sensor::Sensor *strikes_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  sensor::Sensor *battery_voltage_sensor_{nullptr};
  sensor::Sensor *rssi_sensor_{nullptr};
  sensor::Sensor *last_seen_sensor_{nullptr};
  sensor::Sensor *last_strike_sensor_{nullptr};
  sensor::Sensor *lure_age_sensor_{nullptr};
  sensor::Sensor *lure_remaining_sensor_{nullptr};
  sensor::Sensor *co2_remaining_sensor_{nullptr};
  binary_sensor::BinarySensor *kill_alert_bs_{nullptr};
  binary_sensor::BinarySensor *battery_low_bs_{nullptr};
  binary_sensor::BinarySensor *charging_bs_{nullptr};
  binary_sensor::BinarySensor *active_bs_{nullptr};
  binary_sensor::BinarySensor *lure_due_bs_{nullptr};
  binary_sensor::BinarySensor *co2_low_bs_{nullptr};
  binary_sensor::BinarySensor *online_bs_{nullptr};
  text_sensor::TextSensor *model_ts_{nullptr};
  text_sensor::TextSensor *serial_ts_{nullptr};
  text_sensor::TextSensor *firmware_ts_{nullptr};
  text_sensor::TextSensor *mac_ts_{nullptr};
  text_sensor::TextSensor *status_ts_{nullptr};
  text_sensor::TextSensor *last_adv_ts_{nullptr};
  text_sensor::TextSensor *last_frame_ts_{nullptr};
  number::Number *lure_life_number_{nullptr};
  event::Event *strike_event_{nullptr};
};

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

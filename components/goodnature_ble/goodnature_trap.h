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
class ReplacedDate;

// Kinds of per-slot buttons (TrapButton in entities.h). Declared here so the
// trap can leave model-specific ones unregistered.
enum class TrapButtonKind : uint8_t {
  POLL = 0,
  TEST_FIRE,
  RESET_ALERT,
  LURE_REPLACED,
  CO2_REPLACED,
  CO2_SHOT_USED,
  FORGET,
  LOG_DEBUG,
  CLEAR_ERROR,
};

// Work the gateway can do over a GATT connection to a trap.
enum class Job : uint8_t {
  NONE = 0,
  POLL,         // read counters, battery, identity
  TEST_FIRE,    // A24: DE13=0x05 flow. C20: SetCommand(FIRE)
  RESET_ALERT,  // A24: ack read pointers + DE13=0x02. C20: SetCommand(CLEAR)
};

// "poll", "test fire", ... for logs and status text.
const char *job_label(Job job);

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

// An A24's last battery and strike readings, kept in TrapCache where a
// Mouse Trap keeps its log progress, so they are not blank after a gateway
// restart until the trap next wakes (possibly weeks).
struct A24ReadingsCache {
  uint8_t voltage_raw;  // FAD1 or the advertisement; 0 = none
  uint8_t resistance;   // valid with A24_CACHE_RESISTANCE
  int8_t temperature;   // newest kill record's; valid with A24_CACHE_TEMPERATURE
  uint8_t flags;        // A24_CACHE_* bits below
} __attribute__((packed));
static_assert(sizeof(A24ReadingsCache) == sizeof(uint32_t), "shares a u32 in TrapCache");
static constexpr uint8_t A24_CACHE_RESISTANCE = 0x01;
static constexpr uint8_t A24_CACHE_TEMPERATURE = 0x02;
static constexpr uint8_t A24_CACHE_LOW_KNOWN = 0x04;
static constexpr uint8_t A24_CACHE_LOW = 0x08;

// Readings and log-scan progress kept in flash beside TrapPrefs so they
// survive a gateway restart. Kept separate so TrapPrefs keeps its layout
// (ESPHome rejects a stored record whose size changed, which would forget
// every trap on upgrade).
struct TrapCache {
  uint8_t version;          // CACHE_VERSION
  uint8_t battery_percent;  // 0xFF = unknown
  uint8_t flags;            // CACHE_* bits below
  uint8_t co2_extra_shots;  // manual shots not represented by the counter since anchoring
  union {
    uint32_t log_scanned_to;  // C20: striker log counted up to this epoch (0 = never)
    A24ReadingsCache a24;     // A24
  };
  uint32_t last_full_scan;  // C20: epoch of the last replay of the whole log
  union {
    uint32_t test_fires;        // C20: USER striker events seen in the whole log
    uint32_t a24_errors_judged;  // A24: newest error record already judged for Trap Error
  };
  uint32_t kill_history_newest;  // C20: newest KillHistory time already counted (0 = none seen)
} __attribute__((packed));

struct TrapIdentity {
  uint8_t version;
  char serial[16];
  char firmware[24];
} __attribute__((packed));

static constexpr uint8_t CACHE_VERSION = 2;
static constexpr uint8_t CACHE_ACTIVE_KNOWN = 0x01;
static constexpr uint8_t CACHE_ACTIVE = 0x02;
static constexpr uint8_t CACHE_CHARGING_KNOWN = 0x04;
static constexpr uint8_t CACHE_CHARGING = 0x08;
static constexpr uint8_t CACHE_KILL_ALERT_KNOWN = 0x10;
static constexpr uint8_t CACHE_KILL_ALERT = 0x20;
static constexpr uint8_t CACHE_TRAP_ERROR_KNOWN = 0x40;  // A24
static constexpr uint8_t CACHE_TRAP_ERROR = 0x80;

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
  // Renames the device after the bound model, e.g. "Goodnature Mouse Trap 2".
  void name_device(uint8_t number);
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
  void set_battery_volts_sensor(sensor::Sensor *s) { this->battery_volts_sensor_ = s; }
  void set_battery_resistance_sensor(sensor::Sensor *s) { this->battery_resistance_sensor_ = s; }
  void set_strike_temperature_sensor(sensor::Sensor *s) { this->strike_temperature_sensor_ = s; }
  void set_rssi_sensor(sensor::Sensor *s) { this->rssi_sensor_ = s; }
  void set_lure_age_sensor(sensor::Sensor *s) { this->lure_age_sensor_ = s; }
  void set_lure_remaining_sensor(sensor::Sensor *s) { this->lure_remaining_sensor_ = s; }
  void set_co2_remaining_sensor(sensor::Sensor *s) { this->co2_remaining_sensor_ = s; }
  void set_peak_fire_current_sensor(sensor::Sensor *s) { this->peak_fire_sensor_ = s; }
  void set_peak_rewind_current_sensor(sensor::Sensor *s) { this->peak_rewind_sensor_ = s; }
  void set_inner_reading_sensor(sensor::Sensor *s) { this->trigger_sensors_[INNER_READING] = s; }
  void set_inner_ambient_sensor(sensor::Sensor *s) { this->trigger_sensors_[INNER_AMBIENT] = s; }
  void set_inner_threshold_sensor(sensor::Sensor *s) { this->trigger_sensors_[INNER_THRESHOLD] = s; }
  void set_outer_reading_sensor(sensor::Sensor *s) { this->trigger_sensors_[OUTER_READING] = s; }
  void set_outer_ambient_sensor(sensor::Sensor *s) { this->trigger_sensors_[OUTER_AMBIENT] = s; }
  void set_outer_threshold_sensor(sensor::Sensor *s) { this->trigger_sensors_[OUTER_THRESHOLD] = s; }

  void set_kill_alert_binary_sensor(binary_sensor::BinarySensor *s) { this->kill_alert_bs_ = s; }
  void set_battery_low_binary_sensor(binary_sensor::BinarySensor *s) { this->battery_low_bs_ = s; }
  void set_charging_binary_sensor(binary_sensor::BinarySensor *s) { this->charging_bs_ = s; }
  void set_active_binary_sensor(binary_sensor::BinarySensor *s) { this->active_bs_ = s; }
  void set_lure_due_binary_sensor(binary_sensor::BinarySensor *s) { this->lure_due_bs_ = s; }
  void set_co2_low_binary_sensor(binary_sensor::BinarySensor *s) { this->co2_low_bs_ = s; }
  void set_online_binary_sensor(binary_sensor::BinarySensor *s) { this->online_bs_ = s; }
  void set_battery_recovery_binary_sensor(binary_sensor::BinarySensor *s) { this->battery_recovery_bs_ = s; }
  void set_error_binary_sensor(binary_sensor::BinarySensor *s) { this->error_bs_ = s; }

  void set_model_text_sensor(text_sensor::TextSensor *s) { this->model_ts_ = s; }
  void set_battery_status_text_sensor(text_sensor::TextSensor *s) { this->battery_status_ts_ = s; }
  void set_serial_text_sensor(text_sensor::TextSensor *s) { this->serial_ts_ = s; }
  void set_firmware_text_sensor(text_sensor::TextSensor *s) { this->firmware_ts_ = s; }
  void set_bootloader_text_sensor(text_sensor::TextSensor *s) { this->bootloader_ts_ = s; }
  void set_mac_text_sensor(text_sensor::TextSensor *s) { this->mac_ts_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_ts_ = s; }
  void set_last_advertisement_text_sensor(text_sensor::TextSensor *s) { this->last_adv_ts_ = s; }
  void set_last_frame_text_sensor(text_sensor::TextSensor *s) { this->last_frame_ts_ = s; }
  void set_error_data_text_sensor(text_sensor::TextSensor *s) { this->error_data_ts_ = s; }
  void set_trap_state_text_sensor(text_sensor::TextSensor *s) { this->trap_state_ts_ = s; }
  // Timestamps as ISO 8601 text: a float sensor rounds epoch seconds to 128 s.
  void set_last_seen_text_sensor(text_sensor::TextSensor *s) { this->last_seen_ts_ = s; }
  void set_last_strike_text_sensor(text_sensor::TextSensor *s) { this->last_strike_ts_ = s; }
  void set_lure_date(ReplacedDate *d) { this->lure_date_ = d; }
  void set_co2_date(ReplacedDate *d) { this->co2_date_ = d; }

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
  // A button press is the user asking now: skip the retry backoff and give
  // the action its own attempts before it is dropped.
  void clear_backoff() { this->consecutive_failures_ = 0; }
  // Clear Kill Alert. Mouse Trap: SetCommand(CLEAR). A24: the alert is the
  // gateway's own (the trap is acknowledged in every session, like the app
  // does), so this only clears it, at once and without the trap.
  void clear_kill_alert();
  // A24: turn Trap Error off; it stays on until this is pressed.
  void clear_trap_error();
  // Why a test fire must not be sent now (the app's conditions for a Mouse
  // Trap: state known, no error, off USB, no kill waiting, tray closed), or
  // nullptr when it may.
  const char *test_fire_blocked() const;
  // Test Fire button: queues the job, or reports in Status why not.
  void request_test_fire();
  Job pending_job() const { return this->pending_job_; }
  // Next poll replays the whole striker log instead of only what is new.
  void request_full_rescan() { this->full_rescan_requested_ = true; }
  // Mouse Trap firmware after 0.2.550 keeps its last ten kill times
  // (KillHistory), which is what the app counts; older firmware is counted
  // from the striker log. Unknown firmware is assumed to be current.
  bool uses_kill_history() const;
  // Called by the connection when it reaches the logs request, after this
  // session's KillHistory has arrived: whether to ask for the log, and the
  // window start and flags. A KillHistory trap replays the whole log only
  // for the lifetime total (when unknown, and on Poll Now), and otherwise
  // asks only around new kills, for their striker and trigger details.
  bool plan_striker_log(uint32_t now_epoch, uint32_t &start, uint32_t &flags);
  void clear_pending_job() { this->pending_job_ = Job::NONE; }
  // An optional request (one older firmware may ignore) went unanswered;
  // it is not sent to this trap again until its firmware changes.
  void on_optional_unanswered(uint8_t type);
  bool skips_optional(uint8_t type) const;

  // True when the hub should connect to this trap next time it is heard.
  bool wants_connection(uint32_t now_ms) const;

  // Results delivered by GoodnatureConnection.
  void apply_a24_characteristic(uint16_t short_id, const uint8_t *data, size_t len);
  void apply_c20_frame(const protocol::C20Frame &frame);
  void on_job_started(Job job);
  // The trap acknowledged the job's command write (test fire, clear).
  void on_command_delivered(Job job);
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
  // From the Replaced On date pickers; epoch seconds.
  void set_lure_replaced(uint32_t epoch);
  void set_co2_replaced(uint32_t epoch);
  void mark_co2_shot_used();
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
  // Mouse Trap: count KillHistory entries newer than any seen before. After
  // a full log replay the entries are already in the count.
  void apply_kill_history_(bool replayed);
  // Start of the striker-log window for a poll ending at now_epoch, and
  // whether that window is a full replay (hist_full_).
  uint32_t log_window_start_(uint32_t now_epoch);
  // Mouse Trap: keep the newest striker / trigger event seen this session
  // for the motor-current and sensor-reading entities.
  void note_striker_details_(const protocol::C20StrikerEvent &e);
  void note_trigger_details_(const protocol::C20TriggerEvent &t);
  void publish_details_();
  void apply_a24_error_counter_(uint16_t short_id, const uint8_t *data, size_t len);
  void judge_a24_error_(const uint8_t *data, size_t len);
  void apply_a24_advertisement_(const protocol::A24Advertisement &adv);
  // A24: turn Kill Alert on until Clear Kill Alert.
  void raise_kill_alert_(const char *why);
  // Mouse Trap status the way the app ranks it: "Error", "Charging",
  // "Charged", "New strike", "Tray open" or "Set"; nullptr before a poll.
  const char *c20_trap_state_() const;
  std::optional<bool> battery_low_now_() const;
  std::string battery_status_text_() const;
  void publish_dates_();
  void publish_error_();

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
  char device_name_[40]{};  // Device keeps only the pointer
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
  ESPPreferenceObject identity_pref_;
  void load_cache_();
  void save_cache_();
  void load_identity_();
  void save_identity_();
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
  std::optional<bool> adv_low_battery_;  // the advertisement's low-battery bit (both models)
  std::optional<bool> charging_;
  std::optional<bool> usb_connected_;  // C20, from the device state
  std::optional<uint8_t> battery_percent_;
  std::optional<uint16_t> battery_voltage_raw_;  // A24: FAD1 or the advertisement
  std::optional<uint8_t> battery_resistance_;    // A24: FAD2 or the advertisement
  std::optional<int8_t> strike_temperature_;     // A24: from the newest kill record
  std::optional<bool> error_flag_;               // A24: the advertisement's new-error bit
  std::optional<bool> trap_error_;               // A24: latched from error records, until Clear Trap Error
  std::optional<protocol::C20DeviceState> device_state_;  // C20
  std::optional<bool> tray_open_;                         // C20
  std::optional<int16_t> battery_mv_;  // C20, from InputStates
  std::string bootloader_;             // C20, re-read every poll
  bool have_device_counter_{false};
  bool have_adv_counter_{false};  // C20: the advertisement carries the total strike count
  bool gatt_dumped_{false};
  // Raw C20 advertisement bytes whose meaning is not fully known; kept for
  // Log Debug Info and logged when they change so they can be decoded.
  uint8_t adv_flags_raw_{0xFF};
  uint8_t adv_battery_state_raw_{0xFF};
  uint16_t adv_word6_raw_{0xFFFF};  // bytes 6..7; documented as strike count, does not match the app
  std::optional<bool> adv_alert_bit_;  // flag 0x02 in the last advertisement

  // Mouse Trap striker-log replay (1 TRIGGER event = 1 kill, matching the
  // app). Used for the lifetime total and on older firmware; these
  // accumulate during the job.
  uint32_t hist_trigger_count_{0};
  uint32_t hist_user_count_{0};
  uint32_t hist_last_trigger_epoch_{0};
  bool hist_complete_{false};      // the LogsResponse terminator arrived
  bool replaying_history_{false};  // decoding a nested log event, not a live one
  bool hist_full_{false};          // this replay covers the whole log, not just new events
  bool hist_requested_{false};     // this session asked for the log at all
  uint32_t hist_window_end_{0};    // epoch the current log window ends at
  bool full_rescan_requested_{false};
  uint32_t last_adv_change_poll_ms_{0};
  protocol::C20KillHistory kill_history_{};  // from the current session
  bool kill_history_received_{false};
  bool hist_details_only_{false};  // this window is for new kills' details; KillHistory does the counting
  std::vector<uint8_t> unanswered_types_;  // optional C20 requests this firmware ignored

  // Newest striker and trigger events seen since boot (Mouse Trap)
  std::optional<protocol::C20StrikerEvent> last_striker_;
  std::optional<protocol::C20TriggerEvent> last_trigger_;

  // A24 error channel (E771 / E773 counters, E772 record)
  std::optional<uint16_t> error_displayed_;
  std::optional<uint16_t> error_read_;
  std::string error_data_;

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
  bool command_delivered_{false};  // during / after active_job_
  uint32_t last_poll_ms_{0};
  uint32_t last_poll_epoch_{0};
  uint32_t last_attempt_ms_{0};
  uint8_t consecutive_failures_{0};
  bool ever_polled_{false};

  // Published-state caches (to avoid re-sending unchanged binary/text state)
  std::optional<bool> kill_alert_pub_, battery_low_pub_, charging_pub_, active_pub_, lure_due_pub_, co2_low_pub_,
      online_pub_, battery_recovery_pub_, error_pub_;
  std::optional<std::string> model_pub_, battery_status_pub_, serial_pub_, firmware_pub_, bootloader_pub_, mac_pub_,
      status_pub_, error_data_pub_, trap_state_pub_, last_strike_pub_, last_seen_pub_;
  std::optional<float> strikes_pub_, battery_pub_, co2_pub_, lure_age_pub_, lure_remaining_pub_, volts_pub_,
      resistance_pub_, temperature_pub_, peak_fire_pub_, peak_rewind_pub_;

  // Latest trigger-event readings (Mouse Trap)
  enum TriggerReading : uint8_t {
    INNER_READING,
    INNER_AMBIENT,
    INNER_THRESHOLD,
    OUTER_READING,
    OUTER_AMBIENT,
    OUTER_THRESHOLD,
    TRIGGER_READINGS,
  };
  sensor::Sensor *trigger_sensors_[TRIGGER_READINGS]{};
  std::optional<float> trigger_pub_[TRIGGER_READINGS];

  // Entities -----------------------------------------------------------------
  sensor::Sensor *strikes_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  sensor::Sensor *battery_volts_sensor_{nullptr};
  sensor::Sensor *battery_resistance_sensor_{nullptr};
  sensor::Sensor *strike_temperature_sensor_{nullptr};
  sensor::Sensor *rssi_sensor_{nullptr};
  sensor::Sensor *lure_age_sensor_{nullptr};
  sensor::Sensor *lure_remaining_sensor_{nullptr};
  sensor::Sensor *co2_remaining_sensor_{nullptr};
  sensor::Sensor *peak_fire_sensor_{nullptr};
  sensor::Sensor *peak_rewind_sensor_{nullptr};
  binary_sensor::BinarySensor *kill_alert_bs_{nullptr};
  binary_sensor::BinarySensor *battery_low_bs_{nullptr};
  binary_sensor::BinarySensor *charging_bs_{nullptr};
  binary_sensor::BinarySensor *active_bs_{nullptr};
  binary_sensor::BinarySensor *lure_due_bs_{nullptr};
  binary_sensor::BinarySensor *co2_low_bs_{nullptr};
  binary_sensor::BinarySensor *online_bs_{nullptr};
  binary_sensor::BinarySensor *battery_recovery_bs_{nullptr};
  binary_sensor::BinarySensor *error_bs_{nullptr};
  text_sensor::TextSensor *model_ts_{nullptr};
  text_sensor::TextSensor *battery_status_ts_{nullptr};
  text_sensor::TextSensor *serial_ts_{nullptr};
  text_sensor::TextSensor *firmware_ts_{nullptr};
  text_sensor::TextSensor *bootloader_ts_{nullptr};
  text_sensor::TextSensor *mac_ts_{nullptr};
  text_sensor::TextSensor *status_ts_{nullptr};
  text_sensor::TextSensor *last_adv_ts_{nullptr};
  text_sensor::TextSensor *last_frame_ts_{nullptr};
  text_sensor::TextSensor *error_data_ts_{nullptr};
  text_sensor::TextSensor *trap_state_ts_{nullptr};
  text_sensor::TextSensor *last_seen_ts_{nullptr};
  text_sensor::TextSensor *last_strike_ts_{nullptr};
  ReplacedDate *lure_date_{nullptr};
  ReplacedDate *co2_date_{nullptr};
  number::Number *lure_life_number_{nullptr};
  event::Event *strike_event_{nullptr};
};

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

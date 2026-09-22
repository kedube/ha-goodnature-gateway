#pragma once

#ifdef USE_ESP32

#include "goodnature_connection.h"
#include "goodnature_trap.h"
#include "protocol.h"

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#include <vector>

namespace esphome::goodnature_ble {

// The gateway: listens to every BLE advertisement, recognises Goodnature
// traps, assigns each new trap to a free slot (a Home Assistant
// sub-device), keeps passive state up to date, and schedules GATT polls
// through the single shared GoodnatureConnection.
class GoodnatureHub : public Component, public esp32_ble_tracker::ESPBTDeviceListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;

  // ---- configuration ---------------------------------------------------------
  void add_trap(GoodnatureTrap *trap);
  void set_connection(GoodnatureConnection *conn) {
    this->connection_ = conn;
    conn->set_hub(this);
  }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif
  void set_poll_interval(uint32_t ms) { this->poll_interval_ms_ = ms; }
  void set_lure_life_days(uint16_t days) { this->lure_life_days_ = days; }
  void set_co2_capacity(uint16_t shots) { this->co2_capacity_ = shots; }
  void set_co2_low_threshold(uint16_t shots) { this->co2_low_threshold_ = shots; }
  void set_offline_timeout_a24(uint32_t ms) { this->offline_timeout_a24_ms_ = ms; }
  void set_offline_timeout_c20(uint32_t ms) { this->offline_timeout_c20_ms_ = ms; }
  void set_write_time_on_connect(bool v) { this->write_time_on_connect_ = v; }
  void set_discovery_default(bool v) { this->discovery_enabled_ = v; }
  void set_auto_acknowledge(bool v) { this->auto_acknowledge_ = v; }
  void set_a24_battery_calibration(uint16_t empty_raw, uint16_t full_raw, uint8_t low_percent) {
    this->a24_battery_empty_raw_ = empty_raw;
    this->a24_battery_full_raw_ = full_raw;
    this->a24_battery_low_percent_ = low_percent;
  }

  void set_traps_count_sensor(sensor::Sensor *s) { this->traps_count_sensor_ = s; }
  void set_last_discovered_text_sensor(text_sensor::TextSensor *s) { this->last_discovered_ts_ = s; }
  void set_discovery_switch(switch_::Switch *s) { this->discovery_switch_ = s; }
  void set_debug_switch(switch_::Switch *s) { this->debug_switch_ = s; }

  // ---- runtime API used by traps, buttons, connection -----------------------------
  uint32_t poll_interval_ms() const { return this->poll_interval_ms_; }
  uint16_t default_lure_life_days() const { return this->lure_life_days_; }
  uint16_t co2_capacity() const { return this->co2_capacity_; }
  uint16_t co2_low_threshold() const { return this->co2_low_threshold_; }
  uint32_t offline_timeout_ms(protocol::Model model) const {
    return model == protocol::Model::C20 ? this->offline_timeout_c20_ms_ : this->offline_timeout_a24_ms_;
  }
  bool write_time_on_connect() const { return this->write_time_on_connect_; }
  bool auto_acknowledge() const { return this->auto_acknowledge_; }
  uint16_t a24_battery_empty_raw() const { return this->a24_battery_empty_raw_; }
  uint16_t a24_battery_full_raw() const { return this->a24_battery_full_raw_; }
  uint8_t a24_battery_low_percent() const { return this->a24_battery_low_percent_; }

  // Current UTC epoch seconds, or 0 when no valid time source is available.
  uint32_t now_epoch() const;
  bool time_valid() const { return this->now_epoch() != 0; }

  bool discovery_enabled() const { return this->discovery_enabled_; }
  void set_discovery_enabled(bool enabled);

  // Slots: max_slots() are compiled in; active_slots() of them are registered
  // with Home Assistant, namely every slot up to the highest one holding a
  // trap (at least one). The count is fixed at boot, so binding a trap into
  // an unregistered slot, or forgetting the last one, restarts the gateway.
  uint8_t max_slots() const { return static_cast<uint8_t>(this->traps_.size()); }
  uint8_t active_slots() const { return this->active_slots_; }
  // Debug mode: log raw advertisements from anything Goodnature-like, dump
  // GATT tables on every connection, publish raw capture text sensors.
  bool debug_enabled() const { return this->debug_enabled_; }
  void set_debug_enabled(bool enabled);

  // The App's device and entity tables are fixed at boot. When a binding
  // changes what they should hold (a slot came into or out of use, or a slot
  // now holds a different model), restart shortly to rebuild them. Re-checks
  // before rebooting, so a change that is undone in time costs nothing.
  void schedule_restart_if_needed();

  void request_job(GoodnatureTrap *trap, Job job);
  void forget_trap(GoodnatureTrap *trap);
  void forget_all_traps();
  void on_job_finished(GoodnatureTrap *trap, Job job, bool ok);

 protected:
  struct Classification {
    bool is_goodnature{false};
    protocol::Model model{protocol::Model::UNKNOWN};
    bool has_c20_adv{false};
    protocol::C20Advertisement c20_adv{};
  };

  Classification classify_(const esp32_ble_tracker::ESPBTDevice &device) const;
  // True for advertisements worth logging in debug mode even when not
  // classified as Goodnature: Nordic manufacturer data or vendor UUIDs.
  static bool looks_interesting_(const esp32_ble_tracker::ESPBTDevice &device);
  void log_advertisement_(const esp32_ble_tracker::ESPBTDevice &device, const char *why);
  GoodnatureTrap *find_trap_(uint64_t address) const;
  GoodnatureTrap *find_trap_by_serial_(uint32_t serial_raw) const;
  GoodnatureTrap *find_free_slot_() const;
  void maybe_connect_(GoodnatureTrap *trap, uint32_t now_ms);
  void publish_hub_state_();
  size_t bound_count_() const;
  // Slot count the current bindings call for: the highest bound slot, at least 1.
  uint8_t wanted_slots_() const;
  // Why the App tables no longer match the bindings, or nullptr when they do.
  const char *restart_reason_() const;

  std::vector<GoodnatureTrap *> traps_;
  GoodnatureConnection *connection_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif

  uint32_t poll_interval_ms_{15 * 60 * 1000};
  uint16_t lure_life_days_{180};
  uint16_t co2_capacity_{24};
  uint16_t co2_low_threshold_{4};
  uint32_t offline_timeout_a24_ms_{24UL * 60 * 60 * 1000};
  uint32_t offline_timeout_c20_ms_{15UL * 60 * 1000};
  bool write_time_on_connect_{true};
  bool discovery_enabled_{true};
  bool debug_enabled_{false};
  bool auto_acknowledge_{false};
  uint16_t a24_battery_empty_raw_{0};
  uint16_t a24_battery_full_raw_{0};
  uint8_t a24_battery_low_percent_{15};

  // Per-address rate limit for debug advertisement logging.
  struct AdvLogEntry {
    uint64_t address;
    uint32_t last_ms;
  };
  AdvLogEntry adv_log_[8]{};
  uint8_t adv_log_next_{0};

  uint32_t last_periodic_ms_{0};
  uint32_t last_tick_ms_{0};

  uint8_t active_slots_{0};

  sensor::Sensor *traps_count_sensor_{nullptr};
  text_sensor::TextSensor *last_discovered_ts_{nullptr};
  switch_::Switch *discovery_switch_{nullptr};
  switch_::Switch *debug_switch_{nullptr};
};

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

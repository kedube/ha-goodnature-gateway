#include "goodnature_hub.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::goodnature_ble {

static const char *const TAG = "goodnature_ble";

using namespace protocol;
namespace espbt = esphome::esp32_ble_tracker;

static const uint32_t PERIODIC_INTERVAL_MS = 60 * 1000;
// A trap heard this recently can be connected to straight away.
static const uint32_t RECENTLY_SEEN_MS = 15 * 1000;
// Delay before the restart that applies a changed slot count. Long enough
// for the discovery log line and sensor updates to reach Home Assistant, and
// for a trap that was just forgotten to rebind (which cancels the restart).
static const uint32_t SLOTS_RESTART_DELAY_MS = 5000;

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void GoodnatureHub::add_trap(GoodnatureTrap *trap) {
  trap->set_hub(this);
  this->traps_.push_back(trap);
}

void GoodnatureHub::setup() {
  // Decide which slots are visible before anything can list entities: this
  // runs at AFTER_BLUETOOTH, ahead of the API server and web server. The
  // count follows the bindings in flash: every slot up to the highest one
  // holding a trap, and at least one so the gateway shows what it expects.
  for (auto *trap : this->traps_)
    trap->init_prefs();
  uint8_t wanted = this->wanted_slots_();
  this->active_slots_ = wanted;
  for (auto *trap : this->traps_) {
    bool active = trap->index() < wanted;
    trap->set_active(active);
    if (active)
      trap->register_with_app();
  }
#ifdef USE_DEVICES
  // The API's DeviceInfoResponse is a fixed array of ESPHOME_DEVICE_COUNT
  // entries and encodes all of them, so every unregistered slot would reach
  // Home Assistant as a sub-device with id 0 and no name, which it renders as
  // a second copy of the gateway. Fill the unused entries with the first
  // active slot's device; identical identifiers collapse into one device.
  Device *pad = this->traps_.empty() ? nullptr : this->traps_.front()->device();
  if (pad != nullptr) {
    for (uint8_t i = wanted; i < this->max_slots(); i++)
      App.register_device(pad);
  }
#endif

  if (this->discovery_switch_ != nullptr) {
    auto initial = this->discovery_switch_->get_initial_state_with_restore_mode();
    if (initial.has_value())
      this->discovery_enabled_ = *initial;
    this->discovery_switch_->publish_state(this->discovery_enabled_);
  }
  if (this->debug_switch_ != nullptr) {
    auto initial = this->debug_switch_->get_initial_state_with_restore_mode();
    if (initial.has_value())
      this->debug_enabled_ = *initial;
    this->debug_switch_->publish_state(this->debug_enabled_);
  }
  for (auto *trap : this->traps_)
    trap->setup();
  this->publish_hub_state_();
}

void GoodnatureHub::loop() {
  uint32_t now = millis();
  if (this->connection_ != nullptr)
    this->connection_->tick(now);
  if (this->last_periodic_ms_ == 0 || now - this->last_periodic_ms_ >= PERIODIC_INTERVAL_MS) {
    this->last_periodic_ms_ = now;
    for (auto *trap : this->traps_)
      trap->update_periodic();
  }
}

void GoodnatureHub::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Goodnature BLE gateway:\n"
                "  Slots: %u registered of %u compiled (%u assigned)\n"
                "  Discovery: %s\n"
                "  Debug mode: %s\n"
                "  Auto acknowledge: %s\n"
                "  Poll interval: %u s\n"
                "  Lure life: %u days\n"
                "  CO2 canister: %u shots (low at %u)\n"
                "  Time source: %s",
                (unsigned) this->active_slots_, (unsigned) this->traps_.size(), (unsigned) this->bound_count_(),
                ONOFF(this->discovery_enabled_),
                ONOFF(this->debug_enabled_), ONOFF(this->auto_acknowledge_), (unsigned) (this->poll_interval_ms_ / 1000), this->lure_life_days_, this->co2_capacity_, this->co2_low_threshold_,
                this->time_valid() ? "valid" : "not available");
  for (auto *trap : this->traps_) {
    if (!trap->is_active()) {
      ESP_LOGCONFIG(TAG, "  Slot %u: hidden", trap->index() + 1);
    } else if (trap->is_bound()) {
      ESP_LOGCONFIG(TAG, "  Slot %u: %s %s", trap->index() + 1, model_name(trap->model()), trap->address_str());
    } else {
      ESP_LOGCONFIG(TAG, "  Slot %u: free", trap->index() + 1);
    }
  }
}

uint32_t GoodnatureHub::now_epoch() const {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    time_t t = this->time_->timestamp_now();
    if (t > 1600000000)  // September 2020: anything earlier means the clock is not set
      return static_cast<uint32_t>(t);
  }
#endif
  return 0;
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

GoodnatureHub::Classification GoodnatureHub::classify_(const espbt::ESPBTDevice &device) const {
  using esp32_ble::ESPBTUUID;
  static const ESPBTUUID GN_D00D = ESPBTUUID::from_raw(gn_uuid_str(SVC_KILL));
  static const ESPBTUUID GN_D2ED = ESPBTUUID::from_raw(gn_uuid_str(CHR_EVENT_DISPLAYED));
  static const ESPBTUUID GN_DE11 = ESPBTUUID::from_raw(gn_uuid_str(SVC_DEVICE));
  static const ESPBTUUID GN_FADE = ESPBTUUID::from_raw(gn_uuid_str(SVC_BATTERY));
  static const ESPBTUUID GN_E010 = ESPBTUUID::from_raw(gn_uuid_str(SVC_UNKNOWN_E010));
  static const ESPBTUUID LEGACY_1234 = ESPBTUUID::from_uint16(SVC16_LEGACY_1234);
  static const ESPBTUUID LEGACY_600D = ESPBTUUID::from_uint16(SVC16_LEGACY_600D);
  static const ESPBTUUID NUS = ESPBTUUID::from_raw(NUS_SERVICE_UUID);
  static const ESPBTUUID MEMFAULT = ESPBTUUID::from_raw(MEMFAULT_SERVICE_UUID);
  static const ESPBTUUID NORDIC = ESPBTUUID::from_uint16(MFG_ID_NORDIC);

  Classification c;
  bool has_a24_marker = false, has_gn_marker = false, has_600d = false, has_c20_marker = false;
  for (const auto &uuid : device.get_service_uuids()) {
    if (uuid == GN_D00D || uuid == GN_D2ED) {
      has_a24_marker = true;
      has_gn_marker = true;
    } else if (uuid == GN_DE11 || uuid == GN_FADE || uuid == LEGACY_1234) {
      has_gn_marker = true;
    } else if (uuid == GN_E010) {
      has_gn_marker = true;
      has_c20_marker = true;
    } else if (uuid == LEGACY_600D) {
      has_600d = true;
      has_gn_marker = true;
    } else if (uuid == NUS || uuid == MEMFAULT) {
      has_c20_marker = true;
    }
  }
  bool name_is_gn = device.get_name() == "GN";
  c.is_goodnature = name_is_gn || has_gn_marker;
  if (!c.is_goodnature)
    return c;

  for (const auto &md : device.get_manufacturer_datas()) {
    if (md.uuid == NORDIC && md.data.size() == 9) {
      auto adv = parse_c20_advertisement(md.data.data(), md.data.size());
      if (adv.has_value()) {
        c.has_c20_adv = true;
        c.c20_adv = *adv;
      }
    }
  }

  if (has_c20_marker) {
    c.model = Model::C20;
  } else if (has_a24_marker) {
    c.model = Model::A24;
  } else if (c.has_c20_adv && has_600d) {
    c.model = Model::C20;
  }
  return c;
}

bool GoodnatureHub::looks_interesting_(const espbt::ESPBTDevice &device) {
  if (device.get_name() == "GN")
    return true;
  for (const auto &md : device.get_manufacturer_datas()) {
    if (md.uuid == esp32_ble::ESPBTUUID::from_uint16(MFG_ID_NORDIC))
      return true;
  }
  for (const auto &uuid : device.get_service_uuids()) {
    if (uuid.get_uuid().len == ESP_UUID_LEN_128)
      return true;
  }
  return false;
}

void GoodnatureHub::log_advertisement_(const espbt::ESPBTDevice &device, const char *why) {
  // Rate limit per address so a chatty C20 does not flood the log.
  uint64_t address = device.address_uint64();
  uint32_t now = millis();
  for (auto &entry : this->adv_log_) {
    if (entry.address == address) {
      if (now - entry.last_ms < 10000)
        return;
      entry.last_ms = now;
      goto log;
    }
  }
  this->adv_log_[this->adv_log_next_] = AdvLogEntry{address, now};
  this->adv_log_next_ = (this->adv_log_next_ + 1) % 8;

log:
  char addr_buf[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  device.address_str_to(addr_buf);
  const auto &sr = device.get_scan_result();
  char uuid_buf[esp32_ble::UUID_STR_LEN];
  std::string uuids;
  for (const auto &uuid : device.get_service_uuids()) {
    if (!uuids.empty())
      uuids += ",";
    uuids += uuid.to_str(uuid_buf);
  }
  std::string mfg;
  for (const auto &md : device.get_manufacturer_datas()) {
    if (!mfg.empty())
      mfg += ";";
    mfg += md.uuid.to_str(uuid_buf);
    mfg += ":";
    mfg += to_hex(md.data.data(), md.data.size());
  }
  ESP_LOGI(TAG,
           "ADV %s [%s] rssi=%d type=%u name='%s'\n"
           "  services=[%s]\n"
           "  mfg=[%s]\n"
           "  raw adv=%s scan_rsp=%s",
           why, addr_buf, device.get_rssi(), (unsigned) device.get_address_type(), device.get_name().c_str(),
           uuids.c_str(), mfg.c_str(), to_hex(sr.ble_adv, sr.adv_data_len).c_str(),
           to_hex(sr.ble_adv + sr.adv_data_len, sr.scan_rsp_len).c_str());
}

GoodnatureTrap *GoodnatureHub::find_trap_by_serial_(uint32_t serial_raw) const {
  if (serial_raw == 0)
    return nullptr;
  for (auto *trap : this->traps_) {
    if (trap->is_bound() && trap->serial_raw() == serial_raw)
      return trap;
  }
  return nullptr;
}

GoodnatureTrap *GoodnatureHub::find_trap_(uint64_t address) const {
  for (auto *trap : this->traps_) {
    if (trap->is_bound() && trap->address() == address)
      return trap;
  }
  return nullptr;
}

GoodnatureTrap *GoodnatureHub::find_free_slot_() const {
  for (auto *trap : this->traps_) {
    if (!trap->is_bound())
      return trap;
  }
  return nullptr;
}

uint8_t GoodnatureHub::wanted_slots_() const {
  uint8_t highest = 0;
  for (auto *trap : this->traps_) {
    if (trap->is_bound())
      highest = std::max<uint8_t>(highest, trap->index() + 1);
  }
  return std::max<uint8_t>(highest, 1);
}

const char *GoodnatureHub::restart_reason_() const {
  if (this->wanted_slots_() != this->active_slots_)
    return "the number of trap devices changed";
  for (auto *trap : this->traps_) {
    if (trap->needs_reregister())
      return "a slot holds a different trap model";
  }
  return nullptr;
}

void GoodnatureHub::schedule_restart_if_needed() {
  const char *reason = this->restart_reason_();
  if (reason == nullptr) {
    this->cancel_timeout("slots_restart");
    return;
  }
  ESP_LOGI(TAG, "Restarting in %u s so Home Assistant sees the new entity list: %s",
           (unsigned) (SLOTS_RESTART_DELAY_MS / 1000), reason);
  this->set_timeout("slots_restart", SLOTS_RESTART_DELAY_MS, [this]() {
    if (this->restart_reason_() != nullptr)
      App.safe_reboot();
  });
}

size_t GoodnatureHub::bound_count_() const {
  size_t n = 0;
  for (auto *trap : this->traps_)
    if (trap->is_bound())
      n++;
  return n;
}

bool GoodnatureHub::parse_device(const espbt::ESPBTDevice &device) {
  Classification c = this->classify_(device);
  if (!c.is_goodnature) {
    if (this->debug_enabled_ && looks_interesting_(device))
      this->log_advertisement_(device, "unclassified");
    return false;
  }
  if (this->debug_enabled_)
    this->log_advertisement_(device, model_name(c.model));

  uint64_t address = device.address_uint64();
  GoodnatureTrap *trap = this->find_trap_(address);
  if (trap == nullptr && c.has_c20_adv) {
    // A C20 identifies itself by serial; follow it if its address changed.
    trap = this->find_trap_by_serial_(c.c20_adv.serial_number_raw);
    if (trap != nullptr)
      trap->rebind_address(address, device.get_address_type());
  }
  if (trap == nullptr) {
    char addr_buf[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    device.address_str_to(addr_buf);
    if (!this->discovery_enabled_) {
      ESP_LOGV(TAG, "Ignoring unassigned Goodnature trap %s (discovery off)", addr_buf);
      return true;
    }
    trap = this->find_free_slot_();
    if (trap == nullptr) {
      static uint32_t last_warning = 0;
      uint32_t now = millis();
      if (last_warning == 0 || now - last_warning > 60000) {
        last_warning = now;
        ESP_LOGW(TAG, "Goodnature trap %s found but all %u slots are in use; raise max_traps and reflash, or forget a trap",
                 addr_buf, (unsigned) this->traps_.size());
      }
      return true;
    }
    trap->bind(address, device.get_address_type(), c.model, c.has_c20_adv ? c.c20_adv.serial_number_raw : 0);
    ESP_LOGI(TAG, "Discovered Goodnature %s at %s (RSSI %d), assigned to slot %u", model_name(c.model),
             trap->address_str(), device.get_rssi(), trap->index() + 1);
    if (this->last_discovered_ts_ != nullptr)
      this->last_discovered_ts_->publish_state(trap->address_str());
    this->publish_hub_state_();
    // A slot that was not registered at boot has no device in Home Assistant
    // yet, and one registered for another model has the wrong entities.
    this->schedule_restart_if_needed();
  }

  trap->on_advertisement(device, c.model, c.has_c20_adv ? &c.c20_adv : nullptr);
  this->maybe_connect_(trap, millis());
  return true;
}

void GoodnatureHub::maybe_connect_(GoodnatureTrap *trap, uint32_t now_ms) {
  if (this->connection_ == nullptr || !this->connection_->is_idle())
    return;
  if (!trap->wants_connection(now_ms))
    return;
  Job job = trap->pending_job() != Job::NONE ? trap->pending_job() : Job::POLL;
  this->connection_->start(trap, job);
}

// ---------------------------------------------------------------------------
// Runtime API
// ---------------------------------------------------------------------------

void GoodnatureHub::set_discovery_enabled(bool enabled) {
  if (this->discovery_enabled_ == enabled)
    return;
  this->discovery_enabled_ = enabled;
  ESP_LOGI(TAG, "Trap discovery %s", ONOFF(enabled));
  if (this->discovery_switch_ != nullptr)
    this->discovery_switch_->publish_state(enabled);
}

void GoodnatureHub::set_debug_enabled(bool enabled) {
  if (this->debug_enabled_ == enabled)
    return;
  this->debug_enabled_ = enabled;
  ESP_LOGI(TAG, "Debug mode %s", ONOFF(enabled));
  if (this->debug_switch_ != nullptr)
    this->debug_switch_->publish_state(enabled);
}

void GoodnatureHub::request_job(GoodnatureTrap *trap, Job job) {
  trap->request_job(job);
  if (!trap->is_bound())
    return;
  uint32_t now = millis();
  // If the trap was heard a moment ago it is probably still awake; try now.
  // Otherwise the next advertisement triggers the connection.
  if (trap->last_seen_ms() != 0 && now - trap->last_seen_ms() < RECENTLY_SEEN_MS) {
    this->maybe_connect_(trap, now);
  } else {
    ESP_LOGI(TAG, "[%s] %s queued; will connect when the trap next advertises", trap->address_str(),
             job == Job::TEST_FIRE ? "test fire" : (job == Job::RESET_ALERT ? "reset alert" : "poll"));
  }
}

void GoodnatureHub::forget_trap(GoodnatureTrap *trap) {
  if (this->connection_ != nullptr && this->connection_->current_trap() == trap)
    this->connection_->abort("trap forgotten");
  if (trap->is_bound())
    trap->unbind();
  this->publish_hub_state_();
  // Forgetting the highest slot leaves an empty device registered; drop it.
  this->schedule_restart_if_needed();
}

void GoodnatureHub::forget_all_traps() {
  ESP_LOGW(TAG, "Forgetting all traps");
  for (auto *trap : this->traps_)
    this->forget_trap(trap);
}

void GoodnatureHub::on_job_finished(GoodnatureTrap *trap, Job job, bool ok) {
  (void) trap;
  (void) job;
  (void) ok;
  // Another trap may already be waiting; it will be picked up on its next advertisement.
}

void GoodnatureHub::publish_hub_state_() {
  if (this->traps_count_sensor_ != nullptr)
    this->traps_count_sensor_->publish_state(static_cast<float>(this->bound_count_()));
}

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

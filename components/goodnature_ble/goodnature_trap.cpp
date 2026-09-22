#include "goodnature_trap.h"

#ifdef USE_ESP32

#include "goodnature_hub.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome::goodnature_ble {

static const char *const TAG = "goodnature_ble.trap";

using namespace protocol;

// A24 passive burst inference, same defaults as the Home Assistant integration.
static const uint32_t BURST_GAP_MS = 8000;
static const uint8_t MIN_PACKETS_FOR_ACTIVATION = 3;
static const uint32_t ACTIVATION_COOLDOWN_MS = 23000;

// Retry backoff after failed connections.
static const uint32_t RETRY_BACKOFF_BASE_MS = 60 * 1000;
static const uint32_t RETRY_BACKOFF_MAX_MS = 30 * 60 * 1000;
static const uint8_t MAX_FAILURES_TRACKED = 8;
// Mouse Trap log scanning. Each poll asks only for striker events since the
// last completed window (with some overlap, deduplicated by timestamp); the
// whole log is replayed again this often, on the Poll button, and whenever
// the count is not yet known, so a miscount cannot persist.
static const uint32_t FULL_LOG_RESCAN_INTERVAL_S = 24 * 60 * 60;
static const uint32_t LOG_WINDOW_OVERLAP_S = 120;
// A change in the advertisement bytes may be the trap signalling a kill; poll
// straight away, but not more often than this.
static const uint32_t ADV_CHANGE_POLL_MIN_MS = 60 * 1000;

// How often noisy passive values (RSSI, last seen) are republished.
static const uint32_t PASSIVE_PUBLISH_MIN_MS = 30 * 1000;
// How often the raw advertisement capture is republished in debug mode.
static const uint32_t ADV_CAPTURE_MIN_MS = 10 * 1000;

// Marker: "we have not yet seen the trap's strike counter".
static const uint32_t COUNTER_UNKNOWN = 0xFFFFFFFFUL;

static void format_address(uint64_t address, char *out) {
  uint8_t mac[6];
  for (int i = 0; i < 6; i++)
    mac[i] = (address >> (8 * (5 - i))) & 0xFF;
  format_mac_addr_upper(mac, out);
}

static const char *job_label(Job job) {
  switch (job) {
    case Job::POLL:
      return "poll";
    case Job::TEST_FIRE:
      return "test fire";
    case Job::RESET_ALERT:
      return "reset alert";
    default:
      return "none";
  }
}

static void publish_text(text_sensor::TextSensor *ts, std::optional<std::string> &cache, const std::string &value) {
  if (ts == nullptr)
    return;
  if (!cache.has_value() || *cache != value) {
    cache = value;
    ts->publish_state(value);
  }
}

static void publish_float(sensor::Sensor *s, std::optional<float> &cache, float value) {
  if (s == nullptr)
    return;
  if (!cache.has_value() || *cache != value) {
    cache = value;
    s->publish_state(value);
  }
}

static const char *opt_bool(const std::optional<bool> &v) {
  if (!v.has_value())
    return "unknown";
  return *v ? "yes" : "no";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void GoodnatureTrap::init_prefs() {
  uint32_t key = fnv1_hash("goodnature_ble.trap") + this->index_;
  this->pref_ = global_preferences->make_preference<TrapPrefs>(key, true);
  this->load_prefs_();
  uint32_t cache_key = fnv1_hash("goodnature_ble.trap_cache") + this->index_;
  this->cache_pref_ = global_preferences->make_preference<TrapCache>(cache_key, true);
  this->load_cache_();
}

void GoodnatureTrap::register_with_app() {
  Model model = this->is_bound() ? this->model() : Model::UNKNOWN;
  this->registered_model_ = model;
  // What each model lacks. An unknown model gets everything.
  bool a24 = model == Model::A24;    // CO2 powered: no charger, no armed state
  bool mouse = model == Model::C20;  // electric striker: no CO2, no raw battery voltage
#ifdef USE_DEVICES
  if (this->device_ != nullptr)
    App.register_device(this->device_);
#endif
  for (sensor::Sensor *s : {this->strikes_sensor_, this->battery_sensor_, this->rssi_sensor_,
                            this->last_seen_sensor_, this->last_strike_sensor_, this->lure_age_sensor_,
                            this->lure_remaining_sensor_}) {
    if (s != nullptr)
      App.register_sensor(s);
  }
  if (!mouse && this->battery_voltage_sensor_ != nullptr)
    App.register_sensor(this->battery_voltage_sensor_);
  if (!mouse && this->co2_remaining_sensor_ != nullptr)
    App.register_sensor(this->co2_remaining_sensor_);
  for (binary_sensor::BinarySensor *b :
       {this->kill_alert_bs_, this->battery_low_bs_, this->lure_due_bs_, this->online_bs_}) {
    if (b != nullptr)
      App.register_binary_sensor(b);
  }
  if (!mouse && this->co2_low_bs_ != nullptr)
    App.register_binary_sensor(this->co2_low_bs_);
  if (!a24 && this->charging_bs_ != nullptr)
    App.register_binary_sensor(this->charging_bs_);
  if (!a24 && this->active_bs_ != nullptr)
    App.register_binary_sensor(this->active_bs_);
  for (text_sensor::TextSensor *t : {this->model_ts_, this->serial_ts_, this->firmware_ts_, this->mac_ts_,
                                     this->status_ts_, this->last_adv_ts_, this->last_frame_ts_}) {
    if (t != nullptr)
      App.register_text_sensor(t);
  }
  for (const auto &sb : this->buttons_) {
    if (mouse && sb.kind == TrapButtonKind::CO2_REPLACED)
      continue;
    App.register_button(sb.button);
  }
  if (this->lure_life_number_ != nullptr)
    App.register_number(this->lure_life_number_);
  if (this->strike_event_ != nullptr)
    App.register_event(this->strike_event_);
  if (model != Model::UNKNOWN)
    ESP_LOGD(TAG, "Slot %u: entity set registered for %s", this->index_ + 1, model_name(model));
}

void GoodnatureTrap::setup() {
  if (this->is_bound()) {
    format_address(this->prefs_.address, this->address_str_);
    this->last_seen_epoch_ = this->prefs_.last_seen;
    if (this->prefs_.serial_raw != 0) {
      C20Advertisement adv{};
      adv.serial_number_raw = this->prefs_.serial_raw;
      this->serial_ = adv.serial_number();
    }
    ESP_LOGI(TAG, "Slot %u restored: %s %s, %u strikes", this->index_ + 1, model_name(this->model()),
             this->address_str_, (unsigned) this->prefs_.strikes);
    this->restore_from_cache_();
    this->status_ = "Waiting for trap";
  } else {
    this->status_ = "Unassigned";
  }
  if (!this->slot_active_)
    return;  // hidden slot: its entities are not registered, nothing to publish to
  if (this->lure_life_number_ != nullptr)
    this->lure_life_number_->publish_state(this->lure_life_days());
  this->publish_all();
}

void GoodnatureTrap::load_prefs_() {
  TrapPrefs loaded{};
  if (this->pref_.load(&loaded) && loaded.address != 0) {
    this->prefs_ = loaded;
  } else {
    this->prefs_ = TrapPrefs{};
  }
}

void GoodnatureTrap::load_cache_() {
  TrapCache loaded{};
  if (this->is_bound() && this->cache_pref_.load(&loaded) && loaded.version == CACHE_VERSION) {
    this->cache_ = loaded;
  } else {
    this->cache_ = TrapCache{};
    this->cache_.version = CACHE_VERSION;
    this->cache_.battery_percent = 0xFF;
  }
}

void GoodnatureTrap::save_cache_() {
  this->cache_.version = CACHE_VERSION;
  this->cache_.battery_percent = this->battery_percent_.value_or(0xFF);
  uint8_t flags = 0;
  if (this->active_.has_value())
    flags |= CACHE_ACTIVE_KNOWN | (*this->active_ ? CACHE_ACTIVE : 0);
  if (this->charging_.has_value())
    flags |= CACHE_CHARGING_KNOWN | (*this->charging_ ? CACHE_CHARGING : 0);
  this->cache_.flags = flags;
  if (!this->cache_pref_.save(&this->cache_))
    ESP_LOGW(TAG, "Slot %u: failed to save cached readings", this->index_ + 1);
}

// Last polled readings, so Battery, Armed and Charging are not blank from a
// gateway restart until the next poll.
void GoodnatureTrap::restore_from_cache_() {
  if (this->cache_.battery_percent != 0xFF)
    this->battery_percent_ = this->cache_.battery_percent;
  if (this->cache_.flags & CACHE_ACTIVE_KNOWN)
    this->active_ = (this->cache_.flags & CACHE_ACTIVE) != 0;
  if (this->cache_.flags & CACHE_CHARGING_KNOWN)
    this->charging_ = (this->cache_.flags & CACHE_CHARGING) != 0;
}

void GoodnatureTrap::save_prefs_(bool flush) {
  if (!this->pref_.save(&this->prefs_)) {
    ESP_LOGW(TAG, "Slot %u: failed to save preferences", this->index_ + 1);
    return;
  }
  if (flush)
    global_preferences->sync();
}

void GoodnatureTrap::reset_runtime_state_() {
  this->serial_.clear();
  this->firmware_.clear();
  this->have_adv_counter_ = false;
  this->adv_flags_raw_ = 0xFF;
  this->adv_battery_state_raw_ = 0xFF;
  this->adv_word6_raw_ = 0xFFFF;
  this->kill_displayed_.reset();
  this->kill_read_.reset();
  this->kill_alert_.reset();
  this->active_.reset();
  this->battery_low_.reset();
  this->charging_.reset();
  this->battery_percent_.reset();
  this->battery_voltage_raw_.reset();
  this->have_device_counter_ = false;
  this->gatt_dumped_ = false;
  this->burst_started_ms_ = this->last_packet_ms_ = 0;
  this->packets_in_burst_ = 0;
  this->activation_counted_ = false;
  this->last_activation_ms_ = 0;
  this->estimated_activations_ = 0;
  this->activation_needs_poll_ = false;
  this->pending_job_ = Job::NONE;
  this->active_job_ = Job::NONE;
  this->last_poll_ms_ = this->last_poll_epoch_ = this->last_attempt_ms_ = 0;
  this->consecutive_failures_ = 0;
  this->ever_polled_ = false;
  this->last_seen_ms_ = 0;
  this->last_seen_epoch_ = 0;
  this->last_passive_publish_ms_ = 0;
  this->last_adv_publish_ms_ = 0;
  this->full_rescan_requested_ = false;
  this->last_adv_change_poll_ms_ = 0;
}

void GoodnatureTrap::bind(uint64_t address, esp_ble_addr_type_t addr_type, Model model, uint32_t serial_raw) {
  this->prefs_ = TrapPrefs{};
  this->prefs_.address = address;
  this->prefs_.addr_type = static_cast<uint8_t>(addr_type);
  this->prefs_.model = static_cast<uint8_t>(model);
  this->prefs_.serial_raw = serial_raw;
  // Assume the trap was freshly serviced when it first appears; the user
  // presses "Lure Replaced" / "CO2 Replaced" after that.
  uint32_t now = this->hub_->now_epoch();
  this->prefs_.lure_replaced = now;
  this->prefs_.co2_replaced = now;
  this->prefs_.strikes_at_co2 = COUNTER_UNKNOWN;
  this->prefs_.last_seen = now;
  format_address(address, this->address_str_);
  this->reset_runtime_state_();

  this->save_prefs_(true);
  this->save_cache_();
  this->status_ = "Discovered";
  this->publish_all();
  if (this->needs_reregister()) {
    ESP_LOGI(TAG, "Slot %u now holds a %s; its entities were registered for %s", this->index_ + 1,
             model_name(model), model_name(this->registered_model_));
    this->hub_->schedule_restart_if_needed();
  }
}

void GoodnatureTrap::rebind_address(uint64_t address, esp_ble_addr_type_t addr_type) {
  char old_str[18];
  format_address(this->prefs_.address, old_str);
  this->prefs_.address = address;
  this->prefs_.addr_type = static_cast<uint8_t>(addr_type);
  format_address(address, this->address_str_);
  ESP_LOGI(TAG, "Slot %u: trap %s moved from %s to %s", this->index_ + 1, this->serial_.c_str(), old_str,
           this->address_str_);
  this->save_prefs_(true);
  this->publish_identity_();
}

void GoodnatureTrap::unbind() {
  ESP_LOGI(TAG, "Slot %u: forgetting %s", this->index_ + 1, this->address_str_);
  this->prefs_ = TrapPrefs{};
  this->address_str_[0] = '\0';
  this->reset_runtime_state_();
  this->cache_ = TrapCache{};
  this->save_cache_();
  this->save_prefs_(true);
  this->status_ = "Unassigned";

  // Clear published values so the HA device does not show stale data.
  auto clear_sensor = [](sensor::Sensor *s) {
    if (s != nullptr)
      s->publish_state(NAN);
  };
  clear_sensor(this->strikes_sensor_);
  clear_sensor(this->battery_sensor_);
  clear_sensor(this->battery_voltage_sensor_);
  clear_sensor(this->rssi_sensor_);
  clear_sensor(this->last_seen_sensor_);
  clear_sensor(this->last_strike_sensor_);
  clear_sensor(this->lure_age_sensor_);
  clear_sensor(this->lure_remaining_sensor_);
  clear_sensor(this->co2_remaining_sensor_);
  this->strikes_pub_.reset();
  this->battery_pub_.reset();
  this->voltage_pub_.reset();
  this->last_seen_pub_.reset();
  this->last_strike_pub_.reset();
  this->lure_age_pub_.reset();
  this->lure_remaining_pub_.reset();
  this->co2_pub_.reset();
  publish_if_changed(this->kill_alert_bs_, this->kill_alert_pub_, false);
  publish_if_changed(this->battery_low_bs_, this->battery_low_pub_, false);
  publish_if_changed(this->charging_bs_, this->charging_pub_, false);
  publish_if_changed(this->active_bs_, this->active_pub_, false);
  publish_if_changed(this->lure_due_bs_, this->lure_due_pub_, false);
  publish_if_changed(this->co2_low_bs_, this->co2_low_pub_, false);
  publish_if_changed(this->online_bs_, this->online_pub_, false);
  if (this->last_adv_ts_ != nullptr)
    this->last_adv_ts_->publish_state("");
  if (this->last_frame_ts_ != nullptr)
    this->last_frame_ts_->publish_state("");
  this->publish_identity_();
  this->set_status_(this->status_);
}

// ---------------------------------------------------------------------------
// Passive path
// ---------------------------------------------------------------------------

void GoodnatureTrap::on_advertisement(const esp32_ble_tracker::ESPBTDevice &device, Model detected_model,
                                      const C20Advertisement *c20_adv) {
  uint32_t now_ms = millis();
  uint32_t now_epoch = this->hub_->now_epoch();
  bool first_since_boot = this->last_seen_ms_ == 0;
  this->last_seen_ms_ = now_ms;
  if (now_epoch != 0)
    this->last_seen_epoch_ = now_epoch;
  this->rssi_ = device.get_rssi();

  if (this->model() == Model::UNKNOWN && detected_model != Model::UNKNOWN) {
    ESP_LOGI(TAG, "[%s] identified as %s", this->address_str_, model_name(detected_model));
    this->prefs_.model = static_cast<uint8_t>(detected_model);
    this->save_prefs_();
    this->publish_identity_();
    this->hub_->schedule_restart_if_needed();
  }

  if (c20_adv != nullptr) {
    // The Mouse Trap advertises continuously: serial and total strike count
    // are reliable here. Battery percentage, armed and charging state come
    // from the UART poll; the advertisement only carries the battery state
    // enumeration in byte 8, and byte 5 does not match the documented flag
    // bits on real hardware, so it is logged rather than trusted.
    if (this->prefs_.serial_raw != c20_adv->serial_number_raw) {
      this->prefs_.serial_raw = c20_adv->serial_number_raw;
      this->save_prefs_();
    }
    if (this->serial_ != c20_adv->serial_number()) {
      this->serial_ = c20_adv->serial_number();
      this->publish_identity_();
    }
    bool adv_changed = false;
    if (c20_adv->flags != this->adv_flags_raw_ || c20_adv->battery_state_raw != this->adv_battery_state_raw_) {
      if (this->adv_flags_raw_ != 0xFF) {
        ESP_LOGI(TAG, "[%s] advertisement bytes changed: flags 0x%02X -> 0x%02X, battery state %u -> %u",
                 this->address_str_, this->adv_flags_raw_, c20_adv->flags, this->adv_battery_state_raw_,
                 c20_adv->battery_state_raw);
        adv_changed = true;
      }
      this->adv_flags_raw_ = c20_adv->flags;
      this->adv_battery_state_raw_ = c20_adv->battery_state_raw;
    }
    // Documented as "strikes available"; unverified on real hardware, so it
    // can only raise the alert. Clearing is left to the trap's kill state
    // read during a poll, or to Clear Kill Alert.
    if (c20_adv->strikes_available)
      this->kill_alert_ = true;
    switch (static_cast<C20BatteryState>(c20_adv->battery_state_raw)) {
      case C20BatteryState::NORMAL:
        this->battery_low_ = false;
        break;
      case C20BatteryState::LOW:
      case C20BatteryState::CRITICAL:
      case C20BatteryState::NOT_CONNECTED:
        this->battery_low_ = true;
        break;
      default:
        break;
    }
    // Bytes 6..7 are documented as the strike count, but the app showed 0 and
    // 1 kills for traps advertising 3 and 14, so the word is only kept for
    // decoding. Strikes come from the trap's striker events (see
    // apply_c20_frame) and the device state.
    if (c20_adv->strike_count != this->adv_word6_raw_) {
      if (this->adv_word6_raw_ != 0xFFFF) {
        ESP_LOGI(TAG, "[%s] advertisement word6 %u -> %u", this->address_str_, this->adv_word6_raw_,
                 c20_adv->strike_count);
        adv_changed = true;
      }
      this->adv_word6_raw_ = c20_adv->strike_count;
    }
    // Whatever the changed bytes mean, a kill is the likeliest cause; poll
    // now instead of waiting out the interval.
    if (adv_changed && this->active_job_ == Job::NONE && this->pending_job_ == Job::NONE &&
        (this->last_adv_change_poll_ms_ == 0 || now_ms - this->last_adv_change_poll_ms_ >= ADV_CHANGE_POLL_MIN_MS)) {
      this->last_adv_change_poll_ms_ = now_ms;
      ESP_LOGI(TAG, "[%s] polling now to see what changed", this->address_str_);
      this->hub_->request_job(this, Job::POLL);
    }
    this->publish_battery_();
  } else if (this->model() != Model::C20) {
    // A24 Chirp: it wakes and advertises in a burst when triggered. Count a
    // burst of packets as one activation until the trap's own counter has
    // been read over GATT.
    if (this->last_packet_ms_ == 0 || now_ms - this->last_packet_ms_ > BURST_GAP_MS) {
      this->burst_started_ms_ = now_ms;
      this->packets_in_burst_ = 1;
      this->activation_counted_ = false;
    } else if (this->packets_in_burst_ < 255) {
      this->packets_in_burst_++;
    }
    this->last_packet_ms_ = now_ms;

    if (!this->activation_counted_ && this->packets_in_burst_ >= MIN_PACKETS_FOR_ACTIVATION &&
        (this->last_activation_ms_ == 0 || now_ms - this->last_activation_ms_ >= ACTIVATION_COOLDOWN_MS)) {
      this->last_activation_ms_ = now_ms;
      this->activation_counted_ = true;
      this->estimated_activations_++;
      this->activation_needs_poll_ = true;
      ESP_LOGI(TAG, "[%s] advertisement burst (activation #%u this boot)", this->address_str_,
               (unsigned) this->estimated_activations_);
      if (!this->have_device_counter_) {
        this->set_strikes_(this->prefs_.strikes + 1, now_epoch, "inferred");
      }
    }
  }

  if (first_since_boot || now_ms - this->last_passive_publish_ms_ >= PASSIVE_PUBLISH_MIN_MS) {
    this->last_passive_publish_ms_ = now_ms;
    if (this->rssi_sensor_ != nullptr)
      this->rssi_sensor_->publish_state(this->rssi_);
    this->publish_last_seen_();
  }

  // Debug capture of the raw advertisement (advertising data | scan response).
  if (this->last_adv_ts_ != nullptr && this->hub_->debug_enabled() &&
      (first_since_boot || now_ms - this->last_adv_publish_ms_ >= ADV_CAPTURE_MIN_MS)) {
    this->last_adv_publish_ms_ = now_ms;
    const auto &sr = device.get_scan_result();
    size_t adv_len = sr.adv_data_len;
    size_t rsp_len = sr.scan_rsp_len;
    if (adv_len + rsp_len > sizeof(sr.ble_adv))
      rsp_len = sizeof(sr.ble_adv) > adv_len ? sizeof(sr.ble_adv) - adv_len : 0;
    std::string capture = to_hex(sr.ble_adv, adv_len);
    if (rsp_len > 0) {
      capture += "|";
      capture += to_hex(sr.ble_adv + adv_len, rsp_len);
    }
    this->last_adv_ts_->publish_state(capture);
  }

  this->publish_online_();
}

bool GoodnatureTrap::wants_connection(uint32_t now_ms) const {
  if (!this->is_bound() || this->active_job_ != Job::NONE)
    return false;
  if (this->consecutive_failures_ > 0) {
    uint32_t shift = this->consecutive_failures_ - 1;
    uint32_t backoff = RETRY_BACKOFF_BASE_MS << (shift > 5 ? 5 : shift);
    if (backoff > RETRY_BACKOFF_MAX_MS)
      backoff = RETRY_BACKOFF_MAX_MS;
    if (now_ms - this->last_attempt_ms_ < backoff)
      return false;
  }
  if (this->pending_job_ != Job::NONE)
    return true;
  if (this->activation_needs_poll_)
    return true;
  uint32_t interval = this->hub_->poll_interval_ms();
  if (interval == 0)
    return false;
  if (!this->ever_polled_)
    return true;
  return now_ms - this->last_poll_ms_ >= interval;
}

void GoodnatureTrap::request_job(Job job) {
  if (!this->is_bound()) {
    ESP_LOGW(TAG, "Slot %u is not assigned to a trap", this->index_ + 1);
    return;
  }
  this->pending_job_ = job;
  this->set_status_("Waiting for trap");
}

// ---------------------------------------------------------------------------
// Active path: results from the connection
// ---------------------------------------------------------------------------

void GoodnatureTrap::apply_a24_characteristic(uint16_t short_id, const uint8_t *data, size_t len) {
  switch (short_id) {
    case CHR_SERIAL:
      this->serial_ = decode_text(data, len);
      break;
    case CHR_FIRMWARE:
      this->firmware_ = decode_text(data, len);
      break;
    case CHR_KILL_DISPLAYED: {
      auto v = parse_u16_le(data, len);
      if (v.has_value()) {
        this->kill_displayed_ = *v;
        this->have_device_counter_ = true;
        this->set_strikes_(*v, 0, "trap");
      }
      break;
    }
    case CHR_KILL_READ: {
      auto v = parse_u16_le(data, len);
      if (v.has_value())
        this->kill_read_ = *v;
      break;
    }
    case CHR_KILL_PAYLOAD: {
      auto strike = parse_d30d(data, len);
      if (strike.has_value()) {
        ESP_LOGD(TAG, "[%s] last strike id %u flags 0x%02X at %u", this->address_str_, strike->strike_id,
                 strike->flags, (unsigned) strike->epoch_seconds());
        this->note_strike_epoch_(strike->epoch_seconds());
      }
      break;
    }
    case CHR_BATTERY_VOLTAGE: {
      auto v = parse_u16_le(data, len);
      if (v.has_value())
        this->apply_a24_battery_raw_(*v);
      break;
    }
    case CHR_DEVICE_STATE:
      ESP_LOGD(TAG, "[%s] device state raw: %s", this->address_str_, to_hex(data, len).c_str());
      break;
    default:
      ESP_LOGV(TAG, "[%s] %04X: %s", this->address_str_, short_id, to_hex(data, len).c_str());
      break;
  }
  if (this->kill_displayed_.has_value() && this->kill_read_.has_value()) {
    this->kill_alert_ = *this->kill_displayed_ > *this->kill_read_;
  }
}

void GoodnatureTrap::apply_a24_battery_raw_(uint16_t raw) {
  this->battery_voltage_raw_ = raw;
  uint16_t empty = this->hub_->a24_battery_empty_raw();
  uint16_t full = this->hub_->a24_battery_full_raw();
  if (full <= empty)
    return;  // calibration not configured
  float pct = (static_cast<float>(raw) - empty) * 100.0f / (full - empty);
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  this->battery_percent_ = static_cast<uint8_t>(roundf(pct));
  this->battery_low_ = pct <= this->hub_->a24_battery_low_percent();
}

void GoodnatureTrap::apply_c20_frame(const C20Frame &frame) {
  if (frame.subtype != C20_SUBTYPE_RESPONSE)
    return;
  const uint8_t *p = frame.payload.data();
  size_t n = frame.payload.size();
  switch (frame.type) {
    case C20_TYPE_FIRMWARE:
      this->firmware_ = parse_c20_firmware(p, n);
      break;
    case C20_TYPE_BATTERY_LEVEL: {
      auto b = parse_c20_battery_level(p, n);
      if (b.has_value())
        this->battery_percent_ = b->battery_percent;
      break;
    }
    case C20_TYPE_DEVICE_STATE: {
      auto s = parse_c20_device_state(p, n);
      if (!s.has_value())
        break;
      this->active_ = s->device_state == C20DeviceState::ACTIVATED;
      this->kill_alert_ = s->kill_state == C20KillState::DETECTED;
      this->battery_low_ = s->battery_state == C20BatteryState::CRITICAL || s->battery_state == C20BatteryState::LOW ||
                           s->battery_state == C20BatteryState::NOT_CONNECTED;
      this->charging_ = s->charge_state == C20ChargeState::CHARGING;
      ESP_LOGD(TAG, "[%s] state %s kill %s battery %s strikes %u", this->address_str_,
               c20_device_state_str(s->device_state), c20_kill_state_str(s->kill_state),
               c20_battery_state_str(s->battery_state), (unsigned) s->strike_count);
      // This counter read 0 on a trap whose log holds a kill, so it is not
      // the lifetime total (kills since the last clear, most likely). Strikes
      // come from the replayed log instead; see apply_history_().
      break;
    }
    case C20_TYPE_STRIKER_EVENT: {
      auto e = parse_c20_striker_event(p, n);
      if (!e.has_value())
        break;
      ESP_LOGD(TAG, "[%s] striker event%s: source %s field4 %u at %u", this->address_str_,
               this->replaying_history_ ? " (log)" : "", c20_striker_source_str(e->source),
               (unsigned) e->strike_count, (unsigned) e->event_time);
      if (this->replaying_history_) {
        // Counting events is what matches the app; the event's own count
        // field read 8 on a trap with a single kill, so it is not used.
        if (e->source == C20StrikerSource::USER) {
          this->hist_user_count_++;
        } else if (this->hist_full_ || e->event_time > this->prefs_.last_strike) {
          // An incremental window overlaps the last one; a kill at or before
          // the last known strike time has already been counted.
          this->hist_trigger_count_++;
          if (e->event_time >= C20_HISTORY_START_EPOCH && e->event_time > this->hist_last_trigger_epoch_)
            this->hist_last_trigger_epoch_ = e->event_time;
        }
      } else if (this->strike_event_ != nullptr) {
        // Live event during a session (test fire, or a kill while connected).
        // The poll that follows re-reads the log and updates the counters.
        this->strike_event_->trigger(e->source == C20StrikerSource::USER ? "test_fire" : "strike");
      }
      break;
    }
    case C20_TYPE_LOG_EVENT: {
      // LogEvent wraps a nested frame; striker events are what we want.
      auto nested = c20_decode(p, n);
      if (nested.has_value() && nested->type == C20_TYPE_STRIKER_EVENT) {
        this->replaying_history_ = true;
        this->apply_c20_frame(*nested);
        this->replaying_history_ = false;
      }
      break;
    }
    case C20_TYPE_LOGS:
      this->hist_complete_ = true;
      ESP_LOGD(TAG, "[%s] trap log complete: %u kills, %u test fires", this->address_str_,
               (unsigned) this->hist_trigger_count_, (unsigned) this->hist_user_count_);
      break;
    default:
      break;
  }
}

void GoodnatureTrap::on_job_started(Job job) {
  this->active_job_ = job;
  this->last_attempt_ms_ = millis();
  this->hist_trigger_count_ = 0;
  this->hist_user_count_ = 0;
  this->hist_last_trigger_epoch_ = 0;
  this->hist_complete_ = false;
  this->set_status_("Connecting");
}

void GoodnatureTrap::on_job_finished(Job job, bool ok, const char *reason) {
  this->active_job_ = Job::NONE;
  if (ok) {
    this->consecutive_failures_ = 0;
    this->ever_polled_ = true;
    this->last_poll_ms_ = millis();
    this->last_poll_epoch_ = this->hub_->now_epoch();
    this->activation_needs_poll_ = false;
    if (this->pending_job_ == job)
      this->pending_job_ = Job::NONE;
    if (this->model() == Model::C20) {
      ESP_LOGD(TAG, "[%s] %s done; log %s", this->address_str_, job_label(job),
               this->hist_complete_ ? "complete" : "incomplete");
      if (this->hist_complete_)
        this->apply_history_();
    }
    this->save_cache_();
    switch (job) {
      case Job::TEST_FIRE:
        this->set_status_("Test fire sent");
        break;
      case Job::RESET_ALERT:
        this->set_status_("Alert cleared");
        break;
      default:
        this->set_status_("OK");
        break;
    }
    // Optional: acknowledge the kill on the trap once we have reported it.
    if (this->hub_->auto_acknowledge() && job != Job::RESET_ALERT && this->pending_job_ == Job::NONE &&
        this->kill_alert_.value_or(false)) {
      ESP_LOGI(TAG, "[%s] kill alert active, queueing acknowledgement", this->address_str_);
      this->pending_job_ = Job::RESET_ALERT;
    }
  } else {
    if (this->consecutive_failures_ < MAX_FAILURES_TRACKED)
      this->consecutive_failures_++;
    // Do not retry a user action forever.
    if (this->pending_job_ != Job::NONE && this->consecutive_failures_ >= 3) {
      ESP_LOGW(TAG, "[%s] giving up on requested action after repeated failures", this->address_str_);
      this->pending_job_ = Job::NONE;
    }
    this->set_status_(std::string("Failed: ") + reason);
  }
  this->publish_all();
}

void GoodnatureTrap::publish_raw_frame(const std::string &summary) {
  if (this->last_frame_ts_ != nullptr)
    this->last_frame_ts_->publish_state(summary);
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

uint32_t GoodnatureTrap::log_window_start(uint32_t now_epoch) {
  bool never = this->cache_.log_scanned_to == 0 || this->prefs_.strikes_at_co2 == COUNTER_UNKNOWN;
  bool stale = now_epoch - this->cache_.last_full_scan >= FULL_LOG_RESCAN_INTERVAL_S;
  bool backwards = this->cache_.log_scanned_to > now_epoch;  // clock moved; do not trust the window
  this->hist_full_ = never || stale || backwards || this->full_rescan_requested_;
  this->hist_window_end_ = now_epoch;
  if (this->hist_full_)
    return C20_HISTORY_START_EPOCH;
  uint32_t start = this->cache_.log_scanned_to;
  return start > C20_HISTORY_START_EPOCH + LOG_WINDOW_OVERLAP_S ? start - LOG_WINDOW_OVERLAP_S
                                                                 : C20_HISTORY_START_EPOCH;
}

void GoodnatureTrap::apply_history_() {
  bool first = this->prefs_.strikes_at_co2 == COUNTER_UNKNOWN;
  uint32_t count, last;
  if (this->hist_full_) {
    count = this->hist_trigger_count_;
    last = this->hist_last_trigger_epoch_;
    this->cache_.last_full_scan = this->hist_window_end_;
    this->cache_.test_fires = this->hist_user_count_;
    this->full_rescan_requested_ = false;
    ESP_LOGI(TAG, "[%s] trap log: %u kills, %u test fires (was %u)", this->address_str_, (unsigned) count,
             (unsigned) this->hist_user_count_, first ? 0U : (unsigned) this->prefs_.strikes);
  } else {
    count = this->prefs_.strikes + this->hist_trigger_count_;
    last = std::max(this->prefs_.last_strike, this->hist_last_trigger_epoch_);
    this->cache_.test_fires += this->hist_user_count_;
    ESP_LOGD(TAG, "[%s] trap log since last poll: %u kills, %u test fires", this->address_str_,
             (unsigned) this->hist_trigger_count_, (unsigned) this->hist_user_count_);
  }
  this->cache_.log_scanned_to = this->hist_window_end_;

  bool increased = !first && count > this->prefs_.strikes;
  if (!first && count == this->prefs_.strikes && last == this->prefs_.last_strike)
    return;
  this->prefs_.strikes = count;
  this->prefs_.strikes_at_co2 = count;  // "counter known"; CO2 itself is not tracked on this model
  this->prefs_.last_strike = last;
  this->have_device_counter_ = true;
  this->save_prefs_(increased);
  if (last == 0 && this->last_strike_sensor_ != nullptr && this->last_strike_pub_.has_value()) {
    this->last_strike_sensor_->publish_state(NAN);
    this->last_strike_pub_.reset();
  }
  this->publish_counters_();
  if (increased && this->strike_event_ != nullptr)
    this->strike_event_->trigger("strike");
}

void GoodnatureTrap::note_strike_epoch_(uint32_t epoch) {
  if (epoch < 1500000000UL)  // ignore garbage / unset clocks
    return;
  if (epoch > this->prefs_.last_strike) {
    this->prefs_.last_strike = epoch;
    this->save_prefs_();
  }
}

void GoodnatureTrap::set_strikes_(uint32_t strikes, uint32_t strike_epoch, const char *source,
                                  const char *event_type) {
  bool counter_known = this->prefs_.strikes_at_co2 != COUNTER_UNKNOWN;
  bool changed = !counter_known || strikes != this->prefs_.strikes;
  bool new_strike = false;

  if (!counter_known) {
    // First time we learn the trap's counter: anchor the CO2 canister count here.
    this->prefs_.strikes_at_co2 = strikes;
    ESP_LOGI(TAG, "[%s] strike counter is %u (%s)", this->address_str_, (unsigned) strikes, source);
  } else if (strikes > this->prefs_.strikes) {
    ESP_LOGI(TAG, "[%s] strike count %u -> %u (%s)", this->address_str_, (unsigned) this->prefs_.strikes,
             (unsigned) strikes, source);
    if (strike_epoch == 0)
      strike_epoch = this->hub_->now_epoch();
    this->note_strike_epoch_(strike_epoch);
    new_strike = true;
  } else if (strikes < this->prefs_.strikes) {
    ESP_LOGW(TAG, "[%s] strike counter went backwards %u -> %u (%s)", this->address_str_,
             (unsigned) this->prefs_.strikes, (unsigned) strikes, source);
    if (strikes < this->prefs_.strikes_at_co2)
      this->prefs_.strikes_at_co2 = strikes;
  }
  this->prefs_.strikes = strikes;
  if (changed) {
    // A new strike is committed to flash right away instead of waiting for
    // the periodic sync: an inferred A24 burst has no trap-side record to
    // recover from if power is lost.
    this->save_prefs_(new_strike);
    this->publish_counters_();
    this->publish_consumables_();
  }
  if (new_strike && this->strike_event_ != nullptr)
    this->strike_event_->trigger(event_type);
}

// ---------------------------------------------------------------------------
// Consumables
// ---------------------------------------------------------------------------

void GoodnatureTrap::mark_lure_replaced() {
  uint32_t now = this->hub_->now_epoch();
  if (now == 0) {
    ESP_LOGW(TAG, "[%s] cannot record lure replacement: time not synchronised", this->address_str_);
    return;
  }
  ESP_LOGI(TAG, "[%s] lure replaced", this->address_str_);
  this->prefs_.lure_replaced = now;
  this->save_prefs_(true);
  this->publish_consumables_();
}

void GoodnatureTrap::mark_co2_replaced() {
  if (this->model() == Model::C20) {
    ESP_LOGW(TAG, "[%s] the Mouse Trap has no CO2 canister; nothing to record", this->address_str_);
    return;
  }
  ESP_LOGI(TAG, "[%s] CO2 canister replaced", this->address_str_);
  this->prefs_.co2_replaced = this->hub_->now_epoch();
  this->prefs_.strikes_at_co2 = this->have_device_counter_ || this->prefs_.strikes_at_co2 != COUNTER_UNKNOWN
                                    ? this->prefs_.strikes
                                    : COUNTER_UNKNOWN;
  this->save_prefs_(true);
  this->publish_consumables_();
}

void GoodnatureTrap::set_lure_life_days(uint16_t days) {
  this->prefs_.lure_life_days = days;
  if (this->is_bound())
    this->save_prefs_(true);
  this->publish_consumables_();
}

uint16_t GoodnatureTrap::lure_life_days() const {
  if (this->prefs_.lure_life_days != 0)
    return this->prefs_.lure_life_days;
  return this->hub_->default_lure_life_days();
}

void GoodnatureTrap::update_periodic() {
  if (!this->is_bound())
    return;
  if (this->last_seen_epoch_ != this->prefs_.last_seen) {
    this->prefs_.last_seen = this->last_seen_epoch_;
    this->save_prefs_();
  }
  this->publish_consumables_();
  this->publish_online_();
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

void GoodnatureTrap::publish_all() {
  if (!this->slot_active_)
    return;
  this->publish_identity_();
  this->set_status_(this->status_);
  if (!this->is_bound())
    return;
  this->publish_counters_();
  this->publish_battery_();
  this->publish_consumables_();
  this->publish_last_seen_();
  this->publish_online_();
}

void GoodnatureTrap::set_status_(const std::string &status) {
  this->status_ = status;
  publish_text(this->status_ts_, this->status_pub_, status);
}

void GoodnatureTrap::publish_identity_() {
  if (!this->is_bound()) {
    publish_text(this->model_ts_, this->model_pub_, "");
    publish_text(this->serial_ts_, this->serial_pub_, "");
    publish_text(this->firmware_ts_, this->firmware_pub_, "");
    publish_text(this->mac_ts_, this->mac_pub_, "");
    return;
  }
  publish_text(this->model_ts_, this->model_pub_, model_name(this->model()));
  publish_text(this->serial_ts_, this->serial_pub_, this->serial_);
  publish_text(this->firmware_ts_, this->firmware_pub_, this->firmware_);
  publish_text(this->mac_ts_, this->mac_pub_, this->address_str_);
}

void GoodnatureTrap::publish_counters_() {
  if (!this->is_bound())
    return;
  bool counter_known = this->prefs_.strikes_at_co2 != COUNTER_UNKNOWN;
  if (counter_known)
    publish_float(this->strikes_sensor_, this->strikes_pub_, static_cast<float>(this->prefs_.strikes));
  if (this->prefs_.last_strike != 0)
    publish_float(this->last_strike_sensor_, this->last_strike_pub_, static_cast<float>(this->prefs_.last_strike));
  if (this->kill_alert_.has_value())
    publish_if_changed(this->kill_alert_bs_, this->kill_alert_pub_, *this->kill_alert_);
}

void GoodnatureTrap::publish_battery_() {
  if (!this->is_bound())
    return;
  if (this->battery_percent_.has_value())
    publish_float(this->battery_sensor_, this->battery_pub_, static_cast<float>(*this->battery_percent_));
  if (this->battery_voltage_raw_.has_value())
    publish_float(this->battery_voltage_sensor_, this->voltage_pub_, static_cast<float>(*this->battery_voltage_raw_));
  if (this->battery_low_.has_value())
    publish_if_changed(this->battery_low_bs_, this->battery_low_pub_, *this->battery_low_);
  if (this->charging_.has_value())
    publish_if_changed(this->charging_bs_, this->charging_pub_, *this->charging_);
  if (this->active_.has_value())
    publish_if_changed(this->active_bs_, this->active_pub_, *this->active_);
}

void GoodnatureTrap::publish_consumables_() {
  if (!this->is_bound())
    return;
  uint32_t now = this->hub_->now_epoch();

  if (now != 0) {
    // Time may have become valid only after the slot was bound.
    bool dirty = false;
    if (this->prefs_.lure_replaced == 0) {
      this->prefs_.lure_replaced = now;
      dirty = true;
    }
    if (this->prefs_.co2_replaced == 0) {
      this->prefs_.co2_replaced = now;
      dirty = true;
    }
    if (dirty)
      this->save_prefs_();

    float age_days = now > this->prefs_.lure_replaced ? (now - this->prefs_.lure_replaced) / 86400.0f : 0.0f;
    age_days = roundf(age_days * 10.0f) / 10.0f;
    float remaining = roundf((static_cast<float>(this->lure_life_days()) - age_days) * 10.0f) / 10.0f;
    publish_float(this->lure_age_sensor_, this->lure_age_pub_, age_days);
    publish_float(this->lure_remaining_sensor_, this->lure_remaining_pub_, remaining);
    publish_if_changed(this->lure_due_bs_, this->lure_due_pub_, remaining <= 0.0f);
  }

  if (this->model() == Model::C20) {
    // Electric striker, no CO2. Clear anything published before the model was known.
    if (this->co2_pub_.has_value() && this->co2_remaining_sensor_ != nullptr) {
      this->co2_remaining_sensor_->publish_state(NAN);
      this->co2_pub_.reset();
    }
  } else if (this->prefs_.strikes_at_co2 != COUNTER_UNKNOWN) {
    uint32_t used = this->prefs_.strikes >= this->prefs_.strikes_at_co2
                        ? this->prefs_.strikes - this->prefs_.strikes_at_co2
                        : 0;
    uint16_t capacity = this->hub_->co2_capacity();
    float remaining = used >= capacity ? 0.0f : static_cast<float>(capacity - used);
    publish_float(this->co2_remaining_sensor_, this->co2_pub_, remaining);
    publish_if_changed(this->co2_low_bs_, this->co2_low_pub_, remaining <= this->hub_->co2_low_threshold());
  }
}

void GoodnatureTrap::publish_last_seen_() {
  if (this->last_seen_epoch_ != 0)
    publish_float(this->last_seen_sensor_, this->last_seen_pub_, static_cast<float>(this->last_seen_epoch_));
}

void GoodnatureTrap::publish_online_() {
  if (!this->is_bound())
    return;
  uint32_t timeout_ms = this->hub_->offline_timeout_ms(this->model());
  uint32_t now_epoch = this->hub_->now_epoch();
  bool online;
  if (now_epoch != 0 && this->last_seen_epoch_ != 0) {
    // Wall-clock based, so a trap heard before the last reboot still counts.
    online = now_epoch >= this->last_seen_epoch_ && (now_epoch - this->last_seen_epoch_) < timeout_ms / 1000;
  } else {
    online = this->last_seen_ms_ != 0 && (millis() - this->last_seen_ms_) < timeout_ms;
  }
  publish_if_changed(this->online_bs_, this->online_pub_, online);
}

// ---------------------------------------------------------------------------
// Debug
// ---------------------------------------------------------------------------

void GoodnatureTrap::log_debug_info() const {
  ESP_LOGI(TAG, "Slot %u debug info:", this->index_ + 1);
  if (!this->is_bound()) {
    ESP_LOGI(TAG, "  unassigned");
    return;
  }
  uint32_t now_ms = millis();
  bool counter_known = this->prefs_.strikes_at_co2 != COUNTER_UNKNOWN;
  ESP_LOGI(TAG, "  %s %s addr_type=%u serial='%s' fw='%s'", model_name(this->model()), this->address_str_,
           (unsigned) this->prefs_.addr_type, this->serial_.c_str(), this->firmware_.c_str());
  ESP_LOGI(TAG, "  strikes=%u strikes_at_co2=%u last_strike=%u counter_known=%s device_counter=%s",
           (unsigned) this->prefs_.strikes, (unsigned) (counter_known ? this->prefs_.strikes_at_co2 : 0),
           (unsigned) this->prefs_.last_strike, counter_known ? "yes" : "no",
           this->have_device_counter_ ? "yes" : "no");
  ESP_LOGI(TAG, "  kill_displayed=%d kill_read=%d kill_alert=%s armed=%s",
           this->kill_displayed_ ? (int) *this->kill_displayed_ : -1, this->kill_read_ ? (int) *this->kill_read_ : -1,
           opt_bool(this->kill_alert_), opt_bool(this->active_));
  ESP_LOGI(TAG, "  battery=%d%% voltage_raw=%d battery_low=%s charging=%s",
           this->battery_percent_ ? (int) *this->battery_percent_ : -1,
           this->battery_voltage_raw_ ? (int) *this->battery_voltage_raw_ : -1, opt_bool(this->battery_low_),
           opt_bool(this->charging_));
  ESP_LOGI(TAG, "  lure_replaced=%u lure_life=%u co2_replaced=%u", (unsigned) this->prefs_.lure_replaced,
           (unsigned) this->lure_life_days(), (unsigned) this->prefs_.co2_replaced);
  if (this->model() == Model::C20) {
    ESP_LOGI(TAG, "  log scanned_to=%u last_full_scan=%u test_fires=%u", (unsigned) this->cache_.log_scanned_to,
             (unsigned) this->cache_.last_full_scan, (unsigned) this->cache_.test_fires);
    ESP_LOGI(TAG, "  adv flags=0x%02X adv battery_state=%u (%s) adv word6=%u", this->adv_flags_raw_,
             this->adv_battery_state_raw_,
             c20_battery_state_str(static_cast<C20BatteryState>(this->adv_battery_state_raw_)),
             this->adv_word6_raw_);
  }
  ESP_LOGI(TAG, "  last_seen=%u s ago (epoch %u) rssi=%d bursts=%u needs_poll=%s",
           (unsigned) (this->last_seen_ms_ ? (now_ms - this->last_seen_ms_) / 1000 : 0),
           (unsigned) this->last_seen_epoch_, this->rssi_, (unsigned) this->estimated_activations_,
           this->activation_needs_poll_ ? "yes" : "no");
  ESP_LOGI(TAG, "  pending_job=%s active_job=%s polled=%s last_poll=%u s ago failures=%u status='%s'",
           job_label(this->pending_job_), job_label(this->active_job_), this->ever_polled_ ? "yes" : "no",
           (unsigned) (this->last_poll_ms_ ? (now_ms - this->last_poll_ms_) / 1000 : 0),
           (unsigned) this->consecutive_failures_, this->status_.c_str());
}

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

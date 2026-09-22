#include "goodnature_connection.h"

#ifdef USE_ESP32

#include "goodnature_hub.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_gattc_api.h>

#include <cstdio>

namespace esphome::goodnature_ble {

static const char *const TAG = "goodnature_ble.conn";

using namespace protocol;
namespace espbt = esphome::esp32_ble_tracker;

static const uint32_t OP_TIMEOUT_MS = 6000;
static const uint32_t C20_RESPONSE_TIMEOUT_MS = 4000;
// The logs request streams one frame per event before its terminator; the
// timer restarts on every streamed event, so this only bounds the gaps.
static const uint32_t C20_LOGS_TIMEOUT_MS = 25000;
static const uint32_t JOB_TIMEOUT_MS = 45000;
static const uint32_t JOB_HARD_TIMEOUT_MS = JOB_TIMEOUT_MS + 20000;

static const char *job_name(Job job) {
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

void GoodnatureConnection::setup() {
  BLEClientBase::setup();
  // We never want the base class to connect on its own from an
  // advertisement; the hub decides when to connect.
  this->set_auto_connect(false);
}

void GoodnatureConnection::dump_config() {
  ESP_LOGCONFIG(TAG, "Goodnature connection:");
  BLEClientBase::dump_config();
}

void GoodnatureConnection::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  BLEClientBase::gap_event_handler(event, param);
}

// ---------------------------------------------------------------------------
// Job control
// ---------------------------------------------------------------------------

void GoodnatureConnection::start(GoodnatureTrap *trap, Job job) {
  if (this->trap_ != nullptr) {
    ESP_LOGW(TAG, "Busy with %s, cannot start %s for %s", this->trap_->address_str(), job_name(job),
             trap->address_str());
    return;
  }
  if (this->state() != espbt::ClientState::IDLE) {
    ESP_LOGW(TAG, "Connection not idle (%s), cannot start %s", espbt::client_state_to_string(this->state()),
             job_name(job));
    return;
  }
  this->trap_ = trap;
  this->job_ = job;
  this->generation_++;
  this->ops_.clear();
  this->op_index_ = 0;
  this->op_in_flight_ = false;
  this->ops_started_ = false;
  this->disconnect_requested_ = false;
  this->ok_so_far_ = true;
  this->reads_.clear();
  this->assembler_.reset();
  this->nus_rx_ = nullptr;
  this->nus_tx_ = nullptr;
  this->job_started_ms_ = millis();

  this->set_address(trap->address());
  this->set_remote_addr_type(trap->address_type());
  trap->on_job_started(job);
  ESP_LOGI(TAG, "[%s] starting %s (%s)", this->address_str(), job_name(job), model_name(trap->model()));
  // The tracker promotes DISCOVERED clients: it stops scanning and calls connect().
  this->set_state(espbt::ClientState::DISCOVERED);
}

void GoodnatureConnection::tick(uint32_t now_ms) {
  if (this->trap_ == nullptr)
    return;

  if (this->op_in_flight_ && this->op_index_ < this->ops_.size()) {
    const Op &op = this->ops_[this->op_index_];
    uint32_t timeout = OP_TIMEOUT_MS;
    if (op.kind == OpKind::C20_REQUEST)
      timeout = C20_RESPONSE_TIMEOUT_MS;
    else if (op.kind == OpKind::C20_LOGS)
      timeout = C20_LOGS_TIMEOUT_MS;
    else if (op.kind == OpKind::WAIT)
      timeout = op.wait_ms + 2000;
    if (now_ms - this->op_started_ms_ > timeout) {
      if (op.kind == OpKind::C20_LOGS) {
        // History is best effort: the poll still succeeds, but the strike
        // count is left untouched because the log was not read to the end.
        ESP_LOGW(TAG, "[%s] trap log did not finish streaming; strike count unchanged", this->address_str());
      } else {
        ESP_LOGW(TAG, "[%s] operation %u (kind %u) timed out", this->address_str(), (unsigned) this->op_index_,
                 (unsigned) op.kind);
        this->ok_so_far_ = false;
      }
      this->op_done_();
      return;
    }
  }

  if (now_ms - this->job_started_ms_ > JOB_HARD_TIMEOUT_MS) {
    ESP_LOGE(TAG, "[%s] connection stuck in %s, forcing idle", this->address_str(),
             espbt::client_state_to_string(this->state()));
    if (this->conn_id_ != esp32_ble_client::UNSET_CONN_ID)
      esp_ble_gattc_close(this->gattc_if_, this->conn_id_);
    this->conn_id_ = esp32_ble_client::UNSET_CONN_ID;
    this->release_services();
    this->set_state(espbt::ClientState::IDLE);
    this->finish_(false, "stuck");
    return;
  }
  if (now_ms - this->job_started_ms_ > JOB_TIMEOUT_MS && !this->disconnect_requested_) {
    this->abort("timeout");
  }
}

void GoodnatureConnection::abort(const char *reason) {
  if (this->trap_ == nullptr)
    return;
  ESP_LOGW(TAG, "[%s] aborting: %s", this->address_str(), reason);
  auto st = this->state();
  if (st == espbt::ClientState::DISCOVERED) {
    this->set_state(espbt::ClientState::IDLE);
    this->finish_(false, reason);
  } else if (st == espbt::ClientState::IDLE) {
    this->finish_(false, reason);
  } else {
    this->ok_so_far_ = false;
    this->disconnect_requested_ = true;
    this->disconnect();  // finish_ runs on the disconnect/close event
  }
}

void GoodnatureConnection::request_disconnect_() {
  if (this->disconnect_requested_)
    return;
  this->disconnect_requested_ = true;
  this->op_in_flight_ = false;
  ESP_LOGD(TAG, "[%s] session complete, disconnecting", this->address_str());
  this->disconnect();
}

void GoodnatureConnection::finish_(bool ok, const char *reason) {
  GoodnatureTrap *trap = this->trap_;
  Job job = this->job_;
  if (trap == nullptr)
    return;
  this->trap_ = nullptr;
  this->job_ = Job::NONE;
  this->op_in_flight_ = false;
  this->ops_.clear();
  this->reads_.clear();
  this->nus_rx_ = nullptr;
  this->nus_tx_ = nullptr;
  this->generation_++;
  if (this->state() == espbt::ClientState::IDLE)
    this->set_address(0);
  ESP_LOGI(TAG, "[%s] %s %s (%s)", trap->address_str(), job_name(job), ok ? "finished" : "failed", reason);
  trap->on_job_finished(job, ok, reason);
  if (this->hub_ != nullptr)
    this->hub_->on_job_finished(trap, job, ok);
}

// ---------------------------------------------------------------------------
// GATT events
// ---------------------------------------------------------------------------

bool GoodnatureConnection::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                               esp_ble_gattc_cb_param_t *param) {
  if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
    return false;
  if (this->trap_ == nullptr)
    return true;

  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      // The base class drops back to IDLE when the open fails.
      if (this->state() == espbt::ClientState::IDLE)
        this->finish_(false, "connect failed");
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      if (this->state() == espbt::ClientState::ESTABLISHED && !this->ops_started_) {
        this->ops_started_ = true;
        if (this->hub_->debug_enabled() || !this->trap_->gatt_dumped()) {
          this->dump_gatt_();
          this->trap_->set_gatt_dumped();
        }
        this->build_ops_();
        this->run_next_op_();
      }
      break;

    case ESP_GATTC_READ_CHAR_EVT:
      if (this->op_in_flight_ && this->ops_[this->op_index_].kind == OpKind::READ &&
          param->read.handle == this->current_handle_) {
        if (param->read.status == ESP_GATT_OK) {
          this->handle_read_result_(param->read.handle, param->read.value, param->read.value_len);
        } else {
          ESP_LOGW(TAG, "[%s] read %04X failed, status %d", this->address_str(), this->current_gn_uuid_,
                   param->read.status);
        }
        this->op_done_();
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (this->op_in_flight_ && param->write.handle == this->current_handle_) {
        const Op &op = this->ops_[this->op_index_];
        if (op.kind == OpKind::WRITE || op.kind == OpKind::WRITE_ECHO_U16) {
          if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "[%s] write %04X failed, status %d", this->address_str(), op.gn_uuid, param->write.status);
            this->ok_so_far_ = false;
          }
          this->op_done_();
        } else if ((op.kind == OpKind::C20_REQUEST || op.kind == OpKind::C20_LOGS) &&
                   param->write.status != ESP_GATT_OK) {
          ESP_LOGW(TAG, "[%s] UART write failed, status %d", this->address_str(), param->write.status);
          this->ok_so_far_ = false;
          this->op_done_();
        }
      }
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (this->op_in_flight_ && this->ops_[this->op_index_].kind == OpKind::NOTIFY_ON &&
          param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] register for notify failed, status %d", this->address_str(),
                 param->reg_for_notify.status);
        this->ok_so_far_ = false;
        this->op_done_();
      }
      // On success the base class writes the CCCD; we continue on WRITE_DESCR.
      break;

    case ESP_GATTC_WRITE_DESCR_EVT:
      if (this->op_in_flight_ && this->ops_[this->op_index_].kind == OpKind::NOTIFY_ON) {
        if (param->write.status != ESP_GATT_OK) {
          ESP_LOGW(TAG, "[%s] enabling notifications failed, status %d", this->address_str(), param->write.status);
          this->ok_so_far_ = false;
        }
        this->op_done_();
      }
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (this->nus_tx_ != nullptr && param->notify.handle == this->nus_tx_->handle) {
        auto frames = this->assembler_.feed(param->notify.value, param->notify.value_len);
        for (const auto &frame : frames)
          this->handle_c20_frame_(frame);
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      this->finish_(this->disconnect_requested_ && this->ok_so_far_,
                    this->disconnect_requested_ ? (this->ok_so_far_ ? "done" : "partial") : "disconnected");
      break;

    default:
      break;
  }
  return true;
}

void GoodnatureConnection::dump_gatt_() {
  ESP_LOGI(TAG, "[%s] GATT table (%u services):", this->address_str(), (unsigned) this->services_.size());
  for (auto *svc : this->services_) {
    if (!svc->parsed)
      svc->parse_characteristics();
    char uuid_buf[esp32_ble::UUID_STR_LEN];
    ESP_LOGI(TAG, "  service %s handles 0x%04X-0x%04X", svc->uuid.to_str(uuid_buf), svc->start_handle,
             svc->end_handle);
    for (auto *chr : svc->characteristics) {
      char props[8];
      snprintf(props, sizeof(props), "%s%s%s%s%s", (chr->properties & ESP_GATT_CHAR_PROP_BIT_READ) ? "R" : "-",
               (chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE) ? "W" : "-",
               (chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR) ? "w" : "-",
               (chr->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ? "N" : "-",
               (chr->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) ? "I" : "-");
      ESP_LOGI(TAG, "    char %s handle 0x%04X props %s (0x%02X)", chr->uuid.to_str(uuid_buf), chr->handle, props,
               chr->properties);
    }
  }
}

void GoodnatureConnection::handle_read_result_(uint16_t handle, const uint8_t *data, size_t len) {
  (void) handle;
  ESP_LOGD(TAG, "[%s] read %04X: %s", this->address_str(), this->current_gn_uuid_, to_hex(data, len).c_str());
  this->reads_.emplace_back(this->current_gn_uuid_, std::vector<uint8_t>(data, data + len));
  if (this->hub_->debug_enabled()) {
    char summary[16];
    snprintf(summary, sizeof(summary), "%04X:", this->current_gn_uuid_);
    this->trap_->publish_raw_frame(std::string(summary) + to_hex(data, len));
  }
  this->trap_->apply_a24_characteristic(this->current_gn_uuid_, data, len);
}

void GoodnatureConnection::handle_c20_frame_(const C20Frame &frame) {
  if (this->trap_ == nullptr)
    return;  // frame arrived while disconnecting, after the job was closed
  ESP_LOGD(TAG, "[%s] UART frame type 0x%02X subtype 0x%02X len %u", this->address_str(), frame.type, frame.subtype,
           (unsigned) frame.payload.size());
  if (this->hub_->debug_enabled()) {
    char summary[16];
    snprintf(summary, sizeof(summary), "UART %02X/%02X:", frame.type, frame.subtype);
    this->trap_->publish_raw_frame(std::string(summary) + to_hex(frame.payload.data(), frame.payload.size()));
  }
  if (frame.subtype != C20_SUBTYPE_RESPONSE)
    return;
  this->trap_->apply_c20_frame(frame);
  if (this->op_in_flight_) {
    const Op &op = this->ops_[this->op_index_];
    if (op.kind == OpKind::C20_LOGS && frame.type == C20_TYPE_LOG_EVENT)
      this->op_started_ms_ = millis();  // still streaming; keep waiting for the terminator
    if ((op.kind == OpKind::C20_REQUEST && frame.type == op.expect_type) ||
        (op.kind == OpKind::C20_LOGS && frame.type == C20_TYPE_LOGS)) {
      this->op_done_();
    }
  }
}

// ---------------------------------------------------------------------------
// Session scripts
// ---------------------------------------------------------------------------

void GoodnatureConnection::build_ops_() {
  this->ops_.clear();
  Model model = this->trap_->model();
  uint32_t now = this->hub_->now_epoch();

  if (model == Model::C20) {
    this->ops_.push_back(Op{OpKind::NOTIFY_ON});
    if (this->job_ == Job::TEST_FIRE || this->job_ == Job::RESET_ALERT) {
      Op cmd{OpKind::C20_REQUEST};
      cmd.expect_type = C20_TYPE_SET_COMMAND;
      cmd.data = c20_build_set_command(this->job_ == Job::TEST_FIRE ? C20_COMMAND_FIRE : C20_COMMAND_CLEAR);
      this->ops_.push_back(std::move(cmd));
      // Let the async DeviceState / StrikerEvent notifications land.
      Op wait{OpKind::WAIT};
      wait.wait_ms = 2000;
      this->ops_.push_back(std::move(wait));
    }
    this->push_c20_poll_ops_();
    return;
  }

  // A24 (and unknown models, which are probed with the A24 GATT layout;
  // reads of characteristics that do not exist are skipped).
  if (this->hub_->write_time_on_connect() && now != 0) {
    Op w{OpKind::WRITE};
    w.gn_uuid = CHR_TIME;
    put_u32_le(w.data, now / 60);
    this->ops_.push_back(std::move(w));
  }
  if (this->job_ == Job::TEST_FIRE) {
    Op ctl{OpKind::WRITE};
    ctl.gn_uuid = CHR_CONTROL;
    ctl.data = {A24_CONTROL_TEST_FIRE};
    this->ops_.push_back(std::move(ctl));
    Op wait{OpKind::WAIT};
    wait.wait_ms = 500;
    this->ops_.push_back(std::move(wait));
    this->ops_.push_back(Op{OpKind::READ, CHR_KILL_DISPLAYED});
    // The app selects the returned kill by writing the displayed counter back.
    Op sel{OpKind::WRITE_ECHO_U16};
    sel.gn_uuid = CHR_KILL_DISPLAYED;
    sel.src_uuid = CHR_KILL_DISPLAYED;
    this->ops_.push_back(std::move(sel));
    this->ops_.push_back(Op{OpKind::READ, CHR_KILL_PAYLOAD});
  } else if (this->job_ == Job::RESET_ALERT) {
    this->ops_.push_back(Op{OpKind::READ, CHR_KILL_DISPLAYED});
    this->ops_.push_back(Op{OpKind::READ, CHR_EVENT_DISPLAYED});
    Op ack_kill{OpKind::WRITE_ECHO_U16};
    ack_kill.gn_uuid = CHR_KILL_READ;
    ack_kill.src_uuid = CHR_KILL_DISPLAYED;
    this->ops_.push_back(std::move(ack_kill));
    Op ack_event{OpKind::WRITE_ECHO_U16};
    ack_event.gn_uuid = CHR_EVENT_READ;
    ack_event.src_uuid = CHR_EVENT_DISPLAYED;
    this->ops_.push_back(std::move(ack_event));
    Op ctl{OpKind::WRITE};
    ctl.gn_uuid = CHR_CONTROL;
    ctl.data = {A24_CONTROL_ACK};
    this->ops_.push_back(std::move(ctl));
  }
  this->push_a24_poll_ops_();
}

void GoodnatureConnection::push_a24_poll_ops_() {
  for (uint16_t id : {CHR_SERIAL, CHR_FIRMWARE, CHR_DEVICE_STATE, CHR_KILL_DISPLAYED, CHR_KILL_READ, CHR_KILL_PAYLOAD,
                      CHR_BATTERY_VOLTAGE, CHR_BATTERY_RESISTANCE}) {
    this->ops_.push_back(Op{OpKind::READ, id});
  }
}

void GoodnatureConnection::push_c20_poll_ops_() {
  uint32_t now = this->hub_->now_epoch();
  if (this->hub_->write_time_on_connect() && now != 0) {
    Op t{OpKind::C20_REQUEST};
    t.expect_type = C20_TYPE_SET_TIME;
    t.data = c20_build_set_time(now);
    this->ops_.push_back(std::move(t));
  }
  for (uint8_t type : {C20_TYPE_FIRMWARE, C20_TYPE_BATTERY_LEVEL, C20_TYPE_DEVICE_STATE}) {
    Op r{OpKind::C20_REQUEST};
    r.expect_type = type;
    r.data = c20_build_request(type);
    this->ops_.push_back(std::move(r));
  }
  if (now != 0) {
    // Counting the log's TRIGGER events is the only strike count that
    // matched the app. The trap scans its log at a fixed pace (a full replay
    // takes ~12 s), so the trap decides whether this poll needs the whole
    // log or only the events since the last completed window.
    uint32_t start = this->trap_->log_window_start(now);
    Op logs{OpKind::C20_LOGS};
    logs.data = c20_build_logs_request(start, now, C20_LOGS_FLAG_STRIKER_EVENTS);
    this->ops_.push_back(std::move(logs));
  }
}

void GoodnatureConnection::run_next_op_() {
  if (this->trap_ == nullptr || this->disconnect_requested_)
    return;
  if (this->op_index_ >= this->ops_.size()) {
    this->request_disconnect_();
    return;
  }
  Op &op = this->ops_[this->op_index_];
  this->op_in_flight_ = true;
  this->op_started_ms_ = millis();

  switch (op.kind) {
    case OpKind::READ: {
      auto *chr = this->find_gn_char_(op.gn_uuid);
      if (chr == nullptr) {
        ESP_LOGD(TAG, "[%s] characteristic %04X not present, skipping", this->address_str(), op.gn_uuid);
        this->op_done_();
        return;
      }
      this->current_handle_ = chr->handle;
      this->current_gn_uuid_ = op.gn_uuid;
      esp_err_t err = esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, chr->handle, ESP_GATT_AUTH_REQ_NONE);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] read %04X request failed, err %d", this->address_str(), op.gn_uuid, err);
        this->op_done_();
      }
      break;
    }

    case OpKind::WRITE:
    case OpKind::WRITE_ECHO_U16: {
      const uint8_t *data = op.data.data();
      size_t len = op.data.size();
      if (op.kind == OpKind::WRITE_ECHO_U16) {
        const auto *src = this->read_result_(op.src_uuid);
        if (src == nullptr || src->size() < 2) {
          ESP_LOGW(TAG, "[%s] no value read from %04X to write to %04X", this->address_str(), op.src_uuid,
                   op.gn_uuid);
          this->ok_so_far_ = false;
          this->op_done_();
          return;
        }
        data = src->data();
        len = 2;
      }
      auto *chr = this->find_gn_char_(op.gn_uuid);
      if (chr == nullptr) {
        ESP_LOGW(TAG, "[%s] characteristic %04X not present, cannot write", this->address_str(), op.gn_uuid);
        this->op_done_();
        return;
      }
      this->current_handle_ = chr->handle;
      this->current_gn_uuid_ = op.gn_uuid;
      ESP_LOGD(TAG, "[%s] write %04X: %s", this->address_str(), op.gn_uuid, to_hex(data, len).c_str());
      if (!this->write_char_(chr, data, len)) {
        this->ok_so_far_ = false;
        this->op_done_();
      }
      break;
    }

    case OpKind::WAIT: {
      uint32_t gen = this->generation_;
      this->set_timeout(op.wait_ms, [this, gen]() {
        if (gen == this->generation_ && this->trap_ != nullptr && this->op_in_flight_)
          this->op_done_();
      });
      break;
    }

    case OpKind::NOTIFY_ON: {
      this->nus_tx_ = this->find_char_(esp32_ble::ESPBTUUID::from_raw(NUS_TX_UUID));
      this->nus_rx_ = this->find_char_(esp32_ble::ESPBTUUID::from_raw(NUS_RX_UUID));
      if (this->nus_tx_ == nullptr || this->nus_rx_ == nullptr) {
        ESP_LOGW(TAG, "[%s] Nordic UART service not found", this->address_str());
        this->ok_so_far_ = false;
        this->request_disconnect_();
        return;
      }
      esp_err_t err = esp_ble_gattc_register_for_notify(this->gattc_if_, this->remote_bda_, this->nus_tx_->handle);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] register for notify request failed, err %d", this->address_str(), err);
        this->ok_so_far_ = false;
        this->op_done_();
      }
      break;
    }

    case OpKind::C20_REQUEST:
    case OpKind::C20_LOGS: {
      if (this->nus_rx_ == nullptr) {
        this->op_done_();
        return;
      }
      this->current_handle_ = this->nus_rx_->handle;
      ESP_LOGD(TAG, "[%s] UART request: %s", this->address_str(), to_hex(op.data.data(), op.data.size()).c_str());
      if (!this->write_char_(this->nus_rx_, op.data.data(), op.data.size())) {
        this->ok_so_far_ = false;
        this->op_done_();
      }
      // Completion comes from the matching response frame (or a timeout).
      break;
    }
  }
}

void GoodnatureConnection::op_done_() {
  if (this->trap_ == nullptr)
    return;
  this->op_in_flight_ = false;
  this->op_index_++;
  this->run_next_op_();
}

// ---------------------------------------------------------------------------
// GATT helpers
// ---------------------------------------------------------------------------

esp32_ble_client::BLECharacteristic *GoodnatureConnection::find_char_(const esp32_ble::ESPBTUUID &uuid) {
  for (auto *svc : this->services_) {
    auto *chr = svc->get_characteristic(uuid);
    if (chr != nullptr)
      return chr;
  }
  return nullptr;
}

esp32_ble_client::BLECharacteristic *GoodnatureConnection::find_gn_char_(uint16_t short_id) {
  return this->find_char_(esp32_ble::ESPBTUUID::from_raw(gn_uuid_str(short_id)));
}

bool GoodnatureConnection::write_char_(esp32_ble_client::BLECharacteristic *chr, const uint8_t *data, size_t len) {
  esp_gatt_write_type_t type =
      (chr->properties & ESP_GATT_CHAR_PROP_BIT_WRITE) ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
  esp_err_t err = esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, chr->handle, len,
                                           const_cast<uint8_t *>(data), type, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "[%s] write request failed, err %d", this->address_str(), err);
    return false;
  }
  return true;
}

const std::vector<uint8_t> *GoodnatureConnection::read_result_(uint16_t short_id) const {
  for (auto it = this->reads_.rbegin(); it != this->reads_.rend(); ++it) {
    if (it->first == short_id)
      return &it->second;
  }
  return nullptr;
}

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

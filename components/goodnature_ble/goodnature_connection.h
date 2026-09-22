#pragma once

#ifdef USE_ESP32

#include "goodnature_trap.h"
#include "protocol.h"

#include "esphome/components/esp32_ble_client/ble_client_base.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace esphome::goodnature_ble {

class GoodnatureHub;

// A single reusable GATT connection. The hub points it at whichever trap
// needs work, it runs a short scripted session (reads/writes for the A24,
// Nordic UART request/response for the C20), reports results into the
// trap, disconnects, and becomes free again.
class GoodnatureConnection : public esp32_ble_client::BLEClientBase {
 public:
  void setup() override;
  void dump_config() override;
  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

  void set_hub(GoodnatureHub *hub) { this->hub_ = hub; }

  bool is_idle() const { return this->trap_ == nullptr; }
  GoodnatureTrap *current_trap() const { return this->trap_; }

  // Begin a job for a trap that was just heard advertising: marks this
  // client DISCOVERED so the tracker stops scanning and connects.
  void start(GoodnatureTrap *trap, Job job);

  // Called from the hub loop for timeouts (the base class disables its
  // own loop while idle, so timing is driven from the hub).
  void tick(uint32_t now_ms);

  // Cancel whatever is happening and free the connection.
  void abort(const char *reason);

 protected:
  enum class OpKind : uint8_t {
    READ,            // read GN characteristic gn_uuid
    WRITE,           // write data to GN characteristic gn_uuid
    WRITE_ECHO_U16,  // write the first 2 bytes previously read from src_uuid to gn_uuid
    WAIT,            // pause wait_ms
    NOTIFY_ON,       // subscribe to NUS TX
    C20_REQUEST,     // write frame to NUS RX and wait for response type expect_type
    C20_LOGS,        // write logs request and wait for the LogsResponse terminator
  };

  struct Op {
    OpKind kind;
    uint16_t gn_uuid{0};
    uint16_t src_uuid{0};
    uint8_t expect_type{0};
    uint32_t wait_ms{0};
    std::vector<uint8_t> data;
  };

  void dump_gatt_();
  void build_ops_();
  void push_a24_poll_ops_();
  void push_c20_poll_ops_();
  void run_next_op_();
  void op_done_();
  void finish_(bool ok, const char *reason);
  void request_disconnect_();

  esp32_ble_client::BLECharacteristic *find_char_(const esp32_ble::ESPBTUUID &uuid);
  esp32_ble_client::BLECharacteristic *find_gn_char_(uint16_t short_id);
  bool write_char_(esp32_ble_client::BLECharacteristic *chr, const uint8_t *data, size_t len);
  const std::vector<uint8_t> *read_result_(uint16_t short_id) const;
  void handle_c20_frame_(const protocol::C20Frame &frame);
  void handle_read_result_(uint16_t handle, const uint8_t *data, size_t len);

  GoodnatureHub *hub_{nullptr};
  GoodnatureTrap *trap_{nullptr};
  Job job_{Job::NONE};

  std::vector<Op> ops_;
  size_t op_index_{0};
  bool op_in_flight_{false};
  bool ops_started_{false};
  bool disconnect_requested_{false};
  bool ok_so_far_{true};
  uint32_t job_started_ms_{0};
  uint32_t op_started_ms_{0};
  uint32_t generation_{0};

  uint16_t current_handle_{0};
  uint16_t current_gn_uuid_{0};
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> reads_;

  protocol::C20FrameAssembler assembler_;
  esp32_ble_client::BLECharacteristic *nus_rx_{nullptr};
  esp32_ble_client::BLECharacteristic *nus_tx_{nullptr};
};

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

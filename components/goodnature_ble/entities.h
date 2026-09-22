#pragma once

#ifdef USE_ESP32

#include "goodnature_hub.h"
#include "goodnature_trap.h"

#include "esphome/core/helpers.h"
#include "esphome/components/button/button.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"

namespace esphome::goodnature_ble {

class TrapButton : public button::Button, public Parented<GoodnatureTrap> {
 public:
  void set_kind(TrapButtonKind kind) { this->kind_ = kind; }

 protected:
  void press_action() override;
  TrapButtonKind kind_{TrapButtonKind::POLL};
};

class ForgetAllButton : public button::Button, public Parented<GoodnatureHub> {
 protected:
  void press_action() override { this->parent_->forget_all_traps(); }
};

class DiscoverySwitch : public switch_::Switch, public Parented<GoodnatureHub> {
 protected:
  void write_state(bool state) override {
    this->parent_->set_discovery_enabled(state);
    this->publish_state(state);
  }
};

class DebugSwitch : public switch_::Switch, public Parented<GoodnatureHub> {
 protected:
  void write_state(bool state) override {
    this->parent_->set_debug_enabled(state);
    this->publish_state(state);
  }
};

class LureLifeNumber : public number::Number, public Parented<GoodnatureTrap> {
 protected:
  void control(float value) override {
    this->parent_->set_lure_life_days(static_cast<uint16_t>(value));
    this->publish_state(value);
  }
};


}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

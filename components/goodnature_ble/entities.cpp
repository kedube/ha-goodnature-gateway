#include "entities.h"

#ifdef USE_ESP32

namespace esphome::goodnature_ble {

void TrapButton::press_action() {
  GoodnatureTrap *trap = this->parent_;
  GoodnatureHub *hub = trap->hub();
  switch (this->kind_) {
    case TrapButtonKind::POLL:
      trap->request_full_rescan();  // a manual poll also resyncs the strike count
      hub->request_job(trap, Job::POLL);
      break;
    case TrapButtonKind::TEST_FIRE:
      hub->request_job(trap, Job::TEST_FIRE);
      break;
    case TrapButtonKind::RESET_ALERT:
      hub->request_job(trap, Job::RESET_ALERT);
      break;
    case TrapButtonKind::LURE_REPLACED:
      trap->mark_lure_replaced();
      break;
    case TrapButtonKind::CO2_REPLACED:
      trap->mark_co2_replaced();
      break;
    case TrapButtonKind::FORGET:
      hub->forget_trap(trap);
      break;
    case TrapButtonKind::LOG_DEBUG:
      trap->log_debug_info();
      break;
  }
}

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

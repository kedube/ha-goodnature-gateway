#include "entities.h"

#ifdef USE_ESP32

namespace esphome::goodnature_ble {

void TrapButton::press_action() {
  GoodnatureTrap *trap = this->parent_;
  GoodnatureHub *hub = trap->hub();
  switch (this->kind_) {
    case TrapButtonKind::POLL:
      trap->request_full_rescan();  // a manual poll also resyncs the strike count
      trap->clear_backoff();
      hub->request_job(trap, Job::POLL);
      break;
    case TrapButtonKind::TEST_FIRE:
      trap->clear_backoff();
      trap->request_test_fire();
      break;
    case TrapButtonKind::RESET_ALERT:
      trap->clear_backoff();
      trap->clear_kill_alert();
      break;
    case TrapButtonKind::CLEAR_ERROR:
      trap->clear_trap_error();
      break;
    case TrapButtonKind::LURE_REPLACED:
      trap->mark_lure_replaced();
      break;
    case TrapButtonKind::CO2_REPLACED:
      trap->mark_co2_replaced();
      break;
    case TrapButtonKind::CO2_SHOT_USED:
      trap->mark_co2_shot_used();
      break;
    case TrapButtonKind::FORGET:
      hub->forget_trap(trap);
      break;
    case TrapButtonKind::LOG_DEBUG:
      trap->log_debug_info();
      break;
  }
}

void ReplacedDate::show(uint32_t epoch) {
  ESPTime t = ESPTime::from_epoch_local(epoch);
  if (!t.is_valid(false, false))
    return;
  if (this->has_state() && this->year_ == t.year && this->month_ == t.month && this->day_ == t.day_of_month)
    return;
  this->year_ = t.year;
  this->month_ = t.month;
  this->day_ = t.day_of_month;
  this->publish_state();
}

void ReplacedDate::control(const datetime::DateCall &call) {
  GoodnatureTrap *trap = this->parent_;
  uint32_t now = trap->hub()->now_epoch();
  auto year = call.get_year();
  auto month = call.get_month();
  auto day = call.get_day();
  if (now == 0 || !year.has_value() || !month.has_value() || !day.has_value())
    return;
  // Noon on the chosen day, or now for today; never in the future.
  ESPTime t{};
  t.year = *year;
  t.month = *month;
  t.day_of_month = *day;
  t.hour = 12;
  t.recalc_timestamp_local();
  ESPTime today = ESPTime::from_epoch_local(now);
  uint32_t epoch = static_cast<uint32_t>(t.timestamp);
  if (t.year == today.year && t.month == today.month && t.day_of_month == today.day_of_month)
    epoch = now;
  if (t.timestamp <= 0 || epoch > now) {
    // Rejected: show the stored date again.
    this->set_has_state(false);
    trap->publish_all();
    return;
  }
  if (this->co2_)
    trap->set_co2_replaced(epoch);
  else
    trap->set_lure_replaced(epoch);
}

}  // namespace esphome::goodnature_ble

#endif  // USE_ESP32

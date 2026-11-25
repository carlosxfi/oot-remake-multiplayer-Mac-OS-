#pragma once

#include <chrono>
#include <cstddef>

namespace coop::host {

enum class FlagScanReason {
  none,
  baseline,
  event_overflow,
  list_changed,
  loading,
  load_boundary,
  event_audit,
  polling,
};

inline const char* flag_scan_reason_name(FlagScanReason reason) {
  switch (reason) {
    case FlagScanReason::baseline: return "baseline";
    case FlagScanReason::event_overflow: return "event_overflow";
    case FlagScanReason::list_changed: return "list_changed";
    case FlagScanReason::loading: return "loading";
    case FlagScanReason::load_boundary: return "load_boundary";
    case FlagScanReason::event_audit: return "event_audit";
    case FlagScanReason::polling: return "polling";
    case FlagScanReason::none: return "none";
  }
  return "unknown";
}

// Installed only while an opted-in game-frame callback is running.
struct FrameTrace {
  using Clock = std::chrono::steady_clock;

  static double since_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  }

  inline static thread_local FrameTrace* active = nullptr;
  double flag_scan_ms = 0;
  double sync_ms = 0;
  double puppet_and_save_ms = 0;
  double native_ui_ms = 0;
  double read_ms = 0;
  double net_lock_wait_ms = 0;
  std::size_t read_calls = 0;
  std::size_t read_bytes = 0;
  std::size_t flag_keys_read = 0;
  FlagScanReason flag_scan_reason = FlagScanReason::none;
  bool full_flag_scan = false;
};

}  // namespace coop::host

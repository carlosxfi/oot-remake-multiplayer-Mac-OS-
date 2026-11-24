#pragma once

#include <chrono>

namespace coop::host {

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
};

}  // namespace coop::host

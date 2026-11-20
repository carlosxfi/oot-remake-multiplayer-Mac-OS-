#include "calc_bridge.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>

int main() {
  using namespace coop_nxnative;
  // The rare call can fire once during startup and miss ordinary gameplay.
  assert(kCalcHookOffset == 0x6e70f8);
  assert(arm64_branch(kCalcHookOffset, kFrameStepOffset, true) == kOriginalCalcCall);
  assert(arm64_branch(kCalcHookOffset, kCalcCaveOffset, true) == 0x94d57c62u);
  assert(!arm64_branch(0, 3, true));
  assert(!arm64_branch(0, 1ull << 27, true));

  constexpr std::uint64_t callback = 0x1122334455667788ull;
  const auto cave = make_calc_cave(callback);
  std::uint32_t tail = 0;
  std::uint64_t stored_callback = 0;
  std::memcpy(&tail, cave.data() + 36, sizeof(tail));
  std::memcpy(&stored_callback, cave.data() + 40, sizeof(stored_callback));
  assert(tail == 0x17fc2017u);
  assert(stored_callback == callback);
}

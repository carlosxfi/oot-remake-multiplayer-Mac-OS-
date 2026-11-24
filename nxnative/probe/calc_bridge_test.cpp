#include "calc_bridge.hpp"
#include "puppet_guard.hpp"

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

  assert(kPuppetGuardCave == kCalcCaveOffset + cave.size());
  const auto guard = make_puppet_guard();
  for (unsigned i = 0; i < 3; ++i) {
    const auto branch = guard[2 * i];
    assert((branch & 0xff00001fu) == 0xb4000008u);  // cbz x8
    assert(2 * i + ((branch >> 5) & 0x7ffffu) == 7);
    assert(guard[2 * i + 1] == kPuppetGuardOriginal[i + 1]);
  }
  assert(guard[6] == arm64_branch(kPuppetGuardCave + 24, kPuppetGuardSite + 12, false));
  assert(guard[7] == arm64_branch(kPuppetGuardCave + 28, kPuppetGuardSkip, false));
  assert(arm64_branch(kPuppetGuardSite, kPuppetGuardCave, false));

  const auto bow = make_bow_guard();
  assert(kBowGuardCave == kPuppetGuardCave + sizeof(guard));
  assert((bow[0] & 0xff00001fu) == 0xb4000000u);  // cbz x0
  assert(((bow[0] >> 5) & 0x7ffffu) == 3);
  assert(bow[1] == kBowGuardOriginal[1]);
  assert(bow[2] == arm64_branch(kBowGuardCave + 8, kBowGuardSite + 4, false));
  assert(bow[3] == arm64_branch(kBowGuardCave + 12, kBowGuardSkip, false));
  assert(arm64_branch(kBowGuardSite, kBowGuardCave, false));
}

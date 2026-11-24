#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace coop_nxnative {

// Ocarina of Time 1.1.1, main build e0f3ccfaf50065ae.
// Use the recurring moduleCalc_ call identified by CoopSvc and Hookshot.
inline constexpr std::uint64_t kCalcHookOffset = 0x6e70f8;
inline constexpr std::uint64_t kCalcCaveOffset = 0x3c46280;
inline constexpr std::uint64_t kFrameStepOffset = 0x3b4e300;
inline constexpr std::uint32_t kOriginalCalcCall = 0x94d19c82;

inline std::optional<std::uint32_t> arm64_branch(std::uint64_t pc,
                                                 std::uint64_t target,
                                                 bool link) {
  const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
  if ((delta & 3) != 0 || delta < -(1ll << 27) || delta >= (1ll << 27))
    return std::nullopt;
  return (link ? 0x94000000u : 0x14000000u) |
         (static_cast<std::uint32_t>(delta >> 2) & 0x03ffffffu);
}

inline std::array<std::uint8_t, 48> make_calc_cave(std::uint64_t callback,
                                              std::uint64_t cave = kCalcCaveOffset) {
  // Preserve the original call's x0/s0, call a host function on the game's
  // calc thread, then tail-call the instruction the hook replaces.
  std::array<std::uint8_t, 48> out{};
  const auto tail = arm64_branch(cave + 36, kFrameStepOffset, false);
  const std::array<std::uint32_t, 10> words{
      0xa9be7bfdu,  // stp x29, x30, [sp, #-32]!
      0x910003fdu,  // mov x29, sp
      0xf9000be0u,  // str x0, [sp, #16]
      0xfd000fe0u,  // str d0, [sp, #24]
      0x580000d0u,  // ldr x16, literal at +40
      0xd63f0200u,  // blr x16
      0xf9400be0u,  // ldr x0, [sp, #16]
      0xfd400fe0u,  // ldr d0, [sp, #24]
      0xa8c27bfdu,  // ldp x29, x30, [sp], #32
      tail.value_or(0),
  };
  std::memcpy(out.data(), words.data(), sizeof(words));
  std::memcpy(out.data() + sizeof(words), &callback, sizeof(callback));
  return out;
}

}  // namespace coop_nxnative

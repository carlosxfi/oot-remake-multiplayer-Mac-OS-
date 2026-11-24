#pragma once

#include "calc_bridge.hpp"

namespace coop_nxnative {

// Hookshot's 0002-coop-puppets patch documents this chain for build 1.1.1.
inline constexpr std::uint64_t kPuppetGuardSite = 0x4b833c;
inline constexpr std::uint64_t kPuppetGuardSkip = 0x4b83b8;
inline constexpr std::uint64_t kPuppetGuardCave = kCalcCaveOffset + 48;
inline constexpr std::array<std::uint32_t, 5> kPuppetGuardOriginal{
    0xf9413aa8, 0xf9402908, 0xf9401108, 0xf940e508, 0xb4000388,
};

inline std::array<std::uint32_t, 8> make_puppet_guard() {
  // Check the input to each load. On null, go through the game's own exit.
  // On success, rejoin at the original cbz with the same x8 and flags.
  return {
      0xb40000e8, kPuppetGuardOriginal[1],  // cbz x8, cave + 28; ldr x8, [x8,#0x50]
      0xb40000a8, kPuppetGuardOriginal[2],  // cbz x8, cave + 28; ldr x8, [x8,#0x20]
      0xb4000068, kPuppetGuardOriginal[3],  // cbz x8, cave + 28; ldr x8, [x8,#0x1c8]
      arm64_branch(kPuppetGuardCave + 24, kPuppetGuardSite + 12, false).value(),
      arm64_branch(kPuppetGuardCave + 28, kPuppetGuardSkip, false).value(),
  };
}

// Bow/slingshot update once the game has looked up the wearer's Player component.
inline constexpr std::uint64_t kBowGuardSite = 0x2dceef8;
inline constexpr std::uint64_t kBowGuardSkip = 0x2dcef04;
inline constexpr std::uint64_t kBowGuardCave = kPuppetGuardCave + 8 * sizeof(std::uint32_t);
inline constexpr std::array<std::uint32_t, 5> kBowGuardOriginal{
    0x97696f73, 0xb947d808, 0x3100051f, 0x54000181, 0xfd403be8,
};

inline std::array<std::uint32_t, 4> make_bow_guard() {
  return {
      0xb4000060u,  // cbz x0, cave + 12
      kBowGuardOriginal[1],  // ldr w8, [x0, #0x7d8]
      arm64_branch(kBowGuardCave + 8, kBowGuardSite + 4, false).value(),
      arm64_branch(kBowGuardCave + 12, kBowGuardSkip, false).value(),
  };
}

}  // namespace coop_nxnative

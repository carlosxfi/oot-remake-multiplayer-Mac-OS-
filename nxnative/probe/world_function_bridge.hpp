#pragma once

#include "calc_bridge.hpp"

namespace coop_nxnative {

inline constexpr std::uint64_t kWorldFunctionCave = 0x3c46e00;
inline constexpr std::size_t kWorldFunctionBytes = 24;

// The entry gateway is a tail branch to the host dispatcher. The second
// entry replays the one displaced instruction before continuing in the game.
inline std::array<std::uint8_t, kWorldFunctionBytes> make_world_function_cave(
    std::uint64_t cave, std::uint64_t site, std::uint32_t first, std::uint64_t dispatcher) {
  const std::array words{
      0x58000050u,  // ldr x16, literal at +8
      0xd61f0200u,  // br x16
  };
  const std::array original{first, arm64_branch(cave + 20, site + 4, false).value()};
  std::array<std::uint8_t, kWorldFunctionBytes> out{};
  std::memcpy(out.data(), words.data(), sizeof(words));
  std::memcpy(out.data() + 8, &dispatcher, sizeof(dispatcher));
  std::memcpy(out.data() + 16, original.data(), sizeof(original));
  return out;
}

}  // namespace coop_nxnative

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace coop_nxnative {

// Same argument layout as the existing co-op actor director's calls.
struct GameCall {
  std::uint64_t function = 0;
  std::array<std::uint64_t, 8> x{};
  std::array<std::uint64_t, 4> stack{};
  float s0 = 0.0f;
};

static_assert(offsetof(GameCall, x) == 8);
static_assert(offsetof(GameCall, stack) == 72);
static_assert(offsetof(GameCall, s0) == 104);

extern "C" std::uint64_t coop_nxnative_invoke_game_call(const GameCall* call);

bool check_game_call();

}  // namespace coop_nxnative

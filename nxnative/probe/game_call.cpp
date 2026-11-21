#include "game_call.hpp"

#include <array>

namespace coop_nxnative {
namespace {

// Keep the first test independent of the game's memory and actor manager.
std::uint64_t check_arguments(
    float value, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2, std::uint64_t x3,
    std::uint64_t x4, std::uint64_t x5, std::uint64_t x6, std::uint64_t x7,
    std::uint64_t stack0, std::uint64_t stack1, std::uint64_t stack2, std::uint64_t stack3) {
  const std::array actual{x0, x1, x2, x3, x4, x5, x6, x7, stack0, stack1, stack2, stack3};
  constexpr std::array<std::uint64_t, 12> expected{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  return value == 12.5f && actual == expected ? 0x43a1 : 0;
}

}  // namespace

bool check_game_call() {
  const GameCall call{reinterpret_cast<std::uint64_t>(&check_arguments),
                      {1, 2, 3, 4, 5, 6, 7, 8}, {9, 10, 11, 12}, 12.5f};
  return coop_nxnative_invoke_game_call(&call) == 0x43a1;
}

}  // namespace coop_nxnative

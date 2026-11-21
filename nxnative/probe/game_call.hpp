#pragma once

#include <cstddef>
#include <cstdint>

#include "../../hookshot/src/hos_coop_api.h"

namespace coop_nxnative {

// Use the Windows co-op module's call contract directly. The assertions
// guard the assembly's load offsets when that header changes.
using GameCall = HosCoopCall;

static_assert(offsetof(GameCall, x) == 8);
static_assert(offsetof(GameCall, stack) == 72);
static_assert(offsetof(GameCall, s0) == 104);

extern "C" std::uint64_t coop_nxnative_invoke_game_call(const GameCall* call, void* indirect = nullptr);

bool check_game_call();

}  // namespace coop_nxnative

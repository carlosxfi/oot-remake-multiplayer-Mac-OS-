// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_move_state.h"

#include <cstring>

namespace CoopMoveState {

namespace {
void Put64(std::array<std::uint8_t, kBlockBytes>& bytes, std::size_t at, std::uint64_t value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
}  // namespace

std::optional<std::array<std::uint8_t, kBlockBytes>> Block(std::uint64_t base, std::uint64_t actor,
                                                           std::uint32_t next) {
    if (base == 0 || (base & 7) != 0 || base < block::kActorSlot || actor == 0) {
        return std::nullopt;
    }
    std::array<std::uint8_t, kBlockBytes> bytes{};
    Put64(bytes, block::kHolder, base + block::kValuePtr);
    std::memcpy(bytes.data() + block::kValue, &next, sizeof(next));
    Put64(bytes, block::kValuePtr, base + block::kValue);
    Put64(bytes, block::kInner, base + block::kActor - block::kActorSlot);
    Put64(bytes, block::kActor, actor);
    return bytes;
}

bool Done(std::uint32_t result) {
    return (result >> 30) == 2;
}

std::optional<std::uint32_t> AtMadeOurs(std::optional<std::uint32_t> ours_now, std::optional<std::uint32_t> owners) {
    if (!ours_now || !owners || *ours_now == *owners || *owners >= 3) {
        return std::nullopt;
    }
    return owners;
}

}  // namespace CoopMoveState

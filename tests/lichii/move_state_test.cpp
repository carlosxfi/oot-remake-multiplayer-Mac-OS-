// SPDX-License-Identifier: GPL-3.0-or-later
// CoopMoveState (row 287): the node block for the game's own change of a CCT move state, read back the way
// main+0x28fff10 reads it.
#include <doctest/doctest.h>

#include <cstring>

#include "coop_move_state.h"

namespace {
namespace b = CoopMoveState::block;

constexpr std::uint64_t kBase = 0x7000'1000;
constexpr std::uint64_t kActor = 0x9123'4560;

// A word of the block by its guest address, as the game's loads see it.
std::uint64_t Word(const std::array<std::uint8_t, CoopMoveState::kBlockBytes>& bytes, std::uint64_t address) {
    REQUIRE(address >= kBase);
    REQUIRE(address + 8 <= kBase + bytes.size());
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + (address - kBase), sizeof(value));
    return value;
}
}  // namespace

TEST_CASE("row 287: the block walks to the actor and the state as the game's node does") {
    const auto bytes = CoopMoveState::Block(kBase, kActor, 2);
    REQUIRE(bytes.has_value());
    // actor = [[[node + 8] + 8] + 0x1f0]
    const std::uint64_t holder = Word(*bytes, kBase + 8);
    const std::uint64_t inner = Word(*bytes, holder + 8);
    CHECK(Word(*bytes, inner + 0x1f0) == kActor);
    // NextMoveState = u32 at [[node + 0x20] & ~3]
    const std::uint64_t value_at = Word(*bytes, kBase + 0x20) & ~std::uint64_t{3};
    CHECK((Word(*bytes, value_at) & 0xffff'ffff) == 2);
    // the result word the game writes is not one of the words it reads
    CHECK(b::kResult != b::kHolder);
    CHECK(b::kResult != b::kValue);
    CHECK(b::kResult + 4 <= b::kValuePtr);
}

TEST_CASE("row 287: no block for an address the pointers cannot stand at") {
    CHECK_FALSE(CoopMoveState::Block(0, kActor, 2).has_value());
    CHECK_FALSE(CoopMoveState::Block(kBase + 4, kActor, 2).has_value());
    CHECK_FALSE(CoopMoveState::Block(0x100, kActor, 2).has_value());
    CHECK_FALSE(CoopMoveState::Block(kBase, 0, 2).has_value());
}

TEST_CASE("row 287: the game's result word") {
    CHECK(CoopMoveState::Done(0x8000'0000u));
    CHECK(CoopMoveState::Done(0x8000'0123u));
    CHECK_FALSE(CoopMoveState::Done(0xc000'0000u));
    CHECK_FALSE(CoopMoveState::Done(0));
}

TEST_CASE("row 287: made ours, the owner's move state is asked only when ours differs") {
    using CoopMoveState::AtMadeOurs;
    CHECK(AtMadeOurs(1, 2) == std::optional<std::uint32_t>{2});  // the copy's knock-down left 1, the owner is back at 2
    CHECK_FALSE(AtMadeOurs(2, 2).has_value());
    CHECK_FALSE(AtMadeOurs(std::nullopt, 2).has_value());
    CHECK_FALSE(AtMadeOurs(1, std::nullopt).has_value());
    CHECK_FALSE(AtMadeOurs(1, 3).has_value());  // no slot for it in the game's machine
}

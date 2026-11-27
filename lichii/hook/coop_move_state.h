// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Row 287 (Phantom Ganon falls through the floor after a hand-over): the game's own change of a boss's CCT move
// state, called by the module for a boss that was a copy and is made ours. No emulator code: tested in
// tests/lichii/move_state_test.cpp. Not wired into the boss sync yet (reports: shooter-287-222).
//
// The call is OneShotPhysicsCctChangeMoveState's execute, main+0x28fff10 (build e0f3cc, read in the instructions):
//   x0 = node; actor = [[[node + 8] + 8] + 0x1f0]; NextMoveState = u32 at [[node + 0x20] & ~3];
//   it walks the chain of coop_move_probe.h to the machine, and when the current state is another one it ends that
//   state (its vtable + 0x38), zeroes the state's +0x14 (u32) and +0x18 (u8), keeps the old index in machine + 0x5c
//   and writes the new one to machine + 0x58; then it writes its result word to node + 0x18 (top bits 10 = done,
//   11 = a link of the chain was missing). It reads nothing else of the node.
// So the node can be a block of ours: 0x38 bytes, with three pointers into itself. The third one points 0x1f0 below
// the word that holds the actor; the game reads only that word through it.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace CoopMoveState {

constexpr std::uint64_t kChangeExecute = 0x28fff10;  // main+
constexpr std::size_t kBlockBytes = 0x38;

namespace block {
constexpr std::size_t kHolder = 0x08;   // node + 8: -> the block's 0x20
constexpr std::size_t kValue = 0x10;    // u32 NextMoveState (node + 0x10 is not read by the game)
constexpr std::size_t kResult = 0x18;   // u32, written by the game
constexpr std::size_t kValuePtr = 0x20; // node + 0x20: -> kValue
constexpr std::size_t kInner = 0x28;    // [[node + 8] + 8]: -> kActor - 0x1f0
constexpr std::size_t kActor = 0x30;    // the actor
constexpr std::uint64_t kActorSlot = 0x1f0;
}  // namespace block

// The node block for `actor` and move state `next`, as it must stand at guest address `base` (8-byte aligned).
// Nothing when base is 0, not aligned, or below 0x1f0 (the inner pointer would wrap).
std::optional<std::array<std::uint8_t, kBlockBytes>> Block(std::uint64_t base, std::uint64_t actor,
                                                           std::uint32_t next);

// What the game left in the block's result word: true when it ran to the end (a missing link of the chain: false).
bool Done(std::uint32_t result);

// A boss made ours: the move state to ask the game for, or nothing when none is needed. `ours_now` is the state
// its body is in in our game (the copy's own AI left it there); `owners` is the state it was in in the game that
// owned it until now. Both known and different: the owner's. A state the game has no slot for (3 and above) is
// never asked.
std::optional<std::uint32_t> AtMadeOurs(std::optional<std::uint32_t> ours_now, std::optional<std::uint32_t> owners);

}  // namespace CoopMoveState

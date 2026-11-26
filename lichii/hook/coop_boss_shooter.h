// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rows 222 and 286 (the others do not see a boss's shots): the owner's game decides every shot of a boss's Shooter
// component, a copy's game makes the same ones with the game's own functions.
//
// The game's three verbs (hos_coop_boss_shooter.h, patch 0045; build e0f3ccfa):
// - main+0x2eb1ed0 prepare(shooter, prepare params) -> record index, < 0 refused: wakes a kept bullet;
// - main+0x2eb2a40 shoot(shooter, shoot params) -> bool;
// - main+0x2eb26b0 sleep(shooter, u32 key, s32 index) -> bool.
// A boss's AI nodes call them, each game its own, so a copy shot only when its own AI happened to reach the node.
// Now: the owner's calls that the game took are sent with the boss's samples (Shot); a copy's own AI calls are
// answered as the game answers "nothing free" (-1, false) and not run; the owner's are run on the copy by our caller.
//
// The two params are large objects the nodes build inline (no constructor to call: main+0x284ffb0 to 0x28500cc,
// main+0x3023520 to 0x302361c). A copy therefore keeps the bytes of its own node's params (Template) when it refuses
// the call, and runs the owner's shot with a copy of them in our guest block: pointers into the old block moved to
// the new one (Rebased), key, index and place from the owner (PatchPrepare, PatchShoot). No template yet for a key
// (this game's AI never reached that node): the shot is counted as missed, nothing is guessed.
//
// This file is the part without game memory (tested: tests/lichii/boss_shooter_test.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopBossShooter {

namespace addr {
inline constexpr std::uint64_t kPrepare = 0x2eb1ed0;  // main+
inline constexpr std::uint64_t kShoot = 0x2eb2a40;
inline constexpr std::uint64_t kSleep = 0x2eb26b0;
// Prepare params (main+0x284ffe8 to 0x28500cc, the node's own build; main+0x2eb1ed0's reads).
inline constexpr std::size_t kPrepareSize = 0x1978;
inline constexpr std::uint64_t kPrepareKey = 0x0;     // u32
inline constexpr std::uint64_t kPrepareIndex = 0x14;  // s32, -1: the first free record
inline constexpr std::uint64_t kPrepareTail = 0x1930; // place (3 f32, NaN: the maker's), 3x3, scale, flags: to the end
// Shoot params. Size: at most 0x1998, the room the caller at main+0x3023608 leaves between the shoot params
// (sp+0x1930) and the prepare params after them (sp+0x32c8).
inline constexpr std::size_t kShootSize = 0x1998;
inline constexpr std::uint64_t kShootKey = 0x1928;    // u32
inline constexpr std::uint64_t kShootIndex = 0x192c;  // s32
inline constexpr std::uint64_t kShootTail = 0x1938;   // +0 u8, +1 u8 "that index", +0x10 3 f32, +0x1c f32 (NaN: none)
}  // namespace addr

inline constexpr std::size_t kPrepareTailSize = addr::kPrepareSize - addr::kPrepareTail;  // 0x48
inline constexpr std::size_t kShootTailSize = 0x20;                                       // to +0x1958
inline constexpr std::size_t kTailSize = kPrepareTailSize;
inline constexpr std::size_t kMaxShots = 8;  // in one sample

using Kind = coop::protocol::BossShooterKind;
constexpr bool ValidKind(std::uint8_t k) { return k >= 1 && k <= 3; }

// One call the owner's game took, and the owner's list (OBJECT_SAMPLE component 30, coop::protocol: `asked`, `index`,
// `tail` and `seq` as there).
using Shot = coop::protocol::BossShooterCall;
using Shots = coop::protocol::BossShooterCalls;
static_assert(kTailSize == coop::protocol::kBossShooterTail);
static_assert(kShootTailSize == coop::protocol::kBossShooterShootTail);
static_assert(kMaxShots == coop::protocol::kMaxBossShooterCalls);

// The owner's list after one more call (seq + 1, the oldest dropped past kMaxShots).
void Add(Shots& mine, Kind kind, std::uint32_t key, std::int32_t asked, std::int32_t index,
         const std::uint8_t* tail, std::size_t tail_size);

// Which of the owner's shots a copy makes next. `delivered`: the newest seq already made; `started`: false until
// this copy took a first sample from this owner. A first sample, an owner whose count is behind ours (a new owner)
// or a gap the list no longer covers: nothing old is made (a shot is a moment, not a state; bullets in flight end in
// their own game) and the copy goes on from the owner's newest (`delivered` set to it). Else every shot newer than
// `delivered`, in order.
struct Planned {
    std::vector<Shot> make;
    std::uint32_t delivered = 0;
    bool restarted = false;
};
Planned Plan(const Shots& owner, std::uint32_t delivered, bool started);

// What the wrap answers a game's own call (phase 0). A copy's own AI call is not run: prepare answers -1 (what the
// game answers when no record is free; the node at main+0x28500d0 then fails), shoot and sleep answer false. Our
// caller's own call, an owner's and an unsynced boss's run.
enum class Role : std::uint8_t { none, owner, copy };
struct Answer {
    bool skip = false;
    std::uint64_t x0 = 0;
};
Answer OwnCall(Role role, Kind kind, bool our_call);

// A params block as this game's own node built it: its bytes and where they were.
struct Template {
    std::uint64_t at = 0;
    std::vector<std::uint8_t> bytes;
};
// The bytes for a copy at `to`: every aligned 8-byte word that points into the old block points into the new one.
std::vector<std::uint8_t> Rebased(const Template& t, std::uint64_t to);
// The owner's shot written over a rebased block. False: the block is too small or the shot is of another kind.
// PatchPrepare asks the record the owner's caller asked (`asked`), never the owner's answer: each game numbers its
// own records. PatchShoot takes the record this game's prepare gave (`local_index`).
bool PatchPrepare(std::vector<std::uint8_t>& block, const Shot& shot);
bool PatchShoot(std::vector<std::uint8_t>& block, const Shot& shot, std::int32_t local_index);

// A copy's record for each of the owner's: (key, the owner's index) -> ours, from the prepares we ran.
class IndexMap {
public:
    void Took(std::uint32_t key, std::int32_t owner_index, std::int32_t local_index);
    // A shoot or sleep the owner asked for `owner_index` (< 0: the node's "first prepared one", passed on as is).
    // Nothing: we never prepared it (it was before we came, or our prepare was refused): the call is not made.
    std::optional<std::int32_t> Local(std::uint32_t key, std::int32_t owner_index) const;
    void Slept(std::uint32_t key, std::int32_t owner_index);
    void Clear();
    std::size_t Size() const { return map_.size(); }

private:
    std::map<std::pair<std::uint32_t, std::int32_t>, std::int32_t> map_;
};

}  // namespace CoopBossShooter

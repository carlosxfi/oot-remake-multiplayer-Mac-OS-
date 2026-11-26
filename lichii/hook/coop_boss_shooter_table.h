// SPDX-License-Identifier: GPL-3.0-or-later
//
// The owner's game decides a boss's shots (rows 222, 286): the part with game memory. coop_boss_shooter.h says what
// is sent and why; this table holds it per boss part, answers the wrap of the game's three Shooter verbs and makes
// the owner's shots on our copies through the same verbs, called by our caller on the game's thread.
//
// - Ours (owner): each call the game took (prepare: a record, shoot and sleep: true) is added to the part's list and
//   goes out in its samples. A shot stays in the list kKeepMs, then only its count is sent.
// - A copy: until the owner's first list comes (an older owner) its own AI's calls run, as before. From then on
//   they are answered "nothing free" and not run, and the owner's are made here, in order, each once.
// - Every call of a part's own AI (owner or copy) leaves its params block here (Template), so a copy has the block
//   of the node that would have shot. An owner's shot whose block this game's AI has not built yet waits kWaitMs
//   for it, then is counted as missed (nothing is guessed).
// - Hand-over, a new owner, or a gap in the owner's list: the records this game prepared for the old owner and did
//   not shoot are put back with the game's own sleep, and the copy goes on from the owner's newest shot.
// Solo, or a maker that is no synced boss part: nothing is read, kept or skipped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "coop_boss_shooter.h"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossShooter {

namespace addr {
inline constexpr std::uint64_t kMalloc = 0x14c6c90;         // main+, as coop_boss_orders.h
inline constexpr std::uint64_t kActorComponents = 0x270;    // an actor's shooter: [[actor + 0x270] + 0x3d8]
inline constexpr std::uint64_t kComponentShooter = 0x3d8;   // (coop_shooter_probe.h)
inline constexpr std::uint64_t kShooterOwner = 0x18;        // the shooter's actor (main+0x2eb1ef4)
}  // namespace addr

// Our block in guest memory (the game's malloc, once): the prepare params, then the shoot params.
inline constexpr std::size_t kBlockSize = addr::kPrepareSize + addr::kShootSize;

// Guest memory on the wrap's thread; false: not read.
using Reader = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

// A boss part the walk saw this round (coop_objects.cpp, with the native boss list). role: owner or copy.
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;
    std::string name;
};

struct Counts {
    std::uint64_t owner_kept = 0;     // our parts' calls the game took (sent)
    std::uint64_t owner_refused = 0;  // our parts' calls the game refused (not sent)
    std::uint64_t copy_skipped = 0;   // a copy's own AI calls not run (the owner's come)
    std::uint64_t copy_own_ran = 0;   // a copy's own calls run before the owner's first list came
    std::uint64_t made = 0;           // owner's calls made on a copy and taken by the game
    std::uint64_t refused = 0;        // made, and the game refused
    std::uint64_t no_template = 0;    // not made: this game's AI never built that key's block
    std::uint64_t not_prepared = 0;   // not made: a shoot or sleep of a record we never prepared
    std::uint64_t dropped = 0;        // not made: too many queued, or the part's shooter is gone
    std::uint64_t restarts = 0;       // a copy went on from the owner's newest (new owner, or a gap)
    std::uint64_t put_back = 0;       // prepared and never shot records put back with the game's sleep
};

// A game's own call as phase 0 saw it, for phase 1 (same thread).
struct Seen {
    bool valid = false;
    Kind kind = Kind::prepare;
    std::uint64_t actor = 0;
    std::uint32_t key = 0;
    std::int32_t asked = -1;
    std::array<std::uint8_t, kTailSize> tail{};
};

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossShooter;
    static constexpr std::size_t kMaxQueued = 16;
    static constexpr std::uint64_t kKeepMs = 3000;  // an owner's shot stays in its list this long
    static constexpr std::uint64_t kWaitMs = 1500;  // a copy waits this long for its own AI's block

    void SetOn(bool on);
    bool On() const;

    void Publish(std::span<const Member> members);

    // The wrap, phase 0, of prepare (x1 the params), shoot (x1 the params) and sleep (x1 the key, x2 the index):
    // x0 is the shooter. `module_call`: the thread runs one of the module's game calls. skip: the game's function
    // does not run and answers x0. `seen` is for OnDone when the function runs.
    Answer OnCall(const Reader& read, Kind kind, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2,
                  bool module_call, Seen* seen);
    // Phase 1: what the game answered a call that ran.
    void OnDone(const Seen& seen, std::uint64_t x0, std::uint64_t now_ms);

    // Ours: the part's list for its samples (nothing: not ours). A list with seq 0 is sent too.
    std::optional<Shots> OwnerShots(std::uint64_t actor, std::uint64_t now_ms) const;
    // A copy's: the owner's list from its samples.
    void Receive(std::uint64_t actor, const Shots& shots);

    // The caller (coop_callers.h contract).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base, std::uint64_t now_ms);

    void Reset();
    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();

private:
    struct Todo {
        Shot shot;
        bool put_back = false;      // our own sleep of a record prepared for an old owner (local is the record)
        std::int32_t local = -1;
        std::uint64_t since_ms = 0;  // waiting for a template since (0: not yet)
    };
    struct Kept {
        Template block;
        std::uint64_t shooter = 0;  // the shooter it was built for
    };
    using RecordKey = std::pair<std::uint32_t, std::int32_t>;  // key, the owner's index
    struct Entry {
        Member member;
        Shots mine;                           // ours
        std::deque<std::uint64_t> mine_at;    // when each of mine.recent was taken
        bool started = false;                 // a copy: the owner's list has come
        std::uint32_t planned = 0;            // a copy: the newest owner seq queued
        std::deque<Todo> queue;
        IndexMap records;                     // a copy: the owner's records -> ours
        std::map<RecordKey, std::int32_t> held;  // prepared for the owner and not shot yet -> our record
        std::map<std::pair<Kind, std::uint32_t>, Kept> templates;  // by kind and key, the newest
    };
    void Note(std::string text);   // (mutex_ held)
    void Restart(Entry& e);        // (mutex_ held)
    void DropGame();               // (mutex_ held)

    mutable std::mutex mutex_;
    bool on_ = false;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    Counts counts_{};
    std::vector<std::string> notes_;
    std::uint64_t main_base_ = 0;
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    bool malloc_failed_ = false;
    enum class Pending : std::uint8_t { none, malloc, call } pending_ = Pending::none;
    std::uint64_t pending_actor_ = 0;
    std::uint64_t in_flight_ = 0;  // the shooter our caller's call is for (0: none): that call runs
};

Table& TheTable();

}  // namespace CoopBossShooter

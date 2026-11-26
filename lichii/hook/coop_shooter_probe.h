// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 286 (the copy of a boss throws no bullet): a log-only probe of the game's Shooter component. It writes nothing,
// skips nothing and answers nothing. Off unless LICHII_COOP_SHOOTER_PROBE=1.
//
// The three functions it watches (hos_coop_boss_shooter.h, patch 0045; build e0f3ccfa):
// - main+0x2eb1ed0 prepare(shooter, prepare params) -> record index, < 0 refused. The only way into the function that
//   wakes a kept bullet (main+0x2eb4c30), so a count of 0 here means no bullet was woken at all;
// - main+0x2eb2a40 shoot(shooter, shoot params) -> bool;
// - main+0x2eb26b0 sleep(shooter, u32 key, s32 index) -> bool.
// Each call is logged with the maker, our role for it, the key's records before and after, each record's bullet and the
// parameters' words. The bosses of the walk are looked at a few times a second, and their shooters' records (and those
// of the bullets in them) logged when they change: what the copy's own shooter holds while the owner fires.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace CoopShooterProbe {

namespace addr {
inline constexpr std::uint64_t kPrepare = 0x2eb1ed0;  // main+
inline constexpr std::uint64_t kShoot = 0x2eb2a40;
inline constexpr std::uint64_t kSleep = 0x2eb26b0;
// An actor's shooter: [[actor + 0x270] + 0x3d8]; its map of records as coop_shot_reflect.h.
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kComponentShooter = 0x3d8;
inline constexpr std::uint64_t kShooterOwner = 0x18;  // the shooter's actor (main+0x2eb1ef4 reads its state)
inline constexpr std::uint64_t kShooterMap = 0x20;
inline constexpr std::uint64_t kShooterSlots = 0x2c;
inline constexpr std::uint64_t kMapEntrySize = 0x38;
inline constexpr std::uint64_t kEntryCount = 0x8;
inline constexpr std::uint64_t kEntryRecords = 0x10;
inline constexpr std::uint64_t kRecordSize = 0xf8;
inline constexpr std::uint64_t kRecordShot = 0x20;    // the bullet's handle
inline constexpr std::uint64_t kRecordState = 0xde;   // the state; +0xdf the asked one, +0xe0 bit 0: one is asked
inline constexpr std::uint64_t kHandleHolder = 0x8;
inline constexpr std::uint64_t kHolderActor = 0x40;
inline constexpr std::uint64_t kActorState = 0x2c;    // then +0x30 the request bits and +0x34
inline constexpr std::uint64_t kActorName = 0x260;
inline constexpr std::uint64_t kActorPosition = 0x31c;
// The parameters' words of interest.
inline constexpr std::uint64_t kPrepareIndex = 0x14;
inline constexpr std::uint64_t kPrepareTail = 0x1930;  // place, 3x3, scale, flags: to 0x1978
inline constexpr std::uint64_t kShootKey = 0x1928;
inline constexpr std::uint64_t kShootIndex = 0x192c;
inline constexpr std::uint64_t kShootUsesIndex = 0x1939;
}  // namespace addr

inline constexpr std::uint32_t kMaxSlots = 1024;
inline constexpr std::uint32_t kMaxRecords = 8;
inline constexpr std::size_t kTailWords = 9;  // qwords

// Guest memory; false: not read.
using Reader = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

enum class Fn : std::uint8_t { prepare, shoot, sleep };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    bool ours = false;
    std::uint8_t owner = 0;
    std::string name;
};

struct RecordView {
    std::uint8_t state = 0;
    std::uint8_t asked = 0;
    std::uint8_t is_asked = 0;
    std::uint64_t shot = 0;         // the bullet's actor (0: none)
    std::string name;               // the bullet's
    std::uint64_t vtable = 0;       // the bullet's
    std::uint32_t actor_state = 0;  // +0x2c
    std::uint32_t requests = 0;     // +0x30
    std::uint32_t more = 0;         // +0x34
    std::array<float, 3> place{};   // (not compared)
};
bool Same(const RecordView& a, const RecordView& b);  // all but the place

struct EntryView {
    std::uint32_t key = 0;
    std::uint32_t count = 0;
    std::vector<RecordView> records;  // at most kMaxRecords
};
bool Same(const EntryView& a, const EntryView& b);

std::uint64_t ShooterOf(const Reader& read, std::uint64_t actor);
// The entry of `key` in the shooter's map (nothing: no such key, or not read).
std::optional<EntryView> ReadEntry(const Reader& read, std::uint64_t shooter, std::uint32_t key);
// Every entry of the shooter's map.
std::vector<EntryView> ReadEntries(const Reader& read, std::uint64_t shooter);
std::string Text(const EntryView& entry, std::uint64_t main_base);

// A call as phase 0 saw it (kept by the caller until phase 1, on the same thread).
struct Call {
    Fn fn = Fn::prepare;
    std::uint64_t shooter = 0;
    std::uint64_t vtable = 0;  // the shooter's
    std::uint64_t params = 0;  // prepare, shoot: x1
    std::uint64_t from = 0;    // x30: where it returns to
    bool own_call = false;     // the module's own call
    std::uint32_t key = 0;
    std::int32_t index = -1;
    bool uses_index = true;                         // shoot: +0x1939
    std::uint64_t maker = 0;
    std::string maker_name;
    std::array<std::uint64_t, 4> head{};            // params +0
    std::array<std::uint64_t, 4> extra{};           // prepare: what params +8 points to
    std::array<std::uint64_t, kTailWords> tail{};   // prepare: +0x1930, shoot: +0x1928
    std::optional<EntryView> before;
};
// Phase 0 (x0-x2 as the function got them).
Call Begin(const Reader& read, Fn fn, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2, std::uint64_t from,
           bool own_call);

struct Counts {
    std::uint64_t calls = 0;
    std::uint64_t done = 0;     // prepare: index >= 0; shoot, sleep: true
    std::uint64_t refused = 0;
};

class Probe {
public:
    static constexpr std::size_t kMaxLines = 20000;      // in all
    static constexpr std::size_t kMaxLinesPerSec = 60;
    static constexpr std::uint64_t kLookMs = 200;
    static constexpr std::uint64_t kCountsMs = 10'000;
    static constexpr std::uint32_t kWordsFirst = 12;     // a maker's key's first calls come with the params' words
    static constexpr std::uint32_t kWordsEvery = 20;     // then each so many

    void SetOn(bool on);
    bool On() const;

    // The walk's bosses this round (its thread).
    void Publish(std::vector<Member> members);

    // Phase 1 (the game's thread): what the function answered.
    void End(const Reader& read, const Call& call, std::uint64_t x0, std::uint64_t main_base, std::uint64_t now_ms);

    // The frame hook (the game's thread): looks at the bosses' shooters every kLookMs.
    void Look(const Reader& read, std::uint64_t main_base, std::uint64_t now_ms);

    std::vector<std::string> TakeLines();
    // (by function and maker name)
    std::map<std::pair<Fn, std::string>, Counts> GetCounts() const;
    void Reset();

private:
    struct Seen {
        std::uint64_t shooter = 0;
        std::vector<EntryView> entries;
    };
    void Line(std::string text, std::uint64_t now_ms);  // (mutex_ held)
    std::string RoleText(std::uint64_t actor) const;    // (mutex_ held)
    void LookAt(const Reader& read, std::uint64_t actor, const std::string& who, std::uint64_t main_base,
                std::uint64_t now_ms, std::map<std::uint64_t, Seen>& seen, bool nested);  // (mutex_ held)

    mutable std::mutex mutex_;
    bool on_ = false;
    std::vector<Member> members_;
    std::map<std::pair<Fn, std::string>, Counts> counts_;
    std::map<std::string, std::uint32_t> words_;       // calls so far by function + maker + key
    std::string last_;                                 // the last call's text, for repeats
    std::uint64_t repeats_ = 0;
    std::map<std::uint64_t, Seen> seen_;               // by actor
    std::vector<std::string> lines_;
    std::size_t lines_total_ = 0;
    std::uint64_t second_ = 0;
    std::size_t lines_this_second_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t looked_ms_ = 0;
    std::uint64_t counts_ms_ = 0;
    std::uint64_t calls_total_ = 0;
    std::uint64_t calls_said_ = ~0ull;
};

Probe& TheProbe();

}  // namespace CoopShooterProbe

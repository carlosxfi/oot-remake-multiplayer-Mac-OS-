// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 98(2) result only (owner 07:40 UTC 2026-11-17): a room gimmick event another player played is never replayed in
// our game (a replay binds our Link: "movement stuck"); our room gets its RESULT only. Story scenes keep the replay
// (coop_shared_scenes.h).
//
// Where an event's result lives, read from the romfs room graphs by tools/puppet_pack/eye_switches.py --results (every
// asker, EventRequestActor or EventProxyRequestActor, of every event with a live place, coop_live_places.cpp):
// - class live: every room result of the asker reaches our room through a shared flag the room graph reads live, so
//   the other player's flags landing IS the result. Such an event's played flag is not held at all
//   (coop_dungeon.h HeldIn; it replaces the hand-made list of test 6: the Deku basement water and torches).
// - class asker: a result hangs on our asker's own outputs (EventBool, EventBoolEX1/EX2, EventFinish: what the event
//   flow's EventTriggerSetEventBool sets): an actor input, an unshared flag, or a flag the room reads only at its load
//   (UpdateOnce: the Deku ladder's RoomC_Cleared). Our asker's outputs are set with the game's own set-bool
//   (ExecuteLogicSetBool, coop_logic_set.h; the eye switch path's shape, coop_eye_switch.h), one output at a time in
//   table order, each until it reads back 1 for kApplyFrames frames. Then the event's held flags land (Hold below).
//   Asker not found, or an output that never stays 1 within kTryMs: the flags stay held until we leave the dungeon or
//   load (the safe floor of issue 129), one log line.
// - result guards: a shared flag that deletes a class-asker asker (Logic_Delete, read live). The other player's game
//   sets it during or at the end of its event; landing first, it would delete our asker before our result is set (the
//   Deku ladder never fell). It waits in its scene until our result is set this visit or our game played the event.
//
// Only in a session (the hold and the decider exist only there); nothing runs with co-op idle.
// No emulator code: tests/lichii/event_result_test.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop_eye_switch.h"
#include "coop_puppet.h"

namespace CoopEventResult {

// One asker of an event (generated). `outputs` is "" for class live, else the outputs to set joined by '+'.
struct Result {
    std::string_view event;  // without "HavePlayedEvent."
    std::string_view scene;
    std::array<float, 3> at;  // the asker's placement (x, height, z); 0,0,0 for an event a field asks (class live)
    std::string_view outputs;
};
std::span<const Result> Results();

struct Guard {
    std::string_view flag;
    std::string_view scene;
    std::string_view event;
};
std::span<const Guard> Guards();

// Every class-asker line of `event` in `scene`, nearest asker place to `link` first (table order without `link`).
std::vector<const Result*> AskerLines(std::string_view event, std::string_view scene,
                                      const std::optional<std::array<float, 3>>& link = std::nullopt);
// Whether every asker of `event` is class live (and it has at least one line): its played flag is not held.
bool IsLive(std::string_view event);
// The names in `outputs` ("EventBool+EventFinish"), in order.
std::vector<std::string_view> SplitOutputs(std::string_view outputs);

// Trigger guards (review-a38-5): the game's OWN asker of an event asks it when its EventRequestTrigger rises, and
// shared state can raise it: the other player's game plays the event and its state comes here (its shared flags, its
// keys, its kills), our asker asks the event too = a replay that binds our Link. Generated from every asker's trigger
// leaves (both classes; eye_switches.py --results, which also prints why every other leaf cannot rise from shared
// state). Each waits in its scene until our game has played the event (`HavePlayedEvent.<event>`: our asker is then
// disarmed; for class asker it lands once our result is set, for class live it lands live).
// - flag: a shared flag the trigger reads (not negated).
// - key: a trigger actor's shared placement key (a silver rupee counter's own key, an ice block's).
// - token: a key the counter `group` counts (its tokens). A split collection still works: only the LARGEST token
//   still missing here waits, the others land, so shared keys alone never complete our counter (no same-frame race:
//   the one that waits does not depend on which arrive). Our Link taking that token itself completes it: our own play.
// - kill: an actor whose death (Dead, IsDeadNotDelay, IsDeathEventPlay, a negated Logic_Alive) the trigger reads; the
//   object sync's kill or remove of the LARGEST live one of the event waits (KillKeepers), the others die.
// - follow: a world object coop_world follows (review of 70ba0763): the trigger actor itself (a hammer switch's press,
//   a hammer pillar's fall, a statue's or block's stop), a block, statue or rolling rock that can rest on a press
//   switch the trigger reads (one in the switch's room: each waits), or a block a trigger actor links (`group` = that
//   actor's placement hash: the Forest Temple box puzzle's four corners: only the LARGEST waits, the others land, so
//   another game's pushes alone never complete it; our Link pushing it is our own play). Another game's move, rest
//   place and fall of a keeper wait in coop_world (FollowKeepers) and land once our game has played the event.
struct Trigger {
    enum Kind : std::uint8_t { flag, key, token, kill, follow };
    Kind kind;
    std::string_view scene;
    std::string_view event;
    std::string_view name;  // kind flag: the flag's name
    std::uint64_t id;       // key, token: the placement key; kill, follow: the actor's placement hash
    std::uint64_t group;    // token: its counter's key; follow: the linking actor's hash (0: this one waits)
};
std::span<const Trigger> Triggers();

// What the flag hold does this frame (pure): `released` lands although held otherwise (the played flag, the result
// guards and the eye switch guards of every event whose result we set this visit); `waiting` is held (the result
// guards of `scene` whose event's result is not set here this visit and that our game has not played; the flag
// triggers of `scene` whose event our game has not played); `waiting_keys` are placement keys held (the key triggers
// and each counter's largest missing token, by `have_key`: our game's value of a key; none given: every token counts
// as missing).
struct Hold {
    std::vector<std::uint32_t> released;
    std::vector<std::uint32_t> waiting;
    std::vector<std::uint64_t> waiting_keys;
    bool Releases(std::uint32_t hash) const;
    bool Waits(std::uint32_t hash) const;
    bool WaitsKey(std::uint64_t key) const;
};
Hold HoldFor(std::string_view scene, const std::set<std::string>& applied,
             const std::function<bool(const std::string&)>& played,
             const std::function<bool(std::uint64_t)>& have_key = nullptr);

// The kill triggers' keepers in `scene` (pure): for each event our game has not played, the largest placement hash
// of its kill actors that `alive` says is alive in our walk. The object sync keeps a kill or remove of a keeper until
// it is no keeper any more (the event played here, or a scene change).
std::set<std::uint64_t> KillKeepers(std::string_view scene, const std::function<bool(std::uint64_t)>& alive,
                                    const std::function<bool(const std::string&)>& played);

// The follow triggers' keepers in `scene` (pure): for each event our game has not played, its follow actors of group
// 0 and the largest of each linked group. coop_world (WorldSync::SetTriggerKeepers) keeps another game's move, rest
// place and fall of a keeper until it is no keeper any more (the event played here, or a scene change).
std::set<std::uint64_t> FollowKeepers(std::string_view scene, const std::function<bool(const std::string&)>& played);

// Frames an output must read back 1 (after our set, or already) to count as set; and how long one event is tried.
inline constexpr int kApplyFrames = 3;
inline constexpr std::uint64_t kTryMs = 3000;

// The svc caller: one event's result at a time.
class Setter {
public:
    explicit Setter(CoopEyeSwitch::Finder find = CoopEyeSwitch::FindActors) : find_(std::move(find)) {}
    // The hook thread: set our asker's result of `event` in `scene`; `link` picks the nearest asker. False: no class
    // asker line (class live: its flags are the whole result; nothing to set).
    bool Want(const std::string& event, const std::string& scene,
              std::optional<std::array<float, 3>> link = std::nullopt);
    // The hook thread, once a frame: a new scene is a new visit (nothing set yet); a wanted event of another scene, or
    // one `played(event)` says our game has played, is dropped.
    void Visit(const std::string& scene, const std::function<bool(const std::string&)>& played = nullptr);
    // The events whose result was set this visit.
    std::set<std::string> Applied();
    // The calc thread (CoopSvc caller contract).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main, std::uint64_t now_ms);
    std::vector<std::string> TakeNotes();

private:
    struct Try {
        std::string event;
        std::string scene;
        std::optional<std::array<float, 3>> link;
        std::uint64_t until_ms = 0;  // 0: not begun
        std::uint64_t asker = 0;     // our asker, once found
        std::size_t output = 0;      // the output being set
        int ones = 0;                // frames it read 1 in a row
        int sets = 0;                // our set-bools of this output
        int zeros_after_set = 0;     // frames it read 0 after one of our sets (something rewrites it)
    };
    std::optional<CoopPuppet::Call> Frame(CoopPuppet::Guest& guest, std::uint64_t main, std::uint64_t now_ms);
    void End(const std::string& why, bool applied);
    static bool EventRunning(CoopPuppet::Guest& guest, std::uint64_t main);

    CoopEyeSwitch::Finder find_;
    std::mutex mutex_;
    std::deque<Try> wanted_;  // front: the one being set
    std::vector<std::string> notes_;
    std::string scene_;
    std::set<std::string> applied_;  // this visit
    std::set<std::string> ended_;    // this visit: tried and not set (asked again on the next visit)
    std::uint64_t block_ = 0;
    enum class Next { none, malloc, execute };
    Next next_ = Next::none;
    bool failed_ = false;  // the malloc gave nothing: nothing is set (the flags stay held)
};

}  // namespace CoopEventResult

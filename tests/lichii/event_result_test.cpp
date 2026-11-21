// SPDX-License-Identifier: GPL-3.0-or-later
// CoopEventResult (row 98(2), owner 07:40 UTC 2026-11-17): another player's room gimmick event gives our room its
// RESULT only (our asker's outputs, set with the game's set-bool), never a replay that binds our Link.
#include <doctest/doctest.h>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_event_result.h"
#include "coop_eye_switch.h"
#include "coop_live_events.h"
#include "coop_logic_probe.h"
#include "coop_logic_set.h"

namespace {
namespace p = CoopLogicProbe::addr;
namespace ls = CoopLogicSet::addr;

class FakeGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* b = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            b[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) bytes[address + i] = b[i];
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T v) {
        Write(address, &v, sizeof v);
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kMain = 0x7100000000;
constexpr std::uint64_t kAsker = 0x20000;
constexpr std::uint64_t kBlock = 0x9000;
const std::string kLadder = "InsideTheDekuTree_Gimmick_FallingLadder";
const std::string kDeku = "InsideTheDekuTree";
const std::string kWater = "InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop";

// Our asker's live logic: two bool output plugs, EventBool (root) and EventFinish (its right child), both 0.
struct Asker {
    std::uint64_t base = kAsker + 0x1000;
    std::uint64_t ValueAt(int plug) const { return base + 0x5000 + 0x100 * static_cast<std::uint64_t>(plug); }
    void Build(FakeGuest& g) const {
        g.Put<std::uint64_t>(kAsker + p::kActorLogic, base);
        g.Put<std::uint64_t>(base + p::kLogicInner, base + 0x1000);
        g.Put<std::uint64_t>(base + 0x1000 + p::kLogicState, 5);
        g.Put<std::uint64_t>(base + 0x1000 + p::kLogicTable, base + 0x2000);
        g.Put<std::uint64_t>(base + 0x2000 + p::kTableRootHolder, base + 0x3000);
        g.Put<std::uint64_t>(base + 0x3000 + p::kRoot, base + 0x4000);
        const std::uint32_t keys[] = {coop::protocol::flag_hash("EventBool"), coop::protocol::flag_hash("EventFinish")};
        for (int i = 0; i < 2; ++i) {
            const std::uint64_t node = base + 0x4000 + 0x100 * static_cast<std::uint64_t>(i);
            g.Put<std::uint32_t>(node + p::kKey, keys[i] ^ 7u);
            g.Put<std::uint8_t>(node + p::kType, CoopLogicProbe::kBool);
            g.Put<std::uint64_t>(node + p::kValue, ValueAt(i) | 2);
            g.Put<std::uint8_t>(ValueAt(i), 0);
        }
        g.Put<std::uint64_t>(base + 0x4000 + p::kRight, base + 0x4100);
    }
    // The plug index of a key the set-bool node names (the name's key), or -1.
    static int PlugOf(std::uint32_t key) {
        if (key == coop::protocol::flag_hash("EventBool")) return 0;
        if (key == coop::protocol::flag_hash("EventFinish")) return 1;
        return -1;
    }
};

// The svc cave for one frame: Step, run each call it asks as the game would, give back the result, until it asks
// nothing more. The game's set-bool writes 1 into the plug the node names, unless `sticks` is false (the asker's own
// AI writes it back to 0 at once).
struct Cave {
    FakeGuest g;
    Asker asker;
    bool present = true;
    bool sticks = true;
    int executes = 0;
    int mallocs = 0;
    CoopEyeSwitch::Finder Finder() {
        return [this](CoopPuppet::Guest&, std::uint64_t, std::span<const std::string_view> names,
                      const std::array<float, 3>&, float) {
            const bool asked = names.front() == "EventRequestActor";
            return asked && present ? std::vector<std::uint64_t>{kAsker} : std::vector<std::uint64_t>{};
        };
    }
    void Frame(CoopEventResult::Setter& s, std::uint64_t now_ms) {
        auto call = s.Step(g, false, 0, kMain, now_ms);
        for (int guard = 0; call && guard < 8; ++guard) {
            std::uint64_t result = 0;
            if (call->function == kMain + ls::kMalloc) {
                ++mallocs;
                result = kBlock;
            } else if (call->function == kMain + ls::kExecute) {
                ++executes;
                const int plug = Asker::PlugOf(g.Get<std::uint32_t>(kBlock + ls::kNodeKey));
                if (plug >= 0 && sticks) g.Put<std::uint8_t>(asker.ValueAt(plug), 1);
            }
            call = s.Step(g, true, result, kMain, now_ms);
        }
    }
    std::uint8_t Value(int plug) { return g.Get<std::uint8_t>(asker.ValueAt(plug)); }
};

bool Noted(const std::vector<std::string>& notes, const std::string& part) {
    for (const auto& n : notes) {
        if (n.find(part) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("event result: the classes from the room graphs") {
    CHECK(CoopEventResult::IsLive(kWater));  // the basement water: its flags are its result (player test 6)
    CHECK(CoopEventResult::IsLive("InsideTheDekuTree_Gimmick_RoomH_CandleStick"));
    CHECK_FALSE(CoopEventResult::IsLive(kLadder));  // the ladder hangs on our asker's EventBool
    CHECK_FALSE(CoopEventResult::IsLive("NotAnEvent"));  // unknown: not live (held as before)
    const auto lines = CoopEventResult::AskerLines(kLadder, kDeku);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front()->outputs == "EventBool+EventFinish");
    CHECK(CoopEventResult::AskerLines(kWater, kDeku).empty());
    CHECK(CoopEventResult::AskerLines(kLadder, "ForestTemple").empty());
    for (const auto& r : CoopEventResult::Results()) {
        CAPTURE(r.event);
        CHECK_FALSE(r.event.starts_with("HavePlayedEvent."));
        CHECK_FALSE(r.scene.empty());
    }
}

TEST_CASE("event result: the outputs split in order") {
    const auto two = CoopEventResult::SplitOutputs("EventBool+EventFinish");
    REQUIRE(two.size() == 2);
    CHECK(two[0] == "EventBool");
    CHECK(two[1] == "EventFinish");
    CHECK(CoopEventResult::SplitOutputs("EventBool").size() == 1);
    CHECK(CoopEventResult::SplitOutputs("").empty());
}

TEST_CASE("event result: the hold: a result guard waits until our result is set or we played it") {
    const auto cleared = coop::protocol::flag_hash("InsideTheDekuTree_RoomC_Cleared");
    const auto played_ladder = coop::protocol::flag_hash("HavePlayedEvent." + kLadder);
    const auto none = [](const std::string&) { return false; };

    auto h = CoopEventResult::HoldFor(kDeku, {}, none);
    CHECK(h.Waits(cleared));  // it would delete our asker before our result is set
    CHECK_FALSE(h.Releases(played_ladder));

    h = CoopEventResult::HoldFor(kDeku, {kLadder}, none);
    CHECK(h.Releases(played_ladder));  // our result is set: the played flag lands in the room
    CHECK(h.Releases(cleared));
    CHECK_FALSE(h.Waits(cleared));

    h = CoopEventResult::HoldFor(kDeku, {}, [&](const std::string& e) { return e == kLadder; });
    CHECK_FALSE(h.Waits(cleared));  // our game played it: our asker has run

    h = CoopEventResult::HoldFor("KokiriForest", {}, none);
    CHECK_FALSE(h.Waits(cleared));  // only in its scene
}

TEST_CASE("event result: the eye switch path's guards wait only for a class-asker event") {
    const auto none = [](const std::string&) { return false; };
    // GTG room 03's step switch (the trigger reads it negated): its shutter event hangs on our asker.
    const auto gtg = coop::protocol::flag_hash("GerudoTrainingGround_Dungeon_Room03_StepSwitch");
    REQUIRE_FALSE(CoopEventResult::AskerLines("GerudoTrainingGround_03_ShutterOpen", "GerudoTrainingGround").empty());
    CHECK(CoopEventResult::HoldFor("GerudoTrainingGround", {}, none).Waits(gtg));
    CHECK(CoopEventResult::HoldFor("GerudoTrainingGround", {"GerudoTrainingGround_03_ShutterOpen"}, none).Releases(gtg));
    // Forest Temple room 02's gold eye: its shutter event is class live, its guard no longer waits (it waited for the
    // retired replay).
    const auto gold = coop::protocol::flag_hash("ForestTemple_Room02_GoldEyeSwitch_On");
    REQUIRE(CoopEventResult::IsLive("ForestTemple_Room02_ShutterOpen"));
    CHECK_FALSE(CoopEventResult::HoldFor("ForestTemple", {}, none).Waits(gold));
}

TEST_CASE("event result: a class-live event has nothing to set") {
    Cave cave;
    CoopEventResult::Setter s(cave.Finder());
    CHECK_FALSE(s.Want(kWater, kDeku));
    cave.Frame(s, 100);
    CHECK(cave.mallocs == 0);  // nothing asked of the game
}

TEST_CASE("event result: our asker's EventBool then EventFinish, each read 1 for 3 frames, then set") {
    Cave cave;
    cave.asker.Build(cave.g);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 20 && !s.Applied().contains(kLadder); ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kLadder));
    CHECK(cave.mallocs == 1);  // one block, kept
    CHECK(cave.executes == 2);  // one set-bool per output
    CHECK(cave.Value(0) == 1);
    CHECK(cave.Value(1) == 1);
    const auto notes = s.TakeNotes();
    CHECK(Noted(notes, "no replay"));
    CHECK(Noted(notes, "set (EventBool+EventFinish)"));
    // Asked again this visit: nothing more is set.
    CHECK(s.Want(kLadder, kDeku));
    cave.Frame(s, now += 16);
    CHECK(cave.executes == 2);
}

TEST_CASE("event result: an output the asker writes back to 0 is not set, its flags stay held") {
    Cave cave;
    cave.asker.Build(cave.g);
    cave.sticks = false;
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 300; ++f) cave.Frame(s, now += 16);  // past kTryMs
    CHECK_FALSE(s.Applied().contains(kLadder));
    CHECK(Noted(s.TakeNotes(), "an output did not stay 1"));
    // Not tried again this visit; a new visit tries again.
    const int before = cave.executes;
    s.Want(kLadder, kDeku);
    cave.Frame(s, now += 16);
    CHECK(cave.executes == before);
    s.Visit("KokiriForest");
    s.Visit(kDeku);
    CHECK(s.Want(kLadder, kDeku));
    cave.Frame(s, now += 16);
    CHECK(cave.executes == before + 1);
}

TEST_CASE("event result: no asker here: nothing set, the flags stay held") {
    Cave cave;
    cave.present = false;
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 300; ++f) cave.Frame(s, now += 16);
    CHECK_FALSE(s.Applied().contains(kLadder));
    CHECK(cave.executes == 0);
    CHECK(Noted(s.TakeNotes(), "our asker not found"));
}

TEST_CASE("event result: our asker gone after our set (its graph took the result) counts as set") {
    Cave cave;
    cave.asker.Build(cave.g);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    cave.Frame(s, now += 16);  // malloc + the first set-bool
    REQUIRE(cave.executes == 1);
    cave.present = false;  // EventBool -> Logic_Delete: the room graph deleted our asker
    for (int f = 0; f < 5; ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kLadder));
    CHECK(Noted(s.TakeNotes(), "our asker is gone after our set"));
}

TEST_CASE("event result: nothing is set while an event of ours runs") {
    Cave cave;
    cave.asker.Build(cave.g);
    cave.g.Put<std::uint64_t>(kMain + CoopLiveEvents::addr::kEventStatePtr, 0x40000);
    cave.g.Put<std::uint64_t>(0x40000 + CoopLiveEvents::addr::kEventSession, 0x50000);
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    for (int f = 0; f < 10; ++f) cave.Frame(s, now += 16);
    CHECK(cave.executes == 0);
    cave.g.Put<std::uint64_t>(0x40000 + CoopLiveEvents::addr::kEventSession, 0);  // it ended
    for (int f = 0; f < 20; ++f) cave.Frame(s, now += 16);
    CHECK(s.Applied().contains(kLadder));
}

TEST_CASE("event result: a wanted event is dropped once our game played it or we left its scene") {
    Cave cave;
    cave.asker.Build(cave.g);
    cave.present = false;  // not loaded yet: it waits
    CoopEventResult::Setter s(cave.Finder());
    s.Visit(kDeku);
    REQUIRE(s.Want(kLadder, kDeku));
    std::uint64_t now = 1000;
    cave.Frame(s, now += 16);
    s.Visit(kDeku, [&](const std::string& e) { return e == kLadder; });  // our own asker ran it
    cave.present = true;
    for (int f = 0; f < 5; ++f) cave.Frame(s, now += 16);
    CHECK(cave.executes == 0);
    CHECK_FALSE(s.Applied().contains(kLadder));
}

namespace {
constexpr std::uint64_t kGtg02Counter = 0xb1f683aa91daf209ULL;
const std::vector<std::uint64_t> kGtg02Tokens = {0x0e741bc26e2c0697ULL, 0x1da71f60ac02ec12ULL, 0x4ab44ec9fa62841eULL,
                                                 0x77f9feaac7f28534ULL, 0xe09a166c192d8995ULL};
const std::vector<std::uint64_t> kFire15Slugs = {0x524d8f690c11944aULL, 0x7955892af6fcf0a2ULL, 0x8515bd766b65e010ULL,
                                                 0xa36a08d01e93953cULL, 0xf8119818b5e22a95ULL, 0xfc367bdaeb7524efULL};
}  // namespace

TEST_CASE("event result: the trigger table covers silver rupees, kills and shared flags (both classes)") {
    std::set<std::uint64_t> tokens;
    bool counter = false, flag = false, live_kill = false, jabu14 = false;
    for (const auto& t : CoopEventResult::Triggers()) {
        if (t.event == "GerudoTrainingGround_02_SilverToken_Unlock") {
            if (t.kind == CoopEventResult::Trigger::key) counter = counter || t.id == kGtg02Counter;
            if (t.kind == CoopEventResult::Trigger::token && t.group == kGtg02Counter) tokens.insert(t.id);
        }
        if (t.kind == CoopEventResult::Trigger::flag && t.name == "ForestTemple_DefeatAmy") flag = true;
        // g3-objects shares Jabu room 14's water flags: the upper switch's trigger reads one, so it waits too
        if (t.kind == CoopEventResult::Trigger::flag && t.name == "Gimmick_Insidejabujabu_Updown_MoveWater" &&
            t.event == "InsideJabuJabuBellyRoom14StepSwitch")
            jabu14 = true;
        // a class-live event's kills too (its played flag lands live, but our asker asks at the kill)
        if (t.kind == CoopEventResult::Trigger::kill && t.event == "GerudoTrainingGround_10_AppearTreasureBox") live_kill = true;
    }
    CHECK(counter);
    CHECK(tokens == std::set<std::uint64_t>(kGtg02Tokens.begin(), kGtg02Tokens.end()));
    CHECK(flag);
    CHECK(jabu14);
    CHECK(live_kill);
    CHECK(CoopEventResult::IsLive("GerudoTrainingGround_10_AppearTreasureBox"));
    // the silver rupee line is class asker: its result is set, and its trigger is covered too
    CHECK_FALSE(CoopEventResult::AskerLines("GerudoTrainingGround_02_SilverToken_Unlock", "GerudoTrainingGround").empty());
}

TEST_CASE("event result: shared silver rupees never complete our counter before our game played the event") {
    std::set<std::uint64_t> have;
    bool played = false;
    const auto is_played = [&](const std::string& e) {
        return played && e == "GerudoTrainingGround_02_SilverToken_Unlock";
    };
    const auto have_key = [&](std::uint64_t k) { return have.contains(k); };
    const auto hold = [&] { return CoopEventResult::HoldFor("GerudoTrainingGround", {}, is_played, have_key); };
    // nothing taken here: the counter's key and only the largest token wait; the other four land (a split collection)
    auto h = hold();
    CHECK(h.WaitsKey(kGtg02Counter));
    CHECK(h.WaitsKey(0xe09a166c192d8995ULL));
    for (std::size_t i = 0; i + 1 < kGtg02Tokens.size(); ++i) CHECK_FALSE(h.WaitsKey(kGtg02Tokens[i]));
    // the other player took four (they landed), ours never took the largest: it still waits, the counter is one short
    have.insert(kGtg02Tokens.begin(), kGtg02Tokens.end() - 1);
    CHECK(hold().WaitsKey(0xe09a166c192d8995ULL));
    // the largest landed first (our Link took it: our own key), then the last one missing waits instead
    have = {0xe09a166c192d8995ULL, 0x0e741bc26e2c0697ULL, 0x1da71f60ac02ec12ULL, 0x4ab44ec9fa62841eULL};
    h = hold();
    CHECK(h.WaitsKey(0x77f9feaac7f28534ULL));
    CHECK_FALSE(h.WaitsKey(0x0e741bc26e2c0697ULL));
    // our game played it (the played flag landed: our asker is disarmed): everything lands
    played = true;
    h = hold();
    CHECK_FALSE(h.WaitsKey(kGtg02Counter));
    CHECK_FALSE(h.WaitsKey(0x77f9feaac7f28534ULL));
    // another scene: nothing of it waits
    played = false;
    CHECK_FALSE(CoopEventResult::HoldFor("IceCavern", {}, is_played, have_key).WaitsKey(kGtg02Counter));
}

TEST_CASE("event result: a shared trigger flag waits until our game played the event, also once our result is set") {
    const auto amy = coop::protocol::flag_hash("ForestTemple_DefeatAmy");
    bool played = false;
    const auto is_played = [&](const std::string& e) {
        return played && e == "ForestTemple_Room25_CandleLight_ShutterOpen";
    };
    CHECK(CoopEventResult::HoldFor("ForestTemple", {}, is_played).Waits(amy));
    CHECK_FALSE(CoopEventResult::HoldFor("DodongoCavern", {}, is_played).Waits(amy));
    played = true;
    CHECK_FALSE(CoopEventResult::HoldFor("ForestTemple", {}, is_played).Waits(amy));
    // a class-asker event's counter: our result set this visit but the played flag not landed yet: still waits
    const auto h = CoopEventResult::HoldFor("GerudoTrainingGround", {"GerudoTrainingGround_02_SilverToken_Unlock"},
                                            [](const std::string&) { return false; });
    CHECK(h.Releases(coop::protocol::flag_hash("HavePlayedEvent.GerudoTrainingGround_02_SilverToken_Unlock")));
    CHECK(h.WaitsKey(kGtg02Counter));
}

TEST_CASE("event result: the kill keeper: the largest live enemy of an unplayed event waits, the others die") {
    std::set<std::uint64_t> alive(kFire15Slugs.begin(), kFire15Slugs.end());
    bool played = false;
    const auto is_played = [&](const std::string& e) { return played && e == "FireTemple_15_OpenShutter"; };
    const auto keepers = [&] {
        return CoopEventResult::KillKeepers("FireTemple", [&](std::uint64_t h) { return alive.contains(h); }, is_played);
    };
    auto k = keepers();
    CHECK(k.contains(0xfc367bdaeb7524efULL));
    for (std::size_t i = 0; i + 1 < kFire15Slugs.size(); ++i) CHECK_FALSE(k.contains(kFire15Slugs[i]));
    // our Link killed the largest itself: the next largest live one keeps the event
    alive.erase(0xfc367bdaeb7524efULL);
    k = keepers();
    CHECK(k.contains(0xf8119818b5e22a95ULL));
    CHECK_FALSE(k.contains(0xfc367bdaeb7524efULL));
    // played here: nothing is kept; another scene: nothing either
    played = true;
    CHECK(keepers().empty());
    played = false;
    CHECK(CoopEventResult::KillKeepers("WaterTemple", [&](std::uint64_t h) { return alive.contains(h); }, is_played)
              .count(0xf8119818b5e22a95ULL) == 0);
}

TEST_CASE("event result: the follow keepers: blocks on press switches wait, the box puzzle's largest corner waits") {
    std::set<std::string> played;
    const auto is_played = [&](const std::string& e) { return played.contains(e); };
    // Forest Temple Dm_ForestTemple_0014 (ForestTemple_MovableBox_Puzzle_Manager.CorrectPos): its four corners; only
    // the largest waits, so another game's pushes alone never complete our puzzle.
    constexpr std::array<std::uint64_t, 4> kCorners{0x1f0b64dac1bb1499ULL, 0x794f8e0396597b84ULL,
                                                    0x9340d4ba68f5feccULL, 0xebddc3cef6e36708ULL};
    auto k = CoopEventResult::FollowKeepers("ForestTemple", is_played);
    CHECK(k.contains(0xebddc3cef6e36708ULL));
    for (std::size_t i = 0; i + 1 < kCorners.size(); ++i) CHECK_FALSE(k.contains(kCorners[i]));
    // Dodongo's Cavern room 19: every block in the press switch's room waits (any of them can rest on it).
    constexpr std::array<std::uint64_t, 4> kRoom19{0x0f6a348ed7daaef4ULL, 0x244fd0ef8c5e5e01ULL,
                                                   0x641bc828409e0c24ULL, 0xfc81551012767634ULL};
    k = CoopEventResult::FollowKeepers("DodongoCavern", is_played);
    for (const auto h : kRoom19) CHECK(k.contains(h));
    CHECK(k.contains(0x8342f68da2a61311ULL));  // room 12's statue
    // A hammer switch (Spirit Temple room 8): the switch itself.
    CHECK(CoopEventResult::FollowKeepers("SpiritTemple", is_played).contains(0xcb61a9de736c9e1cULL));
    // Played here: that event's keepers go, the others stay; another scene: none of them.
    played.insert("DodongoCavern_Gimmick_SwitchShutter_Room19");
    k = CoopEventResult::FollowKeepers("DodongoCavern", is_played);
    for (const auto h : kRoom19) CHECK_FALSE(k.contains(h));
    CHECK(k.contains(0x8342f68da2a61311ULL));
    CHECK_FALSE(CoopEventResult::FollowKeepers("WaterTemple", is_played).contains(0xebddc3cef6e36708ULL));
    // The flag hold never takes a follow line (the world sync holds them).
    const auto h = CoopEventResult::HoldFor("ForestTemple", {}, is_played);
    for (const auto c : kCorners) CHECK_FALSE(h.WaitsKey(c));
    std::size_t follows = 0;
    for (const auto& t : CoopEventResult::Triggers()) follows += t.kind == CoopEventResult::Trigger::follow;
    CHECK(follows == 22);
}

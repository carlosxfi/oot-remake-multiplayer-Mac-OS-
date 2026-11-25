// SPDX-License-Identifier: GPL-3.0-or-later
// Row 149: an actor's blackboard move state goes with the actor (coop_brain_state.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "coop_brain_state.h"

using namespace CoopBrainState;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kDodongo = 0x1000000;
constexpr std::uint64_t kBoard = 0x2000000;
constexpr std::uint64_t kBlock = 0x3000000;
constexpr char kKing[] = "Enemy_DungeonBoss_Dodongo";
constexpr char kCorner[] = "DungeonBossDodongo_CurrentTargetCornerNum";
constexpr char kReverse[] = "DungeonBossDodongo_IsReverse";
constexpr char kSpawn[] = "DungeonBossDodongo_IsAfterRollingSpawn";
// Their numbers on the wire: the hash of the key's name.
constexpr std::uint32_t kCornerId = KeyId(kCorner);
constexpr std::uint32_t kReverseId = KeyId(kReverse);
constexpr std::uint32_t kSpawnId = KeyId(kSpawn);
static_assert(kCornerId == 0x44f0ae37u && kReverseId == 0x9419f2ceu && kSpawnId == 0xaa89a18fu);

BrainValue C(std::int32_t corner) { return {kCornerId, corner}; }
BrainValue R(std::int32_t reverse) { return {kReverseId, reverse}; }
BrainValue S(std::int32_t spawn) { return {kSpawnId, spawn}; }
// A component as it goes on the wire: its values by rising key.
BrainState State(std::vector<BrainValue> values) {
    std::sort(values.begin(), values.end(), [](const BrainValue& a, const BrainValue& b) { return a.key < b.key; });
    return BrainState{std::move(values)};
}
std::vector<std::string> Sorted(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    return names;
}

using Named = std::vector<std::pair<std::string, std::int32_t>>;

class FakeGuest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            o[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[address + i] = b[i];
        }
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, T value) {
        Write(address, &value, sizeof value);
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T v{};
        Read(address, &v, sizeof v);
        return v;
    }
    // An actor's blackboard as the game links it, [[[actor + 0x270]] + 0x10], with its s32 and bool groups.
    void Blackboard(std::uint64_t actor, std::uint64_t bb, const Named& s32s, const Named& bools) {
        Put<std::uint64_t>(actor + addr::kActorComponents, bb + 0x1000);
        Put<std::uint64_t>(bb + 0x1000, bb + 0x1100);
        Put<std::uint64_t>(bb + 0x1100 + addr::kHolderBlackboard, bb);
        Group(bb, s32s, addr::kS32NameCount, addr::kS32Names, addr::kS32ValueCount, addr::kS32Values, 0x10000, 4);
        Group(bb, bools, addr::kBoolNameCount, addr::kBoolNames, addr::kBoolValueCount, addr::kBoolValues, 0x40000,
              1);
    }
    std::int32_t S32(std::uint64_t bb, std::uint32_t index) {
        return Get<std::int32_t>(Get<std::uint64_t>(bb + addr::kS32Values) + index * 4);
    }
    bool Bool(std::uint64_t bb, std::uint32_t index) {
        return Get<std::uint8_t>(Get<std::uint64_t>(bb + addr::kBoolValues) + index) != 0;
    }
    void SetS32(std::uint64_t bb, std::uint32_t index, std::int32_t value) {
        Put<std::int32_t>(Get<std::uint64_t>(bb + addr::kS32Values) + index * 4, value);
    }
    void SetBool(std::uint64_t bb, std::uint32_t index, bool value) {
        Put<std::uint8_t>(Get<std::uint64_t>(bb + addr::kBoolValues) + index, value ? 1 : 0);
    }
    // The game's malloc and its two setters (main+0x264f170, main+0xd6cb40): the key by its name, a locked key
    // left alone, the value from where x2 points.
    std::uint64_t Invoke(const CoopPuppet::Call& c) {
        if (c.function == game + addr::kMalloc) {
            ++mallocs;
            return malloc_gives;
        }
        const bool is_s32 = c.function == game + addr::kSetS32;
        REQUIRE((is_s32 || c.function == game + addr::kSetBool));
        const std::uint64_t bb = c.x[0];
        const std::string key = Text(Get<std::uint64_t>(c.x[1]));
        const auto count = Get<std::int32_t>(bb + (is_s32 ? addr::kS32NameCount : addr::kBoolNameCount));
        const auto names = Get<std::uint64_t>(bb + (is_s32 ? addr::kS32Names : addr::kBoolNames));
        for (std::int32_t i = 0; i < count; ++i) {
            if (Text(Get<std::uint64_t>(names + static_cast<std::uint64_t>(i) * 8)) != key) {
                continue;
            }
            sets.push_back(key);
            if (locked.count(key) != 0) {
                return 0;
            }
            if (is_s32) {
                SetS32(bb, static_cast<std::uint32_t>(i), Get<std::int32_t>(c.x[2]));
            } else {
                SetBool(bb, static_cast<std::uint32_t>(i), Get<std::uint8_t>(c.x[2]) != 0);
            }
            return 0;
        }
        FAIL("a setter was asked for a key its blackboard does not have: " << key);
        return 0;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
    std::uint64_t game = kMain;  // the game the calls must be of
    std::uint64_t malloc_gives = kBlock;
    int mallocs = 0;
    std::vector<std::string> sets;   // the keys the setters were asked for, in order
    std::set<std::string> locked;

private:
    std::string Text(std::uint64_t at) {
        std::string out;
        for (char ch = Get<char>(at); ch != '\0'; ch = Get<char>(++at)) {
            out.push_back(ch);
        }
        return out;
    }
    void Group(std::uint64_t bb, const Named& keys, std::uint64_t name_count, std::uint64_t names,
               std::uint64_t value_count, std::uint64_t values, std::uint64_t at, std::uint64_t value_size) {
        Put<std::int32_t>(bb + name_count, static_cast<std::int32_t>(keys.size()));
        Put<std::int32_t>(bb + value_count, static_cast<std::int32_t>(keys.size()));
        Put<std::uint64_t>(bb + names, bb + at);
        Put<std::uint64_t>(bb + values, bb + at + 0x8000);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const std::uint64_t text = bb + at + 0x10000 + i * 0x80;
            Put<std::uint64_t>(bb + at + i * 8, text);
            Write(text, keys[i].first.c_str(), keys[i].first.size() + 1);
            if (value_size == 4) {
                Put<std::int32_t>(bb + at + 0x8000 + i * 4, keys[i].second);
            } else {
                Put<std::uint8_t>(bb + at + 0x8000 + i, static_cast<std::uint8_t>(keys[i].second));
            }
        }
    }
};

// King Dodongo's blackboard with other keys around his (the corner is s32 index 2, the way round bool index 3,
// the spawn bool index 1), and a longer key that starts as IsReverse before it.
void King(FakeGuest& g, std::int32_t corner, bool reverse, bool spawn = false) {
    g.Blackboard(kDodongo, kBoard,
                 {{"DodongoCommon_LastEatTargetPerceptionID", -1}, {"Other", 7}, {kCorner, corner}},
                 {{"DungeonBossDodongo_IsReverseCondition", 1},
                  {kSpawn, spawn ? 1 : 0},
                  {"DungeonBossDodongo_IsReachedCorner", 0},
                  {kReverse, reverse ? 1 : 0}});
}

// One frame of the caller as the runtime runs it; returns the calls made.
std::vector<CoopPuppet::Call> Frame(Table& t, FakeGuest& g, std::uint64_t base = kMain) {
    std::vector<CoopPuppet::Call> calls;
    std::optional<CoopPuppet::Call> c = t.Step(g, false, 0, base);
    while (c) {
        REQUIRE(calls.size() < 64);
        calls.push_back(*c);
        c = t.Step(g, true, g.Invoke(*c), base);
    }
    return calls;
}

Member Ours(std::uint64_t actor = kDodongo, const char* name = kKing) { return {actor, Role::owner, 1, name}; }
Member Theirs(std::uint64_t actor = kDodongo, const char* name = kKing) { return {actor, Role::copy, 2, name}; }

void Publish(Table& t, Member m) { t.Publish(std::vector<Member>{std::move(m)}); }

}  // namespace

TEST_CASE("row 149: keys are named on the wire by the hash of their name, and King Dodongo's roll corner is one") {
    // Murmur3 x86_32, seed 0 (the reference values).
    CHECK(KeyId("") == 0u);
    CHECK(KeyId("abc") == 0xb3dd93fau);
    CHECK(KeyId("Hello, world!") == 0xc0363e43u);
    CHECK(KeyId(kCorner) == 0x44f0ae37u);
    for (const Key& k : kKeys) {
        CHECK(k.id == KeyId(k.name));
        const std::vector<Key> of = KeysOf(k.boss);
        CHECK(of.size() <= kMaxBrainValues);
        std::set<std::uint32_t> ids;
        for (std::size_t i = 0; i < of.size(); ++i) {
            CHECK(ids.insert(of[i].id).second);         // no two keys of an actor share a hash
            CHECK((i == 0 || of[i - 1].id < of[i].id)); // the wire's order
        }
    }
    const std::vector<Key> king = KeysOf(kKing);
    REQUIRE(king.size() == 3);
    const auto kind_of = [&king](std::string_view name) -> std::optional<Kind> {
        for (const Key& k : king) {
            if (k.name == name) return k.kind;
        }
        return std::nullopt;
    };
    CHECK(kind_of(kCorner) == Kind::s32);
    CHECK(kind_of(kReverse) == Kind::boolean);
    CHECK(kind_of(kSpawn) == Kind::boolean);
    // Every boss whose AI keeps move state in its blackboard (Twinrova's is carried as orders, row 222).
    for (const char* boss : {"Enemy_DungeonBoss_Volvagia", "Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_PhantomGanon",
                             "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Morpha",
                             "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_Ganon",
                             "Enemy_DungeonBoss_Barinade"}) {
        CAPTURE(boss);
        CHECK_FALSE(KeysOf(boss).empty());
    }
    CHECK(KeysOf("Enemy_Keese").empty());
    CHECK(KeysOf("Enemy_DungeonBoss_Dodongo_Attention").empty());  // (the exact name only)
}

TEST_CASE("row 149: the plan puts in only what differs, only carried and read keys, bools as 0 or 1") {
    const std::vector<Key> king = KeysOf(kKing);
    const BrainState ours = State({C(0), R(0), S(1)});
    CHECK(State(Plan(king, State({C(2), R(1), S(1)}), ours)) == State({C(2), R(1)}));
    CHECK(Plan(king, ours, ours).empty());
    // A bool that came as another non-zero value is true; a key of another actor or of a newer build is passed over.
    const BrainValue other{KeyId("DungeonBossVolvagia_BattleState"), 5};
    CHECK(State(Plan(king, State({R(7), other}), ours)) == State({R(1)}));
    CHECK(Plan(king, State({S(255)}), ours).empty());
    // A key our game could not read is left alone.
    CHECK(State(Plan(king, State({C(2), R(1)}), State({R(0)}))) == State({R(1)}));
}

TEST_CASE("row 149: on the wire the values are component 25's keyed values of the actor itself, a full list") {
    using coop::protocol::KeyedValue;
    using coop::protocol::KeyedValues;
    const BrainState state = State({C(2), R(1), S(0)});
    const KeyedValues keyed = ToKeyed(state);
    CHECK(keyed.full);
    REQUIRE(keyed.values.size() == 3);
    for (std::size_t i = 0; i < keyed.values.size(); ++i) {
        CHECK(keyed.values[i].owner == 0);  // the actor itself
        CHECK(std::holds_alternative<std::int32_t>(keyed.values[i].value));
        CHECK((i == 0 || keyed.values[i - 1].name < keyed.values[i].name));  // the wire's order
    }
    CHECK(FromKeyed(keyed) == state);
    // From the wire: a part's value (another owner), another type and a remove are passed over.
    KeyedValues in = keyed;
    in.values.push_back(KeyedValue{0x1234, kCornerId, std::int32_t{9}});
    in.values.push_back(KeyedValue{0, 0x11111111u, 1.5f});
    in.values.push_back(KeyedValue{0, 0x22222222u, coop::protocol::KeyedRemove{}});
    in.values.push_back(KeyedValue{0, 0x33333333u, std::uint64_t{77}});
    CHECK(FromKeyed(in) == state);
    CHECK(FromKeyed(KeyedValues{}) == BrainState{});
    CHECK(ToKeyed(BrainState{}).values.empty());
}

TEST_CASE("row 149: the owner's game reads its boss's move state for its samples") {
    Table t;
    FakeGuest g;
    King(g, 3, true);
    Publish(t, Ours());
    CHECK_FALSE(t.OwnerBoard(kDodongo));  // (not read yet)
    const auto calls = Frame(t, g);
    REQUIRE(calls.size() == 1);  // the value block, made once
    CHECK(calls[0].function == kMain + addr::kMalloc);
    CHECK(t.OwnerBoard(kDodongo) == State({C(3), R(1), S(0)}));
    // The game moves on: the next frame's read follows it, with no call.
    g.SetS32(kBoard, 2, 0);
    g.SetBool(kBoard, 3, false);
    CHECK(Frame(t, g).empty());
    CHECK(t.OwnerBoard(kDodongo) == State({C(0), R(0), S(0)}));
    CHECK(g.sets.empty());
    CHECK(t.GetCounts().handed == 0);
}

TEST_CASE("row 149: King Dodongo made ours rolls on from his old owner's corner, not our stale one") {
    Table t;
    FakeGuest g;
    // Our copy counted corners by itself: corner 0, the other way round. The owner's is at corner 2, reversed.
    King(g, 0, false);
    Publish(t, Theirs());
    t.Receive(kDodongo, State({C(2), R(1), S(0)}));
    // A copy: nothing of its own game is changed (only the block is made, ahead of the hand-over).
    CHECK(Frame(t, g).size() == 1);
    CHECK(Frame(t, g).empty());
    CHECK(g.sets.empty());
    CHECK(g.S32(kBoard, 2) == 0);
    CHECK_FALSE(t.OwnerBoard(kDodongo));  // (a copy sends nothing)

    // Made ours: until the values are in, his samples carry the old owner's, never our stale corner.
    Publish(t, Ours());
    CHECK(t.OwnerBoard(kDodongo) == State({C(2), R(1), S(0)}));
    // The same frame: both go in through the game's own setters, by the blackboard's own name pointers.
    const auto calls = Frame(t, g);
    REQUIRE(calls.size() == 2);
    const bool corner_first = kCornerId < kReverseId;  // (the wire's order: by key hash)
    const CoopPuppet::Call& corner = calls[corner_first ? 0 : 1];
    const CoopPuppet::Call& reverse = calls[corner_first ? 1 : 0];
    CHECK(corner.function == kMain + addr::kSetS32);
    CHECK(corner.x[0] == kBoard);
    CHECK(corner.x[1] == g.Get<std::uint64_t>(kBoard + addr::kS32Names) + 2 * 8);
    CHECK(corner.x[2] == kBlock);
    CHECK_FALSE(corner.copy_game);
    CHECK(reverse.function == kMain + addr::kSetBool);
    CHECK(reverse.x[0] == kBoard);
    CHECK(reverse.x[1] == g.Get<std::uint64_t>(kBoard + addr::kBoolNames) + 3 * 8);
    CHECK(Sorted(g.sets) == Sorted({kCorner, kReverse}));
    CHECK(g.S32(kBoard, 2) == 2);
    CHECK(g.Bool(kBoard, 3));
    CHECK(g.Bool(kBoard, 0));        // (the longer key that starts the same: untouched)
    CHECK(g.S32(kBoard, 1) == 7);    // (no other key)
    const Counts c = t.GetCounts();
    CHECK(c.handed == 1);
    CHECK(c.put == 2);
    CHECK(c.same == 1);
    CHECK(c.refused == 0);
    // From here on it is our game's: read each frame, nothing put in again.
    g.SetS32(kBoard, 2, 3);
    CHECK(Frame(t, g).empty());
    CHECK(t.OwnerBoard(kDodongo) == State({C(3), R(1), S(0)}));
    t.Receive(kDodongo, State({C(2), R(1), S(0)}));  // (the same again: nothing)
    CHECK(Frame(t, g).empty());
    CHECK(g.S32(kBoard, 2) == 3);
    CHECK(t.GetCounts().handed == 1);
}

TEST_CASE("row 149: a copy follows the owner's newest values, and a hand-over state after the news goes in too") {
    Table t;
    FakeGuest g;
    King(g, 0, false);
    Publish(t, Theirs());
    t.Receive(kDodongo, State({C(1), R(0)}));
    t.Receive(kDodongo, State({C(2), R(0)}));  // the newest counts
    (void)Frame(t, g);
    Publish(t, Ours());
    (void)Frame(t, g);
    CHECK(g.S32(kBoard, 2) == 2);
    CHECK(g.sets == std::vector<std::string>{kCorner});
    // The server's hand-over state came after the owner news, one corner on.
    t.Receive(kDodongo, State({C(3), R(1)}));
    CHECK(t.OwnerBoard(kDodongo) == State({C(3), R(1)}));
    (void)Frame(t, g);
    CHECK(g.S32(kBoard, 2) == 3);
    CHECK(g.Bool(kBoard, 3));
    CHECK(t.GetCounts().handed == 2);
    CHECK(t.GetCounts().put == 3);
}

TEST_CASE("row 149: no move state from the old owner (an older build): our own stays and is sent") {
    Table t;
    FakeGuest g;
    King(g, 1, true);
    Publish(t, Theirs());
    (void)Frame(t, g);
    Publish(t, Ours());
    CHECK(Frame(t, g).empty());
    CHECK(g.sets.empty());
    CHECK(t.OwnerBoard(kDodongo) == State({C(1), R(1), S(0)}));
    CHECK(t.GetCounts().handed == 0);
    // An empty component changes nothing either.
    Publish(t, Theirs());
    t.Receive(kDodongo, BrainState{});
    Publish(t, Ours());
    CHECK(Frame(t, g).empty());
    CHECK(g.sets.empty());
}

TEST_CASE("row 149: a key the game holds locked is left to the game, once") {
    Table t;
    FakeGuest g;
    King(g, 0, false);
    g.locked.insert(kCorner);
    Publish(t, Theirs());
    t.Receive(kDodongo, State({C(2), R(1)}));
    (void)Frame(t, g);
    Publish(t, Ours());
    CHECK(Frame(t, g).size() == 2);
    CHECK(g.S32(kBoard, 2) == 0);  // the game's own rule
    CHECK(g.Bool(kBoard, 3));
    CHECK(t.GetCounts().refused == 1);
    CHECK(t.GetCounts().put == 1);
    CHECK(Frame(t, g).empty());  // not asked again
    CHECK(t.OwnerBoard(kDodongo) == State({C(0), R(1), S(0)}));
}

TEST_CASE("row 149: a call the runtime did not make is asked again, and with no block no stale values are sent") {
    Table t;
    FakeGuest g;
    King(g, 0, false);
    g.malloc_gives = 0;  // the game's malloc gives nothing yet
    Publish(t, Theirs());
    t.Receive(kDodongo, State({C(2), R(0)}));
    CHECK(Frame(t, g).size() == 1);
    Publish(t, Ours());
    CHECK(Frame(t, g).size() == 1);  // asked again, nothing put in
    CHECK(g.sets.empty());
    CHECK(t.OwnerBoard(kDodongo) == State({C(2), R(0)}));  // still the old owner's
    g.malloc_gives = kBlock;
    // A call the runtime did not make (its limit) is asked again the next frame.
    REQUIRE(t.Step(g, false, 0, kMain));
    CHECK(g.S32(kBoard, 2) == 0);
    const auto calls = Frame(t, g);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kMain + addr::kMalloc);
    CHECK(calls[1].function == kMain + addr::kSetS32);
    CHECK(g.S32(kBoard, 2) == 2);
    CHECK(t.GetCounts().put == 1);
}

TEST_CASE("row 149: an actor with no carried key, a boss with no blackboard, and a new game") {
    Table t;
    FakeGuest g;
    t.Publish(std::vector<Member>{Ours(0x5000000, "Enemy_Keese")});
    CHECK(Frame(t, g).empty());  // nothing listed: not even the block
    CHECK(g.mallocs == 0);
    CHECK_FALSE(t.OwnerBoard(0x5000000));
    // King Dodongo before his blackboard is made: told once, read when it is there.
    Publish(t, Ours());
    CHECK(Frame(t, g).size() == 1);
    CHECK_FALSE(t.OwnerBoard(kDodongo));
    (void)Frame(t, g);
    CHECK(t.GetCounts().unread == 1);
    King(g, 2, false);
    (void)Frame(t, g);
    CHECK(t.OwnerBoard(kDodongo) == State({C(2), R(0), S(0)}));
    // A blackboard without his keys (another build's): nothing carried, nothing written.
    FakeGuest other;
    Table t2;
    other.Blackboard(kDodongo, kBoard, {{"Other", 1}}, {{"OtherFlag", 1}});
    Publish(t2, Theirs());
    t2.Receive(kDodongo, State({C(2), R(1)}));
    (void)Frame(t2, other);
    Publish(t2, Ours());
    (void)Frame(t2, other);
    CHECK(other.sets.empty());
    CHECK(t2.GetCounts().unread == 1);
    // A new game process: the block and the bosses were the old one's.
    const int before = g.mallocs;
    g.game = kMain + 0x10000;
    (void)Frame(t, g, kMain + 0x10000);
    CHECK_FALSE(t.OwnerBoard(kDodongo));
    Publish(t, Ours());
    const auto calls = Frame(t, g, kMain + 0x10000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kMain + 0x10000 + addr::kMalloc);
    CHECK(g.mallocs == before + 1);
    t.Reset();
    CHECK(t.GetCounts().put == 0);
    CHECK(t.TakeNotes().empty());
}

TEST_CASE("row 149: the hand-over goes before the boss table's caller") {
    CHECK(CoopCallerOrders::kBrainState < CoopCallerOrders::kBossTable);
    CHECK(Table::kCallerOrder == CoopCallerOrders::kBrainState);
}

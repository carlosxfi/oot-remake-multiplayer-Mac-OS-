// SPDX-License-Identifier: GPL-3.0-or-later
// Rows 222 and 286: the table that makes the owner's boss shots on a copy (coop_boss_shooter_table.h), with a fake
// guest memory.
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <vector>

#include "coop_boss_shooter_table.h"

using namespace CoopBossShooter;

namespace {

constexpr std::uint64_t kMain = 0x8000000;
constexpr std::uint64_t kBoss = 0x1000000;
constexpr std::uint64_t kShooter = 0x1100000;
constexpr std::uint64_t kParams = 0x2000000;  // the game's own node's params (stack in the game)
constexpr std::uint64_t kBlock = 0x3000000;   // our block (the game's malloc)
constexpr std::uint32_t kLightBall = 0x22c6d8cd;  // Phantom Ganon's (reports/row-286-wake.md)
constexpr std::uint32_t kOther = 0x1234;

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
    // The boss's shooter as the game links it: [[actor + 0x270] + 0x3d8], whose maker at +0x18 is the actor.
    void Boss(std::uint64_t actor, std::uint64_t shooter) {
        Put<std::uint64_t>(actor + addr::kActorComponents, actor + 0x800);
        Put<std::uint64_t>(actor + 0x800 + addr::kComponentShooter, shooter);
        Put<std::uint64_t>(shooter + addr::kShooterOwner, actor);
    }
    // A prepare params block the boss's node built: key, the asked index and a tail word.
    void Prepare(std::uint64_t at, std::uint32_t key, std::int32_t asked, float x) {
        Put<std::uint32_t>(at + addr::kPrepareKey, key);
        Put<std::int32_t>(at + addr::kPrepareIndex, asked);
        Put<float>(at + addr::kPrepareTail, x);
        Put<std::uint64_t>(at + 0x40, at + 0x100);  // a pointer into itself (rebased on a copy)
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

Reader ReaderOf(FakeGuest& g) {
    return [&g](std::uint64_t a, void* out, std::size_t n) { return g.Read(a, out, n); };
}

std::vector<Member> Part(Role role, std::uint8_t owner = 2) { return {Member{kBoss, role, owner, "Boss_Test"}}; }

// The game's own AI calls prepare (as the wrap sees it); returns the table's answer, and runs phase 1 with
// `answer` when the call ran.
Answer OwnPrepare(Table& t, FakeGuest& g, std::uint32_t key, std::uint64_t answer, std::uint64_t now_ms,
                  bool module_call = false) {
    g.Prepare(kParams, key, -1, 8.5f);
    Seen seen;
    const Answer a = t.OnCall(ReaderOf(g), Kind::prepare, kShooter, kParams, 0, module_call, &seen);
    if (!a.skip) {
        t.OnDone(seen, answer, now_ms);
    }
    return a;
}

Shots List(std::uint32_t seq, std::vector<Shot> recent) { return Shots{seq, std::move(recent)}; }

Shot PrepareShot(std::uint32_t seq, std::uint32_t key, std::int32_t index) {
    Shot s;
    s.seq = seq;
    s.kind = Kind::prepare;
    s.key = key;
    s.asked = -1;
    s.index = index;
    float x = 3.25f;
    std::memcpy(s.tail.data(), &x, sizeof x);
    return s;
}

Shot ShootShot(std::uint32_t seq, std::uint32_t key, std::int32_t index) {
    Shot s;
    s.seq = seq;
    s.kind = Kind::shoot;
    s.key = key;
    s.asked = index;
    s.index = index;
    return s;
}

}  // namespace

TEST_CASE("boss shooter table: the owner keeps the calls its game took, 3 s each, and sends seq 0 too") {
    Table t;
    t.SetOn(true);
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::owner, 1));
    REQUIRE(t.OwnerShots(kBoss, 100));
    CHECK(t.OwnerShots(kBoss, 100)->seq == 0);  // an owner that shares shots, none yet
    CHECK_FALSE(OwnPrepare(t, g, kLightBall, 2, 1000).skip);
    CHECK_FALSE(OwnPrepare(t, g, kLightBall, ~std::uint64_t{0}, 1100).skip);  // refused by the game: not sent
    const auto shots = t.OwnerShots(kBoss, 1200);
    REQUIRE(shots);
    CHECK(shots->seq == 1);
    REQUIRE(shots->recent.size() == 1);
    CHECK(shots->recent[0].index == 2);
    CHECK(shots->recent[0].asked == -1);
    CHECK(shots->recent[0].key == kLightBall);
    float x = 0;
    std::memcpy(&x, shots->recent[0].tail.data(), sizeof x);
    CHECK(x == 8.5f);
    CHECK(t.GetCounts().owner_kept == 1);
    CHECK(t.GetCounts().owner_refused == 1);
    // Older than kKeepMs: only the seq goes out.
    const auto later = t.OwnerShots(kBoss, 1000 + Table::kKeepMs + 1);
    REQUIRE(later);
    CHECK(later->seq == 1);
    CHECK(later->recent.empty());
    // Not a boss part of the list (an enemy, a bullet's own shooter): nothing read, nothing kept.
    CHECK_FALSE(t.OwnerShots(kBoss + 0x10, 1200));
}

TEST_CASE("boss shooter table: a copy's own AI shoots until the owner's list comes, then it is answered") {
    Table t;
    t.SetOn(true);
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::copy));
    CHECK_FALSE(OwnPrepare(t, g, kLightBall, 1, 100).skip);  // an older owner: today's behaviour
    CHECK(t.GetCounts().copy_own_ran == 1);
    CHECK_FALSE(t.OwnerShots(kBoss, 100));  // a copy sends none
    t.Receive(kBoss, List(0, {}));
    const Answer a = OwnPrepare(t, g, kLightBall, 1, 200);
    CHECK(a.skip);
    CHECK(static_cast<std::int32_t>(a.x0) == -1);  // the game's "no record free"
    Seen seen;
    const Answer shoot = t.OnCall(ReaderOf(g), Kind::shoot, kShooter, kParams, 0, false, &seen);
    CHECK(shoot.skip);
    CHECK(shoot.x0 == 0);  // false
    CHECK(t.GetCounts().copy_skipped == 2);
    // A module call that is not our caller's (the native boss sync's brain call, inside which the AI shoots) is a
    // copy's own call too.
    CHECK(OwnPrepare(t, g, kLightBall, 1, 300, true).skip);
}

TEST_CASE("boss shooter table: the owner's prepare runs on a copy over its own node's block") {
    Table t;
    t.SetOn(true);
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::copy));
    OwnPrepare(t, g, kLightBall, 0, 100);  // this game's AI built the key's block once
    t.Receive(kBoss, List(4, {}));
    t.Receive(kBoss, List(5, {PrepareShot(5, kLightBall, 2)}));
    auto call = t.Step(g, false, 0, kMain, 1000);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kMalloc);
    CHECK(call->x[0] == kBlockSize);
    call = t.Step(g, true, kBlock, kMain, 1000);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kPrepare);
    CHECK(call->x[0] == kShooter);
    CHECK(call->x[1] == kBlock);
    CHECK(g.Get<std::uint32_t>(kBlock + addr::kPrepareKey) == kLightBall);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kPrepareIndex) == -1);  // what the owner's caller asked
    CHECK(g.Get<float>(kBlock + addr::kPrepareTail) == 3.25f);       // the owner's place
    CHECK(g.Get<std::uint64_t>(kBlock + 0x40) == kBlock + 0x100);    // rebased into our block
    // The game runs it: the wrap lets our caller's call through.
    Seen seen;
    CHECK_FALSE(t.OnCall(ReaderOf(g), Kind::prepare, kShooter, kBlock, 0, true, &seen).skip);
    CHECK_FALSE(seen.valid);  // not an owner's own call: nothing kept
    call = t.Step(g, true, 7, kMain, 1000);  // the game took record 7
    CHECK_FALSE(call);
    CHECK(t.GetCounts().made == 1);
    // The owner's shoot of its record 2 names ours, 7.
    t.Receive(kBoss, List(6, {PrepareShot(5, kLightBall, 2), ShootShot(6, kLightBall, 2)}));
    g.Put<std::uint32_t>(kParams + addr::kShootKey, kLightBall);
    {
        Seen own;
        // This game's AI built the shoot block before the owner's came (a copy keeps templates of skipped calls).
        CHECK(t.OnCall(ReaderOf(g), Kind::shoot, kShooter, kParams, 0, false, &own).skip);
    }
    call = t.Step(g, false, 0, kMain, 1100);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kShoot);
    CHECK(call->x[1] == kBlock + addr::kPrepareSize);
    CHECK(g.Get<std::int32_t>(kBlock + addr::kPrepareSize + addr::kShootIndex) == 7);
    CHECK_FALSE(t.Step(g, true, 1, kMain, 1100));
    CHECK(t.GetCounts().made == 2);
}

TEST_CASE("boss shooter table: an owner's shot with no block here waits, then counts as no template") {
    Table t;
    t.SetOn(true);
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::copy));
    t.Receive(kBoss, List(1, {}));
    t.Receive(kBoss, List(2, {PrepareShot(2, kOther, 0)}));
    CHECK_FALSE(t.Step(g, false, 0, kMain, 1000));
    CHECK_FALSE(t.Step(g, false, 0, kMain, 1000 + Table::kWaitMs - 1));
    CHECK(t.GetCounts().no_template == 0);
    CHECK_FALSE(t.Step(g, false, 0, kMain, 1000 + Table::kWaitMs));
    CHECK(t.GetCounts().no_template == 1);
}

TEST_CASE("boss shooter table: on a hand-over, a record prepared and never shot is put back with the game's sleep") {
    Table t;
    t.SetOn(true);
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::copy));
    OwnPrepare(t, g, kLightBall, 0, 100);
    t.Receive(kBoss, List(1, {}));
    t.Receive(kBoss, List(2, {PrepareShot(2, kLightBall, 4)}));
    REQUIRE(t.Step(g, false, 0, kMain, 1000));               // malloc
    REQUIRE(t.Step(g, true, kBlock, kMain, 1000));           // prepare
    CHECK_FALSE(t.Step(g, true, 9, kMain, 1000));            // record 9 taken, not shot yet
    t.Publish(Part(Role::owner, 1));                         // ours now
    auto call = t.Step(g, false, 0, kMain, 1100);
    REQUIRE(call);
    CHECK(call->function == kMain + addr::kSleep);
    CHECK(call->x[0] == kShooter);
    CHECK(call->x[1] == kLightBall);
    CHECK(static_cast<std::int32_t>(call->x[2]) == 9);
    // Our own put-back runs through the wrap and is not sent as the new owner's.
    Seen seen;
    CHECK_FALSE(t.OnCall(ReaderOf(g), Kind::sleep, kShooter, kLightBall, 9, true, &seen).skip);
    CHECK_FALSE(seen.valid);
    CHECK_FALSE(t.Step(g, true, 1, kMain, 1100));
    CHECK(t.GetCounts().put_back == 1);
    CHECK(t.OwnerShots(kBoss, 1100)->seq == 0);  // the new owner counts from 0
    // The new owner's own AI shoots and sends.
    CHECK_FALSE(OwnPrepare(t, g, kLightBall, 1, 1200).skip);
    CHECK(t.OwnerShots(kBoss, 1200)->seq == 1);
}

TEST_CASE("boss shooter table: a part whose shooter is gone drops its queue") {
    Table t;
    t.SetOn(true);
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::copy));
    OwnPrepare(t, g, kLightBall, 0, 100);
    t.Receive(kBoss, List(1, {}));
    t.Receive(kBoss, List(3, {PrepareShot(2, kLightBall, 0), PrepareShot(3, kLightBall, 1)}));
    g.Put<std::uint64_t>(kShooter + addr::kShooterOwner, kBoss + 0x10);  // the record went to another actor
    CHECK_FALSE(t.Step(g, false, 0, kMain, 1000));
    CHECK(t.GetCounts().dropped == 2);
    CHECK(t.GetCounts().made == 0);
}

TEST_CASE("boss shooter table: off, nothing is read or answered") {
    Table t;
    FakeGuest g;
    g.Boss(kBoss, kShooter);
    t.Publish(Part(Role::copy));
    t.Receive(kBoss, List(1, {}));
    CHECK_FALSE(OwnPrepare(t, g, kLightBall, 0, 100).skip);
    CHECK_FALSE(t.OwnerShots(kBoss, 100));
    CHECK_FALSE(t.Step(g, false, 0, kMain, 100));
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Rows 222 and 286: the owner's game decides a boss's shots (coop_boss_shooter.h).
#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "coop_boss_shooter.h"

using namespace CoopBossShooter;

namespace {

constexpr std::uint32_t kLightBall = 0x22c6d8cd;  // Phantom Ganon's (reports/row-286-wake.md, his ShooterParam)

Shots Owner(std::uint32_t calls) {
    Shots s;
    for (std::uint32_t i = 0; i < calls; ++i) {
        Add(s, Kind::prepare, kLightBall, -1, static_cast<std::int32_t>(i % 3), nullptr, 0);
    }
    return s;
}

std::uint64_t Word(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint64_t v = 0;
    std::memcpy(&v, b.data() + at, 8);
    return v;
}

}  // namespace

TEST_CASE("boss shooter: the owner's list counts from 1 and keeps the newest") {
    Shots s = Owner(3);
    CHECK(s.seq == 3);
    REQUIRE(s.recent.size() == 3);
    CHECK(s.recent.front().seq == 1);
    s = Owner(static_cast<std::uint32_t>(kMaxShots) + 5);
    CHECK(s.recent.size() == kMaxShots);
    CHECK(s.recent.back().seq == s.seq);
    CHECK(s.recent.front().seq == s.seq - kMaxShots + 1);
}

TEST_CASE("boss shooter: a tail longer than the shot's is cut, a shorter one leaves zeros") {
    Shots s;
    std::vector<std::uint8_t> big(kTailSize + 16, 0xab);
    Add(s, Kind::prepare, 1, -1, 0, big.data(), big.size());
    CHECK(s.recent[0].tail.back() == 0xab);
    Add(s, Kind::shoot, 1, 0, 0, big.data(), kShootTailSize);
    CHECK(s.recent[1].tail[kShootTailSize - 1] == 0xab);
    CHECK(s.recent[1].tail[kShootTailSize] == 0);
}

TEST_CASE("boss shooter: a copy's first sample makes nothing old") {
    const Planned p = Plan(Owner(4), 0, false);
    CHECK(p.make.empty());
    CHECK(p.delivered == 4);
    CHECK(p.restarted);
}

TEST_CASE("boss shooter: an owner with no shot yet starts the copy at 0, then its first shot is made") {
    Shots owner;
    Planned p = Plan(owner, 0, false);
    CHECK(p.delivered == 0);
    CHECK(p.restarted);
    Add(owner, Kind::prepare, kLightBall, -1, 0, nullptr, 0);
    p = Plan(owner, p.delivered, true);
    REQUIRE(p.make.size() == 1);
    CHECK(p.make[0].seq == 1);
    CHECK(p.delivered == 1);
    CHECK_FALSE(p.restarted);
}

TEST_CASE("boss shooter: every newer shot is made once, in order") {
    const Shots owner = Owner(5);
    Planned p = Plan(owner, 2, true);
    REQUIRE(p.make.size() == 3);
    CHECK(p.make[0].seq == 3);
    CHECK(p.make[2].seq == 5);
    p = Plan(owner, p.delivered, true);
    CHECK(p.make.empty());
    CHECK_FALSE(p.restarted);
}

TEST_CASE("boss shooter: a gap the list no longer covers makes nothing and goes on from the newest") {
    const Shots owner = Owner(static_cast<std::uint32_t>(kMaxShots) + 6);
    const Planned p = Plan(owner, 2, true);
    CHECK(p.make.empty());
    CHECK(p.restarted);
    CHECK(p.delivered == owner.seq);
}

TEST_CASE("boss shooter: a list with a hole is not trusted") {
    Shots owner = Owner(5);
    owner.recent.erase(owner.recent.begin() + 3);  // seq 4 lost
    const Planned p = Plan(owner, 2, true);
    CHECK(p.make.empty());
    CHECK(p.restarted);
}

TEST_CASE("boss shooter: a new owner (its count behind ours) starts the copy again") {
    const Planned p = Plan(Owner(2), 9, true);
    CHECK(p.make.empty());
    CHECK(p.restarted);
    CHECK(p.delivered == 2);
}

TEST_CASE("boss shooter: only a copy's own AI calls are answered for the game") {
    CHECK_FALSE(OwnCall(Role::owner, Kind::prepare, false).skip);
    CHECK_FALSE(OwnCall(Role::none, Kind::shoot, false).skip);
    CHECK_FALSE(OwnCall(Role::copy, Kind::prepare, true).skip);  // our caller's: the owner's shot
    const Answer prepare = OwnCall(Role::copy, Kind::prepare, false);
    CHECK(prepare.skip);
    CHECK(static_cast<std::int32_t>(prepare.x0) == -1);
    const Answer shoot = OwnCall(Role::copy, Kind::shoot, false);
    CHECK(shoot.skip);
    CHECK(shoot.x0 == 0);
    CHECK(OwnCall(Role::copy, Kind::sleep, false).skip);
}

TEST_CASE("boss shooter: a template's own pointers move with it, others stay") {
    Template t;
    t.at = 0x7000'1000;
    t.bytes.assign(addr::kPrepareSize, 0);
    const std::uint64_t inside = t.at + 0xbfc, last = t.at + addr::kPrepareSize - 1, end = t.at + addr::kPrepareSize;
    const std::uint64_t outside = 0x8123'4560, small = 0x3f80'0000'0000'0001;
    std::memcpy(t.bytes.data() + 0xbf0, &inside, 8);
    std::memcpy(t.bytes.data() + 0x8, &outside, 8);
    std::memcpy(t.bytes.data() + 0x10, &last, 8);
    std::memcpy(t.bytes.data() + 0x20, &end, 8);
    std::memcpy(t.bytes.data() + 0x1968, &small, 8);
    const std::uint64_t to = 0x9000'0040;
    const std::vector<std::uint8_t> b = Rebased(t, to);
    CHECK(Word(b, 0xbf0) == to + 0xbfc);
    CHECK(Word(b, 0x10) == to + addr::kPrepareSize - 1);
    CHECK(Word(b, 0x8) == outside);
    CHECK(Word(b, 0x20) == end);  // one past the end is not ours
    CHECK(Word(b, 0x1968) == small);
    CHECK(Rebased(Template{}, to).empty());
}

TEST_CASE("boss shooter: the owner's prepare over a template asks what the owner's caller asked") {
    Shots owner;
    std::array<std::uint8_t, kPrepareTailSize> tail{};
    const float place[3] = {8.72f, 1.0f, -149.46f};
    std::memcpy(tail.data(), place, sizeof place);
    tail.back() = 1;
    Add(owner, Kind::prepare, kLightBall, -1, 2, tail.data(), tail.size());
    std::vector<std::uint8_t> block(addr::kPrepareSize, 0x55);
    REQUIRE(PatchPrepare(block, owner.recent[0]));
    std::uint32_t key = 0;
    std::int32_t index = 0;
    float x = 0;
    std::memcpy(&key, block.data() + addr::kPrepareKey, 4);
    std::memcpy(&index, block.data() + addr::kPrepareIndex, 4);
    std::memcpy(&x, block.data() + addr::kPrepareTail, 4);
    CHECK(key == kLightBall);
    CHECK(index == -1);  // not the owner's answer 2
    CHECK(x == 8.72f);
    CHECK(block[addr::kPrepareSize - 1] == 1);
    CHECK(block[0x18] == 0x55);  // the node's own words stay
    std::vector<std::uint8_t> small(addr::kPrepareSize - 1);
    CHECK_FALSE(PatchPrepare(small, owner.recent[0]));
    Add(owner, Kind::shoot, kLightBall, 2, 2, nullptr, 0);
    CHECK_FALSE(PatchPrepare(block, owner.recent[1]));
}

TEST_CASE("boss shooter: the owner's shoot over a template names this game's record") {
    Shots owner;
    std::array<std::uint8_t, kShootTailSize> tail{};
    tail[1] = 1;  // "that index"
    Add(owner, Kind::shoot, kLightBall, 2, 2, tail.data(), tail.size());
    std::vector<std::uint8_t> block(addr::kShootSize, 0x55);
    REQUIRE(PatchShoot(block, owner.recent[0], 0));
    std::int32_t index = -5;
    std::memcpy(&index, block.data() + addr::kShootIndex, 4);
    CHECK(index == 0);
    CHECK(block[addr::kShootTail + 1] == 1);
    CHECK(block[addr::kShootTail + kShootTailSize] == 0x55);
    CHECK(block[addr::kShootKey - 1] == 0x55);
}

TEST_CASE("boss shooter: a copy's records are found by the owner's") {
    IndexMap m;
    CHECK_FALSE(m.Local(kLightBall, 2).has_value());
    CHECK_FALSE(m.Local(kLightBall, -1).has_value());  // nothing prepared: no "first prepared one" either
    m.Took(kLightBall, 2, 0);
    m.Took(kLightBall, -1, 1);  // (no owner record: not kept)
    m.Took(kLightBall, 3, -1);  // (our prepare was refused: not kept)
    CHECK(m.Size() == 1);
    CHECK(m.Local(kLightBall, 2) == 0);
    CHECK(m.Local(kLightBall, -1) == -1);
    CHECK_FALSE(m.Local(kLightBall + 1, 2).has_value());
    CHECK_FALSE(m.Local(kLightBall + 1, -1).has_value());
    m.Slept(kLightBall, 2);
    CHECK_FALSE(m.Local(kLightBall, 2).has_value());
    m.Took(kLightBall, 1, 1);
    m.Clear();
    CHECK(m.Size() == 0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 286's log-only probe of the Shooter component (lichii/hook/coop_shooter_probe.h).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../../lichii/hook/coop_shooter_probe.h"

namespace {

using namespace CoopShooterProbe;

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kBoss = 0x100000, kShooter = 0x101000, kMap = 0x102000, kRecords = 0x103000;
constexpr std::uint64_t kBullet = 0x110000, kParams = 0x120000;
constexpr std::uint32_t kKey = 0xe8b2edfe;

struct Memory {
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x40000);  // at 0x100000
    Reader reader() {
        return [this](std::uint64_t a, void* out, std::size_t n) {
            if (a < 0x100000 || a + n > 0x100000 + bytes.size()) {
                return false;
            }
            std::memcpy(out, bytes.data() + (a - 0x100000), n);
            return true;
        };
    }
    template <class T>
    void Put(std::uint64_t a, T v) {
        std::memcpy(bytes.data() + (a - 0x100000), &v, sizeof v);
    }
    void Text(std::uint64_t a, const char* text) { std::memcpy(bytes.data() + (a - 0x100000), text, std::strlen(text) + 1); }

    Memory() {
        Put<std::uint64_t>(kBoss + addr::kActorComponents, kBoss + 0x800);
        Put<std::uint64_t>(kBoss + 0x800 + addr::kComponentShooter, kShooter);
        Put<std::uint64_t>(kBoss + addr::kActorName, kBoss + 0x700);
        Text(kBoss + 0x700, "Enemy_DungeonBoss_Twinrova");
        Put<std::uint64_t>(kShooter, kMain + 0x4b4ffc8);
        Put<std::uint64_t>(kShooter + addr::kShooterOwner, kBoss);
        Put<std::uint64_t>(kShooter + addr::kShooterMap, kMap);
        Put<std::uint32_t>(kShooter + addr::kShooterSlots, 4);
        Put<std::uint32_t>(kMap + 2 * addr::kMapEntrySize, kKey);
        Put<std::uint32_t>(kMap + 2 * addr::kMapEntrySize + addr::kEntryCount, 1);
        Put<std::uint64_t>(kMap + 2 * addr::kMapEntrySize + addr::kEntryRecords, kRecords);
        const std::uint64_t holder = kRecords + 0x400;
        Put<std::uint64_t>(kRecords + addr::kRecordShot + addr::kHandleHolder, holder);
        Put<std::uint64_t>(holder + addr::kHolderActor, kBullet);
        Put<std::uint64_t>(kBullet, kMain + 0x4b25e18);
        Put<std::uint32_t>(kBullet + addr::kActorState, 3);
        Put<std::uint64_t>(kBullet + addr::kActorName, kBullet + 0x700);
        Text(kBullet + 0x700, "FireBullet");
        Put<std::uint32_t>(kParams, kKey);
        Put<std::uint64_t>(kParams + 8, kParams + 0x2000);
        Put<std::int32_t>(kParams + addr::kPrepareIndex, -1);
    }
    void State(std::uint8_t state, std::uint8_t asked, std::uint8_t is_asked) {
        Put(kRecords + addr::kRecordState, state);
        Put(kRecords + addr::kRecordState + 1, asked);
        Put(kRecords + addr::kRecordState + 2, is_asked);
    }
};

bool Has(const std::vector<std::string>& lines, std::string_view text) {
    for (const std::string& l : lines) {
        if (l.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("shooter probe: a prepare is logged with the maker, the role, the key's records before and after and the words") {
    Memory m;
    const Reader read = m.reader();
    Probe probe;
    probe.SetOn(true);
    probe.Publish({{kBoss, false, 2, "Enemy_DungeonBoss_Twinrova"}});
    const Call call = Begin(read, Fn::prepare, kShooter, kParams, 0, kMain + 0x28500d0, false);
    CHECK(call.key == kKey);
    CHECK(call.index == -1);
    CHECK(call.maker == kBoss);
    m.State(0, 1, 1);
    m.Put<std::uint32_t>(kBullet + addr::kActorState + 4, 0x280000);
    probe.End(read, call, 0, kMain, 5000);
    const auto lines = probe.TakeLines();
    REQUIRE(lines.size() == 2);
    CHECK(Has(lines, "PREPARE Enemy_DungeonBoss_Twinrova 0x100000 (copy of player 2's) key 0xe8b2edfe index -1 -> 0"));
    CHECK(Has(lines, "vt main+0x4b4ffc8 from main+0x28500d0"));
    CHECK(Has(lines, "before {0xe8b2edfe x1 [0 0/0/0 FireBullet 0x110000 vt main+0x4b25e18 state 3 req 0x0"));
    CHECK(Has(lines, "after {0xe8b2edfe x1 [0 0/1/1 FireBullet 0x110000 vt main+0x4b25e18 state 3 req 0x280000"));
    CHECK(Has(lines, "PREPARE params 0x120000 +0: "));
    const auto counts = probe.GetCounts();
    CHECK(counts.at({Fn::prepare, "Enemy_DungeonBoss_Twinrova"}).done == 1);
}

TEST_CASE("shooter probe: a refused prepare counts as refused, an unknown key is said") {
    Memory m;
    const Reader read = m.reader();
    Probe probe;
    probe.SetOn(true);
    m.Put<std::uint32_t>(kParams, 0x1234);
    const Call call = Begin(read, Fn::prepare, kShooter, kParams, 0, 0, true);
    probe.End(read, call, 0xffffffffu, kMain, 0);
    const auto lines = probe.TakeLines();
    CHECK(Has(lines, "(no boss of the walk) OUR CALL key 0x1234 index -1 -> -1"));
    CHECK(Has(lines, "no such key"));
    CHECK(probe.GetCounts().at({Fn::prepare, "Enemy_DungeonBoss_Twinrova"}).refused == 1);
}

TEST_CASE("shooter probe: a shoot reads its key at +0x1928, a sleep from the registers; the answer is a bool's low byte") {
    Memory m;
    const Reader read = m.reader();
    Probe probe;
    probe.SetOn(true);
    m.Put<std::uint64_t>(kParams, kMain + 0x1000);
    m.Put<std::uint32_t>(kParams + addr::kShootKey, kKey);
    m.Put<std::uint32_t>(kParams + addr::kShootIndex, 0);
    const Call shoot = Begin(read, Fn::shoot, kShooter, kParams, 0, 0, false);
    CHECK(shoot.key == kKey);
    CHECK_FALSE(shoot.uses_index);
    m.State(1, 2, 1);
    probe.End(read, shoot, 1, kMain, 0);
    const Call sleep = Begin(read, Fn::sleep, kShooter, kKey, 0, 0, false);
    CHECK(sleep.key == kKey);
    CHECK(sleep.index == 0);
    probe.End(read, sleep, 0x100, kMain, 0);
    const auto lines = probe.TakeLines();
    CHECK(Has(lines, "SHOOT Enemy_DungeonBoss_Twinrova"));
    CHECK(Has(lines, "(first prepared) -> yes"));
    CHECK(Has(lines, "class main+0x1000"));
    CHECK(Has(lines, "SLEEP Enemy_DungeonBoss_Twinrova"));
    CHECK(Has(lines, "-> no"));
}

TEST_CASE("shooter probe: the same call again is counted, not logged again") {
    Memory m;
    const Reader read = m.reader();
    Probe probe;
    probe.SetOn(true);
    for (int i = 0; i < 5; ++i) {
        probe.End(read, Begin(read, Fn::sleep, kShooter, kKey, 0, 0, false), 0, kMain, 0);
    }
    CHECK(probe.TakeLines().size() == 1);
    probe.End(read, Begin(read, Fn::sleep, kShooter, kKey, 1, 0, false), 0, kMain, 0);
    const auto lines = probe.TakeLines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "(the last call 4 more times)");
    CHECK(probe.GetCounts().at({Fn::sleep, "Enemy_DungeonBoss_Twinrova"}).calls == 6);
}

TEST_CASE("shooter probe: a look logs what a boss's shooter holds, again only when it changes") {
    Memory m;
    const Reader read = m.reader();
    Probe probe;
    probe.Look(read, kMain, 60'000);
    CHECK(probe.TakeLines().empty());  // off
    probe.SetOn(true);
    probe.Publish({{kBoss, true, 1, "Enemy_DungeonBoss_Twinrova"}});
    probe.Look(read, kMain, 60'000);
    auto lines = probe.TakeLines();
    CHECK(Has(lines, "HOLDS Enemy_DungeonBoss_Twinrova 0x100000 (owner) {0xe8b2edfe x1 [0 0/0/0 FireBullet"));
    CHECK(Has(lines, "COUNTS 0 calls"));
    m.Put<float>(kBullet + addr::kActorPosition, 5.0f);  // a place alone is no change
    probe.Look(read, kMain, 60'300);
    CHECK(probe.TakeLines().empty());
    m.State(2, 0, 0);
    probe.Look(read, kMain, 60'400);  // (too soon)
    CHECK(probe.TakeLines().empty());
    probe.Look(read, kMain, 60'600);
    CHECK(Has(probe.TakeLines(), "[0 2/0/0 FireBullet"));
}

TEST_CASE("shooter probe: bounded, and unreadable memory is no crash") {
    Memory m;
    const Reader read = m.reader();
    Probe probe;
    probe.SetOn(true);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        probe.End(read, Begin(read, Fn::sleep, kShooter, kKey + i, 0, 0, false), 0, kMain, 1000);
    }
    CHECK(probe.TakeLines().size() == Probe::kMaxLinesPerSec);
    const Call bad = Begin(read, Fn::shoot, 0x10, 0x20, 0, 0, false);
    probe.End(read, bad, 1, kMain, 9000);
    CHECK(Has(probe.TakeLines(), "SHOOT ? 0x0"));
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' hearts on the game's HUD (hookshot/src/coop_party_hud.h) on a pretend game.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_party_hud.h"

namespace party = CoopPartyHud;
namespace ui = CoopNativeUi;

namespace {

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kManager = 0x96000000, kScreens = 0x97000000, kStatus = 0x93000000, kLayout = 0x94000000;
constexpr std::uint64_t kBlock = 0x9a000000, kPanes = 0x9b000000, kAnims = 0x9c000000;

std::uint64_t At(std::uint64_t main_offset) { return kMain + main_offset; }

struct FakeGame final : party::Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    std::map<std::string, std::uint64_t> panes, anims;   // made as asked, when `pack` has them
    std::map<std::uint64_t, int> shown;                  // pane -> 0/1
    std::map<std::uint64_t, float> frames;               // animation -> the frame it is stopped at
    std::map<std::uint64_t, float> alphas;               // pane -> its own alpha (kPaneAlpha)
    std::map<std::string, std::string> texts;            // text pane path -> text
    bool pack = true;
    std::int32_t gauge_last = 100;
    int calls = 0;

    bool Read(std::uint64_t a, void* out, std::size_t n) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = mem.find(a + i);
            o[i] = it == mem.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) mem[a + i] = p[i];
        return true;
    }
    std::string Str(std::uint64_t a) {
        std::string s;
        for (char c = 0; Read(a++, &c, 1) && c != 0;) s.push_back(c);
        return s;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        ++calls;
        if (f == At(ui::addr::kMalloc)) return kBlock;
        if (f == At(ui::addr::kPaneByName)) {
            const std::string name = Str(x[1]);
            if (!pack || name.rfind("Pa_Coop", 0) != 0) return 0;
            if (panes.count(name) == 0) panes[name] = kPanes + 0x1000 * panes.size();
            return panes[name];
        }
        if (f == At(party::addr::kAnimFind)) {
            const std::string name = Str(x[1]);
            if (!pack || name.find("IsDoubleDefense") != std::string::npos) return 0;
            if (anims.count(name) == 0) {
                const std::uint64_t at = kAnims + 0x100 * anims.size();
                anims[name] = at;
                Put(at + party::addr::kAnimPlayer, at + 0x80);
            }
            return anims[name];
        }
        if (f == At(party::addr::kAnimLastFrame)) {
            return static_cast<std::uint64_t>(static_cast<std::uint32_t>(gauge_last));
        }
        if (f == At(party::addr::kPaneShow)) {
            shown[x[0]] = static_cast<int>(x[1]);
            return 0;
        }
        if (f == At(ui::addr::kSetPaneText)) {
            ui::Ref r;
            Read(x[2], &r, sizeof r);
            texts[Str(x[1])] = Str(r.text);
            return 0;
        }
        return 0;
    }
    std::uint64_t Main() const override { return kMain; }
    std::optional<std::uint64_t> CallFloat(std::uint64_t f, std::uint64_t x0, float s0) override {
        ++calls;
        if (f == At(party::addr::kPaneAlpha)) {
            alphas[x0] = s0;
            return 0;
        }
        if (f != At(party::addr::kAnimStopAt)) return std::nullopt;
        frames[x0] = s0;
        return 0;
    }
    void Put(std::uint64_t a, std::uint64_t v) { Write(a, &v, sizeof v); }
    void Hud(std::uint64_t layout = kLayout) {
        Put(At(ui::addr::kUiManagerGot), kManager - 8);
        Put(kManager - 8, kManager);
        const std::uint32_t one = 1;
        Write(kManager + ui::addr::kUiCount, &one, sizeof one);
        Put(kManager + ui::addr::kUiScreens, kScreens);
        Put(kScreens, kStatus);
        Put(kStatus, At(party::addr::kStatusVtable));
        Put(kStatus + ui::addr::kScreenLayout, layout);
    }
    float Alpha(const std::string& pane) { return alphas.at(panes.at(pane)); }
    bool Shown(const std::string& pane) { return panes.count(pane) != 0 && shown[panes[pane]] == 1; }
    float Frame(const std::string& anim) { return frames.at(anims.at(anim)); }
    std::array<float, 2> Where(const std::string& pane) {
        std::array<float, 2> at{};
        Read(panes.at(pane) + party::addr::kPaneXY, at.data(), sizeof at);
        return at;
    }
};

std::vector<int> Quarters(int life, int max) {
    std::vector<int> out;
    for (int i = 0; i < (max + 3) / 4; ++i) out.push_back(std::clamp(life - 4 * i, 0, 4));
    return out;
}

std::string HeartName(int slot, int i) {
    char name[32];
    std::snprintf(name, sizeof name, "Pa_CoopHeart_%d_%02d", slot, i);
    return name;
}

}  // namespace

TEST_CASE("party hud: a player's hearts as the game sets its own") {
    for (int max = 12; max <= 80; max += 4) {
        for (int life = 0; life <= max; ++life) {
            const party::Plan p = party::PlanHearts(Quarters(life, max));
            CHECK(p.hearts == max / 4);
            int quarters = 0;
            for (int i = 0; i < party::kHearts; ++i) {
                const int state = p.whole[static_cast<std::size_t>(i)];
                CHECK(state == (i >= max / 4 ? 0 : i < life / 4 ? 2 : 1));
                quarters += state == 2 ? 4 : 0;
            }
            CHECK(quarters + p.part_quarters == life);
            CHECK(p.part_at == (life % 4 == 0 ? -1 : life / 4));
        }
    }
    CHECK(party::PlanHearts(std::vector<int>(40, 4)).hearts == party::kHearts);   // never past the copies
    CHECK(party::PlanHearts({}) == party::Plan{});
}

TEST_CASE("party hud: the part heart's frame on a long and on a short animation") {
    CHECK(party::GaugeFrame(1, 100) == 25.0f);
    CHECK(party::GaugeFrame(3, 100) == 75.0f);
    CHECK(party::GaugeFrame(1, 3) == 0.0f);    // the game's damage path: up to 1/4 -> 0 ... more than 3/4 -> 3
    CHECK(party::GaugeFrame(3, 3) == 2.0f);
    CHECK(party::GaugeFrame(4, 3) == 3.0f);
    CHECK(party::GaugeFrame(2, -1) == 0.0f);
}

TEST_CASE("party hud: the blocks stay on the picture") {
    // Three players with two rows each under a tall game HUD: every heart inside the 3840 x 2160 layout.
    float top = party::TopFrom(0.2);
    CHECK(top == doctest::Approx(-0.2f * 2160.0f - party::kBlockGap));
    CHECK(party::TopFrom(std::nullopt) == party::kDefaultTop);
    for (int s = 0; s < party::kSlots; ++s) {
        for (int i = 0; i < party::kHearts; ++i) {
            const auto at = party::HeartAt(top, i);
            CHECK(at[0] > 72.0f * party::kScale);
            CHECK(at[0] < 3840.0f);
            CHECK(at[1] < 0.0f);
            CHECK(at[1] - 72.0f * party::kScale > -2160.0f);
        }
        CHECK(party::HeartAt(top, 10)[0] == party::HeartAt(top, 0)[0]);   // the second row under the first
        CHECK(party::HeartAt(top, 10)[1] == doctest::Approx(party::HeartAt(top, 0)[1] - party::kStepY));
        top -= party::BlockHeight(party::kHearts);
    }
    CHECK(top - party::kLabelHeight > -2160.0f);   // and the "+N" line under them
    CHECK(party::BlockHeight(3) < party::BlockHeight(11));
    CHECK(party::MoreText(3).empty());
    CHECK(party::MoreText(5) == "+2");
}

TEST_CASE("party hud: an older pack keeps the drawn blocks") {
    FakeGame game;
    party::Hud hud;
    std::vector<std::string> log;
    const std::vector<party::Block> one = {{"A", Quarters(12, 12)}};
    CHECK_FALSE(hud.Step(game, one, std::nullopt, &log));   // no HUD yet
    game.Hud();
    game.pack = false;
    CHECK_FALSE(hud.Step(game, one, std::nullopt, &log));
    CHECK_FALSE(hud.native());
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("older pack") != std::string::npos);
    const int calls = game.calls;
    CHECK_FALSE(hud.Step(game, one, std::nullopt, &log));   // asked once per HUD
    CHECK(game.calls == calls);
}

TEST_CASE("party hud: shows, sets and hides the copies") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    std::vector<std::string> log;
    // Player 1: 5 hearts, 2 and 3/4 filled. Player 2: 12 hearts, all full.
    std::vector<party::Block> blocks = {{"Saria  Kokiri Forest", Quarters(11, 20)}, {"Ruto", Quarters(48, 48)}};
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    CHECK(hud.native());
    CHECK(log.back().find("its own heart parts") != std::string::npos);
    CHECK(game.texts["Pa_CoopName_0/T_Text_00"] == "Saria  Kokiri Forest");
    CHECK(game.texts["Pa_CoopName_0/T_Text_01"] == "Saria  Kokiri Forest");
    CHECK(game.texts["Pa_CoopName_1/T_Text_00"] == "Ruto");
    CHECK(game.Shown("Pa_CoopName_0"));
    CHECK_FALSE(game.Shown("Pa_CoopName_2"));
    CHECK_FALSE(game.Shown("Pa_CoopMore_0"));
    const float top = party::TopFrom(0.15);
    for (int i = 0; i < party::kHearts; ++i) {
        const std::string name = HeartName(0, i);
        CHECK(game.Shown(name) == (i < 5));
        if (i >= 5) continue;
        CHECK(game.Frame(name + "/BaseState") == 1.0f);
        CHECK(game.Frame(name + "/HeartState") == (i < 2 ? 1.0f : 0.0f));
        CHECK(game.Where(name) == party::HeartAt(top, i));
    }
    CHECK(game.Shown("Pa_CoopGauge_0"));                                   // the part heart on the third heart
    CHECK(game.Where("Pa_CoopGauge_0") == party::HeartAt(top, 2));
    CHECK(game.Frame("Pa_CoopGauge_0/HeartGauge") == 75.0f);
    CHECK_FALSE(game.Shown("Pa_CoopGauge_1"));
    CHECK(game.Shown(HeartName(1, 11)));
    CHECK(game.Where(HeartName(1, 0)) == party::HeartAt(top - party::BlockHeight(5), 0));   // under the first block
    CHECK_FALSE(game.Shown(HeartName(2, 0)));

    const int calls = game.calls;
    REQUIRE(hud.Step(game, blocks, 0.15, &log));   // nothing changed: nothing set again
    CHECK(game.calls == calls);

    blocks[0].hearts = Quarters(12, 20);           // healed to 3 whole hearts: the part heart goes
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    CHECK_FALSE(game.Shown("Pa_CoopGauge_0"));
    CHECK(game.Frame(HeartName(0, 2) + "/HeartState") == 1.0f);

    blocks.erase(blocks.begin());                  // player 1 leaves: player 2 moves up, slot 1 hidden
    REQUIRE(hud.Step(game, blocks, 0.15, &log));
    CHECK(game.texts["Pa_CoopName_0/T_Text_00"] == "Ruto");
    CHECK(game.Shown(HeartName(0, 11)));
    CHECK_FALSE(game.Shown(HeartName(1, 0)));
    CHECK_FALSE(game.Shown("Pa_CoopName_1"));

    REQUIRE(hud.Step(game, {}, 0.15, &log));       // alone: nothing of ours on the HUD
    for (const auto& [name, pane] : game.panes) CHECK(game.shown[pane] == 0);

    REQUIRE(hud.Step(game, party::TestBlocks(0), std::nullopt, &log));   // four players: three blocks and "+1"
    CHECK(game.texts["Pa_CoopMore_0/T_Text_00"] == "+1");
    CHECK(game.Shown("Pa_CoopMore_0"));
    CHECK(game.Shown(HeartName(0, 19)));

    game.Hud(kLayout + 0x1000);                    // the HUD made again: found and set again
    game.shown.clear();
    REQUIRE(hud.Step(game, party::TestBlocks(0), std::nullopt, &log));
    CHECK(game.Shown(HeartName(0, 19)));
}

TEST_CASE("party hud: a player not connected is dimmed, and lit again") {
    FakeGame game;
    game.Hud();
    party::Hud hud;
    std::vector<party::Block> blocks = {{"Saria", Quarters(11, 20)}, {"Ruto", Quarters(12, 12), true}};
    REQUIRE(hud.Step(game, blocks, 0.15));
    CHECK(game.Alpha("Pa_CoopName_0") == 1.0f);
    CHECK(game.Alpha(HeartName(0, 0)) == 1.0f);
    CHECK(game.Alpha("Pa_CoopGauge_0") == 1.0f);
    CHECK(game.Alpha("Pa_CoopName_1") == party::kDimAlpha);
    CHECK(game.Alpha(HeartName(1, 2)) == party::kDimAlpha);
    blocks[1].dim = false;                          // back: lit again
    REQUIRE(hud.Step(game, blocks, 0.15));
    CHECK(game.Alpha("Pa_CoopName_1") == 1.0f);
    CHECK(game.Alpha(HeartName(1, 2)) == 1.0f);
}

TEST_CASE("party hud: the test players go through every fill") {
    const auto a = party::TestBlocks(0), b = party::TestBlocks(party::kTestStepMs * 3);
    REQUIRE(a.size() == 4);
    CHECK(a[0].hearts.size() == 20);
    CHECK(a[2].hearts == std::vector<int>{4, 4, 4});
    CHECK(b[2].hearts == std::vector<int>{4, 4, 1});
    CHECK(party::TestBlocks(party::kTestStepMs * 12)[2].hearts == std::vector<int>{0, 0, 0});
    CHECK(party::TestBlocks(party::kTestStepMs * 13)[2].hearts == std::vector<int>{4, 4, 4});
}

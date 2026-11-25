// SPDX-License-Identifier: GPL-3.0-or-later
// The others on the game's own map icons (hookshot/src/coop_map_icons.h): the clip's inverse, the plan, and the steps
// against a made-up game memory (the UI manager, the map screen, its layout, the copies and their part layouts).
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "coop_map_icons.h"

namespace {

namespace M = CoopMapIcons;
namespace T = CoopNativeTags;
constexpr std::uint64_t kBase = 0x80020000;

struct FakeGame final : T::Game {
    std::map<std::uint64_t, std::uint8_t> mem;
    std::map<std::pair<std::uint64_t, std::string>, std::uint64_t> panes;   // (layout, name) -> pane
    std::map<std::uint64_t, int> shown;                                     // pane -> last kPaneShow
    std::vector<std::pair<std::uint64_t, std::string>> texts;               // (layout, text) set
    std::uint64_t next = 0xc0000000;

    std::uint64_t MainBase() const override { return kBase; }
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
        for (char ch; Read(a, &ch, 1) && ch != 0; ++a) s.push_back(ch);
        return s;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        if (f == kBase + T::addr::kMalloc) return 0x90000000;
        if (f == kBase + T::addr::kPaneByName) {
            const auto it = panes.find({x[0], Str(x[1])});
            return it == panes.end() ? 0 : it->second;
        }
        if (f == kBase + T::addr::kPaneShow) shown[x[0]] = static_cast<int>(x[1]);
        if (f == kBase + T::addr::kSetPaneText) {
            std::uint64_t text = 0;
            Read(x[2], &text, 8);
            texts.emplace_back(x[0], Str(text));
        }
        return 0;
    }
    template <typename V>
    void Put(std::uint64_t a, V v) { Write(a, &v, sizeof v); }
    std::uint64_t Pane(std::uint64_t layout, const std::string& name) {
        const std::uint64_t p = next += 0x1000;
        panes[{layout, name}] = p;
        return p;
    }
    // A layout object named `name` (+0xf8 -> resource, resource +0x18 -> its name) held by `part_pane` at +0x40.
    std::uint64_t Layout(std::uint64_t part_pane, const std::string& name) {
        const std::uint64_t l = next += 0x1000, res = next += 0x1000, text = next += 0x1000;
        Put<std::uint64_t>(l + 0xf8, res);
        Put<std::uint64_t>(res + 0x18, text);
        Write(text, name.c_str(), name.size() + 1);
        Put<std::uint64_t>(part_pane + 0x40, l);
        return l;
    }
};

constexpr std::uint64_t kScreen = 0xa0002000, kLayout = 0xa0008000;

// The map screen with its layout, the clip at the identity (the layout's middle) and `copies` copies made by the pack.
void SetUp(FakeGame& g, std::size_t copies) {
    g.Put<std::uint64_t>(kBase + T::addr::kUiManagerGot, 0xa0000f00);
    g.Put<std::uint64_t>(0xa0000f00, 0xa0000000);
    g.Put<std::uint32_t>(0xa0000000 + T::addr::kUiCount, 1);
    g.Put<std::uint64_t>(0xa0000000 + T::addr::kUiScreens, 0xa0001000);
    g.Put<std::uint64_t>(0xa0001000, kScreen);
    g.Put<std::uint64_t>(kScreen, kBase + T::kMapMenuVtable);
    g.Put<std::uint64_t>(kScreen + T::addr::kScreenLayout, kLayout);
    const std::uint64_t clip = g.Pane(kLayout, M::kClipPane);
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    g.Write(clip + M::kPaneMatrix, id, sizeof id);
    for (std::size_t i = 0; i < copies; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, M::kCopyName, static_cast<int>(i));
        const std::uint64_t icon = g.Layout(g.Pane(kLayout, name), "PaMapIcon_00");
        for (const char* n : {"N_Door_00", "N_Player_00", "N_Entrance_00", "N_Ruto_00"}) g.Pane(icon, n);
        const std::uint64_t player = g.Layout(g.Pane(icon, "Pa_Player_00"), "PaMapIconPlayer_00");
        g.Pane(player, "N_PlayerRotate_00");
        g.Pane(player, "N_Type_01");
        const std::uint64_t entrance = g.Layout(g.Pane(icon, "Pa_Entrance_00"), "PaMapIconEntrance_00");
        g.Pane(entrance, "T_Location_00");
        g.Pane(entrance, "N_Mini_00");
    }
}

M::Mark At(std::uint8_t n, float x, float y, std::string name = "") {
    M::Mark m;
    m.number = n, m.x = x, m.y = y, m.name = std::move(name);
    return m;
}

}  // namespace

TEST_CASE("map icons: the clip's inverse takes a layout point into its frame (scaled, moved, turned)") {
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    auto inv = M::Inverse(id);
    REQUIRE(inv);
    auto p = M::ToLocal(*inv, 1920, 1080);   // the layout's middle
    CHECK(p[0] == doctest::Approx(0.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    p = M::ToLocal(*inv, 2020, 980);         // right 100, up 100 (y down in, y up out)
    CHECK(p[0] == doctest::Approx(100.0f));
    CHECK(p[1] == doctest::Approx(100.0f));
    // scale 2 at (1536, -690) (the minimap's corner): a point there is the frame's 0, 100 further is 50
    const float sm[12] = {2, 0, 0, 0, 2, 0, 0, 0, 1, 1536, -690, 0};
    inv = M::Inverse(sm);
    REQUIRE(inv);
    p = M::ToLocal(*inv, 1920 + 1536 + 100, 1080 + 690);
    CHECK(p[0] == doctest::Approx(50.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    // turned a quarter counter-clockwise: the layout's up is the frame's right
    const float tm[12] = {0, 1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0};
    inv = M::Inverse(tm);
    REQUIRE(inv);
    p = M::ToLocal(*inv, 1920, 1080 - 100);
    CHECK(p[0] == doctest::Approx(100.0f));
    CHECK(p[1] == doctest::Approx(0.0f));
    const float zero[12] = {};
    CHECK_FALSE(M::Inverse(zero));
    float nan[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    nan[9] = std::nanf("");
    CHECK_FALSE(M::Inverse(nan));
}

TEST_CASE("map icons: one copy per player, at most 15, past the edge left out, turned their way, P<n> unnamed") {
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const auto inv = *M::Inverse(id);
    M::Frame f;
    for (int i = 1; i <= 17; ++i) f.marks.push_back(At(static_cast<std::uint8_t>(i), 100.0f * i, 100.0f, "N" + std::to_string(i)));
    f.marks[0].clamped = true;                 // past the map's edge: the game's clip would cut it
    f.marks[1].angle = -1.57079632679f;        // facing up the screen: not turned
    f.marks[2].angle = 0.0f;                   // facing right: a quarter clockwise (-pi/2)
    f.marks[3].name.clear();
    f.marks[4].floor_delta = 1;                // another floor: the name, no arrow
    const auto plan = M::Plan(f, inv);
    REQUIRE(plan.size() == M::kCopies);
    CHECK(plan[0].name == "N2");
    CHECK(plan[0].copy == 0);
    REQUIRE(plan[0].turn);
    CHECK(*plan[0].turn == doctest::Approx(0.0f));
    REQUIRE(plan[1].turn);
    CHECK(*plan[1].turn == doctest::Approx(-1.57079632679f));
    CHECK(plan[2].name == "P4");
    CHECK_FALSE(plan[3].arrow);
    CHECK(plan[4].arrow);
    CHECK_FALSE(plan[4].turn);                 // no facing known: not turned
    CHECK(plan[14].name == "N16");             // the 17th has no copy
    CHECK(plan[1].at[0] == doctest::Approx(300.0f - 1920.0f));
    CHECK(plan[1].at[1] == doctest::Approx(1080.0f - 100.0f));
    CHECK(M::Plan(M::Frame{}, inv).empty());   // solo: nothing
}

TEST_CASE("map icons: overlapping arrows show only the name of the one nearer to us") {
    const float id[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const auto inv = *M::Inverse(id);
    M::Frame f;
    f.marks = {At(2, 1000, 1000, "far"), At(3, 1010, 1000, "near"), At(4, 1500, 1000, "alone")};
    f.self = std::array<float, 2>{1100.0f, 1000.0f};
    auto plan = M::Plan(f, inv);
    REQUIRE(plan.size() == 3);
    CHECK_FALSE(plan[0].name_shown);
    CHECK(plan[1].name_shown);
    CHECK(plan[2].name_shown);
    f.self.reset();   // our place not known: the lower number keeps its name
    plan = M::Plan(f, inv);
    CHECK(plan[0].name_shown);
    CHECK_FALSE(plan[1].name_shown);
}

TEST_CASE("map icons: the steps show a copy per player with the name on its label, hide the rest, then hide all") {
    FakeGame g;
    SetUp(g, M::kCopies);
    M::Icons icons;
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria"), At(3, 1000, 500, "Mido")};
    REQUIRE(icons.Step(g, f));
    CHECK(icons.Up());
    CHECK(icons.shown() == 2);
    const std::uint64_t c0 = icons.copy(0), c1 = icons.copy(1);
    CHECK(g.shown[c0] == 1);
    CHECK(g.shown[c1] == 1);
    float xy[2] = {};
    g.Read(c0 + T::addr::kPaneXY, xy, sizeof xy);
    CHECK(xy[0] == doctest::Approx(100.0f));
    CHECK(xy[1] == doctest::Approx(100.0f));
    REQUIRE(g.texts.size() == 2);
    CHECK(g.texts[0].second == "Saria");
    CHECK(g.texts[1].second == "Mido");
    // the icon types not ours hidden (in the two copies used), the arrow and the label shown, the other look hidden
    int doors = 0, players = 0, minis = 0;
    for (const auto& [key, pane] : g.panes) {
        const auto it = g.shown.find(pane);
        if (it == g.shown.end()) continue;
        doors += key.second == "N_Door_00" && it->second == 0;
        players += key.second == "N_Player_00" && it->second == 1;
        minis += key.second == "N_Mini_00" && it->second == 0;
    }
    CHECK(doors == 2);
    CHECK(players == 2);
    CHECK(minis == 2);
    // Mido leaves: his copy is hidden again, Saria's name is not set again
    f.marks.pop_back();
    REQUIRE(icons.Step(g, f));
    CHECK(icons.shown() == 1);
    CHECK(g.shown[c1] == 0);
    CHECK(g.texts.size() == 2);
    icons.HideAll(g);
    CHECK(g.shown[c0] == 0);
    CHECK(icons.shown() == 0);
}

TEST_CASE("map icons: a game without the pack's copies has nothing touched and says so") {
    FakeGame g;
    SetUp(g, 0);
    M::Icons icons;
    M::Frame f;
    f.marks = {At(2, 2020, 980, "Saria")};
    CHECK_FALSE(icons.Step(g, f));
    CHECK_FALSE(icons.Up());
    CHECK(icons.why().find("Pa_CoopMapIcon_00") != std::string::npos);
    CHECK(g.shown.empty());
    CHECK(g.texts.empty());
}

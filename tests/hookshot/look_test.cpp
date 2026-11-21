// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's look changed in the pause menu (hookshot/src/coop_look.h) and
// its tunic rewritten in memory (coop_look_tunic.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_colors.h"
#include "coop_look.h"
#include "coop_look_paint.h"
#include "coop_look_parts.h"
#include "coop_look_tunic.h"
#include "coop_settings.h"
#include "coop_title_menu.h"

using CoopLook::Look;
using CoopLook::Part;

namespace {

Look Tunic(std::optional<std::uint32_t> c) {
    Look l;
    l[Part::tunic] = c;
    return l;
}

// One block of the game's memory at `base` (anything else is not there), the
// textures the game made in it and Longshot's overrides of them, as
// host/coop_texture.c keeps them (patch 0020).
class FakeTextures final : public CoopLookTunic::Textures {
public:
    FakeTextures(std::uint64_t base, std::size_t size) : base_(base), bytes_(size) {}
    std::vector<CoopLookTunic::Live> List(std::uint64_t lo, std::uint64_t hi, std::uint64_t* generation) const override {
        std::vector<CoopLookTunic::Live> out;
        for (const auto& t : made_) {
            if (t.size >= lo && t.size <= hi) {
                out.push_back(t);
            }
        }
        if (generation != nullptr) {
            *generation = generation_;
        }
        return out;
    }
    bool Read(std::uint64_t a, void* out, std::size_t n) const override {
        if (!In(a, n)) {
            return false;
        }
        ++reads[a];
        std::memcpy(out, bytes_.data() + (a - base_), n);
        return true;
    }
    bool Override(std::uint64_t a, std::uint64_t serial, const void* in, std::size_t n) override {
        if (in == nullptr) {
            Drop(a, n);
            return true;
        }
        const bool inside = std::any_of(made_.begin(), made_.end(), [&](const auto& t) {
            return t.serial == serial && t.address <= a && a + n <= t.address + t.size;
        });
        if (!inside) {
            return false;
        }
        Drop(a, n);
        const auto* p = static_cast<const std::uint8_t*>(in);
        overrides_[a] = std::vector<std::uint8_t>(p, p + n);
        ++overridden;
        return true;
    }
    // The game makes a texture whose storage is [a, a + n) (its file already in memory).
    void Make(std::uint64_t a, std::uint64_t n) {
        Finalize(a);
        made_.push_back({a, n, ++serials_});
        ++generation_;
    }
    void Finalize(std::uint64_t a) {
        for (auto it = made_.begin(); it != made_.end(); ++it) {
            if (it->address == a) {
                Drop(it->address, it->size);
                made_.erase(it);
                ++generation_;
                return;
            }
        }
    }
    // What the renderer draws at a: the game's memory with the overrides over it.
    std::vector<std::uint8_t> Drawn(std::uint64_t a, std::size_t n) const {
        std::vector<std::uint8_t> out(bytes_.begin() + static_cast<std::ptrdiff_t>(a - base_),
                                      bytes_.begin() + static_cast<std::ptrdiff_t>(a - base_ + n));
        for (const auto& [at, o] : overrides_) {
            for (std::size_t i = 0; i < o.size(); ++i) {
                if (at + i >= a && at + i < a + n) {
                    out[at + i - a] = o[i];
                }
            }
        }
        return out;
    }
    std::uint8_t* at(std::uint64_t a) { return bytes_.data() + (a - base_); }
    std::size_t overrides() const { return overrides_.size(); }
    int overridden = 0;
    mutable std::map<std::uint64_t, int> reads;   // Read calls by address

private:
    void Drop(std::uint64_t a, std::uint64_t n) {
        for (auto it = overrides_.begin(); it != overrides_.end();) {
            it = it->first < a + n && a < it->first + it->second.size() ? overrides_.erase(it) : std::next(it);
        }
    }
    bool In(std::uint64_t a, std::size_t n) const { return a >= base_ && a + n <= base_ + bytes_.size(); }
    std::uint64_t base_;
    std::vector<std::uint8_t> bytes_;
    std::vector<CoopLookTunic::Live> made_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> overrides_;
    std::uint64_t generation_ = 0, serials_ = 0;
};

constexpr std::uint64_t kOffset = 0x100;   // the texels' place in the texture's file
constexpr std::size_t kTexels = 256;
constexpr std::uint64_t kBase = 0x10000;   // where the fake heap starts

std::vector<std::uint8_t> TexelsOf(std::uint8_t seed) {
    std::vector<std::uint8_t> t(kTexels);
    for (std::size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<std::uint8_t>(seed + i * 7);
    }
    return t;
}

// A puppet copy's texels as the pack writes them (look_live.py mark).
std::vector<std::uint8_t> Marked(std::vector<std::uint8_t> t, std::size_t copy) {
    CoopLookParts::Mark(t, copy);
    return t;
}

// A texture file's image at `address`: a header naming `name`, then the
// texels; and the game makes the texture over them.
void PutFile(FakeTextures& m, std::uint64_t address, const std::string& name, const std::vector<std::uint8_t>& texels) {
    std::uint8_t* p = m.at(address);
    std::memcpy(p, "BNTX", 4);
    std::memcpy(p + 0x40, name.c_str(), name.size() + 1);
    std::memcpy(p + kOffset - 16, "BRTD", 4);
    std::memcpy(p + kOffset, texels.data(), texels.size());
    m.Make(address + kOffset, texels.size());
}

using Named = std::vector<std::pair<std::string, std::vector<std::uint8_t>>>;

// A coop_look file, as tools/puppet_pack/look_live.py packs it.
std::vector<std::uint8_t> LookFile(const Named& textures) {
    std::vector<std::uint8_t> out = {'C', 'O', 'O', 'P', 'L', 'O', 'O', 'K'};
    const auto put = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    put(1, 4);
    put(textures.size(), 4);
    for (const auto& [name, texels] : textures) {
        std::string n = name;
        n.resize(64, '\0');
        out.insert(out.end(), n.begin(), n.end());
        put(kOffset, 8);
        put(texels.size(), 8);
        out.insert(out.end(), texels.begin(), texels.end());
    }
    return out;
}

std::shared_ptr<const CoopLookTunic::Texels> Colour(const Named& textures) {
    auto t = CoopLookTunic::Parse(LookFile(textures));
    REQUIRE(t);
    return std::make_shared<const CoopLookTunic::Texels>(std::move(*t));
}

// The renderer draws the texture at `at` in that seed's texels.
bool Drawn(const FakeTextures& m, std::uint64_t at, std::uint8_t seed) {
    return m.Drawn(at, kTexels) == TexelsOf(seed);
}
// The game's memory holds that seed's texels at `at`.
bool Holding(FakeTextures& m, std::uint64_t at, std::uint8_t seed) {
    return std::memcmp(m.at(at), TexelsOf(seed).data(), kTexels) == 0;
}

}  // namespace

TEST_CASE("look: a chosen look waits for the pause screen to close") {
    CoopLook::Choice c(Tunic(0x3070FF));
    CHECK(c.Shown() == Tunic(0x3070FF));
    CHECK_FALSE(c.Unpause());
    c.SetPending(Tunic(0xFF3030));
    CHECK(c.Shown() == Tunic(0xFF3030));
    CHECK(c.Worn() == Tunic(0x3070FF));   // nothing changes before the unpause
    const auto worn = c.Unpause();
    REQUIRE(worn);
    CHECK(*worn == Tunic(0xFF3030));
    CHECK(c.Worn() == Tunic(0xFF3030));
    CHECK_FALSE(c.Unpause());   // once
    // Choosing the worn look again takes the waiting one away.
    c.SetPending(Tunic(std::nullopt));
    c.SetPending(Tunic(0xFF3030));
    CHECK_FALSE(c.Unpause());
}

TEST_CASE("look: colours as the settings files and the puppet pack name them") {
    CHECK(CoopLook::ColourText(std::nullopt).empty());
    CHECK(CoopLook::ColourText(CoopColors::Parse("blue")) == "blue");
    CHECK(CoopLook::ColourText(0x123456u) == "#123456");
    CHECK(CoopColors::Parse(CoopLook::ColourText(0x123456u)) == 0x123456u);
    CHECK(CoopLook::TunicName(std::nullopt) == "green");
    CHECK(CoopLook::TunicName(CoopColors::Parse("green")) == "green");
    CHECK(CoopLook::TunicName(CoopColors::Parse("blue")) == "blue");
    CHECK(CoopLook::TunicName(0xF03020u) == "red");   // the nearest pack colour
}

TEST_CASE("look: the colour is saved in coop-options.ini for a kit, else in coop.ini") {
    std::string coop = "server = 1.2.3.4\n";
    std::string options = "# color = blue\nLICHII_COOP_TUNICS=1\n";
    CHECK(CoopLook::WriteSettings(Tunic(CoopColors::Parse("red")), coop, &options));
    CHECK(coop == "server = 1.2.3.4\n");
    CHECK(options == "color = red\nLICHII_COOP_TUNICS=1\n");
    CHECK_FALSE(CoopLook::WriteSettings(Tunic(CoopColors::Parse("red")), coop, &options));   // unchanged
    CHECK(CoopLook::WriteSettings(Tunic(std::nullopt), coop, &options));
    CHECK(options.rfind("color", 0) != 0);   // commented out: the player number's colour
    CHECK_FALSE(CoopTitleMenu::IniGet(options, "color").has_value());
    CHECK(CoopLook::WriteSettings(Tunic(0x123456u), coop, nullptr));
    CHECK(coop == "server = 1.2.3.4\ncolor = #123456\n");
}

TEST_CASE("look: every part has its number, a key and a row, and the numbers never move") {
    std::set<std::string> keys;
    for (std::size_t i = 0; i < CoopLook::kParts; ++i) {
        const auto part = CoopLook::PartOf(static_cast<std::uint8_t>(i));
        REQUIRE(part);
        const CoopLook::PartInfo& info = CoopLook::InfoOf(*part);
        CHECK(info.part == *part);
        CHECK(keys.insert(info.key).second);
        CHECK(std::string(info.item).size() > 0);
        CHECK(std::string(info.name).size() > 0);
    }
    CHECK_FALSE(CoopLook::PartOf(static_cast<std::uint8_t>(CoopLook::kParts)));
    // The numbers are the settings' and the network's: they never change.
    CHECK(static_cast<int>(Part::tunic) == 0);
    CHECK(static_cast<int>(Part::hylian_frame) == 3);
    CHECK(static_cast<int>(Part::master_blade) == 11);
    CHECK(static_cast<int>(Part::gauntlets) == 16);
}

TEST_CASE("look: every part is saved under its own key, and read back") {
    Look look;
    look[Part::tunic] = CoopColors::Parse("red");
    look[Part::hylian_frame] = 0xE0C030u;
    look[Part::master_hilt] = CoopColors::Parse("blue");
    std::string coop = "server = 1.2.3.4\n";
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    CHECK(coop == "server = 1.2.3.4\ncolor = red\nlook_hylian_frame = #E0C030\nlook_master_hilt = blue\n");
    std::vector<std::string> errors;
    const Look back = CoopLook::ReadParts(coop, &errors);
    CHECK(errors.empty());
    CHECK_FALSE(back[Part::tunic]);   // the tunic comes with the co-op settings, not here
    CHECK(back[Part::hylian_frame] == 0xE0C030u);
    CHECK(back[Part::master_hilt] == CoopColors::Parse("blue"));
    CHECK_FALSE(back[Part::boots]);
    // A part set back to the game's own is commented out; one never set stays out of the file.
    look[Part::hylian_frame].reset();
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    CHECK_FALSE(CoopTitleMenu::IniGet(coop, "look_hylian_frame").has_value());
    CHECK(coop.find("look_boots") == std::string::npos);
    CHECK(CoopLook::ReadParts("look_boots = sparkly\n", &errors) == Look{});
    CHECK(errors.size() == 1);
}

TEST_CASE("look: the wire form lists the set parts by number") {
    Look look;
    look[Part::zora_tunic] = 0x102030u;
    look[Part::boots] = 0xFFFFFFu;
    const auto wire = CoopLook::Wire(look);
    REQUIRE(wire.size() == 2);
    CHECK(wire[0] == std::pair<std::uint8_t, std::uint32_t>{2, 0x102030u});
    CHECK(wire[1] == std::pair<std::uint8_t, std::uint32_t>{15, 0xFFFFFFu});
    CHECK(CoopLook::FromWire(wire) == look);
    // A part a newer build has is passed over.
    CHECK(CoopLook::FromWire({{2, 0x102030u}, {15, 0xFFFFFFu}, {40, 1u}}) == look);
    CHECK(CoopLook::Wire(Look{}).empty());
}

TEST_CASE("look: the module applies a chosen look on the frame after the unpause, once") {
    std::vector<Look> applied;
    CoopLook::Init(Tunic(std::nullopt), [&](const Look& l) { applied.push_back(l); });
    CoopLook::Frame();
    CHECK(applied.empty());   // the start's look is not applied again
    CoopLook::SetPending(Tunic(0x3070FF));
    CoopLook::Frame();
    CHECK(applied.empty());   // still paused
    CHECK(CoopLook::Shown() == Tunic(0x3070FF));
    CHECK(CoopLook::Worn() == Tunic(std::nullopt));
    CoopLook::ApplyOnUnpause();
    CoopLook::Frame();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0] == Tunic(0x3070FF));
    CHECK(CoopLook::Worn() == Tunic(0x3070FF));
    CoopLook::Frame();
    CoopLook::ApplyOnUnpause();   // a pause with no change
    CoopLook::Frame();
    CHECK(applied.size() == 1);
    CoopLook::Init({}, nullptr);
}

TEST_CASE("look tunic: look files are read whole or not at all") {
    const auto file = LookFile({{"Tile_A", TexelsOf(1)}, {"Vest", TexelsOf(2)}});
    const auto t = CoopLookTunic::Parse(file);
    REQUIRE(t);
    REQUIRE(t->size() == 2);
    CHECK((*t)[1].name == "Vest");
    CHECK((*t)[1].offset == kOffset);
    CHECK((*t)[1].texels == TexelsOf(2));
    std::string why;
    CHECK_FALSE(CoopLookTunic::Parse({file.begin(), file.end() - 1}, &why));
    CHECK(why == "the look file is cut short");
    auto other = file;
    other[8] = 2;   // another version
    CHECK_FALSE(CoopLookTunic::Parse(other));
    CHECK(CoopLookTunic::SameLayout(*t, *Colour({{"Tile_A", TexelsOf(9)}, {"Vest", TexelsOf(8)}})));
    CHECK_FALSE(CoopLookTunic::SameLayout(*t, *Colour({{"Tile_A", TexelsOf(9)}})));
}

TEST_CASE("look tunic: a texture is known by its exact texels, with no file in front, a marked copy left alone") {
    FakeTextures m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}});
    std::memcpy(m.at(kBase + 0x3000), TexelsOf(1).data(), kTexels);
    m.Make(kBase + 0x3000, kTexels);   // your own: the game copied the texels into its texture memory
    std::memcpy(m.at(kBase + 0x4000), Marked(TexelsOf(1), 2).data(), kTexels);
    m.Make(kBase + 0x4000, kTexels);   // a puppet's copy 2: the same texels, marked
    m.Make(kBase + 0x20000, kTexels);  // a storage outside the readable memory

    const auto found = CoopLookTunic::Find(m, {green});
    REQUIRE(found.places.size() == 1);
    CHECK(found.places[0].address == kBase + 0x3000);
    CHECK(found.listed == 3);
    CHECK(found.unread == 1);
    CHECK(found.heads == 1);
}

TEST_CASE("look tunic: your Link's textures are found and drawn over, the game's memory and a puppet's copy left alone") {
    FakeTextures m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}, {"Vest", TexelsOf(2)}});
    const auto red = Colour({{"Tile_A", TexelsOf(9)}, {"Vest", TexelsOf(8)}});
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    PutFile(m, kBase + 0x3000, "Vest", TexelsOf(2));
    PutFile(m, kBase + 0x5000, "Tile_A_Red", Marked(TexelsOf(1), 1));   // a puppet's copy: marked
    m.at(kBase + 0x7000)[0] = 1;
    m.Make(kBase + 0x7000, kTexels);   // another texture of that size, other texels

    const auto found = CoopLookTunic::Find(m, {green, red});
    REQUIRE(found.places.size() == 2);
    CHECK(found.heads == 1);
    auto place = found.places[0];
    CHECK(place.address == kBase + 0x1000 + kOffset);
    CHECK((place.held == green));
    CHECK((place.drawn == green));
    REQUIRE(CoopLookTunic::Draw(m, place, red));
    CHECK((place.drawn == red));
    CHECK(place.overridden);
    CHECK(Drawn(m, place.address, 9));
    CHECK(Holding(m, place.address, 1));   // the game's memory: as it was
    CHECK(m.Drawn(kBase + 0x5000 + kOffset, kTexels) == Marked(TexelsOf(1), 1));   // the puppet's copy: as it was
    // Back to the game's own: the override taken away.
    REQUIRE(CoopLookTunic::Draw(m, place, green));
    CHECK_FALSE(place.overridden);
    CHECK(m.overrides() == 0);
    CHECK(Drawn(m, place.address, 1));
    // An override outside a texture the game has is refused.
    m.Finalize(place.address);
    CHECK_FALSE(CoopLookTunic::Draw(m, place, red));
    CHECK(m.overrides() == 0);
    // Another texture made over the same storage after the place was found (a late Draw): refused too.
    m.Make(place.address, kTexels);
    CHECK_FALSE(CoopLookTunic::Draw(m, place, red));
    CHECK(m.overrides() == 0);
}

TEST_CASE("look tunic: the wardrobe keeps your Link in the colour worn") {
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> log;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        if (it == pack.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    host.log = [&](const std::string& l) { log.push_back(l); };
    CoopLookTunic::Wardrobe w(host);
    const std::uint64_t at = kBase + 0x1000 + kOffset;

    w.Wear("green");
    CHECK_FALSE(w.Busy());   // the game's own: nothing to do
    w.Wear("red");
    CHECK(w.Busy());
    w.Step(0);
    CHECK(Drawn(m, at, 9));
    CHECK(Holding(m, at, 1));
    // Nothing changed: nothing set again.
    const int set = m.overridden;
    w.Step(1000);
    CHECK(m.overridden == set);
    // The game makes the texture again (a scene change; its override went with the old one): red again.
    m.Make(at, kTexels);
    CHECK(Drawn(m, at, 1));
    w.Step(2000);
    CHECK(Drawn(m, at, 9));
    // A colour the pack does not have: the one worn stays, said once.
    w.Wear("teal");
    w.Step(3000);
    w.Step(4000);
    CHECK(Drawn(m, at, 9));
    CHECK(std::count_if(log.begin(), log.end(), [](const std::string& l) { return l.find("teal") != std::string::npos; }) == 1);
    // Back to green: the override taken away, then nothing more to do.
    w.Wear("green");
    w.Step(5000);
    CHECK(Drawn(m, at, 1));
    CHECK(m.overrides() == 0);
    CHECK_FALSE(w.Busy());
    // The module stops while red is worn: its override goes.
    w.Wear("red");
    w.Step(6000);
    CHECK(m.overrides() == 1);
    w.Clear();
    CHECK(m.overrides() == 0);
    CHECK(Drawn(m, at, 1));
    // A later start wears red again.
    w.Step(7000);
    CHECK(Drawn(m, at, 9));
}

TEST_CASE("look tunic: a texture the game makes again is drawn at the next step, the others not read again") {
    // 0.11.0 players: the game made Link's textures again (scene changes, cutscenes, its streaming)
    // and each time the game's colours showed until the next round, a second later: flicker.
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::uint64_t other = kBase + 0x3000 + kOffset;   // a texture of the size that is not Link's
    m.Make(other, kTexels);
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    const std::uint64_t at = kBase + 0x1000 + kOffset;
    w.Wear("red");
    w.Step(0);
    CHECK(Drawn(m, at, 9));
    CHECK(m.reads[other] > 0);
    // (the other texture, no colour in it, is read again while it is young: kLateTexelsMs)
    m.Make(at, kTexels);
    w.Step(1100);
    CHECK(Drawn(m, at, 9));
    const int other_reads = m.reads[other];
    // Made again 20 ms later: red at the next step; the other texture (no colour) is not read again.
    m.Make(at, kTexels);
    CHECK(Drawn(m, at, 1));
    w.Step(1120);
    CHECK(Drawn(m, at, 9));
    CHECK(m.reads[other] == other_reads);
    // A new texture of Link's made since: found by the quick step too.
    const std::uint64_t second = kBase + 0x5000 + kOffset;
    std::memcpy(m.at(second), TexelsOf(1).data(), kTexels);
    m.Make(second, kTexels);
    w.Step(1140);
    CHECK(Drawn(m, second, 9));
    CHECK(m.reads[other] == other_reads);
    // Its texels came after it was made (a quick step missed it): read again at the next step, nothing
    // made since (player report #78: before, only the full search a second on found it).
    const std::uint64_t late = kBase + 0x7000 + kOffset;
    m.Make(late, kTexels);
    w.Step(1160);
    CHECK_FALSE(Drawn(m, late, 9));
    std::memcpy(m.at(late), TexelsOf(1).data(), kTexels);
    w.Step(1180);
    CHECK(Drawn(m, late, 9));
    CHECK(m.reads[other] == other_reads);
    // The same with something made in between (the generation moves).
    const std::uint64_t later = kBase + 0x9000 + kOffset;
    m.Make(later, kTexels);
    w.Step(1200);
    std::memcpy(m.at(later), TexelsOf(1).data(), kTexels);
    m.Make(other, kTexels);
    w.Step(1220);
    CHECK(Drawn(m, later, 9));
    CHECK(m.reads[other] > other_reads);
}

TEST_CASE("look tunic: texels that come in late are drawn within a step or two, each texture drawn once") {
    // Player report #78 (0.11.1): your own customised sword showed the game's colours for up to a second
    // after each cutscene or scene, then snapped back. The game makes the texture first, its texels after.
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> said;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [&](const std::string& s) { said.push_back(s); };
    CoopLookTunic::Wardrobe w(host);
    const auto repaints = [&] {
        return std::count_if(said.begin(), said.end(), [](const std::string& s) { return s.find(" now (") != std::string::npos; });
    };
    w.Wear("red");
    w.Step(0);
    REQUIRE(Drawn(m, kBase + 0x1000 + kOffset, 9));
    REQUIRE(repaints() == 1);
    // Made again with nothing in it; its first bytes come in, then the rest, steps apart.
    const std::uint64_t at = kBase + 0x4000 + kOffset;
    m.Make(at, kTexels);
    w.Step(20);
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels / 2);
    w.Step(40);
    CHECK_FALSE(Drawn(m, at, 9));
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels);
    w.Step(60);
    CHECK(Drawn(m, at, 9));
    CHECK(repaints() == 2);
    // Drawn once: later steps (made or not made anything) draw nothing again.
    const int overridden = m.overridden;
    for (std::uint64_t t = 80; t <= 2000; t += 20) {
        if (t % 400 == 0) {
            m.Make(kBase + 0xa000 + kOffset, kTexels);   // (something else made now and then)
        }
        w.Step(t);
    }
    CHECK(m.overridden == overridden);
    CHECK(repaints() == 2);
    CHECK(Drawn(m, at, 9));
}

TEST_CASE("look tunic: a scene's many new textures are read again at most kLateChecks a step, for kLateTexelsMs") {
    FakeTextures m(kBase, 0x40000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    w.Wear("red");
    w.Step(0);
    // 200 textures of the size with no colour in them (the scene's), and one of Link's late.
    std::vector<std::uint64_t> made;
    for (std::uint64_t i = 0; i < 200; ++i) {
        made.push_back(kBase + 0x2000 + i * 0x120);
        m.Make(made.back(), kTexels);
    }
    const std::uint64_t late = made[150];
    w.Step(20);   // (the generation moved: every new one read once)
    const auto reads = [&] {
        int n = 0;
        for (const std::uint64_t a : made) {
            n += m.reads[a];
        }
        return n;
    };
    CHECK(reads() == 200);
    // The textures one step reads (a texture whose first bytes are a colour's is read twice: head, then whole).
    const auto step = [&](std::uint64_t at_ms) {
        const std::map<std::uint64_t, int> was = m.reads;
        w.Step(at_ms);
        std::size_t n = 0;
        for (const std::uint64_t a : made) {
            n += m.reads[a] != (was.count(a) ? was.at(a) : 0) ? 1 : 0;
        }
        return n;
    };
    std::memcpy(m.at(late), TexelsOf(1).data(), kTexels);
    std::size_t n = step(40);
    CHECK(n <= CoopLookTunic::kLateChecks);
    CHECK(n > 0);
    // Every one is read again in turn: Link's is drawn within a few steps.
    std::uint64_t t = 40;
    while (!Drawn(m, late, 9) && t < 200) {
        t += 20;
        CHECK(step(t) <= CoopLookTunic::kLateChecks);
    }
    CHECK(Drawn(m, late, 9));
    CHECK(t <= 20 + 20 * (200 / CoopLookTunic::kLateChecks + 1));
    // Once they are older than kLateTexelsMs, a step with nothing made reads none of them.
    w.Step(20 + CoopLookTunic::kLateTexelsMs + 20);
    CHECK(step(20 + CoopLookTunic::kLateTexelsMs + 40) == 0);
}

TEST_CASE("look tunic: while the game makes textures every step, the young ones are still read at most kLateChecks a step") {
    // review-sword-blink: a scene load makes textures over many steps; each such step reads the new
    // ones and at most kLateChecks of the young misses, not all of them again.
    FakeTextures m(kBase, 0x40000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    w.Wear("red");
    w.Step(0);
    std::vector<std::uint64_t> made;
    for (std::uint64_t i = 0; i < 200; ++i) {
        made.push_back(kBase + 0x2000 + i * 0x120);
        m.Make(made.back(), kTexels);
    }
    const std::uint64_t late = made[150];
    w.Step(20);
    std::memcpy(m.at(late), TexelsOf(1).data(), kTexels);
    // One new texture made before each step (none of Link's): the 200 are read <= kLateChecks a step.
    std::uint64_t t = 20;
    std::uint64_t fresh = kBase + 0x20000;
    while (!Drawn(m, late, 9) && t < 200) {
        t += 20;
        m.Make(fresh, kTexels);
        fresh += 0x120;
        const std::map<std::uint64_t, int> was = m.reads;
        w.Step(t);
        std::size_t n = 0;
        for (const std::uint64_t a : made) {
            n += m.reads[a] != (was.count(a) ? was.at(a) : 0) ? 1 : 0;
        }
        CHECK(n <= CoopLookTunic::kLateChecks);
        CHECK(m.reads[fresh - 0x120] > 0);   // (the new one is read at once)
    }
    CHECK(Drawn(m, late, 9));
    CHECK(t <= 20 + 20 * (200 / CoopLookTunic::kLateChecks + 1));
}

TEST_CASE("look tunic: an override goes when the game's memory there holds something else, its texture not finalized") {
    // 0.11.0 players (tracker #32): effects' textures gone. The game gave the memory of Link's
    // texture to another texture without finalizing it; the override stayed, drawn over the new one.
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> said;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [&](const std::string& s) { said.push_back(s); };
    CoopLookTunic::Wardrobe w(host);
    const std::uint64_t at = kBase + 0x1000 + kOffset;
    w.Wear("red");
    w.Step(0);
    REQUIRE(Drawn(m, at, 9));
    // Another texture's texels over that memory, made a little before it (no finalize of Link's).
    const std::vector<std::uint8_t> effect = TexelsOf(50);
    std::memcpy(m.at(at), effect.data(), kTexels);
    m.Make(at - 0x40, kTexels + 0x40);
    w.Step(20);
    CHECK(m.overrides() == 0);
    CHECK(m.Drawn(at, kTexels) == effect);
    CHECK(w.places().empty());
    CHECK(std::any_of(said.begin(), said.end(), [](const std::string& s) {
        return s.find("taken away") != std::string::npos;
    }));
    // A texture of Link's kept since keeps its override (nothing taken away).
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels);
    m.Make(at, kTexels);
    w.Step(40);
    CHECK(Drawn(m, at, 9));
    const std::size_t lines = said.size();
    m.Make(kBase + 0x9000, kTexels);   // (something else made: the generation moves)
    w.Step(60);
    CHECK(Drawn(m, at, 9));
    CHECK(std::none_of(said.begin() + static_cast<std::ptrdiff_t>(lines), said.end(), [](const std::string& s) {
        return s.find("taken away") != std::string::npos;
    }));
}

// ---------------------------------------------------------------------------
// The gear (coop_look_parts.h)

namespace {

// BC1 texels of kTexels bytes: blocks of a blue end pair and of a grey one in turn.
std::vector<std::uint8_t> Shield() {
    std::vector<std::uint8_t> t;
    for (std::size_t b = 0; b < kTexels / 8; ++b) {
        const std::uint16_t c0 = b % 2 == 0 ? 0x3a54 : 0xb5b6;   // (60,72,160) blue / (180,180,180) grey
        const std::uint16_t c1 = b % 2 == 0 ? 0x1909 : 0x5aeb;
        const std::uint8_t block[8] = {static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(c0 >> 8),
                                       static_cast<std::uint8_t>(c1), static_cast<std::uint8_t>(c1 >> 8),
                                       0x1b, 0x1b, 0x1b, 0x1b};
        t.insert(t.end(), block, block + 8);
    }
    return t;
}

const char* const kHylian = "Mt_Weapon_Shield_Hylian_Alb";

}  // namespace

TEST_CASE("look parts: a look file packed is read back the same") {
    const auto texels = Colour({{"A", TexelsOf(1)}, {"B_Alb", TexelsOf(2)}});
    const auto back = CoopLookTunic::Parse(CoopLookTunic::Pack(*texels));
    REQUIRE(back);
    CHECK(back->size() == 2);
    CHECK(CoopLookTunic::SameLayout(*back, *texels));
    CHECK((*back)[1].texels == TexelsOf(2));
    CHECK(CoopLookTunic::Pack(*texels) == LookFile({{"A", TexelsOf(1)}, {"B_Alb", TexelsOf(2)}}));
}

TEST_CASE("look parts: every gear part has a texture, every texture once") {
    std::set<std::string> names;
    std::set<Part> parts;
    for (const auto& g : CoopLookParts::Gear()) {
        CHECK(names.insert(g.name).second);
        CHECK_FALSE(g.rules.empty());
        for (const auto& r : g.rules) {
            CHECK(r.part != Part::tunic);   // the tunic has the pack's colours (coop_look_tunic.h)
            CHECK(r.tones != 0);
            parts.insert(r.part);
        }
    }
    CHECK(parts.size() == CoopLook::kParts - 1);
}

TEST_CASE("look parts: the gear painted in a look, a painting's name says its colours") {
    Look look;
    CHECK_FALSE(CoopLookParts::AnyGear(look));
    CHECK(CoopLookParts::Name(look) == "gear");
    look[Part::tunic] = 0xFF0000u;
    CHECK(CoopLookParts::Name(look) == "gear");   // the tunic is not gear
    look[Part::hylian_face] = 0x20A040u;
    CHECK(CoopLookParts::AnyGear(look));
    const std::string name = CoopLookParts::Name(look);
    CHECK(name.find("20A040") != std::string::npos);
    look[Part::hylian_face] = 0x20A041u;
    CHECK(CoopLookParts::Name(look) != name);
    look[Part::hylian_face] = 0x20A040u;

    const auto gear = Colour({{kHylian, Shield()}, {"Something_Else_Alb", Shield()}});
    const auto painted = CoopLookParts::Painted(*gear, look);
    REQUIRE(painted.size() == 2);
    CHECK(painted[1].texels == Shield());   // not a gear texture: as it is
    const auto& p = painted[0].texels;
    CHECK(p != Shield());
    // the blue blocks are green now, the grey ones as they were
    CHECK(std::memcmp(p.data() + 8, Shield().data() + 8, 8) == 0);
    const auto ends = CoopLookPaint::Ends(CoopLookPaint::Format::bc1, std::span<const std::uint8_t>(p.data(), 8));
    CHECK(CoopLookPaint::ToneOf(ends[0]) == CoopLookPaint::kGreen);
    // nothing set for this texture's parts: as it is
    Look boots;
    boots[Part::boots] = 0x000080u;
    CHECK(CoopLookParts::Painted(*gear, boots)[0].texels == Shield());
}

namespace {
// metal.bin's bytes (tools/puppet_pack/look_metal.py pack).
struct MetalEntry {
    std::string name;
    std::uint32_t part;
    std::vector<std::uint32_t> offsets;
};
std::vector<std::uint8_t> MetalFile(const std::vector<MetalEntry>& entries) {
    std::vector<std::uint8_t> out = {'C', 'O', 'O', 'P', 'M', 'T', 'L', '1'};
    const auto put = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    put(static_cast<std::uint32_t>(entries.size()));
    for (const MetalEntry& e : entries) {
        std::string n = e.name;
        n.resize(64, '\0');
        out.insert(out.end(), n.begin(), n.end());
        put(e.part);
        put(static_cast<std::uint32_t>(e.offsets.size()));
        for (const std::uint32_t o : e.offsets) {
            put(o);
        }
    }
    return out;
}
const char* const kHylianMetal = "Mt_Weapon_Shield_Hylian_Mtl";
// Eight BC4 blocks (a look file's texture holds 64 bytes or more), every one metal (ends 200 and 120).
std::vector<std::uint8_t> MetalTexels() {
    std::vector<std::uint8_t> t;
    for (int b = 0; b < 8; ++b) {
        const std::uint8_t block[8] = {200, 120, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
        t.insert(t.end(), block, block + 8);
    }
    return t;
}
}  // namespace

TEST_CASE("look parts: metal.bin is read whole or not at all") {
    const auto file = MetalFile({{kHylianMetal, 3, {0, 16}}, {kHylianMetal, 4, {8}}});
    const auto metal = CoopLookParts::ParseMetal(file);
    REQUIRE(metal);
    REQUIRE(metal->size() == 2);
    CHECK((*metal)[0].texture == kHylianMetal);
    CHECK((*metal)[0].part == Part::hylian_frame);
    CHECK((*metal)[0].offsets == std::vector<std::uint32_t>{0, 16});
    CHECK((*metal)[1].part == Part::hylian_face);
    std::string why;
    auto cut = file;
    cut.pop_back();
    CHECK_FALSE(CoopLookParts::ParseMetal(cut, &why));
    CHECK(why == "the metal file is cut short");
    auto magic = file;
    magic[7] = '2';
    CHECK_FALSE(CoopLookParts::ParseMetal(magic, &why));
    CHECK(why == "not a metal file");
    CHECK_FALSE(CoopLookParts::ParseMetal(MetalFile({{kHylianMetal, CoopLook::kParts, {0}}}), &why));
    CHECK(why == "a metal entry names no part");
    CHECK(CoopLookParts::ParseMetal(MetalFile({}))->empty());
}

TEST_CASE("look parts: a painted metal part loses its metal, the other blocks and an unpainted part keep it (#202)") {
    const CoopLookParts::Metal metal = *CoopLookParts::ParseMetal(
        MetalFile({{kHylianMetal, 3, {0, 16, 64}}, {kHylianMetal, 4, {8}}, {"Other_Mtl", 3, {24}}}));
    Look look;
    std::vector<std::uint8_t> t = MetalTexels();
    // nothing painted: the game's own metal
    CHECK(CoopLookParts::PaintMetal(metal, kHylianMetal, t, look) == 0);
    CHECK(t == MetalTexels());
    // the frame painted: its blocks 0 and 2 are 0 (offset 64 is past the texels: left alone)
    look[Part::hylian_frame] = 0xE0C030u;
    CHECK(CoopLookParts::PaintMetal(metal, kHylianMetal, t, look) == 2);
    CHECK(std::all_of(t.begin(), t.begin() + 8, [](std::uint8_t b) { return b == 0; }));
    CHECK(std::equal(t.begin() + 8, t.begin() + 16, MetalTexels().begin() + 8));   // the face's: kept
    CHECK(std::all_of(t.begin() + 16, t.begin() + 24, [](std::uint8_t b) { return b == 0; }));
    CHECK(std::equal(t.begin() + 24, t.end(), MetalTexels().begin() + 24));   // Other_Mtl's entry
    // a misaligned offset is never written
    const CoopLookParts::Metal odd = *CoopLookParts::ParseMetal(MetalFile({{kHylianMetal, 3, {4}}}));
    std::vector<std::uint8_t> u = MetalTexels();
    CHECK(CoopLookParts::PaintMetal(odd, kHylianMetal, u, look) == 0);
    CHECK(u == MetalTexels());

    // Painted uses the table it is given (UseMetal); without one, metal stays.
    const auto gear = Colour({{kHylian, Shield()}, {kHylianMetal, MetalTexels()}});
    CHECK(CoopLookParts::Painted(*gear, look)[1].texels == MetalTexels());
    CoopLookParts::UseMetal(std::make_shared<const CoopLookParts::Metal>(metal));
    const auto painted = CoopLookParts::Painted(*gear, look);
    CHECK(painted[1].texels == t);
    CHECK(painted[0].texels != Shield());   // (the frame's colour as before)
    Look plain;
    CHECK(CoopLookParts::Painted(*gear, plain)[1].texels == MetalTexels());
    // a puppet's copy: renamed, its metal painted down, then marked
    const auto copy = CoopLookParts::CopyTexels(*gear, 2, look);
    REQUIRE(copy.size() == 2);
    CHECK(copy[1].name == "C2_Weapon_Shield_Hylian_Mtl");
    auto marked = t;
    CoopLookParts::Mark(marked, 2);
    CHECK(copy[1].texels == marked);
    CoopLookParts::UseMetal(nullptr);
    CHECK(CoopLookParts::Painted(*gear, look)[1].texels == MetalTexels());
}

TEST_CASE("look parts: the adult's Goron and Zora tunics are painted in all their cloth") {
    // The adult's tunic models name more cloth than the vest and hat (the dump: Armor_Adult_Goron's inner,
    // Armor_Adult_Zora's parts and feather): each is in the table under its tunic.
    const auto has = [](const char* name, Part part) {
        for (const CoopLookParts::GearTexture& g : CoopLookParts::Gear()) {
            if (std::string_view(g.name) == name) {
                return std::any_of(g.rules.begin(), g.rules.end(), [&](const auto& r) { return r.part == part; });
            }
        }
        return false;
    };
    CHECK(has("Mt_Inner_Armor_Adult_Goron_Alb", Part::goron_tunic));
    CHECK(has("Mt_Parts_Armor_Adult_Zora_Alb", Part::zora_tunic));
    CHECK(has("Mt_Feather_Armor_Adult_Zora_Alb", Part::zora_tunic));
    // The Zora feather (BC1, blue ends as Shield's face): painted in the Zora tunic's colour, not the Goron's.
    const auto gear = Colour({{"Mt_Feather_Armor_Adult_Zora_Alb", Shield()}});
    Look goron;
    goron[Part::goron_tunic] = 0x20A040u;
    CHECK(CoopLookParts::Painted(*gear, goron)[0].texels == Shield());
    Look zora;
    zora[Part::zora_tunic] = 0x20A040u;
    const auto p = CoopLookParts::Painted(*gear, zora)[0].texels;
    const auto ends = CoopLookPaint::Ends(CoopLookPaint::Format::bc1, std::span<const std::uint8_t>(p.data(), 8));
    CHECK(CoopLookPaint::ToneOf(ends[0]) == CoopLookPaint::kGreen);
}

TEST_CASE("look parts: the gear wardrobe paints your shield, and one the game makes later") {
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kHylian, Shield());
    Look look;
    look[Part::hylian_frame] = 0xE0C030u;
    const auto base = LookFile({{kHylian, Shield()}});
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        if (c == CoopLookParts::kBase) {
            return base;
        }
        return CoopLookTunic::Pack(CoopLookParts::Painted(*CoopLookTunic::Parse(base), look));
    };
    std::vector<std::string> log;
    host.log = [&](const std::string& l) { log.push_back(l); };
    host.base = CoopLookParts::kBase;
    host.what = "gear";
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLookParts::Name(look));
    w.Step(0);
    const auto want = CoopLookParts::Painted(*CoopLookTunic::Parse(base), look)[0].texels;
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, want.size()) == want);
    CHECK(std::memcmp(m.at(kBase + 0x1000 + kOffset), Shield().data(), kTexels) == 0);   // the game's memory: its own
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("your Link's gear is") != std::string::npos; }));
    // A second shield's texture is made (the game's own texels): painted at the next round.
    PutFile(m, kBase + 0x4000, kHylian, Shield());
    w.Step(1000);
    CHECK(m.Drawn(kBase + 0x4000 + kOffset, want.size()) == want);
    // Back to the game's own colours.
    w.Wear(CoopLookParts::kBase);
    w.Step(2000);
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, kTexels) == Shield());
    CHECK(m.overrides() == 0);
    CHECK_FALSE(w.Busy());
}

TEST_CASE("look: an unpause with the look unchanged is told too") {
    int applied = 0, unpaused = 0;
    CoopLook::Init(Look{}, [&](const Look&) { ++applied; }, [&] { ++unpaused; });
    CoopLook::ApplyOnUnpause();
    CoopLook::Frame();
    CHECK(applied == 0);
    CHECK(unpaused == 1);
    Look l;
    l[Part::boots] = 1u;
    CoopLook::SetPending(l);
    CoopLook::ApplyOnUnpause();
    CoopLook::Frame();
    CHECK(applied == 1);
    CHECK(unpaused == 1);
    CoopLook::Frame();   // no unpause: nothing
    CHECK(unpaused == 1);
    CoopLook::Init(Look{}, nullptr);
}

TEST_CASE("look tunic: the colour chosen before is worn at start, once the game loads your Link's tunic") {
    // A player's report (0.10.1, before look-live): "Chosen color doesn't apply for self, but does for others".
    // StartModule wears the settings' colour (coop.ini / coop-options.ini through LICHII_COOP_COLOR) before the
    // game has loaded Link: the texture comes later (the title, then the save's scene), and is drawn blue then.
    FakeTextures m(kBase, 0x10000);
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"blue", LookFile({{"Tile_A", TexelsOf(7)}})},
    };
    std::vector<std::string> log;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        if (it == pack.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    host.log = [&](const std::string& l) { log.push_back(l); };
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLook::TunicName(0x2050E0));   // a hex colour near blue: the pack's nearest tunic
    CHECK(w.Busy());
    w.Step(0);   // nothing of Link loaded yet
    const std::uint64_t at = kBase + 0x2000 + kOffset;
    PutFile(m, kBase + 0x2000, "Tile_A", TexelsOf(1));   // the game loads the tunic (green, as the game has it)
    w.Step(1000);   // the next round
    CHECK(Drawn(m, at, 7));     // blue, with no pause-menu change and no restart
    CHECK(Holding(m, at, 1));   // (through the texture override: the game's memory keeps its own)
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("is blue now") != std::string::npos; }));
}

TEST_CASE("look parts: a puppet copy's textures are named per copy and painted in its look") {
    CHECK(CoopLookParts::SlotName(kHylian, 2) == "C2_Weapon_Shield_Hylian_Alb");
    CHECK(CoopLookParts::SlotName("Mt_Weapon_Sword_Biggoron_Alb.00", 1) == "C1_Weapon_Sword_Biggoron_Alb.00");
    CHECK(CoopLookParts::SlotName("Angin_Tile_A_Alb", 1).empty());
    CHECK(CoopLookParts::SlotName(kHylian, 0).empty());
    for (const auto& g : CoopLookParts::Gear()) {
        CHECK(CoopLookParts::SlotName(g.name, 8).size() == std::strlen(g.name));   // as long (look_items.py)
    }
    Look red;
    red[Part::hylian_face] = 0xFF0000u;
    const auto gear = Colour({{kHylian, Shield()}, {"Something_Else_Alb", Shield()}});
    const auto plain = CoopLookParts::CopyTexels(*gear, 3, std::nullopt);
    REQUIRE(plain.size() == 1);   // not a gear texture: no copy of it
    CHECK(plain[0].name == "C3_Weapon_Shield_Hylian_Alb");
    CHECK(plain[0].texels == Marked(Shield(), 3));   // marked as the pack's copy file (look_live.py mark)
    CHECK(plain[0].offset == kOffset);
    const auto painted = CoopLookParts::CopyTexels(*gear, 3, red);
    REQUIRE(painted.size() == 1);
    CHECK(painted[0].texels == Marked(CoopLookParts::Painted(*gear, red)[0].texels, 3));
    CHECK(CoopLookTunic::SameLayout(plain, painted));

    CoopLookParts::CopyLooks looks(4);
    CHECK(CoopLookParts::CopiesName(looks) == CoopLookParts::kCopiesBase);
    looks[2] = Look{};   // a look with no gear part: as the game's own
    CHECK(CoopLookParts::CopiesName(looks) == CoopLookParts::kCopiesBase);
    looks[2] = red;
    const std::string name = CoopLookParts::CopiesName(looks);
    CHECK(name != CoopLookParts::kCopiesBase);
    looks[1] = red;
    CHECK(CoopLookParts::CopiesName(looks) != name);   // which copy is painted counts
}

TEST_CASE("look parts: the puppets' wardrobe draws a copy the game makes, and leaves your own shield alone") {
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kHylian, Shield());   // your own
    const auto gear = *CoopLookTunic::Parse(LookFile({{kHylian, Shield()}}));
    Look red;
    red[Part::hylian_face] = 0xFF0000u;
    CoopLookParts::CopyLooks looks(2);
    looks[1] = red;
    const auto all = [&](const CoopLookParts::CopyLooks& l) {
        CoopLookTunic::Texels out;
        for (std::size_t k = 1; k <= l.size(); ++k) {
            const auto t = CoopLookParts::CopyTexels(gear, k, l[k - 1]);
            out.insert(out.end(), t.begin(), t.end());
        }
        return CoopLookTunic::Pack(out);
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        return all(c == CoopLookParts::kCopiesBase ? CoopLookParts::CopyLooks(2) : looks);
    };
    host.log = [](const std::string&) {};
    host.base = CoopLookParts::kCopiesBase;
    host.what = "puppets' gear";
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLookParts::CopiesName(looks));
    w.Step(0);   // no puppet wears a copy yet: nothing drawn
    CHECK(m.overrides() == 0);
    // The game makes copy 2's shield for a puppet (its file, the game's own texels, under the copy's name)...
    PutFile(m, kBase + 0x3000, "C2_Weapon_Shield_Hylian_Alb", Marked(Shield(), 2));
    PutFile(m, kBase + 0x5000, "C1_Weapon_Shield_Hylian_Alb", Marked(Shield(), 1));
    w.Step(1000);
    // ...and it is drawn in that player's look, the game's memory as it was.
    const auto want = Marked(CoopLookParts::Painted(gear, red)[0].texels, 2);
    CHECK(m.Drawn(kBase + 0x3000 + kOffset, want.size()) == want);
    CHECK(std::memcmp(m.at(kBase + 0x3000 + kOffset), Marked(Shield(), 2).data(), kTexels) == 0);
    CHECK(m.Drawn(kBase + 0x5000 + kOffset, kTexels) == Marked(Shield(), 1));   // copy 1: the game's own colours
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, kTexels) == Shield());   // your own: not the puppets'
    w.Clear();
    CHECK(m.overrides() == 0);
}

TEST_CASE("look parts: a copy's texture name never names another folder") {
    CHECK(CoopLookParts::SlotName("Mt_../../x", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_a/b", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_a\\b", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_C:x", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_Weapon_Sword_Biggoron_Alb.01", 2) == "C2_Weapon_Sword_Biggoron_Alb.01");
}

TEST_CASE("look parts: the boots in Link's own model get a copy per tunic colour, painted for its wearer") {
    const std::string kChild = "Mt_Leather_Player_Child_Alb", kAdult = "Mt_Boots_Player_Adult_Alb";
    for (const std::string& b : CoopLookParts::BodyBoots()) {   // gear textures too: your own Link's are painted
        CHECK(std::any_of(CoopLookParts::Gear().begin(), CoopLookParts::Gear().end(),
                          [&](const auto& g) { return b == g.name; }));
        CHECK(CoopLookParts::BootsName(b, 'M').size() == b.size());   // as long (look_items.py boots_name)
    }
    CHECK(CoopLookParts::BootsName(kChild, 'M') == "PM_Leather_Player_Child_Alb");
    CHECK(CoopLookParts::BootsName(kHylian, 'M').empty());
    CHECK(CoopLookParts::LetterOf("pink") == 'M');
    CHECK(CoopLookParts::LetterOf("green") == 'G');
    CHECK_FALSE(CoopLookParts::LetterOf("Pink").has_value());   // CoopLook::TunicName's lower case

    // your own Link first, then the others in order; a colour already worn stays with its first wearer
    CoopLookParts::Wearer own{'M', 0xFFFF00u};
    const auto w = CoopLookParts::Assign(own, {{'M', 0x0000FFu}, {'R', 0x00FF00u}, {'R', 0xFF0000u}, {'B', {}},
                                               {std::nullopt, 0x123456u}});
    CHECK(w == CoopLookParts::Wearers{{'M', 0xFFFF00u}, {'R', 0x00FF00u}});
    CHECK(CoopLookParts::Assign({'B', {}}, {{'B', 0xFF0000u}}).empty());   // your own Link keeps the game's
    CHECK(CoopLookParts::BootsPaintingName({}) == CoopLookParts::kBootsBase);
    CHECK(CoopLookParts::BootsPaintingName(w) == "boots.MFFFF00.R00FF00");

    const auto gear = Colour({{kHylian, Shield()}, {kChild, Shield()}, {kAdult, TexelsOf(3)}});
    const auto plain = CoopLookParts::BootsTexels(*gear, {});
    REQUIRE(plain.size() == 20);   // ten colours, two boots each, no shield
    CHECK(plain[0].name == "PR_Leather_Player_Child_Alb");
    CHECK(plain[0].texels == Marked(Shield(), 'R'));   // marked as the pack's copy (look_items.py boots_mark)
    CHECK(plain[1].name == "PR_Boots_Player_Adult_Alb");
    const auto painted = CoopLookParts::BootsTexels(*gear, w);
    CHECK(CoopLookTunic::SameLayout(plain, painted));
    Look yellow;
    yellow[Part::boots] = 0xFFFF00u;
    const auto want = CoopLookParts::Painted(*gear, yellow);
    for (const auto& t : painted) {
        if (t.name == "PM_Leather_Player_Child_Alb") {
            CHECK(t.texels == Marked(want[1].texels, 'M'));
        } else if (t.name == "PB_Leather_Player_Child_Alb") {
            CHECK(t.texels == Marked(Shield(), 'B'));   // no wearer with boots: the game's colour
        }
    }
}

TEST_CASE("look parts: the boots wardrobe draws a colour's copy, and leaves your own boots alone") {
    const std::string kChild = "Mt_Leather_Player_Child_Alb";
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kChild, Shield());   // your own Link's (the game's model)
    PutFile(m, kBase + 0x3000, "PM_Leather_Player_Child_Alb", Marked(Shield(), 'M'));   // a pink puppet's
    const auto gear = *CoopLookTunic::Parse(LookFile({{kChild, Shield()}}));
    const CoopLookParts::Wearers wearers{{'M', 0x2020FFu}};
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        return CoopLookTunic::Pack(
            CoopLookParts::BootsTexels(gear, c == CoopLookParts::kBootsBase ? CoopLookParts::Wearers{} : wearers));
    };
    host.log = [](const std::string&) {};
    host.base = CoopLookParts::kBootsBase;
    host.what = "puppets' boots";
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLookParts::BootsPaintingName(wearers));
    w.Step(0);
    Look blue;
    blue[Part::boots] = 0x2020FFu;
    const auto want = Marked(CoopLookParts::Painted(gear, blue)[0].texels, 'M');
    CHECK(m.Drawn(kBase + 0x3000 + kOffset, want.size()) == want);
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, kTexels) == Shield());   // your own: not the puppets'
    w.Clear();
    CHECK(m.overrides() == 0);
}

TEST_CASE("look: a look chosen with the menu open is shown on your own Link at once, applied on unpause") {
    std::vector<Look> applied, previewed;
    CoopLook::Init(Look{}, [&](const Look& l) { applied.push_back(l); }, {},
                   [&](const Look& l) { previewed.push_back(l); });
    Look red;
    red[Part::hylian_frame] = 0xFF0000u;
    CoopLook::SetPending(red);
    CoopLook::Frame();   // the menu still open: shown, not applied
    REQUIRE(previewed.size() == 1);
    CHECK(previewed[0] == red);
    CHECK(applied.empty());
    CoopLook::Frame();   // nothing new: shown once
    CHECK(previewed.size() == 1);
    Look blue = red;
    blue[Part::master_blade] = 0x0000FFu;
    CoopLook::SetPending(red);
    CoopLook::SetPending(blue);   // two choices in one frame: the last is shown
    CoopLook::Frame();
    REQUIRE(previewed.size() == 2);
    CHECK(previewed[1] == blue);
    CoopLook::SetPending(red);
    CoopLook::ApplyOnUnpause();   // chosen and unpaused in one frame: applied, not shown first
    CoopLook::Frame();
    CHECK(previewed.size() == 2);
    REQUIRE(applied.size() == 1);
    CHECK(applied[0] == red);
    CHECK(CoopLook::Worn() == red);
    CoopLook::Init(Look{}, nullptr);
}

TEST_CASE("look: your own Link wears your slot colour when you chose none") {
    // None chosen ("by player number"): the slot colour the others see you in, once the number is known.
    CHECK(CoopLook::OwnTunic(std::nullopt, 0) == std::nullopt);   // before the welcome, or co-op off: the game's own
    CHECK(CoopLook::OwnTunic(std::nullopt, 1) == coop::protocol::slot_color(1));
    CHECK(CoopLook::OwnTunic(std::nullopt, 2) == coop::protocol::slot_color(2));   // a rejoin with another number
    CHECK(CoopLook::TunicName(CoopLook::OwnTunic(std::nullopt, 1)) == "blue");
    CHECK(CoopLook::TunicName(CoopLook::OwnTunic(std::nullopt, 2)) == "red");
    // A chosen colour wins over the number.
    CHECK(CoopLook::OwnTunic(0x123456u, 2) == 0x123456u);
    CHECK(CoopLook::OwnTunic(0x123456u, 0) == 0x123456u);
}

TEST_CASE("look tunic: a storage shorter than the pack's texels is not taken (a copy's mark is the image's last byte)") {
    // The pack holds each image at its own size, as the game's storage; a storage that is shorter would
    // leave the mark uncompared, so a puppet's copy could be taken for your own.
    FakeTextures m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}});
    PutFile(m, kBase + 0x1000, "Tile_A", Marked(TexelsOf(1), 3));   // copy 3, marked in its last byte
    m.Make(kBase + 0x1000 + kOffset, kTexels - 1);                   // seen as one byte shorter: the mark cut off
    CHECK(CoopLookTunic::Find(m, {green}).places.empty());
}

TEST_CASE("look tunic: the puppets' file of every copy's gear is read (more than 64 textures)") {
    Named many;
    for (int i = 0; i < 136; ++i) {   // 8 copies of 17 gear textures
        many.push_back({"C" + std::to_string(i) + "_Gear", TexelsOf(static_cast<std::uint8_t>(i))});
    }
    std::string why;
    const auto t = CoopLookTunic::Parse(LookFile(many), &why);
    REQUIRE(t);
    CHECK(t->size() == 136);
    const auto again = CoopLookTunic::Parse(CoopLookTunic::Pack(*t));
    REQUIRE(again);
    CHECK(again->size() == 136);
    CHECK((*again)[135].texels == TexelsOf(135));
}

// ini-reject: the module's own coop.ini (the launcher's lines, Customize's
// colour and parts, Join's last invite and host, the title's client id),
// written as the module writes it, then read back as the next start reads
// it: co-op stays on, nothing is refused.
TEST_CASE("look: coop.ini as Customize and Join write it keeps co-op on") {
    std::string coop = "server = 127.0.0.1\r\nport = 27510\r\nname = AlexA\r\n";
    Look look;
    look[Part::tunic] = CoopColors::Parse("black");
    look[Part::hylian_frame] = 0xE0C030u;
    look[Part::hylian_emblem] = CoopColors::Parse("red");
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    coop = CoopTitleMenu::IniSet(coop, "last_invite", "ABCDE-FGHJK", true);
    coop = CoopTitleMenu::IniSet(coop, "last_host", "26.1.2.3:27510", true);
    coop = CoopTitleMenu::IniSet(coop, "client_id", "0123456789abcdef0123456789abcdef", true);
    // An unset part is commented out, as Customize leaves it.
    look[Part::hylian_emblem].reset();
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));

    const auto ini = std::filesystem::temp_directory_path() / "coop_tests_module.ini";
    {
        std::ofstream out(ini, std::ios::binary | std::ios::trunc);
        out << coop;
    }
    std::vector<std::string> errors;
    const CoopHook::Settings s = CoopHook::LoadSettings(ini, errors);
    std::filesystem::remove(ini);
    for (const auto& e : errors) {
        MESSAGE(e);
    }
    const auto env_set = [](const char* name) {
#ifdef _MSC_VER
        char* v = nullptr;
        std::size_t n = 0;
        const bool set = _dupenv_s(&v, &n, name) == 0 && v != nullptr;
        std::free(v);
        return set;
#else
        return std::getenv(name) != nullptr;
#endif
    };
    if (!env_set("LICHII_COOP_SERVER") && !env_set("LICHII_COOP_NAME") && !env_set("LICHII_COOP_COLOR") &&
        !env_set("LICHII_COOP_PORT") && !env_set("LICHII_COOP_PASSWORD")) {
        CHECK(errors.empty());
        CHECK(s.enabled());
        CHECK(s.server == "127.0.0.1");
        CHECK(s.name == "AlexA");
        CHECK(s.color == CoopColors::Parse("black"));
    }
    const Look back = CoopLook::ReadParts(coop);
    CHECK(back[Part::hylian_frame] == 0xE0C030u);
    CHECK_FALSE(back[Part::hylian_emblem]);
}

TEST_CASE("look: a Nucleus game's coop.ini is known by the handler's first line (row 194)") {
    const std::string marker = "# Written by the Nucleus Co-op handler (tools/nucleus/OoTCoop.js) at each start.";
    CHECK(CoopLook::NucleusWrote(marker + "\r\nserver = 127.0.0.1\r\ncolor = red\r\n"));
    CHECK(CoopLook::NucleusWrote("\xef\xbb\xbf" + marker + "\n"));
    CHECK(CoopLook::NucleusWrote(marker));
    CHECK_FALSE(CoopLook::NucleusWrote(""));
    CHECK_FALSE(CoopLook::NucleusWrote("# Written by"));
    CHECK_FALSE(CoopLook::NucleusWrote("name = Saria\r\ncolor = red\r\n"));   // (a kit's or a player's own)
    CHECK_FALSE(CoopLook::NucleusWrote("name = Saria\r\n" + marker + "\r\n"));   // (only the first line counts)
}

TEST_CASE("look: your own Link starts in the colour of the pack's tunic files, not the switch's") {
    const std::map<std::string, std::vector<std::uint8_t>> self{{"black", {1, 2}}, {"pink", {3, 4}}, {"white", {5}}};
    const CoopLook::FileFn file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = self.find(c);
        return it == self.end() ? std::nullopt : std::optional(it->second);
    };
    // No tile in the pack: the game's own green, whatever the switch says.
    CHECK(CoopLook::PackStartTunic(std::nullopt, file, "") == "green");
    CHECK(CoopLook::PackStartTunic(std::nullopt, file, "pink") == "green");
    // The owner's lane1 run: black files left in the pack, pink chosen (sent), no switch.
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{1, 2}, file, "") == "black");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{1, 2}, file, "pink") == "black");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{3, 4}, file, "pink") == "pink");
    // A tile that is no colour's file (an older pack): the switch, else green.
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{9}, file, "white") == "white");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{9}, file, "") == "green");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{9}, nullptr, "") == "green");
}

TEST_CASE("look: green's own tunic files are known too (npc-colour)") {
    // Your own Link wears its own copies in green as well: the game's tunic
    // textures are on Mido's and Saria's models (every Kokiri).
    const std::map<std::string, std::vector<std::uint8_t>> self{{"red", {1}}, {"green", {7}}};
    const CoopLook::FileFn file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = self.find(c);
        return it == self.end() ? std::nullopt : std::optional(it->second);
    };
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{7}, file, "red") == "green");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{7}, file, "") == "green");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{1}, file, "green") == "red");
}

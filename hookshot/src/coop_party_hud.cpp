// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' hearts on the game's own HUD (coop_party_hud.h).
#include "coop_party_hud.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace CoopPartyHud {

namespace {

constexpr const char* kSay = "coop: party hud: ";
constexpr std::size_t kNameBytes = 64;                    // block+0: a name for one call
constexpr std::size_t kTextBytes = 200;                   // a line's text at most
constexpr std::size_t kLineBytes = sizeof(CoopNativeUi::Ref) + kTextBytes + 8;
constexpr std::size_t kBlockBytes = kNameBytes + kLineBytes * (kSlots + 1);

template <typename T>
bool ReadValue(Game& game, std::uint64_t at, T& out) {
    return game.Read(at, &out, sizeof out);
}

std::string PaneName(const char* what, int slot, int index = -1) {
    char b[40];
    if (index < 0) std::snprintf(b, sizeof b, "Pa_Coop%s_%d", what, slot);
    else std::snprintf(b, sizeof b, "Pa_Coop%s_%d_%02d", what, slot, index);
    return b;
}

// As the game's pane mover (f_8022a3e0, coop_native_tags.cpp MovePane): x, y at +0x6c, then the layout is told.
bool MovePane(Game& game, std::uint64_t pane, const std::array<float, 2>& at) {
    std::uint32_t flags = 0;
    if (pane == 0 || !ReadValue(game, pane + addr::kPaneFlags, flags)) return false;
    if (!game.Write(pane + addr::kPaneXY, at.data(), sizeof(float) * 2)) return false;
    if ((flags & (1u << 20)) == 0) return true;
    std::uint32_t word[2] = {};
    if (!game.Read(pane, word, sizeof word)) return false;
    const std::uint32_t mark = word[1] & 0x40u;
    if ((mark & ~word[0]) != 0) {
        const std::uint32_t set = word[0] | mark;
        game.Write(pane, &set, sizeof set);
        game.Call(game.At(addr::kPaneMarkA), {pane, mark & ~word[0], 0, 0, 0});
    }
    return game.Call(game.At(addr::kPaneMarkB), {pane, 0, 0, 0, 0}).has_value();
}

}  // namespace

Plan PlanHearts(const std::vector<int>& quarters) {
    Plan p;
    p.hearts = static_cast<int>(std::min<std::size_t>(quarters.size(), kHearts));
    for (int i = 0; i < p.hearts; ++i) {
        const int q = std::clamp(quarters[static_cast<std::size_t>(i)], 0, 4);
        p.whole[static_cast<std::size_t>(i)] = q == 4 ? 2 : 1;   // (an empty heart under the part heart)
        if (q > 0 && q < 4 && p.part_at < 0) {
            p.part_at = i;
            p.part_quarters = q;
        }
    }
    return p;
}

float GaugeFrame(int quarters, std::int32_t last_frame) {
    const int q = std::clamp(quarters, 1, 4);
    if (last_frame <= 3) return static_cast<float>(std::min<std::int32_t>(q - 1, std::max<std::int32_t>(last_frame, 0)));
    return static_cast<float>(last_frame) * static_cast<float>(q) / 4.0f;
}

std::array<float, 2> HeartAt(float top, int index) {
    const int i = std::clamp(index, 0, kHearts - 1);
    return {kFirstX + kStepX * static_cast<float>(i % kPerRow),
            top - kLabelHeight - kStepY * (static_cast<float>(i / kPerRow) + 0.5f)};
}

float BlockHeight(int hearts) {
    const int rows = std::clamp((hearts + kPerRow - 1) / kPerRow, 0, kHearts / kPerRow);
    return kLabelHeight + kStepY * static_cast<float>(rows) + kBlockGap;
}

float TopFrom(std::optional<double> game_hud_bottom) {
    if (!game_hud_bottom || !(*game_hud_bottom > 0.0) || !(*game_hud_bottom < 1.0)) return kDefaultTop;
    return -static_cast<float>(*game_hud_bottom) * kLayoutHeight - kBlockGap;
}

std::string MoreText(std::size_t players) {
    return players > static_cast<std::size_t>(kSlots) ? "+" + std::to_string(players - kSlots) : std::string();
}

std::uint64_t Hud::Name(Game& game, const std::string& text) {
    if (block_ == 0 || text.size() + 1 > kNameBytes || !game.Write(block_, text.c_str(), text.size() + 1)) return 0;
    return block_;
}

bool Hud::Find(Game& game, std::uint64_t layout, std::vector<std::string>* log) {
    layout_ = layout;
    native_ = false;
    hearts_ = {};
    gauges_ = {};
    labels_ = {};
    if (block_ == 0) {
        const auto block = game.Call(game.At(CoopNativeUi::addr::kMalloc), {kBlockBytes, 0, 0, 0, 0});
        if (!block || *block == 0) return false;
        block_ = *block;
    }
    const auto pane = [&](const std::string& name) -> std::uint64_t {
        const std::uint64_t n = Name(game, name);
        const auto p = n == 0 ? std::nullopt : game.Call(game.At(CoopNativeUi::addr::kPaneByName), {layout, n, 0, 0, 0});
        return p ? *p : 0;
    };
    const auto anim = [&](const std::string& path, std::int32_t* last = nullptr) -> std::uint64_t {
        const std::uint64_t n = Name(game, path);
        const auto a = n == 0 ? std::nullopt : game.Call(game.At(addr::kAnimFind), {layout, n, 0, 0, 0});
        if (!a || *a == 0) return 0;
        if (last != nullptr) {
            std::uint64_t player = 0;
            const auto l = ReadValue(game, *a + addr::kAnimPlayer, player) && player != 0
                               ? game.Call(game.At(addr::kAnimLastFrame), {player, 0, 0, 0, 0})
                               : std::nullopt;
            *last = l ? static_cast<std::int32_t>(static_cast<std::uint32_t>(*l)) : -1;
        }
        return *a;
    };
    if (pane(PaneName("Heart", 0, 0)) == 0) {
        if (log) log->push_back(std::string(kSay) + "the HUD has no hearts of ours (an older pack): the drawn blocks stay");
        return false;
    }
    bool all = true;
    for (int s = 0; s < kSlots; ++s) {
        for (int i = 0; i <= kHearts; ++i) {   // (the last: the slot's part heart)
            const bool part = i == kHearts;
            Pane& p = part ? gauges_[static_cast<std::size_t>(s)]
                           : hearts_[static_cast<std::size_t>(s)][static_cast<std::size_t>(i)];
            const std::string name = part ? PaneName("Gauge", s) : PaneName("Heart", s, i);
            p.pane = pane(name);
            p.base = anim(name + "/BaseState");
            p.heart = anim(name + "/HeartState");
            if (part) p.gauge = anim(name + "/HeartGauge", &p.gauge_last);
            all = all && p.pane != 0 && p.base != 0 && p.heart != 0 && (!part || p.gauge != 0);
        }
        labels_[static_cast<std::size_t>(s)].pane = pane(PaneName("Name", s));
        all = all && labels_[static_cast<std::size_t>(s)].pane != 0;
    }
    labels_[kSlots].pane = pane("Pa_CoopMore_0");
    if (log) {
        // What the screenshots are read against: every animation of the two heart layouts, and its last frame.
        for (const char* part : {"Pa_CoopHeart_0_00", "Pa_CoopGauge_0"}) {
            for (const char* a : {"BaseState", "HeartState", "GlowState", "Glow", "Pinch", "HeartGauge", "GlowGauge",
                                  "HeartNoticeLoop", "DefenceBorn", "IsDoubleDefense"}) {
                std::int32_t last = -1;
                const bool found = anim(std::string(part) + "/" + a, &last) != 0;
                log->push_back(std::string(kSay) + "anim " + part + "/" + a +
                               (found ? ": last frame " + std::to_string(last) : ": not found"));
            }
        }
        log->push_back(std::string(kSay) + (all ? "the game's HUD shows the other players' hearts (its own heart parts)"
                                                : "some of our hearts or their animations are missing: the drawn blocks stay"));
    }
    native_ = all;
    return all;
}

void Hud::Show(Game& game, Pane& p, bool shown) {
    if (p.shown == (shown ? 1 : 0)) return;
    if (game.Call(game.At(addr::kPaneShow), {p.pane, shown ? 1u : 0u, 0, 0, 0})) p.shown = shown ? 1 : 0;
}

void Hud::Show(Game& game, Label& l, bool shown) {
    if (l.pane == 0 || l.shown == (shown ? 1 : 0)) return;
    if (game.Call(game.At(addr::kPaneShow), {l.pane, shown ? 1u : 0u, 0, 0, 0})) l.shown = shown ? 1 : 0;
}

void Hud::Set(Game& game, Pane& p, int state, float frame, const std::array<float, 2>& at) {
    if (!p.placed || p.at != at) {
        if (MovePane(game, p.pane, at)) p.placed = true, p.at = at;
    }
    if (p.state != state) {   // as f_833dca10: BaseState, then HeartState
        const bool a = game.CallFloat(game.At(addr::kAnimStopAt), p.base, state == 0 ? 0.0f : 1.0f).has_value();
        const bool b = game.CallFloat(game.At(addr::kAnimStopAt), p.heart, state == 2 ? 1.0f : 0.0f).has_value();
        if (a && b) p.state = state;
    }
    if (p.gauge != 0 && p.frame != frame) {   // as f_833dcb60
        if (game.CallFloat(game.At(addr::kAnimStopAt), p.gauge, frame)) p.frame = frame;
    }
    Show(game, p, true);
}

void Hud::SetLabel(Game& game, std::size_t index, const std::string& text, const std::array<float, 2>& at) {
    Label& l = labels_[index];
    if (l.pane == 0) return;
    if (!l.placed || l.at != at) {
        if (MovePane(game, l.pane, at)) l.placed = true, l.at = at;
    }
    if (!l.has_text || l.text != text) {
        // The line's own Ref and text, kept in place while it shows; set on the part's two text panes by their path.
        const std::string clean = CoopNativeUi::Clean(text, kTextBytes);
        const std::uint64_t ref = block_ + kNameBytes + kLineBytes * index;
        CoopNativeUi::Ref r;
        r.text = ref + sizeof r;
        r.bytes = static_cast<std::uint32_t>(clean.size());
        if (game.Write(r.text, clean.c_str(), clean.size() + 1) && game.Write(ref, &r, sizeof r)) {
            const std::string part = index < static_cast<std::size_t>(kSlots)
                                         ? PaneName("Name", static_cast<int>(index)) : std::string("Pa_CoopMore_0");
            bool set = true;
            for (const char* pane : {"/T_Text_00", "/T_Text_01"}) {
                const std::uint64_t n = Name(game, part + pane);
                set = n != 0 && game.Call(game.At(CoopNativeUi::addr::kSetPaneText), {layout_, n, ref, 1, 0}) && set;
            }
            if (set) l.text = text, l.has_text = true;
        }
    }
    Show(game, l, true);
}

bool Hud::Step(Game& game, const std::vector<Block>& blocks, std::optional<double> game_hud_bottom,
               std::vector<std::string>* log) {
    const std::uint64_t screen = CoopNativeUi::ScreenByVtable(game, addr::kStatusVtable);
    std::uint64_t layout = 0;
    if (screen == 0 || !ReadValue(game, screen + CoopNativeUi::addr::kScreenLayout, layout) || layout == 0) {
        layout_ = 0;   // (the HUD is gone: its panes with it; found again when it is back)
        native_ = false;
        return false;
    }
    if (layout != layout_ && !Find(game, layout, log)) return false;
    if (!native_) return false;
    float top = TopFrom(game_hud_bottom);
    for (int s = 0; s < kSlots; ++s) {
        auto& hearts = hearts_[static_cast<std::size_t>(s)];
        Pane& gauge = gauges_[static_cast<std::size_t>(s)];
        Label& label = labels_[static_cast<std::size_t>(s)];
        if (static_cast<std::size_t>(s) >= blocks.size()) {   // nobody there (solo: every slot)
            for (Pane& p : hearts) Show(game, p, false);
            Show(game, gauge, false);
            Show(game, label, false);
            continue;
        }
        const Block& b = blocks[static_cast<std::size_t>(s)];
        const Plan plan = PlanHearts(b.hearts);
        SetLabel(game, static_cast<std::size_t>(s), b.text, {kLabelX, top - kLabelHeight / 2.0f});
        for (int i = 0; i < kHearts; ++i) {
            Pane& p = hearts[static_cast<std::size_t>(i)];
            if (i < plan.hearts) Set(game, p, plan.whole[static_cast<std::size_t>(i)], 0.0f, HeartAt(top, i));
            else Show(game, p, false);
        }
        if (plan.part_at >= 0) {
            Set(game, gauge, 2, GaugeFrame(plan.part_quarters, gauge.gauge_last), HeartAt(top, plan.part_at));
        } else {
            Show(game, gauge, false);
        }
        top -= BlockHeight(plan.hearts);
    }
    const std::string more = MoreText(blocks.size());
    if (more.empty()) Show(game, labels_[kSlots], false);
    else SetLabel(game, kSlots, more, {kLabelX, top - kLabelHeight / 2.0f});
    return true;
}

std::vector<Block> TestBlocks(std::uint64_t now_ms) {
    const int step = static_cast<int>(now_ms / kTestStepMs);
    const auto block = [&](const char* name, int max_quarters) {
        Block b;
        const int life = max_quarters - step % (max_quarters + 1);
        b.text = std::string(name) + " " + std::to_string(life) + "/" + std::to_string(max_quarters);
        for (int i = 0; i < (max_quarters + 3) / 4; ++i) b.hearts.push_back(std::clamp(life - 4 * i, 0, 4));
        return b;
    };
    return {block("Twenty", 80), block("Ten", 40), block("Three", 12), block("Four", 16)};
}

}  // namespace CoopPartyHud

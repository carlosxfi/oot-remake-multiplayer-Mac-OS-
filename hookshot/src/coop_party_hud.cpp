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
constexpr std::size_t kBlockBytes = kNameBytes + kLineBytes * (kSlots + 2);
constexpr std::size_t kMore = kSlots, kSelf = kSlots + 1;   // labels_ after the players' lines


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

std::string LabelPart(std::size_t index) {
    return index < static_cast<std::size_t>(kSlots) ? PaneName("Name", static_cast<int>(index))
           : index == kMore                          ? std::string("Pa_CoopMore_0")
                                                     : std::string("Pa_CoopSelf_0");
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

bool LowLife(int life, int max) {
    if (life <= 0 || max <= 0) return false;
    const int at = max <= 20 ? 4 : max <= 40 ? 6 : max <= 60 ? 8 : 11;
    return life <= at;
}

int GlowHeart(int was, int now, int hearts) {
    if (was < 0 || was == now || hearts <= 0) return -1;
    const int heart = now < was ? now / 4 : (now - 1) / 4;
    return std::clamp(heart, 0, std::min(hearts, kHearts) - 1);
}

float PlayedFrame(std::uint64_t ms, std::int32_t last, bool loop) {
    if (last <= 0) return 0.0f;
    const std::uint64_t f = ms * 30 / 1000;
    if (loop) return static_cast<float>(f % (static_cast<std::uint64_t>(last) + 1));
    return static_cast<float>(std::min<std::uint64_t>(f, static_cast<std::uint64_t>(last)));
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

float TopFrom(std::optional<double> game_hud_bottom, bool own) {
    const float own_line = own ? kLabelHeight + kBlockGap : 0.0f;
    if (!game_hud_bottom || !(*game_hud_bottom > 0.0) || !(*game_hud_bottom < 1.0)) return kDefaultTop;
    // (never so low that the fullest stack, our own line, three blocks of two rows and the "+N" line, leaves the picture)
    const float lowest = -kLayoutHeight + own_line + kSlots * BlockHeight(kHearts) + kLabelHeight + kBlockGap;
    return std::max(-static_cast<float>(*game_hud_bottom) * kLayoutHeight - kBlockGap, lowest);
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
    glows_ = {};
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
        if (log) {
            log->push_back(std::string(kSay) +
                           "the HUD has no hearts of ours (an older pack): the other players' hearts are hidden (nothing "
                           "drawn instead)");
        }
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
            // (glow and pinch: played when there; the hearts show without them)
            p.glow_state = anim(name + "/GlowState");
            p.glow = anim(name + "/Glow", &p.glow_last);
            p.pinch = anim(name + "/Pinch", &p.pinch_last);
            p.defence[0] = anim(name + "/IsDoubleDefense", &p.defence_last[0]);
            p.defence[1] = anim(name + "/DefenceBorn", &p.defence_last[1]);
            all = all && p.pane != 0 && p.base != 0 && p.heart != 0 && (!part || p.gauge != 0);
        }
        labels_[static_cast<std::size_t>(s)].pane = pane(PaneName("Name", s));
        all = all && labels_[static_cast<std::size_t>(s)].pane != 0;
    }
    labels_[kMore].pane = pane(LabelPart(kMore));
    labels_[kSelf].pane = pane(LabelPart(kSelf));   // (an older pack without it: no own line, the hearts still show)
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

void Hud::StopAt(Game& game, std::uint64_t anim, float& had, float frame) {
    if (anim == 0 || had == frame) return;
    if (game.CallFloat(game.At(addr::kAnimStopAt), anim, frame)) had = frame;
}

// The glow (f_833dc830: GlowState 3, Glow played; ended as f_833dc8f0, GlowState 0) and the pinch (Pinch played round
// while life is low), set frame by frame: no game object plays our copies.
// Double defence: IsDoubleDefense and DefenceBorn (the heart's white rim coming) held at their end with it, at 0
// without; which of them the game's own hearts use was not read (the run's log gives their last frames).
void Hud::Effects(Game& game, Pane& p, bool glowing, std::uint64_t glow_ms, bool pinch, std::uint64_t now_ms,
                  bool defence) {
    for (std::size_t i = 0; i < p.defence.size(); ++i) {
        StopAt(game, p.defence[i], p.defence_at[i],
               defence ? static_cast<float>(std::max<std::int32_t>(p.defence_last[i], 0)) : 0.0f);
    }
    if (p.glow != 0 && p.glow_state != 0) {
        StopAt(game, p.glow_state, p.glow_state_at, glowing ? kGlowState : 0.0f);
        StopAt(game, p.glow, p.glow_at, glowing ? PlayedFrame(glow_ms, p.glow_last, false) : 0.0f);
    }
    StopAt(game, p.pinch, p.pinch_at, pinch ? PlayedFrame(now_ms, p.pinch_last, true) : 0.0f);
}

void Hud::Alpha(Game& game, std::uint64_t pane, float& had, float alpha) {
    if (pane == 0 || had == alpha) return;
    if (game.CallFloat(game.At(addr::kPaneAlpha), pane, alpha)) had = alpha;
}

void Hud::Set(Game& game, Pane& p, int state, float frame, const std::array<float, 2>& at, float alpha) {
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
    Alpha(game, p.pane, p.alpha, alpha);
    Show(game, p, true);
}

void Hud::SetLabel(Game& game, std::size_t index, const std::string& text, const std::array<float, 2>& at,
                   float alpha) {
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
            const std::string part = LabelPart(index);
            bool set = true;
            for (const char* pane : {"/T_Text_00", "/T_Text_01"}) {
                const std::uint64_t n = Name(game, part + pane);
                set = n != 0 && game.Call(game.At(CoopNativeUi::addr::kSetPaneText), {layout_, n, ref, 1, 0}) && set;
            }
            if (set) l.text = text, l.has_text = true;
        }
    }
    Alpha(game, l.pane, l.alpha, alpha);
    Show(game, l, true);
}

bool Hud::Step(Game& game, const std::vector<Block>& blocks, std::optional<double> game_hud_bottom,
               std::vector<std::string>* log, std::uint64_t now_ms, const std::string& own) {
    const std::uint64_t screen = CoopNativeUi::ScreenByVtable(game, addr::kStatusVtable);
    std::uint64_t layout = 0;
    if (screen == 0 || !ReadValue(game, screen + CoopNativeUi::addr::kScreenLayout, layout) || layout == 0) {
        layout_ = 0;   // (the HUD is gone: its panes with it; found again when it is back)
        native_ = false;
        return false;
    }
    if (layout != layout_ && !Find(game, layout, log)) return false;
    if (!native_) return false;
    const bool own_shown = !own.empty() && !blocks.empty();
    float top = TopFrom(game_hud_bottom, own_shown);
    if (own_shown) {
        SetLabel(game, kSelf, own, {kLabelX, top - kLabelHeight / 2.0f}, 1.0f);
        top -= kLabelHeight + kBlockGap;
    } else {
        Show(game, labels_[kSelf], false);
    }
    for (int s = 0; s < kSlots; ++s) {
        auto& hearts = hearts_[static_cast<std::size_t>(s)];
        Pane& gauge = gauges_[static_cast<std::size_t>(s)];
        Label& label = labels_[static_cast<std::size_t>(s)];
        Glow& glow = glows_[static_cast<std::size_t>(s)];
        if (static_cast<std::size_t>(s) >= blocks.size()) {   // nobody there (solo: every slot)
            glow = {};
            for (Pane& p : hearts) Show(game, p, false);
            Show(game, gauge, false);
            Show(game, label, false);
            continue;
        }
        const Block& b = blocks[static_cast<std::size_t>(s)];
        const Plan plan = PlanHearts(b.hearts);
        const float alpha = b.dim ? kDimAlpha : 1.0f;
        int life = 0;
        for (int i = 0; i < plan.hearts; ++i) life += std::clamp(b.hearts[static_cast<std::size_t>(i)], 0, 4);
        const std::string who = b.text.substr(0, b.text.find("  "));   // (the name: the place may change)
        if (glow.who != who) glow = Glow{who};
        if (const int h = GlowHeart(glow.life, life, plan.hearts); h >= 0) glow.heart = h, glow.since = now_ms;
        glow.life = life;
        const std::uint64_t glow_ms = now_ms - glow.since;
        const std::int32_t glow_last = hearts[0].glow_last;
        const std::uint64_t glow_for = glow_last > 0 ? static_cast<std::uint64_t>(glow_last) * 1000 / 30 + 1 : kGlowMs;
        if (glow.heart >= 0 && glow_ms > glow_for) glow.heart = -1;
        const bool pinch = LowLife(life, plan.hearts * 4);
        SetLabel(game, static_cast<std::size_t>(s), b.text, {kLabelX, top - kLabelHeight / 2.0f}, alpha);
        for (int i = 0; i < kHearts; ++i) {
            Pane& p = hearts[static_cast<std::size_t>(i)];
            if (i < plan.hearts) {
                const int state = plan.whole[static_cast<std::size_t>(i)];
                Set(game, p, state, 0.0f, HeartAt(top, i), alpha);
                Effects(game, p, glow.heart == i && plan.part_at != i, glow_ms, pinch && state == 2, now_ms,
                        b.double_defence);
            } else {
                Show(game, p, false);
            }
        }
        if (plan.part_at >= 0) {
            Set(game, gauge, 2, GaugeFrame(plan.part_quarters, gauge.gauge_last), HeartAt(top, plan.part_at), alpha);
            Effects(game, gauge, glow.heart == plan.part_at, glow_ms, pinch, now_ms, b.double_defence);
        } else {
            Show(game, gauge, false);
        }
        top -= BlockHeight(plan.hearts);
    }
    const std::string more = MoreText(blocks.size());
    if (more.empty()) Show(game, labels_[kMore], false);
    else SetLabel(game, kMore, more, {kLabelX, top - kLabelHeight / 2.0f}, 1.0f);
    return true;
}

std::string TestOwn() { return "You (talking)"; }

std::vector<Block> TestBlocks(std::uint64_t now_ms) {
    const int step = static_cast<int>(now_ms / kTestStepMs);
    const auto block = [&](const char* name, int max_quarters, bool dim = false) {
        Block b;
        b.dim = dim;
        const int life = max_quarters - step % (max_quarters + 1);
        b.text = std::string(name) + " " + std::to_string(life) + "/" + std::to_string(max_quarters);
        for (int i = 0; i < (max_quarters + 3) / 4; ++i) b.hearts.push_back(std::clamp(life - 4 * i, 0, 4));
        return b;
    };
    // (the second: the longest line a player can have, 16 wide letters + voice words + the longest place name)
    const std::string longest = std::string(16, 'W') + " (talking)  Inside Ganondorf's Castle";
    return {block("Twenty", 80), block(longest.c_str(), 40), block("Three", 12, true), block("Four", 16)};
}

}  // namespace CoopPartyHud

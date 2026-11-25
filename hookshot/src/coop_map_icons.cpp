// SPDX-License-Identifier: GPL-3.0-or-later
// The other players on the game's own maps as copies of its map icon part (coop_map_icons.h).
#include "coop_map_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace CoopMapIcons {

namespace addr = CoopNativeTags::addr;

namespace {

std::mutex g_frame_mutex;
Frame g_frame;

constexpr float kHalfPi = 1.57079632679f;

// The copy's own panes (coop_map_icons.h): the icon types other than the player's arrow and the label are hidden.
constexpr const char* kIconHidden[] = {"N_Door_00",    "N_TreasureRoot_00", "N_Boss_00", "N_Facility_00",
                                       "N_Collect_00", "N_WaterLevel_00",   "N_Ruto_00"};
constexpr const char* kEntranceHidden[] = {"N_Mini_00", "N_Loacal_00", "T_Location_01", "T_Location_02"};
constexpr const char* kIconLayout = "PaMapIcon_00";
constexpr const char* kEntranceLayout = "PaMapIconEntrance_00";
constexpr const char* kPlayerLayout = "PaMapIconPlayer_00";

// A message reference as the game's text setter reads it (as coop_native_tags.cpp's).
struct Ref {
    std::uint64_t text = 0;
    std::uint32_t bytes = 0;
    std::uint32_t pad = 0;
    std::int64_t end = -1;
};
static_assert(sizeof(Ref) == 24);

template <typename T>
bool ReadValue(CoopNativeTags::Game& game, std::uint64_t address, T& out) {
    return game.Read(address, &out, sizeof out);
}

std::string CopyName(std::size_t i) {
    char text[32];
    std::snprintf(text, sizeof text, kCopyName, static_cast<int>(i));
    return text;
}

// The layout object a part's pane points at (in its first 0x200 bytes) whose resource is `part_layout`; 0: none.
std::uint64_t PartLayout(CoopNativeTags::Game& game, std::uint64_t part_pane, const char* part_layout) {
    for (const auto& h : CoopNativeTags::LayoutsHeld(game, part_pane, 0x200, part_layout)) {
        if (h.name == part_layout || h.name.rfind(std::string(part_layout) + ".", 0) == 0) return h.layout;
    }
    return 0;
}

}  // namespace

void Publish(Frame frame) {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    g_frame = std::move(frame);
}

Frame Taken() {
    std::lock_guard<std::mutex> lock(g_frame_mutex);
    return g_frame;
}

std::optional<std::array<float, 6>> Inverse(const float m[12]) {
    const float det = m[0] * m[4] - m[3] * m[1];
    for (const int i : {0, 1, 3, 4, 9, 10}) {
        if (!std::isfinite(m[i])) return std::nullopt;
    }
    if (!(std::fabs(det) > 1e-9f)) return std::nullopt;
    const float a = m[4] / det, c = -m[3] / det, b = -m[1] / det, d = m[0] / det;
    return std::array<float, 6>{a, b, c, d, -(a * m[9] + c * m[10]), -(b * m[9] + d * m[10])};
}

std::array<float, 2> ToLocal(const std::array<float, 6>& v, float x, float y) {
    const float gx = x - addr::kHalfWidth, gy = addr::kHalfHeight - y;
    return {v[0] * gx + v[2] * gy + v[4], v[1] * gx + v[3] * gy + v[5]};
}

std::vector<Slot> Plan(const Frame& frame, const std::array<float, 6>& inverse) {
    std::vector<Slot> out;
    std::vector<const Mark*> used;
    // (the clip's own turn, as the minimap turns with the camera: the inverse turns the other way)
    const float clip_turn = std::atan2(inverse[1], inverse[0]);
    for (const auto& m : frame.marks) {
        if (out.size() >= kCopies) break;
        if (m.clamped || !std::isfinite(m.x) || !std::isfinite(m.y)) continue;
        Slot s;
        s.copy = out.size();
        s.at = ToLocal(inverse, m.x, m.y);
        if (m.angle && std::isfinite(*m.angle)) s.turn = -*m.angle - kHalfPi + clip_turn;
        s.arrow = m.floor_delta == 0;
        s.name = m.name.empty() ? "P" + std::to_string(m.number) : m.name;
        s.rgb = m.rgb & 0xFFFFFFu;
        out.push_back(std::move(s));
        used.push_back(&m);
    }
    // Overlapping arrows: only the name of the one nearer to us (no place of ours: the lower number) shows.
    auto far = [&](const Mark& m) {
        if (!frame.self) return static_cast<float>(m.number);
        return std::hypot(m.x - (*frame.self)[0], m.y - (*frame.self)[1]);
    };
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (std::hypot(used[i]->x - used[j]->x, used[i]->y - used[j]->y) >= kOverlap) continue;
            const float fi = far(*used[i]), fj = far(*used[j]);
            const bool i_loses = fi > fj || (fi == fj && used[i]->number > used[j]->number);
            (i_loses ? out[i] : out[j]).name_shown = false;
        }
    }
    return out;
}

bool Icons::Block(CoopNativeTags::Game& game) {
    if (block_ != 0) return true;
    if (block_failed_) return false;
    names_.clear();
    names_.emplace_back(kClipPane);
    for (std::size_t i = 0; i < kCopies; ++i) names_.push_back(CopyName(i));
    for (const char* n : kIconHidden) names_.emplace_back(n);
    for (const char* n : kEntranceHidden) names_.emplace_back(n);
    for (const char* n : {"N_Player_00", "Pa_Player_00", "N_Entrance_00", "Pa_Entrance_00", "N_Location_00",
                          "T_Location_00", "N_Type_00", "N_Type_01", "N_PlayerRotate_00"}) {
        names_.emplace_back(n);
    }
    const auto got = game.Call(game.MainBase() + addr::kMalloc, {kBlockBytes, 0, 0, 0, 0});
    if (names_.size() > kNames || !got || *got == 0) {
        block_failed_ = true;
        return false;
    }
    std::vector<std::uint8_t> bytes(kBlockBytes, 0);
    for (std::size_t i = 0; i < names_.size(); ++i) {
        std::memcpy(bytes.data() + i * kNameBytes, names_[i].c_str(), std::min(names_[i].size(), kNameBytes - 1));
    }
    if (!game.Write(*got, bytes.data(), bytes.size())) {
        block_failed_ = true;
        return false;
    }
    block_ = *got;
    return true;
}

std::uint64_t Icons::Name(const char* name) {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return block_ + i * kNameBytes;
    }
    return 0;
}

std::uint64_t Icons::Find(CoopNativeTags::Game& game, std::uint64_t layout, const char* name) {
    const std::uint64_t at = Name(name);
    if (layout == 0 || at == 0) return 0;
    const auto pane = game.Call(game.MainBase() + addr::kPaneByName, {layout, at, 0, 0, 0});
    return pane ? *pane : 0;
}

void Icons::Show(CoopNativeTags::Game& game, std::uint64_t pane, bool shown) {
    if (pane != 0) game.Call(game.MainBase() + addr::kPaneShow, {pane, shown ? 1u : 0u, 0, 0, 0});
}

// The copy's inner layouts and panes, once per screen; its other icon types hidden, the arrow and the label shown.
bool Icons::Ready(CoopNativeTags::Game& game, std::size_t i) {
    Copy& c = copies_[i];
    if (c.ready) return true;
    if (c.pane == 0) c.pane = Find(game, layout_, CopyName(i).c_str());
    if (c.pane == 0) return why_ = "no " + CopyName(i), false;
    if (c.icon == 0) c.icon = PartLayout(game, c.pane, kIconLayout);
    if (c.icon == 0) return why_ = CopyName(i) + ": no layout " + kIconLayout, false;
    if (c.player == 0) c.player = PartLayout(game, Find(game, c.icon, "Pa_Player_00"), kPlayerLayout);
    if (c.entrance == 0) c.entrance = PartLayout(game, Find(game, c.icon, "Pa_Entrance_00"), kEntranceLayout);
    if (c.player == 0 || c.entrance == 0) return why_ = CopyName(i) + ": no arrow or label layout", false;
    c.turn = Find(game, c.player, "N_PlayerRotate_00");
    c.label = Find(game, c.entrance, "T_Location_00");
    if (c.label == 0) return why_ = CopyName(i) + ": no T_Location_00", false;
    for (const char* n : kIconHidden) Show(game, Find(game, c.icon, n), false);
    for (const char* n : kEntranceHidden) Show(game, Find(game, c.entrance, n), false);
    Show(game, Find(game, c.player, "N_Type_01"), false);   // N_Type_00: Link's look
    Show(game, Find(game, c.player, "N_Type_00"), true);
    Show(game, Find(game, c.entrance, "N_Location_00"), true);
    Show(game, c.label, true);
    c.ready = true;
    return true;
}

bool Icons::Step(CoopNativeTags::Game& game, const Frame& frame) {
    const std::uint64_t screen = CoopNativeTags::ScreenByVtable(game, CoopNativeTags::kMapMenuVtable);
    if (screen != screen_) {   // a new screen (a load): its panes are new
        screen_ = screen;
        layout_ = clip_ = 0;
        copies_ = {};
        shown_.clear();
    }
    if (screen == 0) return why_ = "no map screen", up_ = false;
    if (layout_ == 0 && !ReadValue(game, screen + addr::kScreenLayout, layout_)) layout_ = 0;
    if (layout_ == 0 || !Block(game)) return why_ = layout_ == 0 ? "no map layout" : "no memory", up_ = false;
    if (clip_ == 0) clip_ = Find(game, layout_, kClipPane);
    float m[12] = {};
    const auto inverse = clip_ != 0 && game.Read(clip_ + kPaneMatrix, m, sizeof m) ? Inverse(m) : std::nullopt;
    if (!inverse) return why_ = clip_ == 0 ? "no S_MapIcon_00" : "S_MapIcon_00 not placed", up_ = up_ && clip_ != 0;
    if (!Ready(game, 0)) return up_ = false;   // (copy 0 missing: no pack; the rest are made with it)
    up_ = true;
    std::vector<std::size_t> now;
    for (const Slot& s : Plan(frame, *inverse)) {
        if (!Ready(game, s.copy)) continue;
        Copy& c = copies_[s.copy];
        const std::string text = s.name_shown ? CoopNativeTags::Clean(s.name, kSlotBytes - sizeof(Ref) - 1) : "";
        if (c.named != text) {
            const std::uint64_t slot = block_ + kNames * kNameBytes + s.copy * kSlotBytes;
            Ref ref;
            ref.text = slot + sizeof(Ref);
            ref.bytes = static_cast<std::uint32_t>(text.size());
            if (game.Write(ref.text, text.c_str(), text.size() + 1) && game.Write(slot, &ref, sizeof ref) &&
                game.Call(game.MainBase() + addr::kSetPaneText, {c.entrance, Name("T_Location_00"), slot, 1, 0})) {
                c.named = text;
            }
        }
        if (!c.rgb_set || c.rgb != s.rgb) {   // (as Balloons::Colour: both colours of the text box)
            std::uint64_t inner = 0, box = 0;
            if (ReadValue(game, c.label + 8, inner) && inner != 0 && ReadValue(game, inner + 8, box) && box != 0) {
                const float rgba[4] = {((s.rgb >> 16) & 0xFF) / 255.0f, ((s.rgb >> 8) & 0xFF) / 255.0f,
                                       (s.rgb & 0xFF) / 255.0f, 1.0f};
                const std::uint64_t at = block_ + kNames * kNameBytes + kCopies * kSlotBytes + s.copy * 16;
                if (game.Write(at, rgba, sizeof rgba)) {
                    game.Call(game.MainBase() + addr::kTextColour0, {box, at, 0, 0, 0});
                    game.Call(game.MainBase() + addr::kTextColour1, {box, at, 0, 0, 0});
                    c.rgb = s.rgb, c.rgb_set = true;
                }
            }
        }
        Show(game, Find(game, c.icon, "N_Player_00"), s.arrow);
        Show(game, Find(game, c.icon, "N_Entrance_00"), true);
        if (turn_at_ != 0 && c.turn != 0 && s.turn) {
            const float turn = *s.turn;
            if (game.Write(c.turn + turn_at_, &turn, sizeof turn)) {
                game.Call(game.MainBase() + addr::kPaneMarkB, {c.turn, 0, 0, 0, 0});
            }
        }
        if (CoopNativeTags::MovePane(game, c.pane, s.at)) {
            Show(game, c.pane, true);
            now.push_back(s.copy);
        }
    }
    for (const std::size_t old : shown_) {
        if (std::find(now.begin(), now.end(), old) == now.end()) Show(game, copies_[old].pane, false);
    }
    shown_ = std::move(now);
    why_.clear();
    return true;
}

std::uint64_t Icons::ArrowTurnPane(CoopNativeTags::Game& game, std::uint64_t icon_layout) {
    if (!Block(game)) return 0;
    const std::uint64_t player = PartLayout(game, Find(game, icon_layout, "Pa_Player_00"), kPlayerLayout);
    return Find(game, player, "N_PlayerRotate_00");
}

void Icons::HideAll(CoopNativeTags::Game& game) {
    for (const std::size_t i : shown_) Show(game, copies_[i].pane, false);
    shown_.clear();
}

}  // namespace CoopMapIcons

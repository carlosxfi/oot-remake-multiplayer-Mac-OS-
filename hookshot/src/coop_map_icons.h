// SPDX-License-Identifier: GPL-3.0-or-later
// The other players on the game's own maps (native-ui-audit item 8, issue #255): the game's map icon part, not drawn
// shapes or text. The pack (tools/puppet_pack/map_icons.py) puts kCopies hidden copies of MapMenu_00's Pa_MapIcon_00
// (layout PaMapIcon_00) under its clip S_MapIcon_00, beside the game's own icons; this module shows one per other
// player, as the game shows Link:
// - the copy's N_Player_00 (PaMapIconPlayer_00: Link's arrow, its N_Type_00 look), turned the way the player faces
//   (N_PlayerRotate_00), the other icon types hidden;
// - the player's name in the copy's N_Entrance_00 > Pa_Entrance_00 (PaMapIconEntrance_00): the game's own place-name
//   label of the map (T_Location_00, the game's font), its entrance pictures hidden, the text in the player's colour
//   as the balloons' names (coop_native_tags.h); "(1 below)" after it for another floor of a dungeon;
// - moved in the clip's own space (the inverse of S_MapIcon_00's global matrix), so the game's clip cuts it at the
//   map's edge, as it cuts its own icons; one past the edge is hidden (the game's clip, not an edge marker).
// When two arrows overlap only the name of the one nearer to us shows. Nobody else on the map (solo): every copy stays
// hidden and nothing of the game's is touched.
//
// The marks come from the present's thread (CoopDraw::MapMarks of the frame's MapShown, Publish), the panes are moved
// on the game's thread (Icons::Step at the frame hook, Taken). A part copy's own panes are in its own layout: found as
// the layout object the copy's pane points at whose resource has the part layout's name (CoopNativeTags::LayoutsHeld),
// then by name with the game's lookup.
//
// Split for tests (tests/hookshot/map_icons_test.cpp): Inverse, ToLocal and Plan are pure.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_native_tags.h"

namespace CoopMapIcons {

inline constexpr char kCopyName[] = "Pa_CoopMapIcon_%02d";   // (tools/puppet_pack/map_icons.py COPY_NAME)
inline constexpr std::size_t kCopies = 15;                    // (MAP_ICONS)
inline constexpr char kClipPane[] = "S_MapIcon_00";
inline constexpr std::uint64_t kPaneMatrix = 0x28;   // pane+: the global matrix, 3 x 4 by column (CoopMapPage)
// pane+: the z turn (radians, as the layout files keep it). 0: not known yet, the arrows are not turned (the
// rotation-offset probe, LICHII_COOP_MAP_PROBE=1, finds it; LICHII_COOP_MAP_TURN_AT=<hex> tries one).
inline constexpr std::uint64_t kPaneTurn = 0;

// One other player on the map shown, in the layout's units (3840 x 2160, x right, y down from the top left).
struct Mark {
    std::uint8_t number = 0;
    float x = 0.0f, y = 0.0f;
    std::optional<float> angle;   // facing on the screen: radians, 0 right, pi/2 down (map::Icon's)
    bool clamped = false;         // past the map's edge
    int floor_delta = 0;          // their floor minus the one shown
    std::uint32_t rgb = 0xFFFFFF;
    std::string name;             // with the floor words; empty: "P<n>"
};
// Our own place on the map shown, the same units (for the overlap rule); none: not known.
struct Frame {
    std::vector<Mark> marks;
    std::optional<std::array<float, 2>> self;
};
void Publish(Frame frame);   // the present's thread
Frame Taken();               // the game's thread: the newest frame

// The 2D part of a 3 x 4 by column matrix (x' = m0 x + m3 y + m9, y' = m1 x + m4 y + m10) inverted:
// {a, b, c, d, tx, ty} with x = a x' + c y' + tx, y = b x' + d y' + ty. None for one that cannot be.
std::optional<std::array<float, 6>> Inverse(const float m[12]);
// A layout point (y down from the top left) in the frame the inverse leaves (y up, as the pane's translation).
std::array<float, 2> ToLocal(const std::array<float, 6>& inverse, float x, float y);

inline constexpr float kOverlap = 64.0f;   // layout units: two arrows closer than this overlap (the arrow's width)
struct Slot {
    std::size_t copy = 0;
    std::array<float, 2> at{};      // in the clip's frame
    std::optional<float> turn;      // the arrow's z turn, radians counter-clockwise from up; none: not turned
    bool arrow = true;              // false: another floor (the game shows Link only on his own)
    std::string name;
    bool name_shown = true;
    std::uint32_t rgb = 0xFFFFFF;
};
// One copy per mark, at most kCopies, in the marks' order; clamped ones and those with no finite point are left out.
std::vector<Slot> Plan(const Frame& frame, const std::array<float, 6>& inverse);

class Icons {
public:
    // False when the map screen, its clip or the copies are not there (no pack, the title, a load): the module then
    // draws nothing on the maps either (CoopHookshot: the overlay's map names only while Up() was never true).
    bool Step(CoopNativeTags::Game& game, const Frame& frame);
    void HideAll(CoopNativeTags::Game& game);
    void TurnAt(std::uint64_t pane_offset) { turn_at_ = pane_offset; }   // 0: not turned
    const std::string& why() const { return why_; }
    // The rotation-offset probe: an icon layout's (a PaMapIcon_00 the map screen holds) arrow N_PlayerRotate_00; 0
    // when not found.
    std::uint64_t ArrowTurnPane(CoopNativeTags::Game& game, std::uint64_t icon_layout);                      // what Step last missed (the log)
    bool Up() const { return up_; }
    std::size_t shown() const { return shown_.size(); }
    std::uint64_t copy(std::size_t i) const { return i < kCopies ? copies_[i].pane : 0; }

    // The guest block: the pane names (0-ended, kNameBytes each), then per copy a 256-byte text slot {ref, text}, then
    // per copy 16 bytes of colour.
    static constexpr std::size_t kNameBytes = 32;
    static constexpr std::size_t kNames = 40;
    static constexpr std::size_t kSlotBytes = 256;
    static constexpr std::size_t kBlockBytes = kNames * kNameBytes + kCopies * (kSlotBytes + 16);

private:
    struct Copy {
        std::uint64_t pane = 0;
        std::uint64_t icon = 0, entrance = 0, player = 0;   // the part layouts inside it
        std::uint64_t turn = 0, label = 0;                  // N_PlayerRotate_00, T_Location_00
        bool ready = false;
        std::string named;
        std::uint32_t rgb = 0;
        bool rgb_set = false;
    };
    bool Block(CoopNativeTags::Game& game);
    std::uint64_t Name(const char* name);   // the guest copy of a pane name, 0 when it is not one of ours
    std::uint64_t Find(CoopNativeTags::Game& game, std::uint64_t layout, const char* name);
    bool Ready(CoopNativeTags::Game& game, std::size_t i);
    void Show(CoopNativeTags::Game& game, std::uint64_t pane, bool shown);

    std::uint64_t screen_ = 0, layout_ = 0, clip_ = 0;
    std::uint64_t block_ = 0;
    bool block_failed_ = false, up_ = false;
    std::uint64_t turn_at_ = kPaneTurn;
    std::string why_;
    std::vector<std::string> names_;
    std::array<Copy, kCopies> copies_{};
    std::vector<std::size_t> shown_;
};

}  // namespace CoopMapIcons

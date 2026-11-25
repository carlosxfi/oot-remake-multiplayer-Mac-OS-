// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' hearts on the game's own HUD, from the game's own heart parts.
//
// The game's HUD (PlayerStatus_00, UIPlayerStatusScreen) has one heart gauge object. It makes 20 whole hearts (part
// layout PaHeartGaugeChildNoGauge_00) and one part heart (PaHeartGaugeChildGauge_00), and sets each with its
// stop-at-frame call (main+0x2d2d90) on the heart's animations, read on 1.1.1's main:
//   whole heart, f_833dca10(heart, state): 0 = BaseState 0, HeartState 0 (no heart there); 1 = BaseState 1, HeartState
//     0 (an empty heart); 2 = BaseState 1, HeartState 1 (a full heart);
//   part heart, f_833dcb60(heart, n) = HeartGauge stopped at n; the part heart is moved onto the heart partly filled.
// That object reads Link's own life and sets its hearts again itself, so they cannot show another player (the HUD
// probe's second run). The pack (tools/puppet_pack/party_hud.py) adds our own copies of the same two part layouts to
// PlayerStatus_00, hidden: kSlots players x (kHearts whole hearts + one part heart + one line of the game's text), and
// one more line. This file places, shows and sets them with the same calls. No game object owns the copies, so
// nothing sets them back. A pack without the copies: Step answers false and the drawn blocks stay (coop_draw).
//
// Split for tests (tests/hookshot/party_hud_test.cpp): everything goes through Game.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_native_ui.h"

namespace CoopPartyHud {

inline constexpr int kSlots = 3;       // party_hud.py SLOTS
inline constexpr int kHearts = 20;     // party_hud.py HEARTS
inline constexpr int kPerRow = 10;

// main+ offsets of build e0f3ccfaf50065ae (game 1.1.1).
namespace addr {
inline constexpr std::uint64_t kStatusVtable = 0x84c0b710 - 0x80020000;   // UIPlayerStatusScreen
inline constexpr std::uint64_t kAnimFind = 0x43d870;       // (layout, "part/animation") -> animation, 0 none
inline constexpr std::uint64_t kAnimPlayer = 0x20;         // animation+: its player
inline constexpr std::uint64_t kAnimLastFrame = 0x2d1340;  // (player) -> s32 its last frame
inline constexpr std::uint64_t kAnimStopAt = 0x2d2d90;     // (animation, s0 frame): stopped at that frame
inline constexpr std::uint64_t kPaneShow = 0x8022aea0 - 0x80020000;   // (pane, 0/1)
inline constexpr std::uint64_t kPaneAlpha = 0x2d8ac0;      // (pane, s0 alpha 0..1): its own alpha (coop_pause_tab.h)
inline constexpr std::uint64_t kPaneMarkA = 0x815457d0 - 0x80020000;  // what the game's pane mover calls after a move
inline constexpr std::uint64_t kPaneMarkB = 0x8154d540 - 0x80020000;
inline constexpr std::uint64_t kPaneXY = 0x6c;             // pane+: f32 x, y
inline constexpr std::uint64_t kPaneFlags = 0xa8;          // pane+: u32; bit 8 placed elsewhere, bit 20 tell the layout
}  // namespace addr

// Where the copies go, in the frame of their parent (N_ScaleL_00: the picture's top left corner, y up; the layout is
// 3840 x 2160). The game's own first heart is at (206, -154) there and its hearts are 78 apart, its rows 61
// (PaHeartGauge_00: Pa_NoGauge_00, Pa_Gauge_00 and Pa_Born_00 sit one heart and one row apart). Ours are kScale of that.
inline constexpr float kScale = 0.75f;                     // party_hud.py HEART_SCALE
inline constexpr float kFirstX = 206.0f - 78.0f * (1.0f - kScale) / 2.0f;
inline constexpr float kStepX = 78.0f * kScale;
inline constexpr float kStepY = 61.0f * kScale;
inline constexpr float kLabelHeight = 76.0f * kScale;      // party_hud.py LABEL_SIZE x LABEL_SCALE
inline constexpr float kLabelX = kFirstX - 30.0f + 500.0f * kScale;   // the line's middle: its left end at the hearts'
inline constexpr float kBlockGap = 14.0f;
inline constexpr float kDefaultTop = -330.0f;              // under two rows of hearts and the magic bar
inline constexpr float kLayoutHeight = 2160.0f;
inline constexpr float kDimAlpha = 150.0f / 255.0f;        // a player not connected: as the drawn blocks (coop_draw)

// The module's game: the chat's, and a call with a float in s0.
class Game : public CoopNativeUi::Game {
public:
    virtual std::optional<std::uint64_t> CallFloat(std::uint64_t function, std::uint64_t x0, float s0) = 0;
};

// One other player.
struct Block {
    std::string text;            // the line over the hearts (name, place, voice)
    std::vector<int> hearts;     // quarters filled in each (CoopHudHearts::Quarters)
    bool dim = false;            // not connected (CoopHudDim): hearts and line at kDimAlpha
    bool double_defence = false; // the Great Fairy's double defence (PlayerStatus.IsDefenseUP; see kDefenceUpHash)
};

// PlayerStatus.IsDefenseUP (docs/items.md): shared by every player (game_items.cpp, R::any), so our own value is
// everyone's. Not in race mode (coop_race_mode.h: no flag is shared, and no message carries another player's): then
// nobody's hearts show it but our own game's.
inline constexpr std::uint32_t kDefenceUpHash = 0x56bbe07cu;

// A player's hearts as the game sets its own: each whole heart's state (0 none, 1 empty, 2 full), and the part heart.
struct Plan {
    std::array<std::uint8_t, kHearts> whole{};
    int part_at = -1;        // the heart the part heart sits on, -1 none
    int part_quarters = 0;   // 1..3
    int hearts = 0;          // shown (at most kHearts)
    friend bool operator==(const Plan&, const Plan&) = default;
};
Plan PlanHearts(const std::vector<int>& quarters);

// HeartGauge's frame for 1..4 quarters on an animation with this last frame. The game's damage path gives 0..3 (a
// fill up to 1/4 -> 0, up to 1/2 -> 1, up to 3/4 -> 2, more -> 3); the HUD probe's first run read a last frame of 100
// on the part heart's own layout, quarters at 25, 50, 75, 100. Both are kept: a short animation takes the game's
// 0..3, a long one the fraction of its length.
float GaugeFrame(int quarters, std::int32_t last_frame);

// Low life, as the game's own pinching hearts start (the original game's rule, Health_IsCritical: at most 1 heart left
// with up to 5, 1 1/2 with up to 10, 2 with up to 15, 2 3/4 above; not read from this game yet). Nothing left (0) is
// not low: the player is down.
bool LowLife(int life_quarters, int max_quarters);
// The heart a change of life glows on (damage: the heart that lost, heal: the heart that filled), -1 none.
int GlowHeart(int was_quarters, int now_quarters, int hearts);
// The frame of a played animation `ms` after it began, at the layouts' 30 frames a second: held at `last` (once), or
// round again (`loop`).
float PlayedFrame(std::uint64_t ms, std::int32_t last, bool loop);
inline constexpr std::uint64_t kGlowMs = 600;     // a glow's time when its animation has no last frame we could read
inline constexpr float kGlowState = 3.0f;         // GlowState while glowing (f_833dc830)

// Where a heart of a block goes, and a block's height, in the parent's frame; `top` is the block's top edge.
std::array<float, 2> HeartAt(float top, int index);
float BlockHeight(int hearts);
// The first block's top from the bottom of the game's own HUD (a fraction of the picture height), kDefaultTop none.
float TopFrom(std::optional<double> game_hud_bottom);

// "+N" for the players past the slots, in the player's language by the caller; "" none.
std::string MoreText(std::size_t players);

class Hud {
public:
    // Each frame on the game's thread: the blocks shown on the game's HUD. False when the HUD has no copies of ours
    // (an older pack) or is not there now: the other players' hearts are hidden then (nothing is drawn instead).
    // `log` gets a line for each thing learnt once (found or not, last frames).
    // `now_ms` (any steady clock) plays the glow on a change of life and the pinch at low life.
    bool Step(Game& game, const std::vector<Block>& blocks, std::optional<double> game_hud_bottom,
              std::vector<std::string>* log = nullptr, std::uint64_t now_ms = 0);
    bool native() const { return native_; }

private:
    struct Pane {
        std::uint64_t pane = 0;
        std::uint64_t base = 0, heart = 0, gauge = 0;   // its animations
        std::int32_t gauge_last = 0;
        std::uint64_t glow_state = 0, glow = 0, pinch = 0;   // the glow and pinch animations (0: not there)
        std::int32_t glow_last = -1, pinch_last = -1;
        float glow_state_at = -1.0f, glow_at = -1.0f, pinch_at = -1.0f;
        // Double defence's look (each one there: at its last frame with it, 0 without)
        std::array<std::uint64_t, 2> defence{};
        std::array<std::int32_t, 2> defence_last{};
        std::array<float, 2> defence_at{-1.0f, -1.0f};
        int shown = -1;                                 // what was last set: -1 unknown
        float alpha = -1.0f;
        int state = -1;
        float frame = -1.0f;
        std::array<float, 2> at{};
        bool placed = false;
    };
    struct Label {
        std::uint64_t pane = 0;
        int shown = -1;
        float alpha = -1.0f;
        bool placed = false;
        std::array<float, 2> at{};
        std::string text;
        bool has_text = false;
    };
    bool Find(Game& game, std::uint64_t layout, std::vector<std::string>* log);
    void Show(Game& game, Pane& p, bool shown);
    void Show(Game& game, Label& l, bool shown);
    void Set(Game& game, Pane& p, int state, float frame, const std::array<float, 2>& at, float alpha);
    void Effects(Game& game, Pane& p, bool glowing, std::uint64_t glow_ms, bool pinch, std::uint64_t now_ms,
                 bool defence);
    static void StopAt(Game& game, std::uint64_t anim, float& had, float frame);
    void SetLabel(Game& game, std::size_t index, const std::string& text, const std::array<float, 2>& at,
                  float alpha);
    static void Alpha(Game& game, std::uint64_t pane, float& had, float alpha);
    std::uint64_t Name(Game& game, const std::string& text);

    std::uint64_t layout_ = 0;     // the layout the panes below were found on (the HUD made again: found again)
    bool native_ = false;
    std::uint64_t block_ = 0;      // guest memory of ours: a name's scratch, then the lines' texts
    std::array<std::array<Pane, kHearts>, kSlots> hearts_{};
    std::array<Pane, kSlots> gauges_{};
    struct Glow {
        std::string who;          // whose life it was (another player in the slot: no glow for the move)
        int life = -1;            // their last life in quarters, -1 none
        int heart = -1;           // glowing on this heart since `since`
        std::uint64_t since = 0;
    };
    std::array<Glow, kSlots> glows_{};
    std::array<Label, kSlots + 1> labels_{};   // the last: "+N"
};

// Test builds, COOP_TEST_PARTY_HUD=1 (scenario hs-party-hud): the blocks of four made-up players, their life going
// down a quarter of a heart every kTestStepMs and round again, so one game alone shows every fill on screenshots;
// the third ("Three") not connected (dimmed); the second has the longest line a player can have (it must fit).
inline constexpr std::uint64_t kTestStepMs = 1500;
std::vector<Block> TestBlocks(std::uint64_t now_ms);

}  // namespace CoopPartyHud

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emotes on our own screen, and a call's voice (coop_emote.h), through the game's own calls:
// - Our own Link plays the gesture we sent: its AS command on his slot 0 with the play call the puppets use
//   (main+0x2022a0), only while he stands idle (slot 0 on one of the game's waiting commands) and no event or downed
//   state drives him. At the hold's end the command he was on is played again, unless the game already plays another
//   (he moved, was hit, an event): then nothing is touched. No input is taken from the player.
// - A call (/here) is spoken by the caller's Navi: the game's own call sound (Def::navi) started on her own sound
//   user (0x43db00 finds it, 0x440630 starts it, as coop_link_sound.h), so the game places it at her. Our own call:
//   our own Navi; another player's: their fairy (coop_fairy.h). No Link sound user has a call.
// Nothing asked for (alone, co-op idle): no call is made and no memory taken. Guest access and game calls come in
// through CoopPuppet::Guest / Call, so the tests run it without a game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_emote.h"

namespace CoopPuppet {
class Guest;
struct Call;
}

namespace CoopEmote {

// Whether `command` on slot 0 is Link standing idle (the game's waiting commands: "Wait", "Wait_...").
bool Idle(std::string_view command);

class Body {
public:
    struct Our {
        std::uint64_t actor = 0;  // our Link (0: none)
        bool busy = false;        // an event, downed or spectating: nothing is played on him
    };
    // Each frame, the hook's side.
    void SetOur(const Our& our);
    // Any thread (the chat box): our own gesture, from the calc thread's next frame.
    void Play(const Def& def);
    // Any thread: `number`'s Navi (0: our own) says `def`'s call, if it has one.
    void Say(std::uint8_t number, const Def& def, std::uint64_t now_ms);
    // The actor whose sound user a call goes to: `number`'s fairy, 0 our own Navi; 0 none. The calc thread, as Next.
    using NaviActor = std::function<std::uint64_t(CoopPuppet::Guest& guest, std::uint8_t number)>;
    void SetNaviActor(NaviActor find);

    // The next game call (CoopCallers::Caller).
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t now_ms, std::uint64_t main_base);
    // Log lines since the last call.
    std::vector<std::string> TakeNotes();

    static constexpr std::uint64_t kStartWithinMs = 500;  // a gesture not startable for this long is dropped
    static constexpr std::uint64_t kVoiceWaitMs = 1500;   // a call waits this long for her to be there, then is dropped
    static constexpr std::size_t kMaxVoices = 8;          // waiting calls; the oldest goes

private:
    enum class Pending : std::uint8_t { none, malloc, gesture, back, search, emit };
    struct Voice {
        std::uint8_t number = 0;
        const Def* def = nullptr;
        std::uint64_t at_ms = 0;
    };

    void Note(std::string text);
    std::optional<CoopPuppet::Call> StepGesture(CoopPuppet::Guest& guest, std::uint64_t now_ms,
                                                std::uint64_t main_base);
    std::optional<CoopPuppet::Call> PlayAs(CoopPuppet::Guest& guest, std::string_view command,
                                           std::uint64_t main_base);
    std::optional<CoopPuppet::Call> StepVoice(CoopPuppet::Guest& guest, std::uint64_t now_ms,
                                              std::uint64_t main_base);

    std::mutex mutex_;
    Our our_;
    NaviActor navi_actor_;
    const Def* asked_ = nullptr;        // not started yet
    std::uint64_t asked_ms_ = 0;        // first seen by Next
    bool asked_seen_ = false;
    const Def* playing_ = nullptr;
    std::uint64_t playing_actor_ = 0;
    std::uint64_t until_ms_ = 0;
    std::string base_;                  // what he was on before
    bool gesture_tried_ = false;        // this frame
    std::deque<Voice> voices_;
    Voice saying_;                      // the voice of the search that just ran
    std::uint64_t voice_user_ = 0;      // its sound user
    bool voice_tried_ = false;          // this frame
    std::size_t handle_ = 0;            // the next handle cell
    Pending pending_ = Pending::none;
    std::uint64_t block_ = 0;
    std::vector<std::string> notes_;
};

Body& TheBody();

} // namespace CoopEmote

// SPDX-License-Identifier: GPL-3.0-or-later
// See coop_emote_body.h.

#include "coop_emote_body.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "coop_dbno_body.h"
#include "coop_link_sound.h"
#include "coop_puppet.h"
#include "coop_puppet_sound.h"

namespace CoopEmote {
namespace {

// Our block in guest memory: the AS play request (as the puppets'), its command's name, then the sound's asset, name
// and handles (one cell per start in turn: a handle the game still holds is not written over at once).
constexpr std::size_t kPlaySize = 0x30;
constexpr std::size_t kTextSize = 0x40;
constexpr std::size_t kTextAt = kPlaySize;
constexpr std::size_t kAssetAt = kTextAt + kTextSize;
constexpr std::size_t kSoundTextAt = kAssetAt + 8;
constexpr std::size_t kHandlesAt = kSoundTextAt + kTextSize;
constexpr std::size_t kHandles = 8;
constexpr std::size_t kBlockSize = kHandlesAt + kHandles * 8;
constexpr float kRate = 1.0f;

std::uint64_t Pointer(CoopPuppet::Guest& guest, std::uint64_t address) {
    std::uint64_t value = 0;
    return address != 0 && guest.Read(address, &value, sizeof value) ? value : 0;
}

bool WriteText(CoopPuppet::Guest& guest, std::uint64_t at, std::string_view text) {
    std::array<char, kTextSize> cell{};
    if (text.size() >= cell.size()) {
        return false;
    }
    std::memcpy(cell.data(), text.data(), text.size());
    return guest.Write(at, cell.data(), cell.size());
}

std::string Whose(std::uint8_t number) {
    return number == 0 ? std::string("our") : "P" + std::to_string(number) + "'s";
}

} // namespace

bool Idle(std::string_view command) {
    return command == "Wait" || command.starts_with("Wait_");
}

void Body::SetOur(const Our& our) {
    std::lock_guard lock(mutex_);
    our_ = our;
}

void Body::Play(const Def& def) {
    std::lock_guard lock(mutex_);
    asked_ = &def;
    asked_seen_ = false;
}

void Body::Say(std::uint8_t number, const Def& def, std::uint64_t now_ms) {
    if (def.navi[0] == '\0') {
        return;
    }
    std::lock_guard lock(mutex_);
    if (voices_.size() >= kMaxVoices) {
        voices_.pop_front();
    }
    voices_.push_back({number, &def, now_ms});
}

void Body::SetNaviActor(NaviActor find) {
    std::lock_guard lock(mutex_);
    navi_actor_ = std::move(find);
}

void Body::Note(std::string text) {
    if (notes_.size() < 32) {
        notes_.push_back(std::move(text));
    }
}

std::vector<std::string> Body::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

std::optional<CoopPuppet::Call> Body::PlayAs(CoopPuppet::Guest& guest, std::string_view command,
                                             std::uint64_t main_base) {
    // (the lock is held; the block exists) As the puppets' (coop_puppet.cpp): +0 actor, +8 the name, +0x10 slot 0,
    // +0x14 rate, +0x18 / +0x1c -1 (unset), +0x20 skip-if-same 0.
    const std::uint64_t text = block_ + kTextAt;
    std::array<std::uint8_t, kPlaySize> request{};
    const float unset = -1.0f;
    std::memcpy(request.data() + 0x00, &our_.actor, 8);
    std::memcpy(request.data() + 0x08, &text, 8);
    std::memcpy(request.data() + 0x14, &kRate, 4);
    std::memcpy(request.data() + 0x18, &unset, 4);
    std::memcpy(request.data() + 0x1c, &unset, 4);
    if (!WriteText(guest, text, command) || !guest.Write(block_, request.data(), request.size())) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    c.function = main_base + CoopPuppet::addr::kPlayAs;
    c.x[0] = block_;
    return c;
}

std::optional<CoopPuppet::Call> Body::StepGesture(CoopPuppet::Guest& guest, std::uint64_t now_ms,
                                                  std::uint64_t main_base) {
    // (the lock is held; the block exists)
    if (asked_ != nullptr && !asked_seen_) {
        asked_seen_ = true;
        asked_ms_ = now_ms;
    }
    if (playing_ != nullptr && our_.actor != playing_actor_) {
        playing_ = nullptr;  // no Link, or a new one (a scene, an age change): not touched
    }
    if (asked_ == nullptr && playing_ == nullptr) {
        return std::nullopt;
    }
    const auto slot = our_.actor != 0 ? CoopDbno::Body::ReadSlot0(guest, our_.actor) : std::nullopt;
    if (playing_ != nullptr && (!slot || slot->name != playing_->command)) {
        Note("our Link's '" + std::string(playing_->command) + "' ended: the game plays '" +
             (slot ? slot->name : std::string("?")) + "'");
        playing_ = nullptr;
    }
    if (asked_ != nullptr) {
        const bool on_ours = playing_ != nullptr;
        if (!our_.busy && slot && (on_ours || Idle(slot->name))) {
            if (!on_ours) {
                base_ = slot->name;
            }
            if (auto call = PlayAs(guest, asked_->command, main_base)) {
                playing_ = std::exchange(asked_, nullptr);
                playing_actor_ = our_.actor;
                until_ms_ = now_ms + std::min(playing_->hold_ms, kMaxHoldMs);
                pending_ = Pending::gesture;
                Note("our Link plays '" + std::string(playing_->command) + "' (was '" + base_ + "')");
                return call;
            }
        }
        if (now_ms - std::min(now_ms, asked_ms_) > kStartWithinMs) {
            Note("our Link plays no '" + std::string(asked_->command) + "': " +
                 (our_.actor == 0 ? std::string("no Link")
                  : our_.busy     ? std::string("an event, or downed")
                  : slot          ? "he is on '" + slot->name + "'"
                                  : std::string("no animation read")));
            asked_ = nullptr;
        }
        return std::nullopt;
    }
    if (playing_ != nullptr && now_ms >= until_ms_) {
        playing_ = nullptr;
        if (!our_.busy) {  // (an event that took him plays its own)
            if (auto call = PlayAs(guest, base_, main_base)) {
                pending_ = Pending::back;
                return call;
            }
        }
    }
    return std::nullopt;
}

std::optional<CoopPuppet::Call> Body::StepVoice(CoopPuppet::Guest& guest, std::uint64_t now_ms,
                                                std::uint64_t main_base) {
    // (the lock is held; the block exists) One search a frame, oldest first; a Navi not here yet does not hold up the
    // others.
    for (auto it = voices_.begin(); it != voices_.end();) {
        const std::uint64_t actor = navi_actor_ ? navi_actor_(guest, it->number) : 0;
        const std::uint64_t user = actor != 0 ? CoopPuppetSound::SoundUser(guest, actor) : 0;
        const std::uint64_t resource = Pointer(guest, user != 0 ? user + CoopLinkSound::addr::kUserResource : 0);
        const std::uint64_t accessor =
            Pointer(guest, resource != 0 ? resource + CoopLinkSound::addr::kResourceAccessor : 0);
        if (accessor == 0 || !CoopPuppetSound::LooksLikeUser(guest, user)) {
            if (now_ms - std::min(now_ms, it->at_ms) < kVoiceWaitMs) {
                ++it;
                continue;
            }
            Note("call: " + Whose(it->number) + " Navi " +
                 (actor == 0 ? "is not here" : "has no sound user") + ": not heard");
            it = voices_.erase(it);
            continue;
        }
        saying_ = *it;
        it = voices_.erase(it);
        const std::uint64_t zero = 0;
        if (!WriteText(guest, block_ + kSoundTextAt, saying_.def->navi) ||
            !guest.Write(block_ + kAssetAt, &zero, sizeof zero)) {
            continue;
        }
        voice_user_ = user;
        CoopPuppet::Call c;
        c.function = main_base + CoopLinkSound::addr::kSearchAsset;
        c.x[0] = accessor;
        c.x[1] = block_ + kAssetAt;
        c.x[2] = block_ + kSoundTextAt;
        pending_ = Pending::search;
        return c;
    }
    return std::nullopt;
}

std::optional<CoopPuppet::Call> Body::Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                           std::uint64_t now_ms, std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    const Pending was = std::exchange(pending_, Pending::none);
    if (!after_call) {
        gesture_tried_ = false;
        voice_tried_ = false;
    } else if (was == Pending::malloc) {
        const std::array<std::uint8_t, kBlockSize> clean{};
        block_ = result != 0 && guest.Write(result, clean.data(), clean.size()) ? result : 0;
        if (block_ == 0) {
            return std::nullopt;  // (no memory: next frame)
        }
    } else if (was == Pending::search) {
        const std::uint64_t asset = (result & 1) != 0 ? Pointer(guest, block_ + kAssetAt) : 0;
        const std::string what = "'" + std::string(saying_.def->navi) + "'";
        const std::string user = "'" + CoopPuppetSound::UserName(guest, voice_user_) + "'";
        if (asset != 0) {
            CoopPuppet::Call c;
            c.function = main_base + CoopLinkSound::addr::kEmit;
            c.x[0] = voice_user_;
            c.x[1] = block_ + kAssetAt;
            c.x[2] = block_ + kHandlesAt + 8 * handle_;
            handle_ = (handle_ + 1) % kHandles;
            pending_ = Pending::emit;
            Note("call: " + Whose(saying_.number) + " Navi says " + what + " (sound user " + user + ")");
            return c;
        }
        Note("call: " + what + " is not in " + Whose(saying_.number) + " Navi's sound user " + user);
    }
    if (main_base == 0 || (asked_ == nullptr && playing_ == nullptr && voices_.empty())) {
        return std::nullopt;  // nothing asked for: no call, no memory
    }
    if (block_ == 0) {
        if (after_call) {
            return std::nullopt;
        }
        CoopPuppet::Call c;
        c.function = main_base + CoopPuppet::addr::kMalloc;
        c.x[0] = kBlockSize;
        pending_ = Pending::malloc;
        return c;
    }
    if (!std::exchange(gesture_tried_, true)) {
        if (auto c = StepGesture(guest, now_ms, main_base)) {
            return c;
        }
    }
    if (!std::exchange(voice_tried_, true)) {
        if (auto c = StepVoice(guest, now_ms, main_base)) {
            return c;
        }
    }
    return std::nullopt;
}

Body& TheBody() {
    static Body body;
    return body;
}

} // namespace CoopEmote

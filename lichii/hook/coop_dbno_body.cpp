// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_dbno_body.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace CoopDbno::Body {

// --- Raising ---

void Raising::Start(std::int32_t to, std::uint64_t now_us) noexcept {
    to_.store(to, std::memory_order_release);
    since_us_ = now_us == 0 ? 1 : now_us;
}

std::optional<std::int32_t> Raising::Frame(std::optional<std::int32_t> life, std::uint64_t now_us) noexcept {
    const std::int32_t to = to_.load(std::memory_order_acquire);
    if (to < 0 || since_us_ == 0) {
        return std::nullopt;
    }
    if (life && *life >= to) {
        Drop();  // the step took it (or something raised it further)
        return std::nullopt;
    }
    if (now_us < since_us_ + kHealWaitUs) {
        return std::nullopt;
    }
    Drop();
    return to;
}

void Raising::Drop() noexcept {
    to_.store(-1, std::memory_order_release);
    since_us_ = 0;
}

// --- Crawl ---

void Crawl::Want(const Wanted& w) {
    std::lock_guard lock(mutex_);
    want_ = w;
    bsa_.store(w.bsa, std::memory_order_release);
}

std::optional<std::string> UnseenWhy(bool downed_or_spectating, std::uint8_t status) {
    namespace st = coop::protocol::status;
    if (downed_or_spectating) {
        return std::string("downed");
    }
    if ((status & st::dead) != 0) {
        return std::string("dead");
    }
    if ((status & st::loading) != 0) {
        return std::string("loading");
    }
    if ((status & st::in_cutscene) != 0) {
        return std::string("in their event");
    }
    if ((status & st::in_menu) != 0) {
        return std::string("in a menu");
    }
    if ((status & st::paused) != 0) {
        return std::string("paused");
    }
    static_assert(st::away == (st::paused | st::loading | st::in_cutscene | st::in_menu | st::dead),
                  "UnseenWhy names every away bit");
    return std::nullopt;
}

void Crawl::SetPuppetActor(PuppetActor find) {
    std::lock_guard lock(mutex_);
    puppet_actor_ = std::move(find);
}

bool Crawl::Skip(std::uint64_t bsa, const CoopBossNative::Request& request, bool own) {
    if (bsa == 0 || own || bsa != bsa_.load(std::memory_order_acquire)) {
        return false;  // (every other actor's request: no lock)
    }
    std::lock_guard lock(mutex_);
    if (bsa != want_.bsa) {
        return false;
    }
    const auto a = CoopBossNative::ActionFromRequest(request);
    if (!a) {
        return false;
    }
    std::uint64_t node = 0;
    std::memcpy(&node, request.data() + CoopBossNative::addr::kReqNode, sizeof node);
    const bool ground = a->verb == verb::kWalk || a->verb == verb::kWait;
    if (ground && node != 0) {
        node_ = node;  // the scheduler's own node, on its main slot
        slot_ = a->slot;
        flags_ = a->flags;
        learnt_ = true;
    }
    if (!want_.lying || want_.event || a->verb == verb::kCrawl) {
        return false;
    }
    if (!crawling_) {
        // Not in our crawl (refused: a ladder, water, mid-air; or the world
        // took our Link out of it): the game's own runs. Back on the ground,
        // the crawl is asked again at once.
        if (ground) {
            asks_ = 0;
            next_ask_ms_ = 0;
        }
        return false;
    }
    if (Passes(a->verb)) {
        crawling_ = false;  // the world moves our Link (a fall, water, the void): asked again after it
        ++counts_.passed;
        Note("our downed Link: the game's action verb " + std::to_string(a->verb) + " let through; crawl again after");
        return false;
    }
    ++counts_.skipped;
    if (counts_.skipped_verbs.size() < 16 &&
        std::find(counts_.skipped_verbs.begin(), counts_.skipped_verbs.end(), a->verb) ==
            counts_.skipped_verbs.end()) {
        counts_.skipped_verbs.push_back(a->verb);
        Note("our downed Link: the game's action verb " + std::to_string(a->verb) + " (slot " +
             std::to_string(a->slot) + ") skipped");
    }
    return true;
}

std::optional<CoopPuppet::Call> Crawl::Ask(CoopPuppet::Guest& guest, std::int32_t v, std::uint64_t main_base) {
    // (the lock is held; the block exists)
    coop::protocol::BossAction action;
    action.slot = slot_;
    action.verb = v;
    action.flags = flags_;
    action.changed = true;
    const CoopBossNative::Request r = CoopBossNative::RequestFromAction(action, node_);
    if (!guest.Write(block_, r.data(), r.size())) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    c.function = main_base + CoopBossNative::addr::kRequestActionVerb;
    c.x[0] = want_.bsa;
    c.x[1] = block_;
    c.x[2] = 1;
    pending_ = v == verb::kCrawl ? Pending::crawl : Pending::stand;
    ++counts_.asked;
    return c;
}

std::optional<CoopPuppet::Call> Crawl::Step(CoopPuppet::Guest& guest, Hold& h, Pending kind, std::uint64_t now_at,
                                            std::uint64_t main_base, bool want) {
    // (the lock is held; the block exists) One hold's step this frame: on
    // while lying, on the component now; off when up, only on the component
    // it was put on and only while that is still our Link's (a new scene's
    // Link has a new one: the old key went with the old component).
    h.tried = true;
    const bool want_on = want && now_at != 0;
    if (want_on && h.on_at == now_at) {
        return std::nullopt;
    }
    if (!want_on && h.on_at == 0) {
        return std::nullopt;
    }
    if (!want_on && h.on_at != now_at) {
        h.on_at = 0;  // (that component is gone)
        return std::nullopt;
    }
    const std::uint32_t key = addr::kOurKey;
    if (!guest.Write(block_ + addr::kBlockKey, &key, sizeof key)) {
        return std::nullopt;
    }
    CoopPuppet::Call c;
    if (kind == Pending::shield) {
        c.function = main_base + addr::kInvincibleNoReaction;
        c.x[0] = now_at;
        c.x[1] = want_on ? 1 : 0;  // Bool: add our key / take it out
        c.x[2] = block_ + addr::kBlockKey;
    } else {
        c.function = main_base + addr::kEmitterSwitch;
        c.x[0] = now_at + addr::kEmitterKeys;
        c.x[1] = block_ + addr::kBlockKey;
        c.x[2] = want_on ? 0 : 1;  // IsEnable
    }
    pending_ = kind;
    pending_on_ = want_on;
    pending_at_ = now_at;
    return c;
}

std::optional<CoopPuppet::Call> Crawl::StepPuppets(CoopPuppet::Guest& guest, std::uint64_t main_base) {
    // (the lock is held; the block exists) Every teammate downed now, and
    // every puppet still holding our key (they got up, or left: their puppet
    // is gone and so is its component).
    for (const auto& u : want_.unseen_puppets) {
        puppets_.try_emplace(u.number);
    }
    for (auto it = puppets_.begin(); it != puppets_.end();) {
        const std::uint8_t n = it->first;
        Hold& h = it->second;
        const auto wanted = std::find_if(want_.unseen_puppets.begin(), want_.unseen_puppets.end(),
                                         [n](const Wanted::Unseen& u) { return u.number == n; });
        const bool want = wanted != want_.unseen_puppets.end();
        if (!want && h.on_at == 0 && !(pending_ == Pending::puppet && pending_number_ == n)) {
            it = puppets_.erase(it);
            continue;
        }
        ++it;
        if (h.tried) {
            continue;
        }
        // Their puppet's PerceptionEmitter now: [[actor + 0x270] + 0x328] (0:
        // no puppet, or a pack without perception).
        std::uint64_t component = 0;
        if (const std::uint64_t actor = puppet_actor_ ? puppet_actor_(n) : 0; actor != 0) {
            std::uint64_t table = 0;
            if (guest.Read(actor + CoopBossNative::addr::kActorComponents, &table, sizeof table) && table != 0 &&
                !guest.Read(table + addr::kComponentEmitter, &component, sizeof component)) {
                component = 0;
            }
        }
        if (auto c = Step(guest, h, Pending::puppet, component, main_base, want)) {
            pending_number_ = n;
            pending_why_ = want ? wanted->why : std::string();
            return c;
        }
    }
    return std::nullopt;
}

std::optional<CoopPuppet::Call> Crawl::Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t now_ms, std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (!after_call) {
        shield_.tried = false;
        unseen_.tried = false;
        for (auto& [n, h] : puppets_) {
            h.tried = false;
        }
    } else {
        const Pending was = std::exchange(pending_, Pending::none);
        switch (was) {
        case Pending::malloc:
            block_ = result;
            break;
        case Pending::shield:
        case Pending::unseen: {
            Hold& h = was == Pending::shield ? shield_ : unseen_;
            h.on_at = pending_on_ ? pending_at_ : 0;
            if (pending_on_) {
                ++(was == Pending::shield ? counts_.shield_on : counts_.unseen_on);
            }
            Note(std::string(was == Pending::shield ? "the game's invincibility (no reaction)" : "the game's perception emitter") +
                 (pending_on_ ? (was == Pending::shield ? " on for our downed Link" : " off for our downed Link")
                              : (was == Pending::shield ? " off: our Link is up" : " on again: our Link is up")));
            break;
        }
        case Pending::puppet: {
            Hold& h = puppets_[pending_number_];
            h.on_at = pending_on_ ? pending_at_ : 0;
            if (pending_on_) {
                ++counts_.puppets_unseen_on;
            }
            Note("the game's perception emitter " + std::string(pending_on_ ? "off" : "on again") + " for player " +
                 std::to_string(pending_number_) + "'s puppet: " + (pending_on_ ? "they are " + pending_why_ : "they are back"));
            break;
        }
        case Pending::crawl:
        case Pending::stand: {
            const bool taken = result == CoopBossNative::addr::kActionTaken;
            if (!taken) {
                ++counts_.refused;
            }
            if (was == Pending::crawl) {
                crawling_ = taken;
                if (taken) {
                    asks_ = 0;  // (a later fall or water may need a fresh round)
                }
                Note(taken ? "our downed Link: Crawl (234) asked and taken"
                           : "our downed Link: Crawl (234) refused (" + std::to_string(result) + ")");
            } else {
                Note(taken ? "our Link up: Wait (40) asked and taken" : "our Link up: Wait (40) refused");
            }
            return std::nullopt;  // (one action ask a frame)
        }
        case Pending::none:
            break;
        }
    }

    bool puppets_held = !want_.unseen_puppets.empty();
    for (const auto& [n, h] : puppets_) {
        puppets_held = puppets_held || h.on_at != 0;
    }
    const bool need_block = want_.lying || shield_.on_at != 0 || unseen_.on_at != 0 || stand_due_ ||
                            want_.lying != was_lying_ || puppets_held;
    if (block_ == 0) {
        if (!need_block || pending_ != Pending::none || (after_call && result == 0)) {
            return std::nullopt;  // (nothing to do, or the game had no memory: next frame)
        }
        CoopPuppet::Call c;
        c.function = main_base + CoopBossNative::addr::kMalloc;
        c.x[0] = addr::kBlockSize;
        pending_ = Pending::malloc;
        return c;
    }

    // Shield and unseen (any time, events too).
    if (!shield_.tried) {
        if (auto c = Step(guest, shield_, Pending::shield, want_.life, main_base, want_.lying)) {
            return c;
        }
    }
    if (!unseen_.tried) {
        if (auto c = Step(guest, unseen_, Pending::unseen, want_.emitter, main_base, want_.lying)) {
            return c;
        }
    }
    if (auto c = StepPuppets(guest, main_base)) {
        return c;
    }

    // The crawl.
    if (want_.lying != was_lying_) {
        was_lying_ = want_.lying;
        asks_ = 0;
        next_ask_ms_ = 0;
        stand_due_ = !want_.lying && crawling_;
        crawling_ = false;
    }
    if (want_.event) {
        crawling_ = false;  // the event moves our Link; asked again after it
        asks_ = 0;
        next_ask_ms_ = 0;
        return std::nullopt;
    }
    if (want_.bsa == 0 || !learnt_) {
        return std::nullopt;
    }
    if (stand_due_) {
        stand_due_ = false;
        return Ask(guest, verb::kWait, main_base);
    }
    if (want_.lying && !crawling_ && asks_ < kMaxAsks && now_ms >= next_ask_ms_) {
        ++asks_;
        next_ask_ms_ = now_ms + kAskEveryMs;
        return Ask(guest, verb::kCrawl, main_base);
    }
    return std::nullopt;
}

Crawl::Counts Crawl::Totals() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Crawl::TakeNotes() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

void Crawl::Note(std::string line) {
    if (notes_.size() < 32) {
        notes_.push_back(std::move(line));
    }
}

Crawl& TheCrawl() {
    static Crawl crawl;
    return crawl;
}

Raising& TheRaising() {
    static Raising raising;
    return raising;
}

}  // namespace CoopDbno::Body

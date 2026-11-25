// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's tabs in Hookshot's menu (hos_coop_menu.h): the page each tab shows, and the presses on it, handed between
// the game's main thread and the thread that records the present (row 279).
//
// Hookshot asks hos_coop_menu_page / _input / _picture from its menu tick and its page draw, on the present's thread,
// while it holds its menu lock (menu_mu) and a virtual core. Anything there that waits on a co-op lock (a tab's own,
// the Multiplayer tab's, the network's) can wait for good: the thread holding that lock may itself be waiting for
// menu_mu (Longshot's settings rows, menu_setting / menu_setting_set), for the core, or for the present. The player's
// game then stops with the menu open (row 279: hosting, then Insert / F1).
//
// So the present's thread does no co-op work at all: it reads the page last published here and queues its presses.
// The game's main thread (CoopPause::Tick, each frame) takes the presses, runs them on the tab with every lock it
// needs, and publishes the page that results. The only lock here is held around a pointer copy or a vector swap, and
// nothing is called while it is held, so it never waits on anything else.
//
// A press answers at once from the page last published: Back is taken when that page had somewhere to go back to
// (else Hookshot closes the menu), every other known event is taken. A second Back in the same game frame is
// answered from the same page (taken), so a fast double Back can leave the menu open one press longer; it never
// closes the menu while the page still had somewhere to go.
#ifndef COOP_MENU_HANDOFF_H
#define COOP_MENU_HANDOFF_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "coop_look_menu.h"
#include "coop_pause_menu.h"
#include "hos_coop_menu.h"

namespace CoopMenuHandoff {

// One press on a tab, as hos_coop_menu_input had it.
struct Event {
    int event = 0;
    int arg = 0;
    int arg2 = 0;
};

// A tab's page as the main thread built it, its words already in the player's language.
struct Page {
    CoopPauseMenu::View view;
    bool back_taken = false;                    // Back on it goes somewhere (a page, typing to end)
    bool empty_words_none = false;              // change/confirm/back "" go to Hookshot as NULL (no prompt)
    std::vector<CoopLookMenu::Shape> picture;   // the Customize page's picture (the Multiplayer tab only)
};

// The events hos_coop_menu.h knows; any other is not queued and not taken.
inline bool Known(int event) {
    return event >= HOS_COOP_MENU_UP && event <= HOS_COOP_MENU_CLICK;
}

class Handoff {
public:
    static constexpr std::size_t kMaxQueued = 64;   // (one game frame's presses; more only if the game is stopped)

    // ---- the present's thread ----
    // The page last published (null: none yet).
    std::shared_ptr<const Page> Read() const {
        std::lock_guard lock(mutex_);
        return page_;
    }
    // Queues the press for the next Tick; the answer hos_coop_menu_input gives (1: taken).
    bool Press(int event, int arg, int arg2) {
        if (!Known(event)) {
            return false;
        }
        bool back_taken = false;
        {
            std::lock_guard lock(mutex_);
            if (queued_.size() < kMaxQueued) {
                queued_.push_back(Event{event, arg, arg2});
            }
            back_taken = page_ != nullptr && page_->back_taken;
        }
        return event == HOS_COOP_MENU_BACK ? back_taken : true;
    }

    // ---- the game's main thread ----
    // The presses queued since the last call, oldest first.
    std::vector<Event> Take() {
        std::vector<Event> out;
        std::lock_guard lock(mutex_);
        out.swap(queued_);
        return out;
    }
    void Publish(Page page) {
        std::shared_ptr<const Page> next = std::make_shared<const Page>(std::move(page));
        {
            std::lock_guard lock(mutex_);
            page_.swap(next);
        }
        // (the page before is freed here, outside the lock)
    }
    // Forgets the page (the tab is gone: the next one shown starts empty).
    void Clear() {
        std::shared_ptr<const Page> gone;
        std::lock_guard lock(mutex_);
        page_.swap(gone);
    }

private:
    mutable std::mutex mutex_;   // around a pointer copy or a vector swap only
    std::shared_ptr<const Page> page_;
    std::vector<Event> queued_;
};

// The page for Hookshot, held until this thread's next Fill with the same Held (hos_coop_menu.h: Hookshot copies the
// strings at once).
struct Held {
    std::shared_ptr<const Page> page;
    std::vector<HosCoopMenuRow> rows;
};

// Fills Hookshot's page from the last published one; false before the first (or a page too small).
inline bool Fill(const Handoff& from, Held& held, HosCoopMenuPage* out) {
    if (out == nullptr || out->size < sizeof(HosCoopMenuPage)) {
        return false;
    }
    std::shared_ptr<const Page> page = from.Read();
    if (page == nullptr) {
        return false;
    }
    held.page = std::move(page);
    const CoopPauseMenu::View& v = held.page->view;
    held.rows.resize(v.rows.size());
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        const CoopPauseMenu::Row& r = v.rows[i];
        HosCoopMenuRow& h = held.rows[i];
        h = HosCoopMenuRow{};
        h.name = r.name.c_str();
        h.value = r.value.c_str();
        h.kind = static_cast<std::uint8_t>(r.kind);
        h.on = r.on ? 1 : 0;
        h.left = r.left ? 1 : 0;
        h.right = r.right ? 1 : 0;
        h.fixed = r.fixed ? 1 : 0;
    }
    const bool none = held.page->empty_words_none;
    const auto words = [none](const std::string& s) -> const char* {
        return none && s.empty() ? nullptr : s.c_str();
    };
    out->rows_n = static_cast<std::int32_t>(held.rows.size());
    out->rows = held.rows.data();
    out->selected = v.selected;
    out->typing = v.typing ? 1 : 0;
    out->help_title = v.help_title.c_str();
    out->help = v.help.c_str();
    out->hint = v.hint.c_str();
    out->gold = v.gold.c_str();
    out->change = words(v.change);
    out->confirm = words(v.confirm);
    out->back = words(v.back);
    return true;
}

}  // namespace CoopMenuHandoff

#endif

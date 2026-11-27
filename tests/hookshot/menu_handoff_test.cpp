// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's tabs in Hookshot's menu: the page and the presses handed between the game's main thread and the present's
// thread (hookshot/src/coop_menu_handoff.h, row 279: hosting, then Insert / F1, stopped the game for good).
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "coop_menu_handoff.h"

using CoopMenuHandoff::Event;
using CoopMenuHandoff::Fill;
using CoopMenuHandoff::Handoff;
using CoopMenuHandoff::Held;
using CoopMenuHandoff::Page;

namespace {

Page MakePage(const std::string& first_row, bool back_taken, bool empty_words_none = false) {
    Page p;
    CoopPauseMenu::Row r;
    r.name = first_row;
    r.value = "On";
    r.kind = CoopPauseMenu::RowKind::kChoice;
    r.left = true;
    p.view.rows.push_back(r);
    CoopPauseMenu::Row h;
    h.name = "Heading";
    h.kind = CoopPauseMenu::RowKind::kHeading;
    p.view.rows.push_back(h);
    p.view.selected = 0;
    p.view.help_title = "Multiplayer";
    p.view.help = "A paragraph.";
    p.view.hint = "ABCD-1234";
    p.view.confirm = "Open";
    p.view.back = "";
    p.back_taken = back_taken;
    p.empty_words_none = empty_words_none;
    return p;
}

HosCoopMenuPage Out() {
    HosCoopMenuPage out{};
    out.size = sizeof out;
    return out;
}

}  // namespace

TEST_CASE("menu handoff: the present's side never waits on a tab's lock (row 279's cycle cannot close)") {
    // The game's main thread holds the tab's lock (as CoopPause::Tick holds the Multiplayer tab's) and waits for the
    // present's thread to finish a page and a press (as it can wait for menu_mu, a core or the present). Before the
    // handoff the present's thread took that same lock: both waited for good.
    Handoff handoff;
    handoff.Publish(MakePage("Host a game", false));
    std::mutex tab_lock;
    std::unique_lock main_holds(tab_lock);
    auto present = std::async(std::launch::async, [&] {
        Held held;
        HosCoopMenuPage out = Out();
        const bool filled = Fill(handoff, held, &out);
        const bool taken = handoff.Press(HOS_COOP_MENU_DOWN, 0, 0);
        return filled && taken && std::string(out.rows[0].name) == "Host a game";
    });
    REQUIRE(present.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    CHECK(present.get());
    main_holds.unlock();
    // The press waits for the main thread's next Tick.
    const std::vector<Event> events = handoff.Take();
    REQUIRE(events.size() == 1);
    CHECK(events[0].event == HOS_COOP_MENU_DOWN);
}

TEST_CASE("menu handoff: presses queue in order and are taken once") {
    Handoff h;
    CHECK(h.Press(HOS_COOP_MENU_SHOW, 0, 0));
    CHECK(h.Press(HOS_COOP_MENU_SELECT, 3, 0));
    CHECK(h.Press(HOS_COOP_MENU_CLICK, 2, -1));
    std::vector<Event> e = h.Take();
    REQUIRE(e.size() == 3);
    CHECK(e[0].event == HOS_COOP_MENU_SHOW);
    CHECK((e[1].event == HOS_COOP_MENU_SELECT && e[1].arg == 3));
    CHECK((e[2].event == HOS_COOP_MENU_CLICK && e[2].arg == 2 && e[2].arg2 == -1));
    CHECK(h.Take().empty());
}

TEST_CASE("menu handoff: an event hos_coop_menu.h does not know is neither taken nor queued") {
    Handoff h;
    CHECK_FALSE(h.Press(42, 0, 0));
    CHECK_FALSE(h.Press(-1, 0, 0));
    CHECK(h.Take().empty());
}

TEST_CASE("menu handoff: Back is answered from the page last published, and it is queued either way") {
    Handoff h;
    CHECK_FALSE(h.Press(HOS_COOP_MENU_BACK, 0, 0));   // no page yet: nothing to go back to (Hookshot closes the menu)
    h.Publish(MakePage("Host a game", false));        // the first page
    CHECK_FALSE(h.Press(HOS_COOP_MENU_BACK, 0, 0));
    h.Publish(MakePage("Private game", true));        // a page under it
    CHECK(h.Press(HOS_COOP_MENU_BACK, 0, 0));
    CHECK(h.Press(HOS_COOP_MENU_UP, 0, 0));           // every other known event: taken
    CHECK(h.Press(HOS_COOP_MENU_HIDE, 0, 0));
    CHECK(h.Take().size() == 5);                       // (the main thread's Press sees every Back)
}

TEST_CASE("menu handoff: the queue holds one stopped game's worth of presses, no more") {
    Handoff h;
    for (std::size_t i = 0; i < Handoff::kMaxQueued + 10; ++i) {
        h.Press(HOS_COOP_MENU_DOWN, 0, 0);
    }
    CHECK(h.Take().size() == Handoff::kMaxQueued);
}

TEST_CASE("menu handoff: Fill copies the page, nothing before the first one, nor into a short page") {
    Handoff h;
    Held held;
    HosCoopMenuPage out = Out();
    CHECK_FALSE(Fill(h, held, &out));
    h.Publish(MakePage("Host a game", false));
    HosCoopMenuPage small = Out();
    small.size = 4;
    CHECK_FALSE(Fill(h, held, &small));
    CHECK_FALSE(Fill(h, held, nullptr));
    REQUIRE(Fill(h, held, &out));
    REQUIRE(out.rows_n == 2);
    CHECK(std::string(out.rows[0].name) == "Host a game");
    CHECK(std::string(out.rows[0].value) == "On");
    CHECK(out.rows[0].kind == HOS_COOP_MENU_CHOICE);
    CHECK(out.rows[0].left == 1);
    CHECK(out.rows[0].right == 0);
    CHECK(out.rows[1].kind == HOS_COOP_MENU_HEADING);
    CHECK(out.selected == 0);
    CHECK(out.typing == 0);
    CHECK(std::string(out.help_title) == "Multiplayer");
    CHECK(std::string(out.hint) == "ABCD-1234");
    CHECK(std::string(out.confirm) == "Open");
    REQUIRE(out.back != nullptr);   // (the Lobby and Mods tabs: "" as it is)
    CHECK(std::string(out.back).empty());
}

TEST_CASE("menu handoff: the Multiplayer tab's empty prompt words go to Hookshot as none") {
    Handoff h;
    h.Publish(MakePage("Host a game", false, true));
    Held held;
    HosCoopMenuPage out = Out();
    REQUIRE(Fill(h, held, &out));
    CHECK(out.back == nullptr);
    CHECK(out.change == nullptr);
    CHECK(std::string(out.confirm) == "Open");
}

TEST_CASE("menu handoff: the strings Hookshot was handed hold after a newer page is published, until the next Fill") {
    Handoff h;
    h.Publish(MakePage("Host a game", false));
    Held held;
    HosCoopMenuPage out = Out();
    REQUIRE(Fill(h, held, &out));
    const char* name = out.rows[0].name;
    h.Publish(MakePage("Join a game", false));   // (the main thread's next Tick)
    CHECK(std::strcmp(name, "Host a game") == 0);
    HosCoopMenuPage again = Out();
    REQUIRE(Fill(h, held, &again));
    CHECK(std::string(again.rows[0].name) == "Join a game");
}

TEST_CASE("menu handoff: Clear forgets the page (a Mods tab gone)") {
    Handoff h;
    h.Publish(MakePage("A mod", true));
    h.Clear();
    Held held;
    HosCoopMenuPage out = Out();
    CHECK_FALSE(Fill(h, held, &out));
    CHECK_FALSE(h.Press(HOS_COOP_MENU_BACK, 0, 0));
}

TEST_CASE("menu handoff: pages and presses from two threads at once stay whole") {
    Handoff h;
    h.Publish(MakePage("row 0", false));
    std::atomic<bool> stop{false};
    std::thread main_thread([&] {
        for (int i = 1; i < 2000; ++i) {
            for (const Event& e : h.Take()) (void)e;
            h.Publish(MakePage("row " + std::to_string(i), i % 2 == 0));
        }
        stop = true;
    });
    Held held;

    while (!stop) {
        HosCoopMenuPage out = Out();
        const bool whole = Fill(h, held, &out) && out.rows_n == 2 && std::strncmp(out.rows[0].name, "row ", 4) == 0;
        CHECK(whole);
        if (!whole) break;
        h.Press(HOS_COOP_MENU_DOWN, 0, 0);

    }
    main_thread.join();

}

TEST_CASE("menu lock timer: logged over the limit, then only when twice the longest logged (row 236)") {
    std::uint64_t most = 0;
    CHECK_FALSE(CoopMenuHandoff::SlowToLog(most, 5, 20));    // (short: never)
    CHECK_FALSE(CoopMenuHandoff::SlowToLog(most, 20, 20));   // (at the limit: not over it)
    CHECK(most == 0);
    CHECK(CoopMenuHandoff::SlowToLog(most, 30, 20));         // the first slow one
    CHECK(most == 30);
    CHECK_FALSE(CoopMenuHandoff::SlowToLog(most, 45, 20));   // under twice 30
    CHECK(CoopMenuHandoff::SlowToLog(most, 60, 20));         // twice
    CHECK(most == 60);
    CHECK_FALSE(CoopMenuHandoff::SlowToLog(most, 25, 20));   // (shorter than logged: no line)
    CHECK(most == 60);
    CHECK(CoopMenuHandoff::SlowToLog(most, 5000, 20));       // a stall: always said
}

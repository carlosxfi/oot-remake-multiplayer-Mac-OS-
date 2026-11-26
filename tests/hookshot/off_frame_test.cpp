// SPDX-License-Identifier: GPL-3.0-or-later
// Row 292: slow reads off the frame (hookshot/src/coop_off_frame.h) and the LAN announcement's networks read on their
// own thread (coop_lan.h, NetworkTargets): the frame's call never waits for a read, however slow.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "coop_lan.h"
#include "coop_off_frame.h"

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

template <class Done>
bool WaitFor(Done done) {   // (at most 10 s: a read that never ends fails the test, not the run)
    for (int i = 0; i < 1000 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return done();
}

// The frame's budget for this work (a frame is 16.7 ms at 60 fps): starting a thread is well under it.
constexpr double kFrameBudgetMs = 5.0;

}  // namespace

TEST_CASE("off frame: a slow job runs on its own thread, Start does not wait for it, and jobs never pile up") {
    std::atomic<bool> release{false};
    std::atomic<int> runs{0};
    std::thread::id job_thread;
    CoopOffFrame::Slot slot;
    const auto t0 = Clock::now();
    CHECK(slot.Start([&] {
        job_thread = std::this_thread::get_id();
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++runs;
    }));
    CHECK(MsSince(t0) < kFrameBudgetMs);
    CHECK(slot.Busy());
    const auto t1 = Clock::now();
    CHECK_FALSE(slot.Start([&] { ++runs; }));   // (still running: not queued behind it)
    CHECK(MsSince(t1) < kFrameBudgetMs);
    release = true;
    REQUIRE(WaitFor([&] { return !slot.Busy(); }));
    CHECK(runs == 1);
    CHECK(job_thread != std::this_thread::get_id());
    CHECK(slot.Start([&] { ++runs; }));   // (done: the next one starts)
    REQUIRE(WaitFor([&] { return !slot.Busy(); }));
    CHECK(runs == 2);
}

TEST_CASE("off frame: a job that throws frees its slot") {
    CoopOffFrame::Slot slot;
    CHECK(slot.Start([] { throw 1; }));
    REQUIRE(WaitFor([&] { return !slot.Busy(); }));
    CHECK(slot.Start([] {}));
}

TEST_CASE("lan: the host's networks are read off the frame: a 1 s read never stalls an announcement (row 292)") {
    std::atomic<int> reads{0};
    std::thread::id read_thread;
    CoopLan::NetworkTargets targets([&] {
        read_thread = std::this_thread::get_id();
        ++reads;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));   // (a player's PC: ~850 ms)
        return std::vector<std::uint32_t>{0xc0a801ffu};                  // 192.168.1.255
    });
    std::uint64_t now = 5000;
    double worst = 0;
    for (int frame = 0; frame < 30; ++frame, now += 16) {   // (half a second of frames while the read runs)
        const auto t0 = Clock::now();
        const auto to = targets.Now(now);
        worst = std::max(worst, MsSince(t0));
        CHECK(to.count(CoopLan::kEveryNetwork) == 1);
        CHECK(to.count(CoopLan::kThisPc) == 1);
        CHECK(to.count(0xc0a801ffu) == 0);   // (not read yet: the always-there two meanwhile)
    }
    CHECK(worst < kFrameBudgetMs);
    REQUIRE(WaitFor([&] { return targets.Reads() == 1; }));
    CHECK(reads == 1);   // (one read at a time, however many frames)
    CHECK(read_thread != std::this_thread::get_id());
    CHECK(targets.Now(now).count(0xc0a801ffu) == 1);
    // Not again before kNetworksEveryMs; then once more, again off the frame.
    targets.Now(5000 + CoopLan::kNetworksEveryMs - 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(reads == 1);
    const auto t0 = Clock::now();
    targets.Now(5000 + CoopLan::kNetworksEveryMs);
    CHECK(MsSince(t0) < kFrameBudgetMs);
    REQUIRE(WaitFor([&] { return reads == 2; }));
}

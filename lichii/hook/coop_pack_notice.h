// SPDX-License-Identifier: GPL-3.0-or-later
//
// No puppet pack (player report, 0.11.0): the other players then show as their
// names only, and the only hint was a line in longshot_log.txt. This says it in
// the co-op chat once per game run, when another player is first shown, with
// what to do. Only the Longshot module turns it on (Enable): the emulator's
// kit says it in its start window. Header only, standard C++ only.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace CoopPackNotice {

// The kit's start file per system: the Windows kit's Start Squadala.bat, the Linux kit's start-squadala.sh.
#ifdef _WIN32
inline constexpr const char* kText =
    "No puppet pack: other players show as names only. Close the game and run Start Squadala.bat again.";
#else
inline constexpr const char* kText =
    "No puppet pack: other players show as names only. Close the game and run start-squadala.sh again.";
#endif

// The once-per-run decision, apart for the tests.
class Once {
public:
    // `pack`: a puppet pack is loaded. `others`: another player is shown now.
    // True the first time there is no pack and someone to see; never again.
    bool Due(bool pack, bool others) {
        if (said_ || pack || !others) {
            return false;
        }
        said_ = true;
        return true;
    }

private:
    bool said_ = false;
};

namespace detail {
inline std::atomic<int>& State() {  // 0 off, 1 on with a pack, 2 on without one
    static std::atomic<int> s{0};
    return s;
}
inline Once& TheOnce() {
    static Once o;
    return o;
}
}  // namespace detail

// The module's start, once: `pack` is whether the puppet pack is there (Longshot
// indexes its mod files once at start, so it does not change while it runs).
inline void Enable(bool pack) { detail::State().store(pack ? 1 : 2); }

// Present thread, once per frame: the line to add to the chat, or nothing.
inline std::vector<std::string> TakeLines(bool others) {
    const int s = detail::State().load(std::memory_order_relaxed);
    if (s == 0 || !detail::TheOnce().Due(s == 1, others)) {
        return {};
    }
    return {kText};
}

}  // namespace CoopPackNotice

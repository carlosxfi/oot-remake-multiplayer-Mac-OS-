// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's game-free part (docs/playtest.md, "Bridge protocol"): the frame hold, the requests'
// checks and the stick's numbers. coop_playtest.cpp puts it in the game (the socket, Longshot's control bridge,
// the module's test commands); the unit tests (tests/hookshot/playtest_test.cpp) drive it with plain threads.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_mod_json.h"

namespace CoopPlaytest {

inline constexpr int kProtocol = 1;
inline constexpr std::size_t kMaxLine = 64 * 1024;
inline constexpr std::uint32_t kMaxFrames = 3600;   // step, input
inline constexpr double kDefaultIdleS = 600.0;      // a hold with no command this long ends by itself

// LICHII_COOP_PLAYTEST_PORT: 1024 to 65535; anything else (unset, empty, 0, words) is off.
std::optional<std::uint16_t> PortFrom(std::string_view value);
// LICHII_COOP_PLAYTEST_IDLE: seconds (0 never); unset or bad: the default.
double IdleFrom(std::string_view value);
// A dump's tag: 1 to 64 of letters, digits, '-' and '_' (it names a file in the dump folder: no path).
bool ValidTag(std::string_view tag);

// The game's frame hold. The game's calc thread calls AtFrame at the top of every frame hook; the bridge's thread
// calls the rest. While held, AtFrame waits (the frame's update has not run yet) and runs the jobs the bridge
// hands it there, on the game's thread.
class Gate {
public:
    using Clock = std::chrono::steady_clock;
    explicit Gate(double idle_s = kDefaultIdleS) : idle_s_(idle_s) {}

    // (calc thread) One frame hook: counts the frame, and holds while asked.
    void AtFrame();

    std::uint64_t Frame() const;
    bool Held() const;     // the calc thread waits in AtFrame now
    bool Holding() const;  // a hold is asked (held now, or at the next frame)

    // Hold at the next frame; true once held (false: no frame came in time: a load, the game closed).
    bool Pause(std::chrono::milliseconds timeout);
    // Run in real time again (also: the bridge's client left; the idle time ran out).
    void Resume();
    // Let exactly `frames` frames run, then hold. Holds first when running. `from`: the frame it started at.
    bool Step(std::uint32_t frames, std::chrono::milliseconds timeout, std::uint64_t* from = nullptr);
    // Run `job` on the calc thread inside the hold (holds first). False: not held in time (the job is dropped).
    bool RunHeld(std::function<void()> job, std::chrono::milliseconds timeout);
    // A command came: the idle time starts again.
    void Touch();
    // How the last hold ended by itself, once (for the log): "idle" or empty.
    std::string TakeEndNote();

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t frame_ = 0;
    std::uint32_t budget_ = 0;  // frames still to run in a step
    bool want_hold_ = false;
    bool held_ = false;
    std::deque<std::function<void()>*> jobs_;
    Clock::time_point touched_ = Clock::now();
    double idle_s_;
    std::string end_note_;
};

// The stick: angle in degrees (0 up, 90 right, clockwise), strength 0 to 1, as Longshot's axes (-1 to 1, y up).
struct Axes {
    double x = 0;
    double y = 0;
};
Axes StickAxes(double angle_deg, double strength);

// A checked request.
struct Request {
    std::string cmd;
    CoopModJson::Value id;  // (null when none)
    std::uint32_t frames = 1;
    std::vector<std::string> buttons;
    std::optional<Axes> stick, rstick;
    bool exclusive = true;
    std::string scene, entrance, name, item, tag, value;
    double timeout_s = 60;
};
// The request on one line, or `error` (bad_request's message).
std::optional<Request> ParseRequest(std::string_view line, std::string& error);

// Longshot's sequence request for an input (one step: the buttons and both sticks, `frames` frames, from the next
// controller read), not waited on: the bridge steps the frames itself.
std::string ControlInput(const Request& r, int control_id);

// The reply's first fields: id, ok, frame, paused; `rest` (a JSON object's members, may be empty) after them.
std::string Reply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, const std::string& rest);
std::string ErrorReply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, std::string_view code,
                       std::string_view message);
// A JSON string literal.
std::string Quote(std::string_view text);
// A value as JSON on one line, no spaces (CoopModJson::Write is for files: it indents objects and ends with a
// newline, which would split a reply in two).
std::string Compact(const CoopModJson::Value& value);

}  // namespace CoopPlaytest

// The chat lines the co-op window shows: received chat, our own sent chat
// and room notices ("Saria joined"). The network thread adds, the Qt window
// reads; header only, standard C++ only.
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace CoopChat {

using Clock = std::chrono::steady_clock;

// How many lines the banner reads (Lines); older ones are only in the history.
inline constexpr std::size_t kKeep = 6;
// How many lines the session's history keeps (History: the - menu's Players view lists them, row 282); older ones
// are dropped. The log lives as long as the session (Coop): a new session starts with none.
inline constexpr std::size_t kHistory = 100;

struct Line {
    std::string text;
    bool notice = false;  // a join, leave or connection line, not chat
    Clock::time_point at{};
};

class Log {
public:
    void Add(std::string text, bool notice) {
        std::lock_guard lock(mutex_);
        lines_.push_back({std::move(text), notice, Clock::now()});
        while (lines_.size() > kHistory) {
            lines_.pop_front();
        }
    }

    // The last kKeep lines, oldest first (the banner and the window's chat).
    std::vector<Line> Lines() const {
        std::lock_guard lock(mutex_);
        const std::size_t from = lines_.size() > kKeep ? lines_.size() - kKeep : 0;
        return {lines_.begin() + static_cast<std::ptrdiff_t>(from), lines_.end()};
    }

    // Every kept line (at most kHistory), oldest first.
    std::vector<Line> History() const {
        std::lock_guard lock(mutex_);
        return {lines_.begin(), lines_.end()};
    }

private:
    mutable std::mutex mutex_;
    std::deque<Line> lines_;
};

} // namespace CoopChat

// SPDX-License-Identifier: GPL-3.0-or-later
// After a crash, the next title asks the player to send it to us (a player's idea, 0.11.9): one page on the title's own
// four buttons, in the game's own look (the pack's live texts, as co-op's title pages), shown once for that crash and
// gone with any press. Nothing else ever waits for it: a guest's host save loading by itself, or co-op's own page on the
// title, shuts it (coop_title.cpp).
//
// What counts as a crash is Longshot's own record: its crash watcher writes bug-reports/pending/crash-<UTC>.txt in the
// game folder (host/hos_crash_dialog.h, hos_crash_record_name: crash-YYYYMMDDTHHMMSSZ[-<pid>-<n>].txt). An "unclean-"
// record (the game ended without its own exit: a closed console, a split screen's instance stopped, a power cut) is not
// asked about: those are mostly not problems. Which records were asked about: bug-reports/coop-asked.txt (ours; Longshot's
// own shown.txt is left alone). A crash the kit has already sent to us (the player said yes to sending problem reports:
// tools/release/kit/scripts/problem-send.ps1, linux-longshot/scripts/problem_report.py) is not asked about either: the kit
// lists it in bug-reports/coop-sent.txt (a name a line) once the bot took the report. With sending off the file is never
// written, and the ask stays as it was.
//
// Logic only, plus the two files (tests/hookshot/problem_ask_test.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace CoopProblemAsk {

// A crash is asked about for this long after it (a player who comes back days later still sees it once).
inline constexpr std::int64_t kMaxAgeS = 3 * 86400;

// A crash record's time (seconds since 1970, UTC) from its name, or nothing when the name is not one (an unclean record,
// another file).
std::optional<std::int64_t> CrashTime(std::string_view name);

// The crash to ask about: the newest crash record in `pending` that `asked` (coop-asked.txt, a name a line) does not
// list, at most kMaxAgeS old and not more than 5 minutes ahead of `now_s` (a clock set wrong). Nothing: no ask.
std::optional<std::string> Due(std::span<const std::string> pending, std::string_view asked, std::int64_t now_s);

// coop-asked.txt once the player saw the ask: every crash record pending now (one ask for several crashes in a row; the
// names Longshot has since cleared out are dropped, so the file stays small).
std::string AskedAfter(std::span<const std::string> pending);

// The page's words in English (coop_title.cpp translates them), one a title button: three lines and OK. Each fits a
// title button (CoopTitleNative::kTextBytes).
inline constexpr std::array<const char*, 4> kWords{"The game crashed last time.", "Please send /bugreport on Discord",
                                                    "with the zip of collect-logs.cmd.", "OK"};

// The game folder's files: the names in bug-reports/pending (none when it is not there), and coop-asked.txt read and
// written whole ("" when it is not there; false when it could not be written).
std::vector<std::string> PendingNames(const std::filesystem::path& game_folder);
std::string ReadAsked(const std::filesystem::path& game_folder);
// bug-reports/coop-sent.txt, the kit's (read only here): "" when it is not there. Due() takes it with coop-asked.txt.
std::string ReadSent(const std::filesystem::path& game_folder);
bool WriteAsked(const std::filesystem::path& game_folder, const std::string& text);

}  // namespace CoopProblemAsk

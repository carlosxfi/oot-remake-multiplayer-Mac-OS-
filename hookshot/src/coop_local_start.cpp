// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_local_start.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "coop_title_menu.h"

namespace CoopLocalStart {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<int> Numbers(std::string_view text) {
    std::vector<int> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t comma = std::min(text.find(',', at), text.size());
        const std::string_view part = CoopTitleMenu::Trim(text.substr(at, comma - at));
        int n = 0;
        if (!part.empty() && std::from_chars(part.data(), part.data() + part.size(), n).ec == std::errc{} && n >= 2 &&
            n <= 9) {
            out.push_back(n);
        }
        at = comma + 1;
    }
    return out;
}

std::string List(const std::vector<int>& players) {
    std::string out;
    for (std::size_t i = 0; i < players.size(); ++i) {
        out += (i == 0 ? "" : i + 1 == players.size() ? " and " : ", ") + std::to_string(players[i]);
    }
    return out;
}

// One argument for powershell -File: in quotes, a quote inside doubled backslashes before it (CommandLineToArgvW's
// rules; a folder name with spaces, "OOT REMAKE", stays one argument).
std::string Quoted(const std::string& arg) {
    std::string out = "\"";
    std::size_t slashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        if (c == '"') {
            out.append(slashes * 2 + 1, '\\');
        } else {
            out.append(slashes, '\\');
        }
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');   // (before the closing quote)
    out += '"';
    return out;
}

std::string Utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

int PlayerOf(const std::filesystem::path& game_dir) {
    std::filesystem::path dir = game_dir;
    if (!dir.empty() && !dir.has_filename()) dir = dir.parent_path();   // (a trailing slash)
    const std::string leaf = Lower(Utf8(dir.filename()));
    const std::string parent = Lower(Utf8(dir.parent_path().filename()));
    if (parent != "local-players" || leaf.size() != 7 || leaf.rfind("player", 0) != 0) return 1;
    const char d = leaf[6];
    return d >= '2' && d <= '9' ? d - '0' : 1;
}

std::optional<Session> SessionOf(std::string_view coop_ini) {
    std::string server(CoopTitleMenu::Trim(CoopTitleMenu::IniGet(coop_ini, "server").value_or("")));
    std::optional<std::uint16_t> port;
    if (const auto colon = server.rfind(':'); colon != std::string::npos && server.find(':') == colon) {
        port = CoopTitleMenu::ParsePort(server.substr(colon + 1));
        server.resize(colon);
    } else {
        port = CoopTitleMenu::ParsePort(CoopTitleMenu::IniGet(coop_ini, "port").value_or(""));
    }
    if ((server != "127.0.0.1" && Lower(server) != "localhost") || !port) return std::nullopt;
    Session s;
    s.port = *port;
    s.password = std::string(CoopTitleMenu::Trim(CoopTitleMenu::IniGet(coop_ini, "password").value_or("")));
    if (s.password.empty()) return std::nullopt;   // (a local session always has one)
    return s;
}

int GamesListed(std::string_view text) {
    int most = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string_view line = CoopTitleMenu::Trim(text.substr(at, end - at));
        if (line.size() >= 3 && line[0] >= '2' && line[0] <= '9' && line[1] == '=') most = std::max(most, line[0] - '0');
        at = end + 1;
    }
    return most;
}

std::optional<Result> ParseResult(std::string_view text) {
    const auto started = CoopTitleMenu::IniGet(text, "started");
    if (!started) return std::nullopt;   // (not written yet, or not the launcher's)
    Result r;
    r.started = Numbers(*started);
    r.running = Numbers(CoopTitleMenu::IniGet(text, "running").value_or(""));
    r.failed = Numbers(CoopTitleMenu::IniGet(text, "failed").value_or(""));
    r.problem = std::string(CoopTitleMenu::Trim(CoopTitleMenu::IniGet(text, "problem").value_or("")));
    return r;
}

std::string ResultNote(const Result& r) {
    // (a native list line holds 46 letters: the most important first, the problem line in the page's help)
    if (!r.failed.empty()) return "Did not start: player " + List(r.failed) + ".";
    if (!r.started.empty()) return "Started: player " + List(r.started) + ". They join soon.";
    if (!r.running.empty()) return "Every game runs already.";
    return r.problem.empty() ? std::string("No game was started.") : std::string("Not started: see Start's help.");
}

std::string NewPassword(std::uint64_t seed) {
    static constexpr char kLetters[] = "abcdefghijkmnpqrstuvwxyz23456789";
    std::uint64_t x = seed ? seed : 0x9e3779b97f4a7c15ull;
    std::string out;
    for (int i = 0; i < 12; ++i) {
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;   // (xorshift: a password nobody types, only this PC's games read)
        out += kLetters[x % (sizeof kLetters - 1)];
    }
    return out;
}

std::string CommandLine(const std::filesystem::path& kit, const std::filesystem::path& exe, int players,
                        const Session& session, std::uint32_t host_pid, const std::filesystem::path& result) {
    const std::filesystem::path script = kit / "scripts" / kScript;
    std::string out = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File ";
    out += Quoted(Utf8(script));
    out += " -Kit " + Quoted(Utf8(kit));
    out += " -Exe " + Quoted(Utf8(exe));
    out += " -Players " + std::to_string(std::clamp(players, 2, 4));
    out += " -Port " + std::to_string(session.port);
    out += " -Password " + Quoted(session.password);
    out += " -HostPid " + std::to_string(host_pid);
    out += " -Result " + Quoted(Utf8(result));
    return out;
}

}  // namespace CoopLocalStart

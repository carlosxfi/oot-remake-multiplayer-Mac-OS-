// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_problem_ask.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace CoopProblemAsk {
namespace {

constexpr std::string_view kPrefix = "crash-";

// Days since 1970-01-01 of a date in the Gregorian calendar (H. Hinnant's days_from_civil).
std::int64_t DaysFromCivil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

int Digits(std::string_view s, std::size_t at, std::size_t n) {
    int v = 0;
    for (std::size_t i = at; i < at + n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

bool Listed(std::string_view asked, std::string_view name) {
    std::size_t at = 0;
    while (at <= asked.size()) {
        std::size_t end = asked.find('\n', at);
        if (end == std::string_view::npos) end = asked.size();
        std::string_view line = asked.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == name) return true;
        at = end + 1;
    }
    return false;
}

}  // namespace

std::optional<std::int64_t> CrashTime(std::string_view name) {
    // crash-YYYYMMDDTHHMMSSZ[-<pid>-<n>].txt, as hos_crash_record_name takes it
    if (name.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    const std::string_view p = name.substr(kPrefix.size());
    if (p.size() < 16) return std::nullopt;
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = p[i];
        if (i == 8 ? c != 'T' : i == 15 ? c != 'Z' : (c < '0' || c > '9')) return std::nullopt;
    }
    std::string_view s = p.substr(16);
    const auto number = [&s]() {
        std::size_t n = 0;
        while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
        s.remove_prefix(n);
        return n > 0;
    };
    if (!s.empty() && s.front() == '-') {
        s.remove_prefix(1);
        if (!number() || s.empty() || s.front() != '-') return std::nullopt;
        s.remove_prefix(1);
        if (!number()) return std::nullopt;
    }
    if (s != ".txt") return std::nullopt;
    const int mo = Digits(p, 4, 2), d = Digits(p, 6, 2), h = Digits(p, 9, 2), mi = Digits(p, 11, 2), se = Digits(p, 13, 2);
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return std::nullopt;
    const std::int64_t days = DaysFromCivil(Digits(p, 0, 4), static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return days * 86400 + h * 3600 + mi * 60 + se;
}

std::optional<std::string> Due(std::span<const std::string> pending, std::string_view asked, std::int64_t now_s) {
    std::optional<std::string> best;
    std::int64_t best_t = 0;
    for (const std::string& name : pending) {
        const auto t = CrashTime(name);
        if (!t || *t > now_s + 300 || now_s - *t > kMaxAgeS || Listed(asked, name)) continue;
        if (!best || *t > best_t || (*t == best_t && name > *best)) best = name, best_t = *t;
    }
    return best;
}

std::string AskedAfter(std::span<const std::string> pending) {
    std::vector<std::string> names;
    for (const std::string& name : pending) {
        if (CrashTime(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    std::string out;
    for (const std::string& name : names) out += name + "\n";
    return out;
}

std::vector<std::string> PendingNames(const std::filesystem::path& game_folder) {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(game_folder / "bug-reports" / "pending", ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto u8 = it->path().filename().u8string();   // (std::string or std::u8string: the bytes either way)
        std::string name;
        for (const auto c : u8) name.push_back(static_cast<char>(c));
        names.push_back(std::move(name));
    }
    return names;
}

namespace {

std::string ReadSmall(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream s;
    s << in.rdbuf();
    return std::move(s).str().substr(0, 64 << 10);
}

}  // namespace

std::string ReadAsked(const std::filesystem::path& game_folder) {
    return ReadSmall(game_folder / "bug-reports" / "coop-asked.txt");
}

std::string ReadSent(const std::filesystem::path& game_folder) {
    return ReadSmall(game_folder / "bug-reports" / "coop-sent.txt");
}

bool WriteAsked(const std::filesystem::path& game_folder, const std::string& text) {
    std::error_code ec;
    const std::filesystem::path dir = game_folder / "bug-reports";
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "coop-asked.txt", part = dir / "coop-asked.txt.part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.flush()) return false;
    }
    std::filesystem::rename(part, path, ec);
    return !ec;
}

}  // namespace CoopProblemAsk

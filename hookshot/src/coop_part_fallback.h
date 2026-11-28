// Row 280: a mod part the kit made (Character Select's) whose files named a file neither the part nor the game has.
// The kit's tools/puppet_pack/part_check.py left those files out, so the game uses its own (without that, every load
// of them waits for good: a black world), and wrote <mod>/coop-fallback.txt, one line in English for the player. The
// module shows that line on the game's banner (SimpleInfo_00) once each time play starts from the title, and on the
// Mods tab. Solo too: it is about this PC's files, not co-op. Logic here; coop_hookshot.cpp and
// coop_mods_tab_module.cpp call it. Tests: tests/hookshot/part_fallback_test.cpp.
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "coop_mod_library.h"

namespace CoopPartFallback {

inline constexpr const char* kFile = "coop-fallback.txt";
inline constexpr std::size_t kMaxBytes = 240;

// The first line of <mods_root>/<mod>/coop-fallback.txt for each mod named, in that order: printable ASCII only (the
// kit writes English; anything else in it is not shown), cut at kMaxBytes; a mod without the file, or an empty or
// unreadable one, gives nothing.
inline std::vector<std::string> Lines(const std::filesystem::path& mods_root, const std::vector<std::string>& mods) {
    std::vector<std::string> out;
    for (const std::string& m : mods) {
        if (m.empty()) continue;
        std::error_code ec;
        const std::filesystem::path p = mods_root / CoopMods::FromUtf8(m) / kFile;
        if (!std::filesystem::is_regular_file(p, ec)) continue;
        std::ifstream f(p, std::ios::binary);
        std::string line;
        if (!f || !std::getline(f, line)) continue;
        std::string clean;
        for (const char c : line) {
            if (c == '\r') break;
            if (c < 0x20 || c > 0x7E) {
                clean.clear();
                break;
            }
            clean.push_back(c);
        }
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (clean.size() > kMaxBytes) clean.resize(kMaxBytes);
        if (!clean.empty()) out.push_back(std::move(clean));
    }
    return out;
}

// Once each time play starts: true at the first frame Link is in the world after the module started or after the
// title screen was up; then false until the title is up again.
class OncePerLoad {
public:
    bool Step(bool title_up, bool link_here) {
        if (title_up) {
            armed_ = true;
            return false;
        }
        if (!link_here || !armed_) return false;
        armed_ = false;
        return true;
    }

private:
    bool armed_ = true;
};

}  // namespace CoopPartFallback

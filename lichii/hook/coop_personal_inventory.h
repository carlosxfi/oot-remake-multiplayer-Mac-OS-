#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace CoopPersonalInventory {

// GameData Int fields. Capacity comes from the incoming host save.
inline constexpr std::array stock{0x5fadf7fcu, 0x518ec74bu, 0xdb1bcd4fu,
                                  0xf30d9f54u, 0xb9494411u, 0xa77921d7u};
inline constexpr std::array capacity{0xae8cc742u, 0x418db791u, 0xe753508bu,
                                     0x735b00f7u, 0x457616b0u, 0xe0010a50u};
using Counts = std::array<std::uint32_t, stock.size()>;

inline bool Restore(std::span<std::uint8_t> bytes, const Counts& counts, Counts* applied = nullptr) {
    const auto word = [&](std::size_t at) {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i) result |= std::uint32_t(bytes[at + i]) << (8 * i);
        return result;
    };
    constexpr std::size_t table = 0x20;
    if (bytes.size() < table || word(0) != 0x01020304) return false;
    const auto pool = word(8);
    if (pool < table || pool > bytes.size()) return false;

    std::array<std::size_t, stock.size()> positions{};
    std::array<std::size_t, capacity.size()> limits{};
    int kind = -1;
    for (std::size_t at = table; pool - at >= 8; at += 8) {
        const auto hash = word(at);
        if (hash == 0) {
            if (word(at + 4) != static_cast<unsigned>(++kind)) return false;
            if (kind != 0x20) continue;
            if (pool - at < 16) return false;
            for (std::size_t i = 0; i < counts.size(); ++i) {
                if (positions[i] == 0 || limits[i] == 0 || word(limits[i]) > 10000) return false;
            }
            for (std::size_t i = 0; i < counts.size(); ++i) {
                const auto count = std::min(counts[i], word(limits[i]));
                if (applied) (*applied)[i] = count;
                for (unsigned b = 0; b < 4; ++b) {
                    bytes[positions[i] + b] = static_cast<std::uint8_t>(count >> (8 * b));
                }
            }
            return true;
        }
        if (kind < 0) return false;
        if (kind != 2) continue;  // Int, as in tools/autotest/save_flags.py
        for (std::size_t i = 0; i < stock.size(); ++i) {
            if (hash == stock[i]) {
                if (positions[i]) return false;
                positions[i] = at + 4;
            }
            if (hash == capacity[i]) {
                if (limits[i]) return false;
                limits[i] = at + 4;
            }
        }
    }
    return false;
}

}  // namespace CoopPersonalInventory

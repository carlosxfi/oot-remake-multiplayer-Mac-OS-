// SPDX-License-Identifier: GPL-3.0-or-later
// The room list's plain parts (no network, no thread): what a listing means and what its lines say. Kept apart from
// coop_room_list.cpp (the relay asks) so the unit tests link them.
#include <cstdint>
#include <string>
#include <utility>

#include "coop/protocol/messages.hpp"
#include "coop_i18n.h"
#include "coop_room_list.h"

namespace CoopRoomList {

std::string CellText(const Room& r, int my_protocol) {
    std::string tail;
    if (r.protocol != my_protocol) {
        tail = " (other version)";
    } else if (r.max > 0 && r.players >= r.max) {
        tail = " (full)";
    } else if (r.max > 0) {
        tail = " (" + std::to_string(r.players) + "/" + std::to_string(r.max) + ")";
    }
    // (the browser's list shows the name the room is listed under on its own line, CoopPauseTab::ListName)
    return r.room + tail;
}

bool Joinable(const Room& r, int my_protocol) {
    return r.protocol == my_protocol && (r.max <= 0 || r.players < r.max);
}

static_assert(kPublicListingBit == coop::protocol::kPublicListingBit);
static_assert(kListingDifficultyShift == coop::protocol::kListingDifficultyShift);

Room FromListing(std::string room, std::string host, int players, int max, int version) {
    const auto v = static_cast<std::uint16_t>(version);
    Room r{std::move(room), std::move(host), players, max, coop::protocol::listing_protocol(v), false, false};
    r.is_public = (v & coop::protocol::kPublicListingBit) != 0;
    r.difficulty = static_cast<int>(coop::protocol::listing_difficulty(v));
    return r;
}

const char* DifficultyWord(int listed) {
    using L = coop::protocol::ListedDifficulty;
    switch (static_cast<L>(listed)) {
    case L::automatic: return "Automatic";
    case L::easy: return "Easy";
    case L::normal: return "Normal";
    case L::hard: return "Hard";
    case L::custom: return "Custom";
    case L::none: break;
    }
    return "";
}

std::string DetailText(const Room& r, int my_protocol) {
    // (each piece in the player's language: coop_i18n's table has the pieces, not the built line)
    using CoopI18n::Tr;
    if (r.protocol > my_protocol) return Tr("Needs an update: this game is newer than yours");
    if (r.protocol != my_protocol) return Tr("Another version: the host needs to update");
    std::string out = r.official    ? Tr("Official public game")
                      : r.is_public ? Tr("Public game")
                                    : Tr("Needs the host's password");
    if (r.max > 0) {
        out += Tr(", ") + (r.players >= r.max ? Tr("Full")
                                              : Tr("Players") + " " + std::to_string(r.players) + "/" +
                                                    std::to_string(r.max));
    }
    // (the lobby's difficulty, the host's pick when making it: the Difficulty row's own words)
    if (const char* word = DifficultyWord(r.difficulty); word[0] != 0) out += Tr(", ") + Tr(word);
    return out;
}

}  // namespace CoopRoomList

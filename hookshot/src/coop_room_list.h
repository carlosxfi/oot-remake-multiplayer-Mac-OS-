// SPDX-License-Identifier: GPL-3.0-or-later
// The relay's public room list (server/relay_link.hpp, list_rooms_on_relay; docs/play-online.md) for the pause
// menu's game browser: asked from a thread of its own while the browser shows its Online list, every kEveryMs, and
// kept for the page to read. A room is listed only when its host asked (coop-options.ini list_room = on); the list
// never carries a password, a join code or an address: a room is joined by its name and the password its host gives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace CoopRoomList {

inline constexpr std::uint64_t kEveryMs = 10000;   // asked again this often while shown
inline constexpr std::uint64_t kKeepMs = 30000;    // what was heard is kept this long after the list is left

struct Room {
    std::string room, host;
    int players = 0, max = 0, protocol = 0;
};

// What a room's cell says: "kokiri (2/4)", "(full)", "(other version)" as the games on this network's rows.
std::string CellText(const Room& r, int my_protocol);
// Whether it can be joined from here (same protocol, room for one more).
bool Joinable(const Room& r, int my_protocol);

struct Snapshot {
    std::vector<Room> rooms;
    std::string state;   // "" asking (nothing heard yet) or rooms listed; else why there are none
};

// The module's part (coop_room_list.cpp): `wanted` while the browser shows its Online list; `relay` the kit's relay
// ("host" or "host:port", coop-options.ini relay), "" none set.
void Look(bool wanted, std::uint64_t now_ms, const std::string& relay);
Snapshot Now();
// The list asked again at the next Look (the browser's Refresh), what was heard dropped meanwhile.
void AskNow();

}  // namespace CoopRoomList

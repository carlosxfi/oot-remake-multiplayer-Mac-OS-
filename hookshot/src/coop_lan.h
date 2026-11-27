// SPDX-License-Identifier: GPL-3.0-or-later
// Games on this network (the pause menu's Multiplayer page, "Games on this network"): a hosting game says it is there
// on the local network every two seconds (a UDP datagram to each network's broadcast address, port kPort), and a game
// looking for one lists the ones it heard in the last few seconds; A on one joins it at its address and port. The
// announcement names the game (the host's name, its port, how many play and fit, its room name, the protocol version)
// and never carries the join password: a guest types it on the Join view, as for a room. docs/protocol.md, "Games on
// this network". This file: the datagram and the list (no sockets: coop_lan_net.h).
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_off_frame.h"

namespace CoopLan {

inline constexpr std::uint16_t kPort = 27508;             // where the announcements go (the game's own is 27510)
inline constexpr std::uint64_t kEveryMs = 2000;           // a host announces itself this often
inline constexpr std::uint64_t kGoneMs = 7000;            // a game not heard this long is off the list
inline constexpr std::size_t kMaxDatagram = 512;
inline constexpr std::size_t kMaxName = 32;               // bytes of a host's name kept (cut at a UTF-8 letter)
inline constexpr std::size_t kMaxGames = 32;              // the most the list keeps

struct Game {
    std::string name;          // the host's name (coop.ini name)
    std::uint16_t port = 0;    // its game port
    int players = 0;           // in the game now, the host included
    int max = 0;               // the most it takes
    std::string room;          // its relay room name, "" none
    int protocol = 0;          // its protocol version (coop::protocol::kProtocolVersion): another one cannot join
    std::string id;            // a random token the game picks once (NewId): the same game heard from several of its
                               // networks (Wi-Fi, a VPN, a virtual switch) is one row. "" none (then address and port)
};

inline constexpr std::size_t kIdLength = 16;   // hex digits

// A new id: kIdLength hex digits from the system's random source.
std::string NewId();

// The datagram: "SQUADALA LAN 1" then "key=value" lines (name, port, players, max, room, protocol, id).
std::string Encode(const Game& g);
// A datagram heard: the game it names, or nothing for anything else (not ours, another format, out of range).
std::optional<Game> Decode(std::string_view datagram);

// A game heard, where from.
struct Heard {
    std::string address;   // its IPv4 address, as the datagram came from it ("192.168.1.20")
    Game game;
    std::uint64_t at_ms = 0;
};

// The games heard, newest word kept per game: per id, or per address and port for a game without one.
class List {
public:
    // A game with an id heard from another address keeps the address it was first heard at (a steady row), except that
    // a network address takes the place of 127.0.0.1.
    void Add(const std::string& address, const Game& g, std::uint64_t now_ms);
    // The ones heard in the last kGoneMs, by name then address (a steady order for the page's rows).
    std::vector<Heard> Now(std::uint64_t now_ms) const;
    void Clear() { heard_.clear(); }

private:
    std::vector<Heard> heard_;
};

// What a game's row says: "Saria's game (2/4)", "... (full)", "... (other version)".
std::string RowText(const Heard& h, int my_protocol);
// Whether it can be joined from here (same protocol, room for one more).
bool Joinable(const Heard& h, int my_protocol);
// Nucleus's "Join a game on this network" (coop.ini join_lan): the game to join among those heard. `want` "first" (or
// empty): the first that can be joined, in the list's order; else the one whose host has that name (any case). None:
// nothing.
std::optional<Heard> PickToJoin(const std::vector<Heard>& heard, const std::string& want, int my_protocol);

// Row 292: where a host's announcement goes. This PC's networks (a cable, a VPN) are read again every
// kNetworksEveryMs, but on a thread of their own (CoopOffFrame): reading them (GetAdaptersAddresses) took ~850 ms a
// time on a player's PC, in the frame, every 30 s. Now never waits for a read: until the first one is in, and while
// one runs, it gives the last addresses read; 255.255.255.255 (this network) and 127.0.0.1 (another game window on
// this PC) always. `read` gives the broadcast addresses (host order) and is called only on the read's own thread.
inline constexpr std::uint64_t kNetworksEveryMs = 30000;
inline constexpr std::uint32_t kEveryNetwork = 0xffffffffu, kThisPc = 0x7f000001u;
class NetworkTargets {
public:
    using Read = std::function<std::vector<std::uint32_t>()>;
    explicit NetworkTargets(Read read) : read_(std::move(read)) {}
    std::set<std::uint32_t> Now(std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        if ((asked_ms_ == 0 || now_ms - asked_ms_ >= kNetworksEveryMs) &&
            slot_.Start([this] {
                const std::vector<std::uint32_t> got = read_();
                std::lock_guard done(mutex_);
                read_targets_ = std::set<std::uint32_t>(got.begin(), got.end());
                ++reads_;
            })) {
            asked_ms_ = now_ms == 0 ? 1 : now_ms;   // (a read still running: asked again next time)
        }
        std::set<std::uint32_t> out = read_targets_;
        out.insert(kEveryNetwork);
        out.insert(kThisPc);
        return out;
    }
    int Reads() const {
        std::lock_guard lock(mutex_);
        return reads_;
    }

private:
    Read read_;
    mutable std::mutex mutex_;
    std::uint64_t asked_ms_ = 0;
    std::set<std::uint32_t> read_targets_;
    int reads_ = 0;
    CoopOffFrame::Slot slot_;   // (last: its end waits for a read still running, before the rest goes)
};

}  // namespace CoopLan

// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mods' data channel (issue 250; docs/protocol.md "Mods", MOD_DATA; docs/mods.md "Talking to the other players"):
// each player's mods give small values of their own (at most kMaxModDataBytes) by key, and every game in the session
// gets them. A value is set only for this game's own player and only under the mod's own id; a game hands a value only
// to the mod of the same id. Co-op's own keys go under the mod id "coop" (no mod has that id).
//
// Store is the pure part (no network, no clock of its own: the tests drive it). The functions below it are the glue a
// game uses: OnEvent on the network thread, Frame on the present thread, and the mods' calls (Set, Get, Players,
// MyNumber, TakeChanges) from any thread.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "coop/protocol/messages.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}  // namespace coop::net

namespace CoopModData {

// Co-op's own values (mod id "coop"). `skin`: the puppet skin a player shows (issue 250 part 2: its id and hash).
inline constexpr std::string_view kCoopModId = "coop";
inline constexpr std::string_view kSkinKey = "skin";

using Bytes = std::vector<std::uint8_t>;

// A value of another player that changed: `value` empty when it was cleared (also when that player left or this game
// left the session).
struct Change {
    std::uint8_t player = 0;
    std::string mod_id;
    std::string key;
    Bytes value;
    bool operator==(const Change&) const = default;
};

enum class SetResult {
    done,           // kept (and sent when in a session)
    bad_id,         // the mod id is not one (coop::protocol ModEntry::id's rule)
    bad_key,        // 1 to 32 of a-z 0-9 _
    too_long,       // more than kMaxModDataBytes
    too_many_keys,  // a 9th key of that mod (clear one first)
    too_many_mods,  // a 17th mod with values
};
const char* Words(SetResult result);

// The most changes kept for the mods to take; older ones are dropped past it (counted in Store::dropped).
inline constexpr std::size_t kMaxChanges = 512;

class Store {
public:
    // This game's own value; an empty value clears the key.
    SetResult Set(std::string_view mod_id, std::string_view key, std::span<const std::uint8_t> value);
    // A value: player 0 (or this game's own number) is this game's own. nullopt when none.
    std::optional<Bytes> Get(std::uint8_t player, std::string_view mod_id, std::string_view key) const;

    // A MOD_DATA the server relayed (another player's): kept, and a Change when it differs.
    void Received(const coop::protocol::ModData& data);
    // That player left: its values are forgotten (a Change each).
    void PlayerLeft(std::uint8_t player);
    // A new connection was welcomed as `number`: the server knows none of ours, so all are due again; the others'
    // values of before are forgotten (a Change each; the server sends what it keeps now).
    void Welcomed(std::uint8_t number);
    // Out of the session: the others' values are forgotten (a Change each); ours stay for the next session.
    void Left();

    // The next MOD_DATA to send at `now_ms`, at most one each kModDataSendGapMs: a key whose value is not the one the
    // server has (a cleared key first, so a new key never comes before the clear that makes room for it). nullopt
    // when nothing is due, the gap has not passed or this game is not in a session. MarkSent once it went out, with
    // the connection() read with Next: a send of a connection that has been replaced since (a reconnect between the
    // send and MarkSent) is not marked, so the new connection sends the value again.
    std::optional<coop::protocol::ModData> Next(std::uint64_t now_ms) const;
    void MarkSent(const coop::protocol::ModData& data, std::uint64_t now_ms, std::uint64_t connection);
    // Counts the connections (each Welcomed and Left).
    std::uint64_t connection() const { return connection_; }

    std::vector<Change> TakeChanges();
    // The players with values now, and this game's number (0: not in a session).
    std::uint8_t number() const { return number_; }
    std::uint64_t dropped() const { return dropped_; }

private:
    using Values = std::map<std::string, std::map<std::string, Bytes, std::less<>>, std::less<>>;
    void Note(std::uint8_t player, const std::string& mod_id, const std::string& key, const Bytes& value);
    void ForgetOthers();

    Values mine_;
    Values sent_;  // what the server has of ours on this connection
    std::map<std::uint8_t, Values> others_;
    std::vector<Change> changes_;
    std::uint8_t number_ = 0;
    std::optional<std::uint64_t> last_send_ms_;
    std::uint64_t connection_ = 0;
    std::uint64_t dropped_ = 0;
};

// ---- the game's glue (one Store for the game) ----

// Network thread, from the session's on_event (the network lock is held).
void OnEvent(const coop::net::Event& event, coop::net::Client& client);
// Present thread, once per frame: sends what is due. `session` null when not connected.
void Frame(coop::host::Session* session, std::uint64_t now_ms);

SetResult Set(std::string_view mod_id, std::string_view key, std::span<const std::uint8_t> value);
std::optional<Bytes> Get(std::uint8_t player, std::string_view mod_id, std::string_view key);
// The players in the session now (this game's own too), by number, with their names; empty when alone.
std::vector<std::pair<std::uint8_t, std::string>> Players();
std::uint8_t MyNumber();
std::vector<Change> TakeChanges();

}  // namespace CoopModData

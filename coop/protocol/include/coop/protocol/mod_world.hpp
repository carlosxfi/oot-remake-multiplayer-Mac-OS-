// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which world a game plays, from its MOD_LIST (docs/protocol.md "Mods", docs/mods.md "The randomizer"): a randomizer
// seed (OOTRR's mod folder randomizer-<seed word>) makes another world (other items in chests). The one check of a
// player's seed against the host's, so the games and the server cannot differ (row 281): a player whose seed is not
// the host's is held out of the host's world (world_held): its game sends none of the world (race_private's list) and
// the server takes none of it, until it plays the host's seed (its game builds it and starts again).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace coop::protocol {

// How a mod's hash was made, marked in its last three bytes ('S', 'Q', the way's number). Way 2 (0.11.13, row 274):
// only what decides the mod's world: its game files, with the files the kit rewrites on each PC read from the mod's own
// kept copies, and its size tables left out. Unmarked: way 1 (every file as it was on that PC). Hashes made two ways
// are not compared: the higher way is the newer kit.
inline constexpr std::uint8_t kHashWay = 2;
void mark_hash_way(ModHash& hash);
std::uint8_t hash_way(const ModHash& hash);

// A randomizer seed's mod id (the folder OOTRR builds, randomizer-<seed word>; its id is the folder's name in lower
// case), and its seed word in capitals as the chat says it ("randomizer-n2bteykm-keysanity" -> "N2BTEYKM-KEYSANITY").
bool is_seed_id(std::string_view id);
std::string seed_word_of(std::string_view id);

// The host's seed that a player's list lacks: its seed word; empty when the host plays no seed or the player has it.
std::string seed_missing(const ModList& host, const ModList& theirs);
// The host's seed that a player has with other files (both hashes known and made the same way): built by another
// OOTRR, another world; its seed word, else empty.
std::string seed_files_other(const ModList& host, const ModList& theirs);
// The host's seed that a player has hashed another way (one of the two kits is older): its seed word in `seed`, and
// > 0 when the player's kit is the newer one, < 0 when the player's is older; 0: not so (`seed` untouched).
int seed_kit_other(const ModList& host, const ModList& theirs, std::string* seed);
// A seed the player plays that the host's list lacks (the host plays the base game or another seed): its seed word.
std::string seed_extra(const ModList& host, const ModList& theirs);

// Whether a player's world is not the host's (row 281): what differs, first found in this order.
enum class WorldDiff : std::uint8_t {
  same = 0,
  missing = 1,      // seed_missing: the player has not got the host's seed (the base game, or not built yet)
  other_files = 2,  // seed_files_other: the same seed word built by another OOTRR
  extra = 3,        // seed_extra: the player plays a seed the host does not
};
struct World {
  WorldDiff diff = WorldDiff::same;
  std::string seed;  // the seed word it is about (the host's; for extra the player's)
};
// Hashed another way (seed_kit_other) is not a difference here: it can't be checked, and the games only say so.
World world_of(const ModList& host, const ModList& theirs);
inline bool world_held(const ModList& host, const ModList& theirs) {
  return world_of(host, theirs).diff != WorldDiff::same;
}

// The randomizer's mod id (hookshot/src/coop_rando.h kId).
inline constexpr const char* kRandoModId = "oot-remake-randomizer";
// The host's randomizer settings as a line names them: its preset ("keysanity"), or "settings string OOTRR1-..." cut;
// empty when `values` (the host's MOD_SETTINGS) have no randomizer setting.
std::string rando_settings_name(const std::vector<ModSetting>& values);
// Row 281: the room list's tag for a host playing a randomizer seed (server/relay_wire.hpp kRoomTag): "Randomizer
// SEED (settings name)", at most kMaxWorldTagBytes printable ASCII bytes (the settings name cut, anything else
// turned to '?'); empty when the host plays no seed (the base game: no tag). `values`: the host's MOD_SETTINGS
// values, nullptr when not known yet (the tag names the seed only).
inline constexpr std::size_t kMaxWorldTagBytes = 48;  // (= relay_wire.hpp kMaxRoomTagBytes)
std::string world_tag(const ModList& host, const std::vector<ModSetting>* values);

}  // namespace coop::protocol

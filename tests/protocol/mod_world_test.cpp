// SPDX-License-Identifier: GPL-3.0-or-later
// Row 281: the one check of a player's randomizer seed against the host's (coop/protocol/mod_world.hpp).
#include <doctest/doctest.h>

#include <string>

#include "coop/protocol/mod_world.hpp"

namespace proto = coop::protocol;

namespace {
proto::ModEntry entry(const char* id, std::uint8_t hash = 0, bool way2 = false) {
  proto::ModEntry e{id, "", {}};
  e.hash[0] = hash;
  if (way2) proto::mark_hash_way(e.hash);
  return e;
}
}  // namespace

TEST_CASE("mod world: a seed id and its word") {
  CHECK(proto::is_seed_id("randomizer-n2bteykm"));
  CHECK(proto::is_seed_id("randomizer-n2bteykm-keysanity"));
  CHECK_FALSE(proto::is_seed_id("randomizer-"));
  CHECK_FALSE(proto::is_seed_id("oot-remake-randomizer"));
  CHECK_FALSE(proto::is_seed_id("randomizer-N2BTEYKM"));
  CHECK(proto::seed_word_of("randomizer-n2bteykm-keysanity") == "N2BTEYKM-KEYSANITY");
}

TEST_CASE("mod world: what differs from the host's seed, in order, and the host's own list is the same world") {
  const proto::ModList host{1, {entry("hud", 7), entry("randomizer-abc", 5)}};
  CHECK(proto::world_of(host, host).diff == proto::WorldDiff::same);
  CHECK_FALSE(proto::world_held(host, host));
  // Other mods never hold a player (they are only told): a missing hud is the same world.
  CHECK_FALSE(proto::world_held(host, proto::ModList{2, {entry("randomizer-abc", 5)}}));
  // The base game (vanilla, or the seed not built yet).
  auto w = proto::world_of(host, proto::ModList{2, {entry("hud", 7)}});
  CHECK(w.diff == proto::WorldDiff::missing);
  CHECK(w.seed == "ABC");
  // Another seed: the host's is missing first.
  w = proto::world_of(host, proto::ModList{2, {entry("randomizer-xyz", 5)}});
  CHECK(w.diff == proto::WorldDiff::missing);
  CHECK(w.seed == "ABC");
  // The same seed word, other files (another OOTRR), both hashed the same way.
  w = proto::world_of(host, proto::ModList{2, {entry("randomizer-abc", 6)}});
  CHECK(w.diff == proto::WorldDiff::other_files);
  CHECK(w.seed == "ABC");
  // A hash not known yet is not compared.
  CHECK_FALSE(proto::world_held(host, proto::ModList{2, {entry("randomizer-abc", 0)}}));
  // Hashed another way (an older kit): can't be checked, not held (the games say so).
  CHECK_FALSE(proto::world_held(host, proto::ModList{2, {entry("randomizer-abc", 6, true)}}));
  std::string seed;
  CHECK(proto::seed_kit_other(host, proto::ModList{2, {entry("randomizer-abc", 6, true)}}, &seed) == 1);
  CHECK(seed == "ABC");
  // The host plays the base game, the player a seed.
  w = proto::world_of(proto::ModList{1, {entry("hud", 7)}}, proto::ModList{2, {entry("randomizer-xyz", 1)}});
  CHECK(w.diff == proto::WorldDiff::extra);
  CHECK(w.seed == "XYZ");
  // Neither plays a seed.
  CHECK_FALSE(proto::world_held(proto::ModList{1, {}}, proto::ModList{2, {entry("hud", 7)}}));
}

TEST_CASE("mod world: the hash way mark") {
  proto::ModHash h{};
  CHECK(proto::hash_way(h) == 1);
  proto::mark_hash_way(h);
  CHECK(proto::hash_way(h) == proto::kHashWay);
}

#include <doctest/doctest.h>

#include <bit>
#include <set>

#include "coop/host/game_items.hpp"
#include "coop/host/key_item_tally.hpp"

using namespace coop::host;

TEST_CASE("tally totals count the item table's groups") {
  KeyItemTally t = key_item_tally([](std::uint32_t) { return false; });
  CHECK(t.at(TallyGroup::songs).total == 13);
  CHECK(t.at(TallyGroup::medallions).total == 6);
  CHECK(t.at(TallyGroup::stones).total == 3);
  CHECK(t.at(TallyGroup::dungeon_items).total > 0);
  for (auto& c : t.groups) CHECK(c.owned == 0);
}

TEST_CASE("tally counts owned flags and nothing else") {
  std::set<std::uint32_t> held;
  for (const ItemFlag& i : item_flags()) {
    if (i.name == "IsGet.Item_Melody_TimeSong" || i.name == "IsGet.Item_Medal_Fire" ||
        i.name == "IsGet.Item_Bomb")
      held.insert(i.hash);
  }
  KeyItemTally t = key_item_tally([&](std::uint32_t h) { return held.count(h) != 0; });
  CHECK(t.at(TallyGroup::songs).owned == 1);
  CHECK(t.at(TallyGroup::medallions).owned == 1);
  CHECK(t.at(TallyGroup::stones).owned == 0);
}

TEST_CASE("tally with no lookup is all zero owned") {
  KeyItemTally t = key_item_tally({});
  CHECK(t.at(TallyGroup::songs).owned == 0);
}

TEST_CASE("tally mask: one bit per held flag, in the group's table order, owned = popcount") {
  std::set<std::uint32_t> held;
  for (const ItemFlag& i : item_flags()) {
    if (i.name == "IsGet.Item_Melody_TimeSong" || i.name == "IsGet.Item_Melody_SunSong") held.insert(i.hash);
  }
  KeyItemTally t = key_item_tally([&](std::uint32_t h) { return held.count(h) != 0; });
  const auto& songs = t.at(TallyGroup::songs);
  CHECK(songs.owned == 2);
  CHECK(std::popcount(songs.mask) == 2);
  CHECK(songs.mask < (std::uint64_t{1} << songs.total));
  KeyItemTally none = key_item_tally({});
  CHECK(none.at(TallyGroup::songs).mask == 0);
  // the same flags held give the same mask (the digest compares games by it)
  CHECK(key_item_tally([&](std::uint32_t h) { return held.count(h) != 0; }).at(TallyGroup::songs).mask == songs.mask);
}

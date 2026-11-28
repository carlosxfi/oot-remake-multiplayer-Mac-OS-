#pragma once

// Row 223: a count of the key items a player holds, by group, from the shared
// item table (game_items.hpp). Pure: the caller says which boolean item flags
// the game holds (a lookup by flag hash), so this works solo and in a session.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace coop::host {

enum class TallyGroup : std::uint8_t { songs, medallions, stones, dungeon_items };

struct TallyCount {
  unsigned owned = 0;
  unsigned total = 0;
  // Which are held: bit i is the group's i-th flag in item_flags() order (by hash, the same in every game), so two
  // games' masks say WHICH item differs, not only how many (the sync digest, coop_digest.h).
  std::uint64_t mask = 0;
};

struct KeyItemTally {
  std::array<TallyCount, 4> groups{};
  const TallyCount& at(TallyGroup g) const { return groups[static_cast<std::size_t>(g)]; }
};

// The group a boolean item flag counts in, by name, or false for none.
bool tally_group_of(std::string_view name, TallyGroup& out);

// `has(hash)` is true when the game holds that boolean flag.
KeyItemTally key_item_tally(const std::function<bool(std::uint32_t)>& has);

// A group's label (English only).
std::string_view tally_group_label(TallyGroup g);

}  // namespace coop::host

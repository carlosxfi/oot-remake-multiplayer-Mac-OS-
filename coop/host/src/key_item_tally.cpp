#include "coop/host/key_item_tally.hpp"

#include "coop/host/game_items.hpp"
#include "coop/protocol/messages.hpp"

namespace coop::host {

namespace {
bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}
}  // namespace

bool tally_group_of(std::string_view name, TallyGroup& out) {
  if (starts_with(name, "IsGet.Item_Melody_")) {
    out = TallyGroup::songs;
    return true;
  }
  if (starts_with(name, "IsGet.Item_Medal_")) {
    out = TallyGroup::medallions;
    return true;
  }
  if (starts_with(name, "IsGet.Item_Stone_")) {
    out = TallyGroup::stones;
    return true;
  }
  if (starts_with(name, "DungeonInfo.") &&
      (ends_with(name, ".IsGetMap") || ends_with(name, ".IsGetCompass") || ends_with(name, ".IsGetBossKey"))) {
    out = TallyGroup::dungeon_items;
    return true;
  }
  return false;
}

KeyItemTally key_item_tally(const std::function<bool(std::uint32_t)>& has) {
  KeyItemTally t;
  for (const ItemFlag& item : item_flags()) {
    if (!item.on || item.kind != protocol::FlagKind::boolean) continue;
    TallyGroup g;
    if (!tally_group_of(item.name, g)) continue;
    TallyCount& c = t.groups[static_cast<std::size_t>(g)];
    ++c.total;
    if (has && has(item.hash)) ++c.owned;
  }
  return t;
}

std::string_view tally_group_label(TallyGroup g) {
  switch (g) {
    case TallyGroup::songs: return "Songs";
    case TallyGroup::medallions: return "Medallions";
    case TallyGroup::stones: return "Spiritual Stones";
    case TallyGroup::dungeon_items: return "Dungeon items";
  }
  return {};
}

}  // namespace coop::host

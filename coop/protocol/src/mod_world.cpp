// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop/protocol/mod_world.hpp"

#include <tuple>

namespace coop::protocol {

namespace {
constexpr std::size_t kWayAt = std::tuple_size_v<ModHash> - 3;  // 'S', 'Q', the way
constexpr std::string_view kSeedPrefix = "randomizer-";

bool has_id(const ModList& list, const std::string& id) {
  for (const auto& e : list.mods) {
    if (e.id == id) return true;
  }
  return false;
}
}  // namespace

void mark_hash_way(ModHash& hash) {
  hash[kWayAt] = 'S';
  hash[kWayAt + 1] = 'Q';
  hash[kWayAt + 2] = kHashWay;
}

std::uint8_t hash_way(const ModHash& hash) {
  if (hash[kWayAt] != 'S' || hash[kWayAt + 1] != 'Q' || hash[kWayAt + 2] < 2) return 1;
  return hash[kWayAt + 2];
}

bool is_seed_id(std::string_view id) {
  if (id.size() <= kSeedPrefix.size() || id.substr(0, kSeedPrefix.size()) != kSeedPrefix) return false;
  for (const char c : id.substr(kSeedPrefix.size())) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

std::string seed_word_of(std::string_view id) {
  std::string word(id.size() > kSeedPrefix.size() ? id.substr(kSeedPrefix.size()) : std::string_view());
  for (char& c : word) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return word;
}

std::string seed_missing(const ModList& host, const ModList& theirs) {
  for (const auto& h : host.mods) {
    if (is_seed_id(h.id) && !has_id(theirs, h.id)) return seed_word_of(h.id);
  }
  return {};
}

std::string seed_files_other(const ModList& host, const ModList& theirs) {
  for (const auto& h : host.mods) {
    if (!is_seed_id(h.id) || h.hash == ModHash{}) continue;
    for (const auto& t : theirs.mods) {
      if (t.id == h.id && t.hash != ModHash{} && t.hash != h.hash && hash_way(t.hash) == hash_way(h.hash)) {
        return seed_word_of(h.id);
      }
    }
  }
  return {};
}

int seed_kit_other(const ModList& host, const ModList& theirs, std::string* seed) {
  for (const auto& h : host.mods) {
    if (!is_seed_id(h.id) || h.hash == ModHash{}) continue;
    for (const auto& t : theirs.mods) {
      if (t.id != h.id || t.hash == ModHash{} || hash_way(t.hash) == hash_way(h.hash)) continue;
      if (seed != nullptr) *seed = seed_word_of(h.id);
      return hash_way(t.hash) > hash_way(h.hash) ? 1 : -1;
    }
  }
  return 0;
}

std::string seed_extra(const ModList& host, const ModList& theirs) {
  for (const auto& t : theirs.mods) {
    if (is_seed_id(t.id) && !has_id(host, t.id)) return seed_word_of(t.id);
  }
  return {};
}

World world_of(const ModList& host, const ModList& theirs) {
  if (std::string s = seed_missing(host, theirs); !s.empty()) return {WorldDiff::missing, std::move(s)};
  if (std::string s = seed_files_other(host, theirs); !s.empty()) return {WorldDiff::other_files, std::move(s)};
  if (std::string s = seed_extra(host, theirs); !s.empty()) return {WorldDiff::extra, std::move(s)};
  return {};
}

}  // namespace coop::protocol

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace coop_nxnative {

struct MainModule {
  std::uint64_t base = 0;
  std::uint64_t size = 0;
};

inline bool parse_main_module(const char* line, MainModule& module) {
  const char* marker = std::strstr(line, "main: base ");
  unsigned long long base = 0, size = 0;
  if (!marker || std::sscanf(marker, "main: base %llx size %llx", &base, &size) != 2)
    return false;
  if (!base || size < 8 || base > std::numeric_limits<std::uint64_t>::max() - size)
    return false;
  module = {base, size};
  return true;
}

enum class ModuleStatus { valid, bad_range, unreadable_header, bad_offset, unreadable_mod0, bad_magic };

inline const char* status_text(ModuleStatus status) {
  switch (status) {
    case ModuleStatus::valid: return "valid MOD0";
    case ModuleStatus::bad_range: return "invalid module range";
    case ModuleStatus::unreadable_header: return "module header is not readable in this process";
    case ModuleStatus::bad_offset: return "MOD0 offset is outside the module";
    case ModuleStatus::unreadable_mod0: return "MOD0 is not readable in this process";
    case ModuleStatus::bad_magic: return "MOD0 signature does not match";
  }
  return "unknown";
}

template <typename Reader>
ModuleStatus inspect_module(MainModule module, Reader read, std::uint32_t& offset) {
  offset = 0;
  if (!module.base || module.size < 8 ||
      module.base > std::numeric_limits<std::uint64_t>::max() - module.size)
    return ModuleStatus::bad_range;
  if (!read(module.base + 4, offset)) return ModuleStatus::unreadable_header;
  // MOD0 may be in data, tens of megabytes after the original text starts.
  // Bound it by the loader's image size, not a fixed 16 MB guess.
  if (offset < 8 || offset > module.size - 4) return ModuleStatus::bad_offset;
  char magic[4]{};
  if (!read(module.base + offset, magic)) return ModuleStatus::unreadable_mod0;
  return std::memcmp(magic, "MOD0", sizeof(magic)) == 0
             ? ModuleStatus::valid : ModuleStatus::bad_magic;
}

}  // namespace coop_nxnative

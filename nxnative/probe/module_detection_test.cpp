#include "module_detection.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <vector>

using namespace coop_nxnative;

int main() {
  MainModule module;
  // Synthetic addresses and sizes; no game bytes are used by these tests.
  assert(parse_main_module("[dm] main: base 0x200000000 size 0x6000000\n", module));
  assert(module.base == 0x200000000 && module.size == 0x6000000);
  assert(!parse_main_module("[dm] sdk: base 0x200000000 size 0x6000000", module));
  assert(!parse_main_module("main: base 0x200000000", module));
  assert(!parse_main_module("main: base 0 size 0x1000", module));
  assert(!parse_main_module("main: base 0xfffffffffffffff0 size 0x1000", module));

  std::map<std::uint64_t, std::vector<unsigned char>> memory;
  const auto store = [&](std::uint64_t address, const auto& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    memory[address] = std::vector<unsigned char>(bytes, bytes + sizeof(value));
  };
  const auto read = [&](std::uint64_t address, auto& value) {
    const auto it = memory.find(address);
    if (it == memory.end() || it->second.size() != sizeof(value)) return false;
    std::memcpy(&value, it->second.data(), sizeof(value));
    return true;
  };
  std::uint32_t observed = 0;
  assert(inspect_module(module, read, observed) == ModuleStatus::unreadable_header);

  const std::uint32_t large_offset = 0x3000000;
  const char magic[4] = {'M', 'O', 'D', '0'};
  store(module.base + 4, large_offset);
  assert(inspect_module(module, read, observed) == ModuleStatus::unreadable_mod0);
  store(module.base + large_offset, magic);
  // Regression: a readable MOD0 beyond 16 MB must be accepted.
  assert(inspect_module(module, read, observed) == ModuleStatus::valid);
  assert(observed == large_offset);

  const char wrong[4] = {'N', 'O', 'P', 'E'};
  store(module.base + large_offset, wrong);
  assert(inspect_module(module, read, observed) == ModuleStatus::bad_magic);
  const auto outside = static_cast<std::uint32_t>(module.size - 3);
  store(module.base + 4, outside);
  assert(inspect_module(module, read, observed) == ModuleStatus::bad_offset);
  const auto boundary = static_cast<std::uint32_t>(module.size - 4);
  store(module.base + 4, boundary);
  store(module.base + boundary, magic);
  assert(inspect_module(module, read, observed) == ModuleStatus::valid);
  const std::uint32_t zero = 0;
  store(module.base + 4, zero);
  assert(inspect_module(module, read, observed) == ModuleStatus::bad_offset);
  assert(inspect_module({0, 8}, read, observed) == ModuleStatus::bad_range);
  std::cout << "Module detection checks passed.\n";
}

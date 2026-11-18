#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "coop/host/game_memory.hpp"

namespace coop_nxnative {

// The same-process copy is supplied by the Mac host. Tests supply a sparse
// synthetic address space instead. No direct dereference or write API.
class LocalMemory final : public coop::host::GameMemory {
 public:
  using Copy = bool (*)(std::uint64_t, std::span<std::uint8_t>, void*);
  static constexpr std::size_t kMaxReadBytes = 1024 * 1024;

  LocalMemory(std::uint64_t base, Copy copy, void* context = nullptr)
      : base_(base), copy_(copy), context_(context) {}

  std::uint64_t main_base() const override { return base_; }
  void set_main_base(std::uint64_t base) { base_ = base; }

  bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
    if (out.empty()) return true;
    if (!copy_ || address == 0 || out.size() > kMaxReadBytes ||
        address > std::numeric_limits<std::uint64_t>::max() - out.size())
      return false;
    return copy_(address, out, context_);
  }

 private:
  std::uint64_t base_;
  Copy copy_;
  void* context_;
};

}  // namespace coop_nxnative

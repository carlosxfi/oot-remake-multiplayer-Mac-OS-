#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "module_detection.hpp"

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {

constexpr std::uint64_t kGameUiModulePtr = 0x4e7cfa8ULL;
constexpr std::uint64_t kGameUiPoseFlags = 0x58aULL;
constexpr std::uint64_t kGameUiPosition = 0x590ULL;
constexpr std::uint64_t kGameUiIsChild = 0x5c0ULL;
constexpr std::uint16_t kPoseValid = 1;

std::atomic<bool> running{true};

std::uint64_t configured_main_base() {
  const char* value = std::getenv("COOP_NXNATIVE_MAIN_BASE");
  if (value == nullptr || *value == '\0') return 0;
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(value, &end, 0);
  if (errno != 0 || end == value || *end != '\0') {
    std::fprintf(stderr, "[coop-nxnative] bad COOP_NXNATIVE_MAIN_BASE=%s; discovering it\n",
                 value);
    return 0;
  }
  return parsed;
}

template <typename T>
bool read_at(std::uint64_t address, T& value) {
#ifdef __APPLE__
  mach_vm_size_t copied = 0;
  const kern_return_t result = mach_vm_read_overwrite(
      mach_task_self(), static_cast<mach_vm_address_t>(address), sizeof(value),
      reinterpret_cast<mach_vm_address_t>(&value), &copied);
  return result == KERN_SUCCESS && copied == sizeof(value);
#else
  (void)address;
  (void)value;
  return false;
#endif
}

[[maybe_unused]] bool valid_main_base(std::uint64_t base, std::uint64_t size) {
  std::uint32_t mod0 = 0;
  return coop_nxnative::inspect_module({base, size},
      [](std::uint64_t address, auto& value) { return read_at(address, value); }, mod0)
      == coop_nxnative::ModuleStatus::valid;
}

std::uint64_t discover_main_base_from_symbols() {
#ifdef __APPLE__
  // Some dmhost builds export the loader table. Only module_t's stable prefix
  // is needed, keeping this probe separate from nxnative's private headers.
  struct ModulePrefix {
    char name[16];
    std::uint8_t* base;
    std::size_t size;
  };
  void* symbol = dlsym(RTLD_DEFAULT, "g_mod");
  if (symbol == nullptr) return 0;
  std::uint64_t modules = 0;
  ModulePrefix module{};
  if (!read_at(reinterpret_cast<std::uint64_t>(symbol), modules) || modules == 0 ||
      !read_at(modules, module) || std::strncmp(module.name, "main", sizeof(module.name)) != 0)
    return 0;
  const auto base = reinterpret_cast<std::uint64_t>(module.base);
  return valid_main_base(base, module.size) ? base : 0;
#else
  return 0;
#endif
}

std::uint64_t discover_main_base_from_log() {
  char path[1024]{};
  if (const char* configured = std::getenv("COOP_NXNATIVE_LOG"); configured && *configured)
    std::snprintf(path, sizeof(path), "%s", configured);
  else if (const char* home = std::getenv("HOME"); home && *home)
    std::snprintf(path, sizeof(path), "%s/Library/Logs/nxnative/0400486024DEC000.log", home);
  else
    return 0;

  FILE* file = std::fopen(path, "r");
  static bool reported_open_error = false;
  if (file == nullptr) {
    if (!reported_open_error)
      std::fprintf(stderr, "[coop-nxnative] cannot open module log: %s (%s)\n", path, std::strerror(errno));
    reported_open_error = true;
    return 0;
  }
  reported_open_error = false;
  std::uint64_t found = 0;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    coop_nxnative::MainModule module;
    if (!coop_nxnative::parse_main_module(line, module)) continue;
    std::uint32_t mod0 = 0;
    const auto status = coop_nxnative::inspect_module(module,
        [](std::uint64_t address, auto& value) { return read_at(address, value); }, mod0);
    if (status == coop_nxnative::ModuleStatus::valid) {
      found = module.base;
    } else {
      static std::uint64_t last_base = 0;
      static auto last_status = coop_nxnative::ModuleStatus::valid;
      if (module.base != last_base || status != last_status)
        std::fprintf(stderr, "[coop-nxnative] log main 0x%llx size 0x%llx MOD0 offset 0x%x: %s\n",
            static_cast<unsigned long long>(module.base), static_cast<unsigned long long>(module.size),
            static_cast<unsigned>(mod0), coop_nxnative::status_text(status));
      last_base = module.base;
      last_status = status;
    }
  }
  std::fclose(file);
  return found;
}

std::uint64_t discover_main_base() {
  const std::uint64_t symbol_base = discover_main_base_from_symbols();
  return symbol_base != 0 ? symbol_base : discover_main_base_from_log();
}

void probe() {
  std::uint64_t base = configured_main_base();
  std::fprintf(stderr, "[coop-nxnative] position probe loaded; %s\n",
               base ? "using the configured main base" : "waiting for nxnative's main module");

  bool announced_wait = false;
  bool was_valid = false;
  while (running.load(std::memory_order_relaxed)) {
    if (base == 0) {
      base = discover_main_base();
      if (base != 0)
        std::fprintf(stderr, "[coop-nxnative] discovered main base 0x%llx\n",
                     static_cast<unsigned long long>(base));
      else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
    }
    std::uint64_t game_ui = 0;
    std::uint16_t flags = 0;
    float position[3]{};
    std::uint8_t child = 0;
    const bool readable = read_at(base + kGameUiModulePtr, game_ui) && game_ui != 0 &&
                          read_at(game_ui + kGameUiPoseFlags, flags);
    const bool valid = readable && (flags & kPoseValid) != 0 &&
                       read_at(game_ui + kGameUiPosition, position) &&
                       read_at(game_ui + kGameUiIsChild, child);

    if (valid) {
      std::fprintf(stderr, "[coop-nxnative] Link %s position %.3f %.3f %.3f\n",
                   child ? "child" : "adult", position[0], position[1], position[2]);
      announced_wait = false;
    } else if (!announced_wait || was_valid) {
      std::fprintf(stderr, "[coop-nxnative] waiting for a valid player state\n");
      announced_wait = true;
    }
    was_valid = valid;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace

__attribute__((constructor)) static void coop_nxnative_start() {
  std::thread(probe).detach();
}

__attribute__((destructor)) static void coop_nxnative_stop() {
  running.store(false, std::memory_order_relaxed);
}

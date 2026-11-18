#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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

std::uint64_t discover_main_base() {
#ifdef __APPLE__
  // nxnative's loader exposes g_mod. The first profile module is main. Only
  // the stable prefix of module_t is needed here, keeping this probe separate
  // from nxnative's private headers.
  struct ModulePrefix {
    char name[16];
    std::uint8_t* base;
    std::size_t size;
  };
  void* symbol = dlsym(RTLD_DEFAULT, "g_mod");
  if (symbol == nullptr) return 0;
  auto* modules = *static_cast<ModulePrefix**>(symbol);
  if (modules == nullptr || std::strncmp(modules[0].name, "main", sizeof(modules[0].name)) != 0)
    return 0;
  return reinterpret_cast<std::uint64_t>(modules[0].base);
#else
  return 0;
#endif
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

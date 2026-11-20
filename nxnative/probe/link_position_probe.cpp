#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include "calc_bridge.hpp"
#include "module_detection.hpp"
#include "local_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/sampler.hpp"
#ifdef COOP_NXNATIVE_NETWORK
#include "loopback.hpp"
#include "remote_peer.hpp"
#endif

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> running{true};
std::atomic<std::uint64_t> calc_frames{0};

template <typename T>
bool read_at(std::uint64_t address, T& value);

extern "C" __attribute__((visibility("default"))) void coop_nxnative_calc_frame() {
  calc_frames.fetch_add(1, std::memory_order_relaxed);
}

#ifdef __APPLE__
bool write_code(std::uint64_t address, std::span<const std::uint8_t> bytes) {
  const auto page_size = static_cast<std::uint64_t>(getpagesize());
  const auto page = address & ~(page_size - 1);
  const auto end = (address + bytes.size() + page_size - 1) & ~(page_size - 1);
  mach_vm_address_t alias = 0;
  vm_prot_t current = 0, maximum = 0;
  const auto task = mach_task_self();
  kern_return_t result = mach_vm_remap(task, &alias, end - page, 0,
      VM_FLAGS_ANYWHERE, task, page, FALSE, &current, &maximum, VM_INHERIT_NONE);
  if (result != KERN_SUCCESS) return false;
  result = mach_vm_protect(task, alias, end - page, FALSE, VM_PROT_READ | VM_PROT_WRITE);
  if (result == KERN_SUCCESS) {
    std::memcpy(reinterpret_cast<void*>(alias + address - page), bytes.data(), bytes.size());
    sys_icache_invalidate(reinterpret_cast<void*>(address), bytes.size());
  }
  mach_vm_deallocate(task, alias, end - page);
  return result == KERN_SUCCESS;
}

bool install_calc_bridge(std::uint64_t base) {
  std::uint32_t original = 0;
  if (!read_at(base + coop_nxnative::kCalcHookOffset, original) ||
      original != coop_nxnative::kOriginalCalcCall) {
    std::fprintf(stderr, "[coop-nxnative] calc bridge refused: game hook bytes do not match build 1.1.1\n");
    return false;
  }
  const auto cave = coop_nxnative::make_calc_cave(
      reinterpret_cast<std::uint64_t>(&coop_nxnative_calc_frame));
  const auto hook = coop_nxnative::arm64_branch(
      coop_nxnative::kCalcHookOffset, coop_nxnative::kCalcCaveOffset, true);
  if (!hook || !write_code(base + coop_nxnative::kCalcCaveOffset, cave)) {
    std::fprintf(stderr, "[coop-nxnative] calc bridge refused: could not install its code cave\n");
    return false;
  }
  const std::span<const std::uint8_t> hook_bytes(
      reinterpret_cast<const std::uint8_t*>(&*hook), sizeof(*hook));
  if (!write_code(base + coop_nxnative::kCalcHookOffset, hook_bytes)) {
    std::fprintf(stderr, "[coop-nxnative] calc bridge refused: could not install its frame hook\n");
    return false;
  }
  std::fprintf(stderr, "[coop-nxnative] calc-thread bridge installed (no game writes or guest calls yet)\n");
  return true;
}
#endif

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

bool copy_local(std::uint64_t address, std::span<std::uint8_t> out, void*) {
#ifdef __APPLE__
  mach_vm_size_t copied = 0;
  const kern_return_t result = mach_vm_read_overwrite(
      mach_task_self(), static_cast<mach_vm_address_t>(address), out.size(),
      reinterpret_cast<mach_vm_address_t>(out.data()), &copied);
  return result == KERN_SUCCESS && copied == out.size();
#else
  (void)address;
  (void)out;
  return false;
#endif
}

template <typename T>
bool read_at(std::uint64_t address, T& value) {
  coop_nxnative::LocalMemory memory(0, copy_local);
  return memory.read(address, {reinterpret_cast<std::uint8_t*>(&value), sizeof(value)});
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
  coop_nxnative::LocalMemory memory(base, copy_local);
  coop::host::PlayerReader reader(memory);
  coop::host::Sampler sampler;
  std::uint64_t packets = 0, bytes = 0, next_report = 0;
#ifdef __APPLE__
  bool calc_bridge_installed = false;
  std::uint64_t next_calc_attempt = 0, next_calc_report = 0;
#endif
  std::fprintf(stderr, "[coop-nxnative] shared local sampler ready (read-only)\n");
#ifdef COOP_NXNATIVE_NETWORK
  coop_nxnative::Loopback loopback;
  const char* enable_loopback = std::getenv("COOP_NXNATIVE_LOOPBACK");
  const bool want_loopback = enable_loopback && std::strcmp(enable_loopback, "1") == 0;
  const char* remote_host = std::getenv("COOP_NXNATIVE_HOST");
  const bool want_remote = remote_host && *remote_host;
  const bool network_on = !want_remote && want_loopback && loopback.start();
  std::unique_ptr<coop_nxnative::RemotePeer> remote;
  std::uint64_t next_remote_report = 0;
  if (want_remote && want_loopback) {
    std::fprintf(stderr, "[coop-nxnative] choose either LAN joining or loopback, not both\n");
  } else if (want_remote) {
    const auto env = [](const char* key, const char* fallback) {
      const char* value = std::getenv(key);
      return value && *value ? value : fallback;
    };
    auto config = coop_nxnative::remote_config(remote_host, env("COOP_NXNATIVE_PORT", "27510"),
        env("COOP_NXNATIVE_NICKNAME", "MacLink"), env("COOP_NXNATIVE_PASSWORD", ""));
    if (config) {
      remote = std::make_unique<coop_nxnative::RemotePeer>(std::move(*config));
      if (remote->start())
        std::fprintf(stderr, "[coop-nxnative] LAN client started (player state only)\n");
      else {
        std::fprintf(stderr, "[coop-nxnative] LAN client could not start\n");
        remote.reset();
      }
    } else {
      std::fprintf(stderr, "[coop-nxnative] invalid LAN settings: need IPv4, port, ASCII name and password\n");
    }
  } else if (want_loopback) {
    std::fprintf(stderr, "[coop-nxnative] encrypted loopback check %s\n", network_on ? "started" : "not started");
  }
#endif
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
    memory.set_main_base(base);
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#ifdef __APPLE__
    if (!calc_bridge_installed && now >= next_calc_attempt) {
      calc_bridge_installed = install_calc_bridge(base);
      next_calc_attempt = now + 5'000'000;
    }
    if (calc_bridge_installed && now >= next_calc_report) {
      std::fprintf(stderr, "[coop-nxnative] calc-thread bridge frames=%llu\n",
          static_cast<unsigned long long>(calc_frames.load(std::memory_order_relaxed)));
      next_calc_report = now + 1'000'000;
    }
#endif
    // A valid GameUI copy may still hold old coordinates. Prefer Link's actor
    // for the Mac probe and use GameUI when the actor position is unavailable.
    const auto reading = reader.read(coop::host::PlayerReader::PositionOrder::actor_first);
#ifdef COOP_NXNATIVE_NETWORK
    if (network_on) loopback.tick(reading);
    if (remote) remote->tick(reading);
#endif
    // The diagnostic sampler is independent of the LAN client's server clock.
    const auto outgoing = sampler.frame(reading, now);
    packets += outgoing.size();
    for (const auto& packet : outgoing) bytes += packet.bytes.size();

#ifdef COOP_NXNATIVE_NETWORK
    if (remote && now >= next_remote_report) {
      const auto stats = remote->stats();
      const auto state = coop::net::to_string(stats.state);
      std::fprintf(stderr, "[coop-nxnative] LAN state=%.*s self=%u peers=%zu sent=%llu received=%llu failures=%llu %s\n",
          static_cast<int>(state.size()), state.data(), static_cast<unsigned>(stats.self), stats.peers,
          static_cast<unsigned long long>(stats.sent), static_cast<unsigned long long>(stats.received_samples),
          static_cast<unsigned long long>(stats.send_failures), stats.detail.c_str());
      if (stats.last_received && stats.last_received->position) {
        const auto& p = *stats.last_received->position;
        std::fprintf(stderr, "[coop-nxnative] remote player %u position %.3f %.3f %.3f scene=%s\n",
            static_cast<unsigned>(stats.last_received->player_number), p[0], p[1], p[2],
            stats.last_received->scene ? stats.last_received->scene->c_str() : "?");
      }
      next_remote_report = now + 1'000'000;
    }
#endif
    if (reading) {
      if (!was_valid || now >= next_report) {
        const char* age = !reading->age ? "unknown" :
            (*reading->age == coop::protocol::Age::child ? "child" : "adult");
        std::fprintf(stderr, "[coop-nxnative] Link %s position %.3f %.3f %.3f scene=%s "
            "rotation=%s animation=%s equipment=%s samples=%u packets=%llu bytes=%llu\n",
            age, reading->position[0], reading->position[1], reading->position[2],
            reading->scene ? reading->scene->c_str() : "?", reading->rotation ? "ok" : "?",
            reading->animation ? reading->animation->name.c_str() : "?",
            reading->equipment ? "ok" : "?", static_cast<unsigned>(sampler.next_sequence()),
            static_cast<unsigned long long>(packets), static_cast<unsigned long long>(bytes));
        next_report = now + 1'000'000;
#ifdef COOP_NXNATIVE_NETWORK
        if (network_on) {
          const auto stats = loopback.stats();
          std::fprintf(stderr, "[coop-nxnative] loopback game=%s observer=%s encrypted=%zu "
              "sent=%llu received=%llu failures=%llu\n",
              stats.game_connected ? "connected" : "waiting", stats.observer_connected ? "connected" : "waiting",
              stats.encrypted_connections, static_cast<unsigned long long>(stats.sent),
              static_cast<unsigned long long>(stats.received_samples), static_cast<unsigned long long>(stats.send_failures));
          if (stats.last_received && stats.last_received->position) {
            const auto& p = *stats.last_received->position;
            std::fprintf(stderr, "[coop-nxnative] observer received Link position %.3f %.3f %.3f scene=%s\n",
                p[0], p[1], p[2], stats.last_received->scene ? stats.last_received->scene->c_str() : "?");
          }
        }
#endif
      }
      announced_wait = false;
    } else if (!announced_wait || was_valid) {
      std::fprintf(stderr, "[coop-nxnative] waiting for a valid player state\n");
      announced_wait = true;
    }
    was_valid = reading.has_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

}  // namespace

__attribute__((constructor)) static void coop_nxnative_start() {
#ifdef __APPLE__
  // The launcher spawns dmhost and redirects stderr into the nxnative log.
  // The launcher has no guest memory. Its inherited DYLD_INSERT_LIBRARIES
  // loads us again in the child, which is the process we need to sample.
  char path[4096]{};
  std::uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) != 0) return;
  const char* name = std::strrchr(path, '/');
  if (std::strcmp(name ? name + 1 : path, "dmhost") != 0) return;
#endif
  std::thread(probe).detach();
}

__attribute__((destructor)) static void coop_nxnative_stop() {
  running.store(false, std::memory_order_relaxed);
}


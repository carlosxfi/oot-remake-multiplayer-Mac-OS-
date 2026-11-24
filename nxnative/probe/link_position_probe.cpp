#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include "calc_bridge.hpp"
#include "puppet_guard.hpp"
#include "module_detection.hpp"
#include "local_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/sampler.hpp"
#ifdef COOP_NXNATIVE_NETWORK
#include <algorithm>
#include <mutex>
#include "loopback.hpp"
#include "remote_peer.hpp"
#ifdef __APPLE__
#include "game_call.hpp"
#include "world_function_bridge.hpp"
#include "../../hookshot/src/hos_coop_bosses.h"
#include "../../hookshot/src/hos_coop_sound.h"
#include "../../hookshot/src/coop_pause_tab.h"
#include "../../hookshot/src/hos_coop_boss_orders.h"
#include "../../hookshot/src/hos_coop_draw.h"
#include "../../hookshot/src/hos_coop_save_load.h"
#include "../../hookshot/src/hos_coop_world.h"
#include "multiplayer_menu.hpp"
#include "../../lichii/hook/coop_colors.h"
#include <filesystem>
#endif
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
#ifdef COOP_NXNATIVE_NETWORK
std::mutex remote_targets_mutex;
coop_nxnative::RemoteTargets remote_targets{};
std::atomic<unsigned> calc_remote_targets{0};
#ifdef __APPLE__
enum class GameCallCheck { pending, passed, failed };
std::atomic<GameCallCheck> game_call_check{GameCallCheck::pending};
std::atomic<std::uint64_t> spawn_test_base{0};
std::atomic<hos_coop_frame_fn> shared_frame{nullptr};
HosCoopApi shared_api{};
thread_local bool in_shared_frame = false;
alignas(16) thread_local std::array<std::uint8_t, 256> shared_scratch{};
alignas(16) thread_local std::array<std::uint8_t, 128> shared_indirect_result{};

using UpdateScreen = void (*)(std::uint64_t);
using ScreenEvent = void (*)(void*, std::uint64_t);
using FilterPad = std::uint64_t (*)(std::uint64_t, std::int32_t*);
using RegisterPad = int (*)(FilterPad);
std::atomic<UpdateScreen> original_screen_update{nullptr};
std::atomic<bool> native_menu_active{false};
std::atomic<std::uint64_t> native_pad_buttons{0};
ScreenEvent native_screen_event = nullptr;
FilterPad native_pad_filter = nullptr;
std::uint64_t remote_scene_generation = 0;
bool remote_local_scene_valid = false;
std::array<char, 64> remote_actor_name{};
void tick_spawn_test(std::uint64_t base, const coop_nxnative::RemoteTarget& target,
                     std::uint64_t generation, bool local_scene_valid, const char* actor_name);
#endif
#endif

template <typename T>
bool read_at(std::uint64_t address, T& value);

extern "C" __attribute__((visibility("default"))) void coop_nxnative_calc_frame() {
  calc_frames.fetch_add(1, std::memory_order_relaxed);
#if defined(__APPLE__) && defined(COOP_NXNATIVE_NETWORK)
  if (const auto frame = shared_frame.load(std::memory_order_acquire)) {
    if (!in_shared_frame) {
      in_shared_frame = true;
      frame(&shared_api, HOS_COOP_SITE_FRAME);
      in_shared_frame = false;
    }
    return;
  }
#endif
#ifdef COOP_NXNATIVE_NETWORK
  // Networking publishes on the sampler thread. Never wait for it in the game.
  unsigned target_count = 0;
#ifdef __APPLE__
  coop_nxnative::RemoteTarget target;
  std::uint64_t generation = 0;
  bool local_scene_valid = false;
  std::array<char, 64> actor_name{};
#endif
  {
    const std::unique_lock lock(remote_targets_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    target_count = static_cast<unsigned>(std::count_if(remote_targets.begin(), remote_targets.end(),
        [](const auto& entry) { return entry.player != 0; }));
#ifdef __APPLE__
    const auto first = std::find_if(remote_targets.begin(), remote_targets.end(),
        [](const auto& entry) { return entry.player != 0; });
    if (first != remote_targets.end()) target = *first;
    generation = remote_scene_generation;
    local_scene_valid = remote_local_scene_valid;
    actor_name = remote_actor_name;
#endif
  }
  calc_remote_targets.store(target_count, std::memory_order_relaxed);
#ifdef __APPLE__
  if (target_count > 0 && game_call_check.load(std::memory_order_relaxed) == GameCallCheck::pending) {
    game_call_check.store(coop_nxnative::check_game_call() ? GameCallCheck::passed : GameCallCheck::failed,
                          std::memory_order_relaxed);
  }
  const auto base = spawn_test_base.load(std::memory_order_acquire);
  if (base && game_call_check.load(std::memory_order_relaxed) == GameCallCheck::passed)
    tick_spawn_test(base, target, generation, local_scene_valid, actor_name.data());
#endif
#endif
}

#ifdef __APPLE__
template <class Write>
bool with_writable_alias(std::uint64_t address, std::size_t size, Write write) {
  const auto page_size = static_cast<std::uint64_t>(getpagesize());
  const auto page = address & ~(page_size - 1);
  const auto end = (address + size + page_size - 1) & ~(page_size - 1);
  mach_vm_address_t alias = 0;
  vm_prot_t current = 0, maximum = 0;
  const auto task = mach_task_self();
  kern_return_t result = mach_vm_remap(task, &alias, end - page, 0,
      VM_FLAGS_ANYWHERE, task, page, FALSE, &current, &maximum, VM_INHERIT_NONE);
  if (result != KERN_SUCCESS) return false;
  result = mach_vm_protect(task, alias, end - page, FALSE, VM_PROT_READ | VM_PROT_WRITE);
  if (result == KERN_SUCCESS) {
    write(reinterpret_cast<void*>(alias + address - page));
  }
  mach_vm_deallocate(task, alias, end - page);
  return result == KERN_SUCCESS;
}

bool write_code(std::uint64_t address, std::span<const std::uint8_t> bytes) {
  return with_writable_alias(address, bytes.size(), [&](void* alias) {
    std::memcpy(alias, bytes.data(), bytes.size());
    sys_icache_invalidate(reinterpret_cast<void*>(address), bytes.size());
  });
}

bool install_puppet_guard(std::uint64_t base) {
  using namespace coop_nxnative;
  std::array<std::uint32_t, kPuppetGuardOriginal.size()> original{};
  if (!read_at(base + kPuppetGuardSite - 4, original) || original != kPuppetGuardOriginal) {
    std::fprintf(stderr, "[coop-nxnative] puppet guard refused: instruction mismatch\n");
    return false;
  }

  const auto cave = make_puppet_guard();
  const auto hook = arm64_branch(kPuppetGuardSite, kPuppetGuardCave, false).value();
  const auto code = [](const auto& value) {
    return std::span(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
  };
  if (!write_code(base + kPuppetGuardCave, code(cave)) ||
      !write_code(base + kPuppetGuardSite, code(hook))) {
    std::fprintf(stderr, "[coop-nxnative] puppet guard refused: could not install patch\n");
    return false;
  }
  std::fprintf(stderr, "[coop-nxnative] puppet guard installed (component chain)\n");
  return true;
}

bool install_bow_guard(std::uint64_t base) {
  using namespace coop_nxnative;
  std::array<std::uint32_t, kBowGuardOriginal.size()> original{};
  std::array<std::uint32_t, 4> padding{};
  if (!read_at(base + kBowGuardSite - 4, original) || original != kBowGuardOriginal ||
      !read_at(base + kBowGuardCave, padding) ||
      !std::all_of(padding.begin(), padding.end(), [](std::uint32_t word) {
        return word == 0 || word == 0xd503201f;
      })) return false;
  const auto cave = make_bow_guard();
  const auto hook = arm64_branch(kBowGuardSite, kBowGuardCave, false).value();
  const auto code = [](const auto& value) {
    return std::span(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value));
  };
  return write_code(base + kBowGuardCave, code(cave)) &&
      write_code(base + kBowGuardSite, code(hook));
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

#if defined(__APPLE__) && defined(COOP_NXNATIVE_NETWORK)
// Slot 17 is UIScreen::Update (docs/game-ui.md). Keep the game's call and
// dispatch the shared pause-menu code on that screen's own update thread.
void update_pause_screen(std::uint64_t screen) {
  const auto original = original_screen_update.load(std::memory_order_acquire);
  if (native_menu_active.load(std::memory_order_acquire) && !in_shared_frame) {
    in_shared_frame = true;
    native_screen_event(&shared_api, screen);
    in_shared_frame = false;
  }
  original(screen);
}

// The title uses dedicated lifecycle functions, separate from pause updates.
using TitleMethod = std::uint64_t (*)(std::uint64_t);
std::array<std::atomic<TitleMethod>, 3> original_title_methods{};
std::atomic<bool> native_title_active{false};
hos_coop_title_fn native_title_event = nullptr;

void dispatch_title(std::uint64_t screen, int event) {
  if (!native_title_active.load(std::memory_order_acquire) || in_shared_frame) return;
  in_shared_frame = true;
  native_title_event(&shared_api, screen, event);
  in_shared_frame = false;
}

std::uint64_t title_gone(std::uint64_t screen) {
  dispatch_title(screen, HOS_COOP_TITLE_GONE);
  return original_title_methods[0].load(std::memory_order_acquire)(screen);
}

std::uint64_t title_setup(std::uint64_t screen) {
  const auto result = original_title_methods[1].load(std::memory_order_acquire)(screen);
  dispatch_title(screen, HOS_COOP_TITLE_SETUP);
  return result;
}

std::uint64_t title_step(std::uint64_t screen) {
  dispatch_title(screen, HOS_COOP_TITLE_STEP);
  return original_title_methods[2].load(std::memory_order_acquire)(screen);
}

bool install_native_title(void* module, std::uint64_t base) {
  constexpr std::uint64_t kTitleVtable = 0x4bf06e8;
  constexpr std::array<std::uint64_t, 3> offsets{0x34067a0, 0x34067d0, 0x3406d70};
  constexpr std::array<TitleMethod, 3> wrappers{title_gone, title_setup, title_step};
  const auto table = base + kTitleVtable;
  std::array<std::uint64_t, 3> slots{0, table + 93 * sizeof(std::uint64_t), table + 96 * sizeof(std::uint64_t)};
  // Verify the deleting destructor rather than assuming which leading slot it uses.
  for (std::size_t i = 0; i < 2; ++i) {
    std::uint64_t current = 0;
    const auto slot = table + i * sizeof(std::uint64_t);
    if (read_at(slot, current) && current == base + offsets[0]) slots[0] = slot;
  }
  if (!slots[0]) return false;
  native_title_event = reinterpret_cast<hos_coop_title_fn>(dlsym(module, "hos_coop_title"));
  if (!native_title_event) return false;

  for (std::size_t i = 0; i < slots.size(); ++i) {
    std::uint64_t current = 0;
    if (!read_at(slots[i], current) || current != base + offsets[i]) return false;
    original_title_methods[i].store(reinterpret_cast<TitleMethod>(current), std::memory_order_release);
  }
  for (std::size_t i = 0; i < slots.size(); ++i) {
    if (!with_writable_alias(slots[i], sizeof(std::uint64_t), [&](void* alias) {
      __atomic_store_n(static_cast<std::uint64_t*>(alias),
                       reinterpret_cast<std::uint64_t>(wrappers[i]), __ATOMIC_RELEASE);
    })) return false;  // any installed wrappers still forward to the original methods
  }
  native_title_active.store(true, std::memory_order_release);
  return true;
}

std::uint64_t filter_pause_pad(std::uint64_t buttons, std::int32_t* axes) {
  if (!native_menu_active.load(std::memory_order_acquire)) return buttons;
  const auto filtered = native_pad_filter(buttons, axes);
  native_pad_buttons.store(filtered, std::memory_order_relaxed);
  return filtered;
}

bool install_native_pause_menu(void* module, std::uint64_t base, RegisterPad register_pad) {
  constexpr std::uint64_t kUpdateOffset = 0x2d1380;
  constexpr std::size_t kUpdateSlot = 17;
  constexpr std::array tables{
    CoopPauseTab::kPagesVtable, CoopPauseTab::kStripVtable,
    CoopPauseTab::kBlockerVtables[0], CoopPauseTab::kBlockerVtables[1],
    CoopPauseTab::kBlockerVtables[2], CoopPauseTab::kBlockerVtables[3],
    CoopPauseTab::kBlockerVtables[4], CoopPauseTab::kBlockerVtables[5],
    CoopPauseTab::kBlockerVtables[6], CoopPauseTab::kBlockerVtables[7],
  };
  native_screen_event = reinterpret_cast<ScreenEvent>(dlsym(module, "hos_coop_game_screen"));
  native_pad_filter = reinterpret_cast<FilterPad>(dlsym(module, "hos_coop_pad"));
  if (!native_screen_event || !native_pad_filter) return false;

  const auto slot = [&](std::size_t index) { return base + tables[index] + kUpdateSlot * sizeof(std::uint64_t); };
  const std::uint64_t original = base + kUpdateOffset;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    std::uint64_t actual = 0;
    if (!read_at(slot(i), actual) || actual != original) return false;
  }
  if (!register_pad(filter_pause_pad)) return false;

  original_screen_update.store(reinterpret_cast<UpdateScreen>(original), std::memory_order_release);
  const auto put = [](std::uint64_t where, std::uint64_t value) {
    return with_writable_alias(where, sizeof(value), [&](void* alias) {
      __atomic_store_n(static_cast<std::uint64_t*>(alias), value, __ATOMIC_RELEASE);
    });
  };
  const auto wrapper = reinterpret_cast<std::uint64_t>(&update_pause_screen);
  for (std::size_t i = 0; i < tables.size(); ++i) {
    if (put(slot(i), wrapper)) continue;
    // A slot that could not be restored still calls the original; forwarding
    // to co-op remains disabled until all hooks are installed.
    for (std::size_t j = 0; j < i; ++j) put(slot(j), original);
    return false;
  }
  const auto hud_slot = base + CoopPauseTab::kHudVtable + kUpdateSlot * sizeof(std::uint64_t);
  std::uint64_t hud_update = 0;
  const bool hud_hook = read_at(hud_slot, hud_update) && hud_update == original && put(hud_slot, wrapper);
  std::fprintf(stderr, "[coop-nxnative] minimap HUD bridge %s\n", hud_hook ? "installed" : "unavailable");
  native_menu_active.store(true, std::memory_order_release);
  return true;
}

// Save sequence methods and Twinrova's receiver are reached through their
// vtables. Direct calls by the module still reach the original game methods.
using WorldMethod = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

struct WorldSlot {
  unsigned id;
  std::uint64_t table;
  unsigned slot;
  std::uint64_t original;
};

constexpr std::array kSaveWorldSlots{
    WorldSlot{HOS_COOP_WRAP_SAVE_LOADED, 0x4bb4558, 39, 0x32d8690},
    WorldSlot{HOS_COOP_WRAP_SAVE_LOAD_START, 0x4ba6cf8, 29, 0x32c1db0},
    WorldSlot{HOS_COOP_WRAP_TEMP_LOAD_START, 0x4ba6e40, 29, 0x32c2080},
    WorldSlot{HOS_COOP_WRAP_GAME_OVER_SAVE, 0x4ba4a28, 29, 0x32bbce0},
};
constexpr WorldSlot kBossOrderSlot{HOS_COOP_WRAP_BOSS_ORDER, 0x49a8568, 39, 0x2723f80};

std::array<std::atomic<WorldMethod>, HOS_COOP_WRAP_BOSS_ORDER + 1> original_world_methods{};
std::atomic<hos_coop_wrap_fn> world_method_event{nullptr};
std::atomic<std::uint64_t> enabled_world_methods{0};

template<unsigned Id>
std::uint64_t dispatch_world_method(std::uint64_t x0, std::uint64_t x1, std::uint64_t x2, std::uint64_t x3,
                                    std::uint64_t x4, std::uint64_t x5, std::uint64_t x6, std::uint64_t x7) {
  const auto original = original_world_methods[Id].load(std::memory_order_acquire);
  if (enabled_world_methods.load(std::memory_order_acquire) & (std::uint64_t{1} << Id)) {
    const auto wrap = world_method_event.load(std::memory_order_relaxed);
    std::array<std::uint64_t, 32> registers{x0, x1, x2, x3, x4, x5, x6, x7};
    if (wrap(Id, 0, registers.data(), &shared_api)) return registers[0];
  }
  return original(x0, x1, x2, x3, x4, x5, x6, x7);
}

std::uint64_t install_world_slots(std::uint64_t base, std::span<const WorldSlot> slots,
                                  std::span<const WorldMethod> wrappers) {
  const auto address = [base](const WorldSlot& s) { return base + s.table + s.slot * sizeof(std::uint64_t); };
  for (const auto& s : slots) {
    std::uint64_t current = 0;
    if (!read_at(address(s), current) || current != base + s.original) return 0;
    original_world_methods[s.id].store(reinterpret_cast<WorldMethod>(current), std::memory_order_release);
  }

  std::uint64_t mask = 0;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    if (!with_writable_alias(address(slots[i]), sizeof(std::uint64_t), [&](void* alias) {
      __atomic_store_n(static_cast<std::uint64_t*>(alias),
                       reinterpret_cast<std::uint64_t>(wrappers[i]), __ATOMIC_RELEASE);
    })) return 0;  // A partially installed group still forwards to the game.
    mask |= std::uint64_t{1} << slots[i].id;
  }
  return mask;
}

struct WorldFunction {
  unsigned id;
  std::uint64_t offset;
  std::array<std::uint32_t, 4> instructions;
};

constexpr std::array kWorldFunctions{
    WorldFunction{HOS_COOP_WRAP_FLAG_APPLY, 0xb99980, {0xd503237f, 0xd10243ff, 0xfd0013e8, 0xa9037bfd}},
    WorldFunction{HOS_COOP_WRAP_TIME_SET, 0xeee640, {0xd503237f, 0xa9bd7bfd, 0xf9000bfc, 0xa9024ff4}},
    WorldFunction{HOS_COOP_WRAP_EVENT_REQUEST, 0xd7a820, {0xb940b808, 0x37580808, 0xd503237f, 0xd10103ff}},
    WorldFunction{HOS_COOP_WRAP_BRAIN_VERB, 0x6733b0, {0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4}},
    WorldFunction{HOS_COOP_WRAP_ACTION_VERB, 0xc08f0, {0xd503237f, 0xd10743ff, 0xa9187bfd, 0xf900cbfc}},
    WorldFunction{HOS_COOP_WRAP_SOUND_EMIT, 0x440630, {0xd503237f, 0xa9bb7bfd, 0xa90167fa, 0xa9025ff8}},
};

using DirectWorldMethod = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                            double, double, double, double, double, double, double, double);
std::array<std::atomic<DirectWorldMethod>, kWorldFunctions.size()> original_world_functions{};

template<std::size_t Index>
std::uint64_t dispatch_world_function(std::uint64_t x0, std::uint64_t x1, std::uint64_t x2, std::uint64_t x3,
                                      std::uint64_t x4, std::uint64_t x5, std::uint64_t x6, std::uint64_t x7,
                                      double d0, double d1, double d2, double d3,
                                      double d4, double d5, double d6, double d7) {
  constexpr unsigned id = kWorldFunctions[Index].id;
  const auto original = original_world_functions[Index].load(std::memory_order_acquire);
  const auto wrap = (enabled_world_methods.load(std::memory_order_acquire) & (std::uint64_t{1} << id))
      ? world_method_event.load(std::memory_order_relaxed) : nullptr;
  std::array<std::uint64_t, 32> registers{x0, x1, x2, x3, x4, x5, x6, x7};
  if (!wrap || !wrap(id, 0, registers.data(), &shared_api)) {
    registers[0] = original(registers[0], registers[1], registers[2], registers[3],
                            registers[4], registers[5], registers[6], registers[7], d0, d1, d2, d3, d4, d5, d6, d7);
  }
  if (wrap) wrap(id, 1, registers.data(), &shared_api);
  return registers[0];
}

std::uint64_t install_world_functions(std::uint64_t base) {
  using namespace coop_nxnative;
  const char* enabled = std::getenv("COOP_NXNATIVE_WORLD_FUNCTIONS");
  if (!enabled || std::strcmp(enabled, "1") != 0) return 0;
  constexpr std::array<DirectWorldMethod, kWorldFunctions.size()> wrappers{
      dispatch_world_function<0>, dispatch_world_function<1>, dispatch_world_function<2>,
      dispatch_world_function<3>, dispatch_world_function<4>, dispatch_world_function<5>,
  };

  std::uint64_t mask = 0;
  for (std::size_t i = 0; i < kWorldFunctions.size(); ++i) {
    const auto& function = kWorldFunctions[i];
    const auto cave = kWorldFunctionCave + i * kWorldFunctionBytes;
    auto expected = function.instructions;
    std::array<std::uint32_t, 4> actual{};
    std::array<std::uint32_t, kWorldFunctionBytes / sizeof(std::uint32_t)> padding{};
    if (!read_at(base + function.offset, actual) || !read_at(base + cave, padding)) continue;
    // nxnative may remove pointer-authentication instructions before loading.
    for (std::size_t w = 0; w < actual.size(); ++w) {
      if (expected[w] == 0xd503237f && actual[w] == 0xd503201f) expected[w] = actual[w];
    }
    if (actual != expected || !std::all_of(padding.begin(), padding.end(), [](std::uint32_t word) {
      return word == 0 || word == 0xd503201f;
    })) continue;

    const auto bytes = make_world_function_cave(cave, function.offset, actual.front(),
                                                reinterpret_cast<std::uint64_t>(wrappers[i]));
    const auto branch = arm64_branch(function.offset, cave, false);
    if (!branch || !write_code(base + cave, bytes)) continue;
    original_world_functions[i].store(reinterpret_cast<DirectWorldMethod>(base + cave + 16),
                                     std::memory_order_release);
    const auto entry = base + function.offset;
    if (!with_writable_alias(entry, sizeof(*branch), [&](void* alias) {
      __atomic_store_n(static_cast<std::uint32_t*>(alias), *branch, __ATOMIC_RELEASE);
      sys_icache_invalidate(reinterpret_cast<void*>(entry), sizeof(*branch));
    })) continue;
    mask |= std::uint64_t{1} << function.id;
  }
  std::fprintf(stderr, "[coop-nxnative] direct world bridge mask %#llx\n",
               static_cast<unsigned long long>(mask));
  return mask;
}

void install_world_bridges(void* module, std::uint64_t base) {
  const auto wrap = reinterpret_cast<hos_coop_wrap_fn>(dlsym(module, "hos_coop_wrap"));
  const auto world = reinterpret_cast<hos_coop_world_fn>(dlsym(module, "hos_coop_world"));
  if (!wrap || !world) return;
  constexpr std::array<WorldMethod, 4> save_wrappers{
      dispatch_world_method<HOS_COOP_WRAP_SAVE_LOADED>,
      dispatch_world_method<HOS_COOP_WRAP_SAVE_LOAD_START>,
      dispatch_world_method<HOS_COOP_WRAP_TEMP_LOAD_START>,
      dispatch_world_method<HOS_COOP_WRAP_GAME_OVER_SAVE>,
  };
  constexpr std::array<WorldMethod, 1> boss_wrappers{dispatch_world_method<HOS_COOP_WRAP_BOSS_ORDER>};
  const auto save_mask = install_world_slots(base, kSaveWorldSlots, save_wrappers);
  const auto boss_mask = install_world_slots(base, std::span{&kBossOrderSlot, 1}, boss_wrappers);
  const auto mask = save_mask | boss_mask | install_world_functions(base);
  const bool enabled = mask && world(HOS_COOP_WORLD_HELLO, HOS_COOP_WORLD_VERSION, mask);
  if (enabled) {
    world_method_event.store(wrap, std::memory_order_relaxed);
    enabled_world_methods.store(mask, std::memory_order_release);
  }
  std::fprintf(stderr, "[coop-nxnative] native save lifecycle bridge %s\n",
               enabled && save_mask ? "installed" : "unavailable");
  std::fprintf(stderr, "[coop-nxnative] Twinrova order bridge %s\n",
               enabled && boss_mask ? "installed" : "unavailable");
}

bool start_shared_module(const char* path, std::uint64_t base) {
  const char* game_dir = std::getenv("COOP_NXNATIVE_SHARED_DIR");
  if (!game_dir || !*game_dir) {
    std::fprintf(stderr, "[coop-nxnative] shared module refused: missing state directory\n");
    return false;
  }
  void* module = dlopen(path, RTLD_LOCAL | RTLD_NOW);
  if (!module) {
    std::fprintf(stderr, "[coop-nxnative] shared module open failed: %s\n", dlerror());
    return false;
  }
  const auto start = reinterpret_cast<hos_coop_start_fn>(dlsym(module, "hos_coop_start"));
  const auto frame = reinterpret_cast<hos_coop_frame_fn>(dlsym(module, "hos_coop_frame"));
  if (!start || !frame) {
    std::fprintf(stderr, "[coop-nxnative] shared module refused: missing co-op exports\n");
    dlclose(module);
    return false;
  }
  const bool want_menu = std::getenv("COOP_NXNATIVE_MENU") != nullptr;
  const char* overlay = std::getenv("DM_ROMFS_OVERLAY");
  const auto register_pad = reinterpret_cast<RegisterPad>(dlsym(RTLD_DEFAULT, "dm_coop_set_pad_filter"));
  std::error_code overlay_error;
  const bool has_pause_layout = overlay && std::filesystem::is_regular_file(std::filesystem::path(overlay) /
      "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs", overlay_error);
  const bool try_native_menu = want_menu && register_pad && has_pause_layout &&
      std::getenv("COOP_NXNATIVE_PAUSE_PACK");
  shared_api.version = HOS_COOP_API_VERSION;
  shared_api.size = try_native_menu ? offsetof(HosCoopApi, setting) : offsetof(HosCoopApi, pad_buttons);
  if (try_native_menu) {
    shared_api.pad_buttons = [] { return native_pad_buttons.load(std::memory_order_relaxed); };
    shared_api.scratch = [](void* ctx, const void* data, std::size_t size) -> std::uint64_t {
      if (!in_shared_frame || ctx != &shared_api || !data || size > shared_scratch.size()) return 0;
      std::memcpy(shared_scratch.data(), data, size);
      return reinterpret_cast<std::uint64_t>(shared_scratch.data());
    };
  }
  shared_api.main_base = base;
  shared_api.main_build_id = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348";
  shared_api.game_dir = game_dir;
  shared_api.read = [](std::uint64_t address, void* out, std::size_t size) -> int {
    return address && out && size && address + size > address &&
        copy_local(address, {static_cast<std::uint8_t*>(out), size}, nullptr);
  };
  shared_api.write = [](std::uint64_t address, const void* bytes, std::size_t size) -> int {
    return address && bytes && size && address + size > address && size <= UINT32_MAX &&
        mach_vm_write(mach_task_self(), address, reinterpret_cast<vm_offset_t>(bytes),
                      static_cast<mach_msg_type_number_t>(size)) == KERN_SUCCESS;
  };
  shared_api.cas32 = [](std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t*) -> int {
    return 0;
  };
  shared_api.call = [](void* ctx, const HosCoopCall* call, std::uint64_t* result) -> int {
    if (!in_shared_frame || ctx != &shared_api || !call || !result || (call->function & 3) ||
        call->function < shared_api.main_base ||
        call->function - shared_api.main_base >= coop_nxnative::kCalcCaveOffset) return 0;
    *result = coop_nxnative::coop_nxnative_invoke_game_call(
        call, shared_api.scratch ? shared_indirect_result.data() : nullptr);
    return 1;
  };
  shared_api.log = [](const char* line) {
    std::fprintf(stderr, "[coop-nxnative shared] %s\n", line);
  };
  // Keep the module and its globals alive for the lifetime of the game.
  if (!start(&shared_api)) {
    std::fprintf(stderr, "[coop-nxnative] shared module start refused\n");
    return false;
  }
  install_world_bridges(module, base);
  shared_frame.store(frame, std::memory_order_release);
  using SetCamera = void (*)(hos_coop_camera_wanted_fn, hos_coop_camera_fn);
  const auto set_camera = reinterpret_cast<SetCamera>(dlsym(RTLD_DEFAULT, "dm_coop_set_camera"));
  const auto camera_wanted = reinterpret_cast<hos_coop_camera_wanted_fn>(dlsym(module, "hos_coop_camera_wanted"));
  const auto camera_publish = reinterpret_cast<hos_coop_camera_fn>(dlsym(module, "hos_coop_camera"));
  if (set_camera && camera_wanted && camera_publish) {
    set_camera(camera_wanted, camera_publish);
    std::fprintf(stderr, "[coop-nxnative] camera capture bridge installed\n");
  }
  if (want_menu) {
    const bool native_menu = try_native_menu && install_native_pause_menu(module, base, register_pad);
    if (native_menu) {
      std::fprintf(stderr, "[coop-nxnative] native pause-menu bridge installed\n");
      std::error_code title_error;
      if (overlay && std::filesystem::is_regular_file(std::filesystem::path(overlay) /
          "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs", title_error)) {
        std::fprintf(stderr, "[coop-nxnative] native title-menu bridge %s\n",
                     install_native_title(module, base) ? "installed" : "refused: unexpected title layout or functions");
      }
    } else if (try_native_menu) {
      std::fprintf(stderr, "[coop-nxnative] native pause-menu bridge refused: screen or pad hook unavailable\n");
    } else {
      std::fprintf(stderr, "[coop-nxnative] native pause menu needs the controller bridge and pause-menu pack\n");
    }
    install_multiplayer_menu(module, !native_menu);
  }
  std::fprintf(stderr, "[coop-nxnative] shared module active on calc thread\n");
  return true;
}

template <typename T>
bool write_value(std::uint64_t address, const T& value) {
  return mach_vm_write(mach_task_self(), address, reinterpret_cast<vm_offset_t>(&value),
                       sizeof(value)) == KERN_SUCCESS;
}

// Same row-major conversion used by the shared puppet director.
std::array<float, 9> puppet_rotation(const coop::protocol::Quat& q) {
  const auto [qx, qy, qz, qw] = q;
  const double length = std::hypot(std::hypot(qx, qy), std::hypot(qz, qw));
  if (!(length > 1e-6) || !std::isfinite(length)) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
  const double x = qx / length, y = qy / length, z = qz / length, w = qw / length;
  return {
      float(1 - 2 * (y*y + z*z)), float(2 * (x*y - z*w)), float(2 * (x*z + y*w)),
      float(2 * (x*y + z*w)), float(1 - 2 * (x*x + z*z)), float(2 * (y*z - x*w)),
      float(2 * (x*z - y*w)), float(2 * (y*z + x*w)), float(1 - 2 * (x*x + y*y)),
  };
}

void tick_spawn_test(std::uint64_t base, const coop_nxnative::RemoteTarget& target,
                     std::uint64_t generation, bool local_scene_valid, const char* actor_name) {
  using Clock = std::chrono::steady_clock;
  using coop_nxnative::GameCall;
  using coop_nxnative::coop_nxnative_invoke_game_call;

  // Build 1.1.1 factory calls and object layout, as used in coop_puppet.cpp.
  constexpr std::uint64_t malloc_offset = 0x14c6c90, manager_offset = 0x4e91888,
      default_offset = 0x4c7dad0, create_offset = 0x372a70, take_offset = 0x36fbd0,
      drop_offset = 0x36fde0, delete_offset = 0xa6490;
  enum class Phase { waiting, creating, live, done };
  static Phase phase = Phase::waiting;
  static Clock::time_point since{}, last_target{};
  static std::uint64_t scene = 0, block = 0, slot = 0, actor = 0, vtable = 0;
  static coop_nxnative::RemoteMotion motion = coop_nxnative::RemoteMotion::unknown;
  static unsigned anim_buffer = 0;
  static std::int32_t next_partial_slot = 0;
  static std::uint8_t player = 0;
  static coop::protocol::Age age{};
  const auto now = Clock::now();

  if (phase == Phase::done) return;
  if (phase != Phase::waiting && local_scene_valid && generation != scene) {
    // The engine owns old-scene actors and requests during scene teardown.
    phase = Phase::done;
    std::fprintf(stderr, "[coop-nxnative] puppet spawn test ended on local scene change\n");
    return;
  }
  if (!local_scene_valid) return;

  if (phase == Phase::waiting) {
    if (target.player == 0 || player != target.player || scene != generation || age != target.age) {
      player = target.player;
      age = target.age;
      scene = generation;
      since = now;
      return;
    }
    if (!*actor_name || now - since < std::chrono::seconds(2)) return;

    std::uint64_t manager = 0, create_default = 0, manager_vtable = 0;
    if (!read_at(base + manager_offset, manager) || !manager ||
        !read_at(manager, manager_vtable) || !manager_vtable ||
        !read_at(base + default_offset, create_default) || !create_default)
      return;
    phase = Phase::done;  // one request per launch, even if creation is refused
    std::fprintf(stderr, "[coop-nxnative] puppet spawn test allocating request\n");
    const GameCall allocate{base + malloc_offset, {0x2e0}, {}, 0.0f};
    block = coop_nxnative_invoke_game_call(&allocate);
    if (!block) {
      std::fprintf(stderr, "[coop-nxnative] puppet spawn test allocation failed\n");
      return;
    }
    std::array<std::uint8_t, 0x100> arg{};
    const auto rotation = puppet_rotation(target.rotation.value_or(coop::protocol::Quat{0, 0, 0, 1}));
    constexpr std::array<float, 3> scale{1, 1, 1};
    std::memcpy(arg.data(), target.position.data(), sizeof(target.position));
    std::memcpy(arg.data() + 0x0c, rotation.data(), sizeof(rotation));
    std::memcpy(arg.data() + 0x30, scale.data(), sizeof(scale));
    std::memcpy(arg.data() + 0x40, &create_default, sizeof(create_default));
    arg[0x98] = 1;
    std::array<char, 64> name{};
    std::strcpy(name.data(), actor_name);
    const std::array<std::uint64_t, 2> name_ref{block + 0x140, name.size()};
    slot = block + 0x110;
    if (!write_value(block, arg) || !write_value(block + 0x100, name_ref) ||
        !write_value(slot, std::uint64_t{0}) || !write_value(block + 0x140, name)) {
      std::fprintf(stderr, "[coop-nxnative] puppet spawn test could not write request\n");
      return;
    }

    std::fprintf(stderr, "[coop-nxnative] puppet spawn test requesting %s\n", actor_name);
    const GameCall create{base + create_offset, {manager, block + 0x100, block, slot, 1}, {}, 0.0f};
    const auto accepted = coop_nxnative_invoke_game_call(&create);
    std::uint64_t request = 0;
    if (!static_cast<std::uint32_t>(accepted) && (!read_at(slot, request) || !request)) {
      std::fprintf(stderr, "[coop-nxnative] puppet spawn test create refused\n");
      return;
    }
    phase = Phase::creating;
    since = now;
    last_target = now;
    return;
  }

  if (target.player == player && target.age == age) last_target = now;
  if (phase == Phase::creating) {
    std::uint64_t request = 0;
    std::uint32_t word = 0;
    if (read_at(slot, request) && request && read_at(request, word)) {
      const auto state = word & 0xffff;
      if (state == 3) {
        const GameCall take{base + take_offset, {request, 1}, {}, 0.0f};
        actor = coop_nxnative_invoke_game_call(&take);
        write_value(slot, std::uint64_t{0});
        phase = actor && read_at(actor, vtable) && vtable ? Phase::live : Phase::done;
        std::fprintf(stderr, "[coop-nxnative] puppet spawn test %s\n",
            phase == Phase::live ? "actor taken" : "take returned no actor");
        return;
      }
      if (state == 4 || state == 5 || state == 6) {
        if (state == 4) {
          const GameCall drop{base + drop_offset, {request}, {}, 0.0f};
          coop_nxnative_invoke_game_call(&drop);
        }
        write_value(slot, std::uint64_t{0});
        phase = Phase::done;
        std::fprintf(stderr, "[coop-nxnative] puppet spawn test request failed (state %u)\n", state);
        return;
      }
    }
    if (since != Clock::time_point::max() && now - since > std::chrono::seconds(10)) {
      std::fprintf(stderr, "[coop-nxnative] puppet spawn test still pending after 10 seconds\n");
      // Keep draining the outstanding request. Do not reissue it or lose a
      // late actor that will need to be taken and deleted.
      since = Clock::time_point::max();
    }
    return;
  }

  std::uint64_t observed_vtable = 0;
  std::uint32_t state = 0;
  if (!read_at(actor, observed_vtable) || observed_vtable != vtable ||
      !read_at(actor + 0x2c, state) || state >= 7) {
    phase = Phase::done;
    return;
  }
  if (state != 4) return;
  if (now - last_target > std::chrono::seconds(1)) {
    const GameCall remove{base + delete_offset, {actor}, {}, 0.0f};
    coop_nxnative_invoke_game_call(&remove);
    phase = Phase::done;
    std::fprintf(stderr, "[coop-nxnative] puppet spawn test delete requested\n");
  } else if (target.player == player && target.age == age) {
    write_value(actor + 0x358, target.position);
    write_value(actor + 0x31c, target.position);
    if (target.rotation) {
      const auto rotation = puppet_rotation(*target.rotation);
      write_value(actor + 0x364, rotation);
      write_value(actor + 0x328, rotation);
    }

    using coop_nxnative::RemoteMotion;
    const bool new_motion = target.motion != RemoteMotion::unknown && target.motion != motion;
    if (!new_motion && next_partial_slot == 0) return;
    std::uint64_t controller = actor;
    for (const auto offset : {0x270, 0x40, 0x18, 0x18}) {
      if (!read_at(controller + offset, controller) || !controller) return;
    }

    // The main command moves the legs. Copy it to the partial slots, like
    // CoopPuppet::Director does, so the upper body also leaves its bind pose.
    if (next_partial_slot != 0) {
      constexpr std::int32_t max_slots = 8;
      std::int32_t count = 0;
      if (!read_at(controller + 0x18, count)) return;
      if (next_partial_slot < std::min(count, max_slots)) {
        const GameCall sync{base + 0x3abec10,
                            {actor, 0, static_cast<std::uint64_t>(next_partial_slot)}, {}, 0.0f};
        coop_nxnative_invoke_game_call(&sync);
        ++next_partial_slot;
        return;
      }
      std::fprintf(stderr, "[coop-nxnative] puppet spawn test synced %d partial animation slots\n",
                   next_partial_slot - 1);
      next_partial_slot = 0;
    }
    if (!new_motion) return;

    // The game may retain the previous animation name pointer across calls.
    const auto text = block + 0x240 + 0x50 * anim_buffer;
    const auto request_address = block + 0x200;
    const char* name = target.motion == RemoteMotion::move ? "Move" : "Wait";
    std::array<char, 0x50> name_buffer{};
    std::strcpy(name_buffer.data(), name);
    std::array<std::uint8_t, 0x30> request{};
    constexpr float rate = 1.0f, unset = -1.0f;
    std::memcpy(request.data(), &actor, sizeof(actor));
    std::memcpy(request.data() + 0x08, &text, sizeof(text));
    std::memcpy(request.data() + 0x14, &rate, sizeof(rate));
    std::memcpy(request.data() + 0x18, &unset, sizeof(unset));
    std::memcpy(request.data() + 0x1c, &unset, sizeof(unset));
    if (!write_value(text, name_buffer) || !write_value(request_address, request)) return;
    const GameCall play{base + 0x2022a0, {request_address}, {}, 0.0f};
    coop_nxnative_invoke_game_call(&play);
    anim_buffer ^= 1;
    motion = target.motion;
    next_partial_slot = 1;
    std::fprintf(stderr, "[coop-nxnative] puppet spawn test requested animation %s\n", name);
  }
}
#endif

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

#if defined(__APPLE__) && defined(COOP_NXNATIVE_NETWORK)
std::vector<std::string> available_tunic_actors() {
  std::vector<std::string> result;
  const char* overlay = std::getenv("DM_ROMFS_OVERLAY");
  if (!overlay || !*overlay) return result;
  const auto actor_dir = std::filesystem::path(overlay) / "Pack" / "Actor";
  for (const auto suffix : CoopColors::Tunics()) {
    for (const auto* plain : {"CoopPuppet_Child", "CoopPuppet_Adult"}) {
      auto name = std::string(plain) + "_" + std::string(suffix);
      std::error_code error;
      const auto size = std::filesystem::file_size(actor_dir / (name + ".pack.zs"), error);
      if (!error && size > 0) result.push_back(std::move(name));
    }
  }
  return result;
}
#endif

void probe() {
#if defined(__APPLE__) && defined(COOP_NXNATIVE_NETWORK)
  const auto tunic_actors = available_tunic_actors();
  const char* shared_path = std::getenv("COOP_NXNATIVE_SHARED_MODULE");
  const bool want_shared = shared_path && *shared_path;
#elif defined(COOP_NXNATIVE_NETWORK)
  constexpr bool want_shared = false;
#endif
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
  const bool want_remote = !want_shared && remote_host && *remote_host;
  const bool network_on = !want_shared && !want_remote && want_loopback && loopback.start();
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
      if (calc_bridge_installed && std::getenv("COOP_NXNATIVE_PUPPET_PREFLIGHT")) {
        const bool guarded = install_puppet_guard(base);
        if (guarded) {
          const bool bow_guarded = install_bow_guard(base);
          std::fprintf(stderr, "[coop-nxnative] puppet bow guard %s\n",
                       bow_guarded ? "installed" : "unavailable");
        }
#ifdef COOP_NXNATIVE_NETWORK
        const char* spawn_test = std::getenv("COOP_NXNATIVE_SPAWN_TEST");
        if (want_shared) {
          if (guarded) start_shared_module(shared_path, base);
          else std::fprintf(stderr, "[coop-nxnative] shared module skipped: puppet guard unavailable\n");
        } else if (guarded && spawn_test && std::strcmp(spawn_test, "1") == 0) {
          spawn_test_base.store(base, std::memory_order_release);
        }
#else
        (void)guarded;
#endif
      }
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
    if (remote) {
      remote->tick(reading);
      const auto targets = reading && reading->scene
          ? remote->targets(*reading->scene) : coop_nxnative::RemoteTargets{};
#ifdef __APPLE__
      std::array<char, 64> actor_name{};
      const auto first = std::find_if(targets.begin(), targets.end(),
          [](const auto& entry) { return entry.player != 0; });
      if (first != targets.end()) {
        const auto* plain = first->age == coop::protocol::Age::child ? "CoopPuppet_Child" : "CoopPuppet_Adult";
        const auto suffix = CoopColors::TunicOf(first->color);
        const auto colored = std::string(plain) + "_" + std::string(suffix);
        const auto found = std::find(tunic_actors.begin(), tunic_actors.end(), colored);
        std::snprintf(actor_name.data(), actor_name.size(), "%s",
                      found == tunic_actors.end() ? plain : found->c_str());
      }
#endif
      const std::lock_guard lock(remote_targets_mutex);
      remote_targets = targets;
#ifdef __APPLE__
      remote_actor_name = actor_name;
      static std::string local_scene;
      remote_local_scene_valid = reading && reading->scene.has_value();
      if (remote_local_scene_valid && *reading->scene != local_scene) {
        local_scene = *reading->scene;
        ++remote_scene_generation;
      }
#endif
    }
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
      std::fprintf(stderr, "[coop-nxnative] calc-thread remote targets=%u\n",
          calc_remote_targets.load(std::memory_order_relaxed));
#ifdef __APPLE__
      const auto call_check = game_call_check.load(std::memory_order_relaxed);
      std::fprintf(stderr, "[coop-nxnative] calc-thread actor call check=%s\n",
          call_check == GameCallCheck::passed ? "passed" :
          call_check == GameCallCheck::failed ? "failed" : "waiting for target");
#endif
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


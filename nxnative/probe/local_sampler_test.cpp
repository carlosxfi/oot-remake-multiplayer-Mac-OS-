#include "local_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/sampler.hpp"
#include "coop/protocol/codec.hpp"
#include "../../coop/host/src/addresses.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <iostream>
#include <limits>
#include <map>

namespace {
struct SyntheticMemory {
  std::map<std::uint64_t, std::uint8_t> bytes;
  unsigned copies = 0;

  template <typename T>
  void put(std::uint64_t at, const T& value) {
    const auto data = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    for (std::size_t i = 0; i < data.size(); ++i) bytes[at + i] = data[i];
  }
  static bool copy(std::uint64_t at, std::span<std::uint8_t> out, void* context) {
    auto& self = *static_cast<SyntheticMemory*>(context);
    ++self.copies;
    for (std::size_t i = 0; i < out.size(); ++i) {
      const auto found = self.bytes.find(at + i);
      if (found == self.bytes.end()) return false;
      out[i] = found->second;
    }
    return true;
  }
};
}  // namespace

int main() {
  static_assert(std::endian::native == std::endian::little);
  namespace addr = coop::host::addr;
  namespace protocol = coop::protocol;
  SyntheticMemory fake;
  constexpr std::uint64_t base = 0x200000000, ui = 0x300000000;
  coop_nxnative::LocalMemory memory(base, SyntheticMemory::copy, &fake);
  assert(memory.main_base() == base);
  std::array<std::uint8_t, 8> buffer{};
  assert(memory.read(0, {}));
  assert(!memory.read(0, buffer));
  assert(!memory.read(std::numeric_limits<std::uint64_t>::max() - 3, buffer));
  assert(fake.copies == 0);
  assert(!memory.read(ui, buffer));
  assert(fake.copies == 1);
  coop_nxnative::LocalMemory no_copy(base, nullptr);
  assert(!no_copy.read(ui, buffer));

  fake.put(base + addr::kGameUiModulePtr, ui);
  fake.put(ui + addr::kGameUiPoseFlags, std::uint16_t{1});
  fake.put(ui + addr::kGameUiIsChild, std::uint8_t{1});
  const protocol::Vec3 initial{1.0f, 2.0f, 3.0f};
  fake.put(ui + addr::kGameUiPosition, initial);
  coop::host::PlayerReader reader(memory);
  auto reading = reader.read();
  assert(reading && reading->position == initial);
  assert(reading->age == protocol::Age::child);
  assert(!reading->scene && !reading->rotation && !reading->animation && !reading->equipment);

  coop::host::Sampler sampler;
  auto outgoing = sampler.frame(reading, 1'000'000);
  bool sample_seen = false, status_seen = false;
  for (const auto& packet : outgoing) {
    protocol::Message message;
    assert(protocol::decode(packet.bytes, message) == protocol::Error::none);
    assert(protocol::check_received(message, protocol::Sender::client, packet.channel) == protocol::Error::none);
    if (const auto* sample = std::get_if<protocol::PlayerSample>(&message)) {
      sample_seen = true;
      assert(sample->position == initial && sample->age == protocol::Age::child);
    }
    if (std::holds_alternative<protocol::PlayerStatus>(message)) status_seen = true;
  }
  assert(sample_seen && status_seen);
  assert(sampler.frame(reader.read(), 1'010'000).empty());
  auto moved = initial;
  moved[0] += 1;
  fake.put(ui + addr::kGameUiPosition, moved);
  outgoing = sampler.frame(reader.read(), 1'020'000);
  assert(outgoing.size() == 1);
  protocol::Message decoded;
  assert(protocol::decode(outgoing.front().bytes, decoded) == protocol::Error::none);
  const auto* sample = std::get_if<protocol::PlayerSample>(&decoded);
  assert(sample && sample->position == moved && sample->velocity);

  moved[0] = std::numeric_limits<float>::quiet_NaN();
  fake.put(ui + addr::kGameUiPosition, moved);
  assert(!reader.read());
  assert(sampler.frame(reader.read(), 1'040'000).empty());
  fake.put(ui + addr::kGameUiPosition, initial);
  fake.put(ui + addr::kGameUiPoseFlags, std::uint16_t{0});
  assert(!reader.read());
  memory.set_main_base(base + 0x1000);
  assert(memory.main_base() == base + 0x1000 && !reader.read());
  memory.set_main_base(base);
  fake.put(ui + addr::kGameUiPoseFlags, std::uint16_t{1});
  assert(reader.read());
  outgoing = sampler.frame(reader.read(), 1'500'000);
  assert(!outgoing.empty() && !sampler.last_sample()->velocity);
  std::cout << "Local memory, player reader and sampler checks passed.\n";
}

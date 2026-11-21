#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "coop/host/player_reader.hpp"

namespace coop_nxnative {

struct LoopbackStats {
  bool game_connected = false;
  bool observer_connected = false;
  std::uint64_t sent = 0;
  std::uint64_t received_samples = 0;
  std::uint64_t send_failures = 0;
  std::size_t encrypted_connections = 0;
  std::optional<coop::protocol::PlayerSample> last_received;
};

// An isolated network proof: the existing encrypted relay on 127.0.0.1,
// one sampled game client, and one receiving observer. No guest writes.
class Loopback {
 public:
  Loopback();
  ~Loopback();
  Loopback(const Loopback&) = delete;
  Loopback& operator=(const Loopback&) = delete;
  bool start();
  void tick(const std::optional<coop::host::PlayerReading>& reading);
  LoopbackStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace coop_nxnative

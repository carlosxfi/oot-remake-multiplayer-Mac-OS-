#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "coop/host/player_reader.hpp"
#include "coop/net/client.hpp"

namespace coop_nxnative {

// Numeric IPv4 only: avoids a blocking DNS lookup on the sampling thread.
std::optional<coop::net::ClientConfig> remote_config(
    std::string_view host, std::string_view port,
    std::string_view nickname, std::string_view password);

struct RemoteStats {
  coop::net::State state = coop::net::State::idle;
  std::uint8_t self = 0;
  std::size_t peers = 0;
  std::uint64_t sent = 0, received_samples = 0, send_failures = 0;
  std::optional<coop::protocol::PlayerSample> last_received;
  std::string detail;
};

// Reads/sends player state and buffers incoming presence; never writes guest
// memory. Requires another game to remain the room's progress host.
class RemotePeer {
 public:
  explicit RemotePeer(coop::net::ClientConfig config);
  ~RemotePeer();
  RemotePeer(const RemotePeer&) = delete;
  RemotePeer& operator=(const RemotePeer&) = delete;
  bool start();
  void tick(const std::optional<coop::host::PlayerReading>& reading);
  RemoteStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace coop_nxnative

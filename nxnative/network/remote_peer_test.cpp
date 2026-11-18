#include "remote_peer.hpp"

#include <enet/enet.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "enet_server.hpp"

namespace {
template <typename Step, typename Done>
bool wait_for(Step step, Done done, unsigned milliseconds = 5000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < until) {
    step();
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}
}  // namespace

int main() {
  using coop_nxnative::remote_config;
  assert(remote_config("127.0.0.1", "27510", "MacLink", "synthetic-password"));
  assert(!remote_config("127.0.0.1:27510", "27510", "MacLink", "synthetic-password"));
  assert(!remote_config("localhost", "27510", "MacLink", "synthetic-password"));
  assert(!remote_config("256.0.0.1", "27510", "MacLink", "synthetic-password"));
  assert(!remote_config("127.00.0.1", "27510", "MacLink", "synthetic-password"));
  assert(!remote_config("127.0.0.1", "0", "MacLink", "synthetic-password"));
  assert(!remote_config("127.0.0.1", "65536", "MacLink", "synthetic-password"));
  assert(!remote_config("127.0.0.1", "27510x", "MacLink", "synthetic-password"));
  assert(!remote_config("127.0.0.1", "27510", " ", "synthetic-password"));
  assert(!remote_config("127.0.0.1", "27510", "MacLink", ""));
  assert(enet_initialize() == 0);
  {
    coop::server::ServerOptions options;
    options.bind = 0x7f000001;
    options.port = 0;
    options.session.join_password = "synthetic-password";
    options.session.game_build_id = "e0f3ccfaf50065ae";
    coop::server::EnetServer server(options);
    assert(server.start());
    const auto port = std::to_string(server.port());
    auto host_config = remote_config("127.0.0.1", port, "Windows-test", "synthetic-password");
    assert(host_config);
    coop::net::Client windows(*host_config);
    assert(windows.start());
    assert(wait_for([&] {
      server.service(0); windows.service(0);
      while (windows.next_event()) {}
    }, [&] { return windows.state() == coop::net::State::connected; }));

    auto mac_config = remote_config("127.0.0.1", port, "Mac-test", "synthetic-password");
    assert(mac_config);
    coop_nxnative::RemotePeer mac(*mac_config);
    assert(mac.start());
    coop::host::PlayerReading reading;
    reading.position = {1, 2, 3};
    reading.scene = "SyntheticScene";
    reading.age = coop::protocol::Age::child;
    std::optional<coop::protocol::PlayerSample> received_by_windows;
    std::uint32_t sequence = 0;
    auto next_send = std::chrono::steady_clock::now();
    const auto step = [&] {
      server.service(0);
      windows.service(0);
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_send) {
        coop::protocol::PlayerSample sample;
        sample.time = {windows.server_now_us(), sequence++};
        sample.position = coop::protocol::Vec3{4, 5, 6};
        sample.scene = "SyntheticScene";
        assert(windows.send(sample));
        next_send = now + std::chrono::milliseconds(20);
      }
      while (auto event = windows.next_event()) {
        if (event->kind == coop::net::EventKind::message) {
          if (const auto* sample = std::get_if<coop::protocol::PlayerSample>(&event->message))
            received_by_windows = *sample;
        }
      }
      mac.tick(reading);
    };
    assert(wait_for(step, [&] {
      return received_by_windows && mac.stats().received_samples > 0;
    }));
    auto stats = mac.stats();
    assert(stats.state == coop::net::State::connected);
    assert(stats.self != windows.player_number() && stats.self != 0);
    assert(stats.peers == 1 && stats.send_failures == 0 && stats.sent > 0);
    assert(server.encrypted_connections() == 2);
    assert(received_by_windows->position == reading.position);
    assert(stats.last_received->position == (coop::protocol::Vec3{4, 5, 6}));
    assert(stats.last_received->player_number == windows.player_number());

    reading.position = {7, 8, 9};
    assert(wait_for(step, [&] { return received_by_windows->position == reading.position; }));
    auto bad_config = remote_config("127.0.0.1", port, "Wrong-password", "wrong");
    assert(bad_config);
    coop_nxnative::RemotePeer rejected(*bad_config);
    assert(rejected.start());
    assert(wait_for([&] { step(); rejected.tick(std::nullopt); }, [&] {
      return rejected.stats().state == coop::net::State::refused;
    }));
    assert(!rejected.stats().detail.empty());
  }
  enet_deinitialize();
  std::cout << "Remote peer configuration, two-way encrypted samples and refusal checks passed.\n";
}

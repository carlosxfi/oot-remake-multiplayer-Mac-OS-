#include "remote_peer.hpp"

#include <enet/enet.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "enet_server.hpp"
#include "coop/protocol/codec.hpp"

namespace {
template <typename T>
void roundtrip(const T& original) {
  const auto bytes = coop::protocol::encode(original);
  assert(bytes);
  coop::protocol::Message decoded;
  assert(coop::protocol::decode(*bytes, decoded) == coop::protocol::Error::none);
  const auto* actual = std::get_if<T>(&decoded);
  assert(actual && *actual == original);
}

void check_protocol() {
  namespace protocol = coop::protocol;
  static_assert(protocol::kProtocolVersion == 18);
  roundtrip(protocol::JoinKnock{1, 60, 0, "Mac-test"});
  roundtrip(protocol::JoinAllow{1, true, false});
  protocol::Damage damage;
  damage.id.value = 1;
  damage.amount = 1;
  roundtrip(damage);
  damage.hit_at = protocol::Vec3{1, 2, 3};
  roundtrip(damage);
  protocol::HitRecord record{};  // synthetic data, no game content
  record[5] = 42;
  damage.hit_record = record;
  roundtrip(damage);
  damage.hit_at.reset();
  roundtrip(damage);
  protocol::ObjectSample object;
  object.id.value = 1;
  object.state.time = {1000, 1};
  object.state.hit = protocol::ObjectHit{1, record};
  roundtrip(object);
}

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
  check_protocol();
  using coop_nxnative::remote_config;
  const auto compatible = remote_config("127.0.0.1", "27510", "MacLink", "synthetic-password");
  assert(compatible && compatible->hello.protocol_version == 18);
  auto legacy = compatible->hello;
  legacy.protocol_version = 16;
  assert(coop::protocol::check_hello(legacy, legacy.game_build_id) ==
         coop::protocol::RefuseReason::wrong_protocol_version);
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
    options.session.approve_joins = true;
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
    std::optional<std::uint32_t> pending_knock;
    std::uint32_t sequence = 0;
    coop::protocol::Vec3 windows_position{4, 5, 6};
    std::string windows_scene = "SyntheticScene";
    bool send_windows_samples = true;
    std::string windows_animation = "Wait";
    const coop::protocol::Quat windows_rotation{0, 1, 0, 0};
    auto next_send = std::chrono::steady_clock::now();
    const auto step = [&] {
      server.service(0);
      windows.service(0);
      const auto now = std::chrono::steady_clock::now();
      if (send_windows_samples && now >= next_send) {
        coop::protocol::PlayerSample sample;
        sample.time = {windows.server_now_us(), sequence++};
        sample.position = windows_position;
        sample.scene = windows_scene;
        sample.age = coop::protocol::Age::child;
        sample.rotation = windows_rotation;
        sample.animation = coop::protocol::Animation{windows_animation, 0.0f, 1.0f};
        assert(windows.send(sample));
        next_send = now + std::chrono::milliseconds(20);
      }
      while (auto event = windows.next_event()) {
        if (event->kind == coop::net::EventKind::message) {
          if (const auto* sample = std::get_if<coop::protocol::PlayerSample>(&event->message))
            received_by_windows = *sample;
          if (const auto* knock = std::get_if<coop::protocol::JoinKnock>(&event->message);
              knock && knock->wait_s != 0)
            pending_knock = knock->knock;
        }
      }
      mac.tick(reading);
    };
    assert(wait_for(step, [&] {
      return pending_knock && mac.stats().detail == "waiting for Windows host approval";
    }));
    assert(mac.stats().state != coop::net::State::connected && mac.stats().sent == 0);
    assert(windows.send(coop::protocol::JoinAllow{*pending_knock, true, false}));
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

    const auto windows_slot = windows.player_number() - 1;
    assert(mac.targets("SyntheticScene")[windows_slot].position == windows_position);
    assert(mac.targets("OtherScene")[windows_slot].player == 0);
    assert(mac.targets("")[windows_slot].player == 0);
    assert(mac.targets("SyntheticScene")[windows_slot].color ==
           coop::protocol::slot_color(windows.player_number()));
    coop::protocol::PlayerEquip equip;
    equip.equipment.color = 0xFF3030;
    assert(windows.send(equip));
    assert(wait_for(step, [&] {
      return mac.targets("SyntheticScene")[windows_slot].color == 0xFF3030;
    }));
    assert(mac.targets("SyntheticScene")[windows_slot].rotation == windows_rotation);
    assert(mac.targets("SyntheticScene")[windows_slot].motion == coop_nxnative::RemoteMotion::idle);
    windows_animation = "Move";
    windows_position = {10, 11, 12};
    assert(wait_for(step, [&] {
      const auto target = mac.targets("SyntheticScene")[windows_slot];
      return target.position == windows_position && target.motion == coop_nxnative::RemoteMotion::move;
    }));
    windows_scene = "OtherScene";
    assert(wait_for(step, [&] {
      return mac.targets("SyntheticScene")[windows_slot].player == 0 &&
             mac.targets("OtherScene")[windows_slot].player == windows.player_number();
    }));
    reading.position = {7, 8, 9};
    assert(wait_for(step, [&] { return received_by_windows->position == reading.position; }));
    send_windows_samples = false;
    assert(wait_for(step, [&] { return mac.targets("OtherScene")[windows_slot].player == 0; }));
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


#include "loopback.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
  coop_nxnative::Loopback network;
  assert(network.start());
  assert(!network.start());
  coop::host::PlayerReading reading;
  reading.position = {12, 34, 56};
  reading.age = coop::protocol::Age::child;
  reading.scene = "SyntheticScene";
  reading.rotation = coop::protocol::Quat{0, 0, 0, 1};
  reading.animation = coop::protocol::Animation{"Walk", 1, 1};

  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < until) {
    network.tick(reading);
    if (network.stats().received_samples >= 2) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  auto stats = network.stats();
  assert(stats.game_connected && stats.observer_connected);
  assert(stats.encrypted_connections == 2 && stats.received_samples >= 2);
  assert(stats.send_failures == 0 && stats.last_received);
  assert(stats.last_received->position == reading.position);
  assert(stats.last_received->scene == reading.scene);
  assert(stats.last_received->age == reading.age);
  assert(stats.last_received->rotation == reading.rotation);
  assert(stats.last_received->animation == reading.animation);
  assert(stats.last_received->player_number != coop::protocol::kPlayerSelf);

  const auto previous = stats.received_samples;
  reading.position = {13, 35, 57};
  const auto moved_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < moved_until) {
    network.tick(reading);
    stats = network.stats();
    if (stats.received_samples > previous && stats.last_received->position == reading.position) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(stats.received_samples > previous && stats.last_received->position == reading.position);
  assert(stats.send_failures == 0);
  std::cout << "Encrypted loopback relay and observer checks passed.\n";
}

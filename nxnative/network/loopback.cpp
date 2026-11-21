#include "loopback.hpp"

#include <enet/enet.h>

#include <string>
#include <utility>

#include "coop/host/sampler.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/codec.hpp"
#include "coop/protocol/join_password.hpp"
#include "enet_server.hpp"

namespace coop_nxnative {
namespace {
namespace protocol = coop::protocol;

std::string fresh_password() {
  const auto nonce = protocol::random_nonce();
  constexpr char hex[] = "0123456789abcdef";
  std::string password;
  for (const auto byte : nonce) {
    password.push_back(hex[byte >> 4]);
    password.push_back(hex[byte & 15]);
  }
  return password;
}

coop::net::ClientConfig config(std::uint16_t port, const std::string& password, const char* name) {
  coop::net::ClientConfig c;
  c.host = "127.0.0.1";
  c.port = port;
  c.join_password = password;
  c.hello.nickname = name;
  c.hello.build_id = "nxnative-network-probe";
  c.hello.game_build_id = "e0f3ccfaf50065ae";
  c.hello.game_version = "1.1.1";
  return c;
}
}  // namespace

struct Loopback::Impl {
  bool enet_ready = false;
  std::unique_ptr<coop::server::EnetServer> server;
  std::unique_ptr<coop::net::Client> game, observer;
  coop::host::Sampler sampler;
  LoopbackStats totals;

  ~Impl() {
    observer.reset();
    game.reset();
    server.reset();
    if (enet_ready) enet_deinitialize();
  }

  void drain_game() {
    while (auto event = game->next_event()) {
      if (event->kind == coop::net::EventKind::welcomed ||
          event->kind == coop::net::EventKind::reconnected) {
        sampler.reset();
        sampler.set_player_number(event->welcome.player_number);
      }
    }
  }

  void drain_observer() {
    while (auto event = observer->next_event()) {
      if (event->kind != coop::net::EventKind::message) continue;
      if (const auto* sample = std::get_if<protocol::PlayerSample>(&event->message);
          sample && sample->player_number == game->player_number()) {
        totals.last_received = *sample;
        ++totals.received_samples;
      }
    }
  }
};

Loopback::Loopback() = default;
Loopback::~Loopback() = default;

bool Loopback::start() {
  if (impl_) return false;
  auto state = std::make_unique<Impl>();
  if (enet_initialize() != 0) return false;
  state->enet_ready = true;
  const auto password = fresh_password();
  coop::server::ServerOptions options;
  options.bind = 0x7f000001;  // loopback only, never all adapters
  options.port = 0;         // the OS chooses an unused port
  options.max_connections = 2;
  options.session.max_players = 2;
  options.session.game_build_id = "e0f3ccfaf50065ae";
  options.session.join_password = password;
  state->server = std::make_unique<coop::server::EnetServer>(options);
  if (!state->server->start()) return false;
  const auto port = state->server->port();
  if (!port) return false;
  state->game = std::make_unique<coop::net::Client>(config(port, password, "nxnative"));
  state->observer = std::make_unique<coop::net::Client>(config(port, password, "observer"));
  if (!state->game->start() || !state->observer->start()) return false;
  impl_ = std::move(state);
  return true;
}

void Loopback::tick(const std::optional<coop::host::PlayerReading>& reading) {
  if (!impl_) return;
  auto& s = *impl_;
  s.server->service(0);
  s.game->service(0);
  s.observer->service(0);
  s.drain_game();
  s.drain_observer();
  if (s.game->state() == coop::net::State::connected) {
    for (const auto& packet : s.sampler.frame(reading, s.game->server_now_us())) {
      protocol::Message message;
      if (protocol::decode(packet.bytes, message) == protocol::Error::none &&
          s.game->send_on(message, packet.channel))
        ++s.totals.sent;
      else
        ++s.totals.send_failures;
    }
  }
  s.game->service(0);
  s.server->service(0);
  s.observer->service(0);
  s.drain_observer();
}

LoopbackStats Loopback::stats() const {
  if (!impl_) return {};
  auto result = impl_->totals;
  result.game_connected = impl_->game->state() == coop::net::State::connected;
  result.observer_connected = impl_->observer->state() == coop::net::State::connected;
  result.encrypted_connections = impl_->server->encrypted_connections();
  return result;
}

}  // namespace coop_nxnative

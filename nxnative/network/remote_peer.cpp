#include "remote_peer.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <utility>

#include "coop/host/sampler.hpp"
#include "coop/presence/presence.hpp"
#include "coop/protocol/codec.hpp"

namespace coop_nxnative {
namespace {
bool number(std::string_view text, unsigned max, unsigned& value) {
  if (text.empty()) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() && value <= max;
}
bool ipv4(std::string_view text) {
  for (unsigned i = 0; i < 4; ++i) {
    const auto dot = text.find('.');
    const auto part = dot == std::string_view::npos ? text : text.substr(0, dot);
    unsigned byte = 0;
    if (part.size() > 3 || (part.size() > 1 && part.front() == '0') || !number(part, 255, byte)) return false;
    if (i == 3) return dot == std::string_view::npos;
    if (dot == std::string_view::npos) return false;
    text.remove_prefix(dot + 1);
  }
  return false;
}
}  // namespace

std::optional<coop::net::ClientConfig> remote_config(
    std::string_view host, std::string_view port,
    std::string_view nickname, std::string_view password) {
  unsigned parsed_port = 0;
  if (!ipv4(host) || !number(port, 65535, parsed_port) || parsed_port == 0 ||
      password.empty() || password.size() > 1024 || nickname.empty() ||
      nickname.size() > coop::protocol::kMaxNicknameChars ||
      !std::all_of(nickname.begin(), nickname.end(), [](unsigned char c) { return c >= 0x20 && c <= 0x7e; }) ||
      std::all_of(nickname.begin(), nickname.end(), [](char c) { return c == ' '; }))
    return std::nullopt;
  coop::net::ClientConfig result;
  result.host = std::string(host);
  result.port = static_cast<std::uint16_t>(parsed_port);
  result.hello.nickname = std::string(nickname);
  result.hello.build_id = "nxnative-lan-probe";
  result.hello.game_build_id = "e0f3ccfaf50065ae";
  result.hello.game_version = "1.1.1";
  result.join_password = std::string(password);
  return result;
}

struct RemotePeer::Impl {
  explicit Impl(coop::net::ClientConfig config) : client(std::move(config)) {
    // This bring-up must not accept authority over progress changes.
    client.set_flag_answer([](std::uint8_t, const coop::protocol::FlagEntry&)
        -> std::optional<coop::protocol::FlagEntry> { return std::nullopt; });
  }
  coop::net::Client client;
  coop::host::Sampler sampler;
  coop::presence::Presence presence;
  RemoteStats totals;
  using Clock = std::chrono::steady_clock;
  struct TimedSample {
    coop::protocol::PlayerSample sample;
    Clock::time_point arrival;
  };
  std::array<std::optional<TimedSample>, coop::protocol::kMaxPlayers> latest{};

  RemoteTargets targets(std::string_view scene) const {
    RemoteTargets result{};
    if (scene.empty() || client.state() != coop::net::State::connected) return result;
    const auto now = Clock::now();
    for (std::size_t i = 0; i < latest.size(); ++i) {
      const auto& entry = latest[i];
      if (!entry || now - entry->arrival > std::chrono::seconds(1)) continue;
      const auto& sample = entry->sample;
      const auto* player = presence.player(sample.player_number);
      if (!player || player->has_flag(coop::protocol::status::loading) ||
          !sample.scene || *sample.scene != scene || !sample.age || !sample.position)
        continue;
      auto motion = RemoteMotion::unknown;
      if (sample.animation) {
        if (sample.animation->name == "Wait") motion = RemoteMotion::idle;
        if (sample.animation->name == "Move") motion = RemoteMotion::move;
      }
      result[i] = {sample.player_number, *sample.age, *sample.position, sample.rotation, motion};
    }
    return result;
  }

  void drain() {
    while (auto event = client.next_event()) {
      using Kind = coop::net::EventKind;
      if (event->kind == Kind::welcomed || event->kind == Kind::reconnected) {
        sampler.reset();
        sampler.set_player_number(event->welcome.player_number);
        presence.on_welcome(event->welcome);
        totals.last_received.reset();
        latest = {};
        totals.detail = "welcomed";
      } else if (event->kind == Kind::waiting_for_host) {
        totals.detail = "waiting for Windows host approval";
      } else if (event->kind == Kind::disconnected || event->kind == Kind::connect_failed) {
        presence.clear();
        totals.last_received.reset();
        latest = {};
        totals.detail = std::string(coop::net::to_string(event->kind));
      } else if (event->kind == Kind::refused) {
        totals.detail = std::string(coop::net::refuse_text(*event));
        presence.clear();
        totals.last_received.reset();
        latest = {};
      } else if (event->kind == Kind::message) {
        presence.apply(event->message);
        if (const auto* joined = std::get_if<coop::protocol::PlayerJoined>(&event->message);
            joined && joined->player_number > 0 && joined->player_number <= latest.size()) {
          latest[joined->player_number - 1].reset();
        }
        if (const auto* sample = std::get_if<coop::protocol::PlayerSample>(&event->message);
            sample && sample->player_number != client.player_number()) {
          presence.seen(sample->player_number, sample->time.server_time_us, client.server_now_us());
          totals.last_received = *sample;
          ++totals.received_samples;
          if (sample->player_number > 0 && sample->player_number <= latest.size() &&
              presence.player(sample->player_number)) {
            auto& slot = latest[sample->player_number - 1];
            const auto delta = slot ? sample->time.sequence - slot->sample.time.sequence : 1;
            // Sequence numbers wrap. Half the unsigned range distinguishes older packets.
            if (delta > 0 && delta < (std::uint32_t{1} << 31))
              slot = TimedSample{*sample, Clock::now()};
          }
        }
      } else if (event->kind == Kind::host_changed && event->player == client.player_number()) {
        // Keep Windows as progress host. Taking over would require flag and
        // save synchronization, which this read-only bridge does not provide.
        totals.detail = "Windows progress host left; read-only client stopped";
        client.stop();
        presence.clear();
        totals.last_received.reset();
        latest = {};
      }
    }
  }
};

RemotePeer::RemotePeer(coop::net::ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
RemotePeer::~RemotePeer() = default;
bool RemotePeer::start() { return impl_->client.start(); }

void RemotePeer::tick(const std::optional<coop::host::PlayerReading>& reading) {
  auto& s = *impl_;
  s.client.service(0);
  s.drain();
  if (s.client.state() == coop::net::State::connected) {
    for (const auto& packet : s.sampler.frame(reading, s.client.server_now_us())) {
      coop::protocol::Message message;
      if (coop::protocol::decode(packet.bytes, message) == coop::protocol::Error::none &&
          s.client.send_on(message, packet.channel))
        ++s.totals.sent;
      else
        ++s.totals.send_failures;
    }
  }
  s.client.service(0);
  s.drain();
}

RemoteTargets RemotePeer::targets(std::string_view scene) const {
  return impl_->targets(scene);
}

RemoteStats RemotePeer::stats() const {
  auto result = impl_->totals;
  result.state = impl_->client.state();
  result.self = impl_->client.player_number();
  result.peers = impl_->presence.players().size();
  return result;
}

}  // namespace coop_nxnative


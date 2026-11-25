// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_mod_data.h"
#include <mutex>
#include <variant>
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/codec.hpp"

namespace CoopModData {
namespace {
namespace proto = coop::protocol;
}  // namespace

const char* Words(SetResult result) {
    switch (result) {
    case SetResult::done: return "done";
    case SetResult::bad_id: return "not a mod id";
    case SetResult::bad_key: return "not a key (1 to 32 of a-z 0-9 _)";
    case SetResult::too_long: return "longer than 64 bytes";
    case SetResult::too_many_keys: return "a 9th key (a mod has at most 8: clear one first)";
    case SetResult::too_many_mods: return "a 17th mod with values";
    }
    return "?";
}

SetResult Store::Set(std::string_view mod_id, std::string_view key, std::span<const std::uint8_t> value) {
    if (!proto::is_mod_id(mod_id)) return SetResult::bad_id;
    if (!proto::is_mod_key(key)) return SetResult::bad_key;
    if (value.size() > proto::kMaxModDataBytes) return SetResult::too_long;
    auto mod = mine_.find(mod_id);
    if (value.empty()) {
        if (mod != mine_.end()) {
            if (auto k = mod->second.find(key); k != mod->second.end()) mod->second.erase(k);
            if (mod->second.empty()) mine_.erase(mod);
        }
        return SetResult::done;
    }
    if (mod == mine_.end()) {
        if (mine_.size() >= proto::kMaxModDataMods) return SetResult::too_many_mods;
        mod = mine_.emplace(std::string(mod_id), std::map<std::string, Bytes, std::less<>>{}).first;
    }
    auto k = mod->second.find(key);
    if (k == mod->second.end()) {
        if (mod->second.size() >= proto::kMaxModDataKeysPerMod) {
            if (mod->second.empty()) mine_.erase(mod);
            return SetResult::too_many_keys;
        }
        mod->second.emplace(std::string(key), Bytes(value.begin(), value.end()));
    } else {
        k->second.assign(value.begin(), value.end());
    }
    return SetResult::done;
}

std::optional<Bytes> Store::Get(std::uint8_t player, std::string_view mod_id, std::string_view key) const {
    const Values* values = &mine_;
    if (player != 0 && player != number_) {
        const auto p = others_.find(player);
        if (p == others_.end()) return std::nullopt;
        values = &p->second;
    }
    const auto mod = values->find(mod_id);
    if (mod == values->end()) return std::nullopt;
    const auto k = mod->second.find(key);
    if (k == mod->second.end()) return std::nullopt;
    return k->second;
}

void Store::Note(std::uint8_t player, const std::string& mod_id, const std::string& key, const Bytes& value) {
    if (changes_.size() >= kMaxChanges) {
        changes_.erase(changes_.begin());
        ++dropped_;
    }
    changes_.push_back(Change{player, mod_id, key, value});
}

void Store::Received(const proto::ModData& data) {
    if (data.player_number == 0 || data.player_number == number_) return;  // ours never comes back
    Values& values = others_[data.player_number];
    auto mod = values.find(data.mod_id);
    if (data.value.empty()) {
        if (mod == values.end()) return;
        const auto k = mod->second.find(data.key);
        if (k == mod->second.end()) return;
        mod->second.erase(k);
        if (mod->second.empty()) values.erase(mod);
        if (values.empty()) others_.erase(data.player_number);
    } else {
        if (mod == values.end()) mod = values.emplace(data.mod_id, std::map<std::string, Bytes, std::less<>>{}).first;
        Bytes& slot = mod->second[data.key];
        if (slot == data.value) return;
        slot = data.value;
    }
    Note(data.player_number, data.mod_id, data.key, data.value);
}

void Store::PlayerLeft(std::uint8_t player) {
    const auto p = others_.find(player);
    if (p == others_.end()) return;
    for (const auto& [mod_id, keys] : p->second) {
        for (const auto& entry : keys) Note(player, mod_id, entry.first, {});
    }
    others_.erase(p);
}

void Store::ForgetOthers() {
    while (!others_.empty()) PlayerLeft(others_.begin()->first);
}

void Store::Welcomed(std::uint8_t number) {
    ForgetOthers();
    number_ = number;
    sent_.clear();
    last_send_ms_.reset();
    ++connection_;
}

void Store::Left() {
    ForgetOthers();
    number_ = 0;
    sent_.clear();
    last_send_ms_.reset();
    ++connection_;
}

std::optional<proto::ModData> Store::Next(std::uint64_t now_ms) const {
    if (number_ == 0) return std::nullopt;
    if (last_send_ms_ && now_ms - *last_send_ms_ < proto::kModDataSendGapMs) return std::nullopt;
    // A cleared key first (the server then has room for a new one).
    for (const auto& [mod_id, keys] : sent_) {
        const auto mod = mine_.find(mod_id);
        for (const auto& entry : keys) {
            if (mod == mine_.end() || !mod->second.contains(entry.first)) {
                return proto::ModData{proto::kPlayerSelf, mod_id, entry.first, {}};
            }
        }
    }
    for (const auto& [mod_id, keys] : mine_) {
        const auto mod = sent_.find(mod_id);
        for (const auto& [key, value] : keys) {
            if (mod == sent_.end()) return proto::ModData{proto::kPlayerSelf, mod_id, key, value};
            const auto k = mod->second.find(key);
            if (k == mod->second.end() || k->second != value) {
                return proto::ModData{proto::kPlayerSelf, mod_id, key, value};
            }
        }
    }
    return std::nullopt;
}

void Store::MarkSent(const proto::ModData& data, std::uint64_t now_ms, std::uint64_t connection) {
    if (connection != connection_) return;  // sent on a connection that is gone: the new one sends it again
    last_send_ms_ = now_ms;
    if (data.value.empty()) {
        if (const auto mod = sent_.find(data.mod_id); mod != sent_.end()) {
            mod->second.erase(data.key);
            if (mod->second.empty()) sent_.erase(mod);
        }
        return;
    }
    sent_[data.mod_id][data.key] = data.value;
}

std::vector<Change> Store::TakeChanges() {
    std::vector<Change> out;
    out.swap(changes_);
    return out;
}

// ---- the game's glue ----

namespace {

struct Glue {
    std::mutex mutex;
    Store store;
    std::map<std::uint8_t, std::string> names;  // everyone in the session, this game's own too
};

Glue& TheGlue() {
    static Glue g;
    return g;
}

}  // namespace

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    switch (event.kind) {
    case EventKind::welcomed:
    case EventKind::reconnected:
        g.store.Welcomed(client.player_number());
        g.names.clear();
        for (const auto& p : client.players()) g.names[p.number] = p.nickname;
        return;
    case EventKind::disconnected:
    case EventKind::connect_failed:
    case EventKind::refused:
        g.store.Left();
        g.names.clear();
        return;
    case EventKind::player_joined:
        g.names[event.player] = event.nickname;
        return;
    case EventKind::player_left:
        g.store.PlayerLeft(event.player);
        g.names.erase(event.player);
        return;
    case EventKind::message:
        if (const auto* data = std::get_if<proto::ModData>(&event.message)) g.store.Received(*data);
        return;
    default:
        return;
    }
}

void Frame(coop::host::Session* session, std::uint64_t now_ms) {
    Glue& g = TheGlue();
    std::optional<proto::ModData> next;
    std::uint64_t connection = 0;
    {
        std::lock_guard lock(g.mutex);
        if (session == nullptr) {
            if (g.store.number() != 0) {
                g.store.Left();  // (a stop without a disconnect event)
                g.names.clear();
            }
            return;
        }
        next = g.store.Next(now_ms);
        connection = g.store.connection();
    }
    if (!next) return;
    const bool sent = session->with_client([&](coop::net::Client& client) {
        return client.state() == coop::net::State::connected && client.send(*next);
    });
    if (sent) {
        std::lock_guard lock(g.mutex);
        g.store.MarkSent(*next, now_ms, connection);
    }
}

SetResult Set(std::string_view mod_id, std::string_view key, std::span<const std::uint8_t> value) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.store.Set(mod_id, key, value);
}

std::optional<Bytes> Get(std::uint8_t player, std::string_view mod_id, std::string_view key) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.store.Get(player, mod_id, key);
}

std::vector<std::pair<std::uint8_t, std::string>> Players() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    std::vector<std::pair<std::uint8_t, std::string>> out;
    if (g.store.number() == 0) return out;
    for (const auto& [number, name] : g.names) out.emplace_back(number, name);
    return out;
}

std::uint8_t MyNumber() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.store.number();
}

std::vector<Change> TakeChanges() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.store.TakeChanges();
}

}  // namespace CoopModData

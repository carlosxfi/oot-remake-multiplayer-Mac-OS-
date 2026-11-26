// SPDX-License-Identifier: GPL-3.0-or-later
//
// The owner's game decides a boss's shots (rows 222, 286): coop_boss_shooter.h.
#include "coop_boss_shooter.h"

#include <algorithm>
#include <cstring>

namespace CoopBossShooter {

void Add(Shots& mine, Kind kind, std::uint32_t key, std::int32_t asked, std::int32_t index,
         const std::uint8_t* tail, std::size_t tail_size) {
    Shot s;
    s.seq = ++mine.seq;
    s.kind = kind;
    s.key = key;
    s.asked = asked;
    s.index = index;
    if (tail != nullptr) {
        std::memcpy(s.tail.data(), tail, std::min(tail_size, s.tail.size()));
    }
    mine.recent.push_back(s);
    if (mine.recent.size() > kMaxShots) {
        mine.recent.erase(mine.recent.begin(), mine.recent.end() - static_cast<std::ptrdiff_t>(kMaxShots));
    }
}

Planned Plan(const Shots& owner, std::uint32_t delivered, bool started) {
    Planned out;
    out.delivered = delivered;
    if (!started || owner.seq < delivered) {
        out.delivered = owner.seq;  // a first sample or a new owner: from its next shot on
        out.restarted = true;
        return out;
    }
    if (owner.seq == delivered) {
        return out;
    }
    // Covered only when the list holds every seq from delivered + 1 to the newest.
    const std::uint32_t missing = owner.seq - delivered;
    if (owner.recent.size() < missing || owner.recent.back().seq != owner.seq ||
        owner.recent[owner.recent.size() - missing].seq != delivered + 1) {
        out.delivered = owner.seq;
        out.restarted = true;
        return out;
    }
    out.make.assign(owner.recent.end() - static_cast<std::ptrdiff_t>(missing), owner.recent.end());
    out.delivered = owner.seq;
    return out;
}

Answer OwnCall(Role role, Kind kind, bool our_call) {
    if (role != Role::copy || our_call) {
        return {};
    }
    return {true, kind == Kind::prepare ? ~std::uint64_t{0} : 0};  // -1, or false
}

std::vector<std::uint8_t> Rebased(const Template& t, std::uint64_t to) {
    std::vector<std::uint8_t> out = t.bytes;
    const std::uint64_t size = out.size();
    for (std::size_t i = 0; i + 8 <= out.size(); i += 8) {
        std::uint64_t v = 0;
        std::memcpy(&v, out.data() + i, 8);
        if (v >= t.at && v - t.at < size) {
            v = v - t.at + to;
            std::memcpy(out.data() + i, &v, 8);
        }
    }
    return out;
}

bool PatchPrepare(std::vector<std::uint8_t>& block, const Shot& shot) {
    if (shot.kind != Kind::prepare || block.size() < addr::kPrepareSize) {
        return false;
    }
    std::memcpy(block.data() + addr::kPrepareKey, &shot.key, 4);
    std::memcpy(block.data() + addr::kPrepareIndex, &shot.asked, 4);
    std::memcpy(block.data() + addr::kPrepareTail, shot.tail.data(), kPrepareTailSize);
    return true;
}

bool PatchShoot(std::vector<std::uint8_t>& block, const Shot& shot, std::int32_t local_index) {
    if (shot.kind != Kind::shoot || block.size() < addr::kShootTail + kShootTailSize) {
        return false;
    }
    std::memcpy(block.data() + addr::kShootKey, &shot.key, 4);
    std::memcpy(block.data() + addr::kShootIndex, &local_index, 4);
    std::memcpy(block.data() + addr::kShootTail, shot.tail.data(), kShootTailSize);
    return true;
}

void IndexMap::Took(std::uint32_t key, std::int32_t owner_index, std::int32_t local_index) {
    if (owner_index >= 0 && local_index >= 0) {
        map_[{key, owner_index}] = local_index;
    }
}

std::optional<std::int32_t> IndexMap::Local(std::uint32_t key, std::int32_t owner_index) const {
    if (owner_index < 0) {
        // "the first prepared one": only when we hold a prepared one of this key at all
        const auto it = map_.lower_bound({key, 0});
        return it != map_.end() && it->first.first == key ? std::optional<std::int32_t>(owner_index) : std::nullopt;
    }
    const auto it = map_.find({key, owner_index});
    return it == map_.end() ? std::nullopt : std::optional<std::int32_t>(it->second);
}

void IndexMap::Slept(std::uint32_t key, std::int32_t owner_index) { map_.erase({key, owner_index}); }

void IndexMap::Clear() { map_.clear(); }

}  // namespace CoopBossShooter

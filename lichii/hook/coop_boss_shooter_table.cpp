// SPDX-License-Identifier: GPL-3.0-or-later
//
// The owner's game decides a boss's shots (rows 222, 286): coop_boss_shooter_table.h.
#include "coop_boss_shooter_table.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace CoopBossShooter {

namespace {

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(v));
    return text;
}

const char* Name(Kind k) { return k == Kind::prepare ? "prepare" : k == Kind::shoot ? "shoot" : "sleep"; }

bool Took(Kind kind, std::uint64_t x0) {
    return kind == Kind::prepare ? static_cast<std::int32_t>(x0) >= 0 : (x0 & 0xff) != 0;
}

}  // namespace

void Table::SetOn(bool on) {
    std::lock_guard lock(mutex_);
    on_ = on;
    if (!on) {
        entries_.clear();
    }
}

bool Table::On() const {
    std::lock_guard lock(mutex_);
    return on_;
}

void Table::Note(std::string text) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(text));
    }
}

void Table::Restart(Entry& e) {
    // The owner's shots not made yet are of the old count: dropped. What we prepared for it and never shot is put
    // back by the game's own sleep (a shot bullet ends in its own game).
    std::erase_if(e.queue, [](const Todo& t) { return !t.put_back; });
    for (const auto& [record, local] : e.held) {
        Todo t;
        t.shot.kind = Kind::sleep;
        t.shot.key = record.first;
        t.put_back = true;
        t.local = local;
        e.queue.push_back(t);
    }
    e.held.clear();
    e.records.Clear();
    e.started = false;
    e.planned = 0;
}

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        auto it = entries_.find(m.actor);
        const bool seen = it != entries_.end();
        Entry e = seen ? std::move(it->second) : Entry{};
        if (seen && (e.member.role != m.role || e.member.owner != m.owner)) {
            Note(m.name + ": shots: " +
                 (m.role == Role::owner ? std::string("ours now: our own AI's shots run and are sent")
                                        : "player " + std::to_string(m.owner) + "'s now: its shots come"));
            Restart(e);
            e.mine = {};
            e.mine_at.clear();
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

Answer Table::OnCall(const Reader& read, Kind kind, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2,
                     bool module_call, Seen* seen) {
    *seen = {};
    std::lock_guard lock(mutex_);
    std::uint64_t actor = 0;
    if (!on_ || x0 == 0 || entries_.empty() || !read(x0 + addr::kShooterOwner, &actor, sizeof actor)) {
        return {};
    }
    const auto it = entries_.find(actor);
    if (actor == 0 || it == entries_.end()) {
        return {};  // no synced boss part's shooter (solo, an enemy, a bullet's own): the game's own
    }
    if (module_call && in_flight_ == x0) {
        return {};  // our caller's: the owner's shot, made here
    }
    Entry& e = it->second;
    Seen s;
    s.kind = kind;
    s.actor = actor;
    bool ok = true;
    if (kind == Kind::sleep) {
        s.key = static_cast<std::uint32_t>(x1);
        s.asked = static_cast<std::int32_t>(x2);
    } else {
        const bool prepare = kind == Kind::prepare;
        ok = x1 != 0 && read(x1 + (prepare ? addr::kPrepareKey : addr::kShootKey), &s.key, 4) &&
             read(x1 + (prepare ? addr::kPrepareIndex : addr::kShootIndex), &s.asked, 4) &&
             read(x1 + (prepare ? addr::kPrepareTail : addr::kShootTail), s.tail.data(),
                  prepare ? kPrepareTailSize : kShootTailSize);
        if (ok) {
            Kept k;
            k.shooter = x0;
            k.block.at = x1;
            k.block.bytes.resize(prepare ? addr::kPrepareSize : addr::kShootSize);
            if (read(x1, k.block.bytes.data(), k.block.bytes.size())) {
                const bool first = !e.templates.contains({kind, s.key});
                e.templates[{kind, s.key}] = std::move(k);
                if (first) {
                    Note(e.member.name + ": shots: " + Name(kind) + " block of key " + Hex(s.key) + " kept");
                }
            }
        }
    }
    if (!ok) {
        return {};  // not read: the game's own call runs, nothing is kept
    }
    if (e.member.role == Role::owner) {
        s.valid = true;
        *seen = s;
        return {};
    }
    if (!e.started) {
        ++counts_.copy_own_ran;
        return {};
    }
    ++counts_.copy_skipped;
    return OwnCall(Role::copy, kind, false);
}

void Table::OnDone(const Seen& seen, std::uint64_t x0, std::uint64_t now_ms) {
    if (!seen.valid) {
        return;
    }
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(seen.actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::owner) {
        return;
    }
    if (!Took(seen.kind, x0)) {
        ++counts_.owner_refused;
        return;
    }
    Entry& e = it->second;
    const std::int32_t index = seen.kind == Kind::prepare ? static_cast<std::int32_t>(x0) : seen.asked;
    Add(e.mine, seen.kind, seen.key, seen.asked, index, seen.tail.data(),
        seen.kind == Kind::prepare ? kPrepareTailSize : seen.kind == Kind::shoot ? kShootTailSize : 0);
    e.mine_at.push_back(now_ms);
    while (e.mine_at.size() > e.mine.recent.size()) {
        e.mine_at.pop_front();
    }
    ++counts_.owner_kept;
    if (e.mine.seq == 1) {
        Note(e.member.name + ": shots: our first (" + Name(seen.kind) + ", key " + Hex(seen.key) + ") is sent");
    }
}

std::optional<Shots> Table::OwnerShots(std::uint64_t actor, std::uint64_t now_ms) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::owner) {
        return std::nullopt;
    }
    const Entry& e = it->second;
    Shots out;
    out.seq = e.mine.seq;
    for (std::size_t i = 0; i < e.mine.recent.size() && i < e.mine_at.size(); ++i) {
        if (now_ms - e.mine_at[i] <= kKeepMs) {
            out.recent.push_back(e.mine.recent[i]);
        }
    }
    return out;
}

void Table::Receive(std::uint64_t actor, const Shots& shots) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy) {
        return;
    }
    Entry& e = it->second;
    const bool was_started = e.started;
    Planned plan = Plan(shots, e.planned, e.started);
    if (plan.restarted && was_started) {
        ++counts_.restarts;
        Restart(e);
    }
    if (!was_started) {
        Note(e.member.name + ": shots: the owner's list came: our own AI's shots are answered, the owner's made");
    }
    e.started = true;
    e.planned = plan.delivered;
    for (const Shot& s : plan.make) {
        Todo t;
        t.shot = s;
        e.queue.push_back(t);
    }
    while (e.queue.size() > kMaxQueued) {
        e.queue.pop_front();
        ++counts_.dropped;
    }
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        if (main_base_ != 0) {
            Note("shots: a new game: block, parts and call in flight dropped");
            DropGame();
        }
        main_base_ = main_base;
        after_call = false;
    }
    if (!after_call) {
        malloc_failed_ = false;
        if (pending_ == Pending::malloc) {
            allocating_ = false;
        }
        pending_ = Pending::none;  // last frame's call was not made: the shot stays queued
    }
    in_flight_ = 0;
    if (after_call) {
        switch (std::exchange(pending_, Pending::none)) {
        case Pending::malloc:
            allocating_ = false;
            if (result != 0) {
                block_ = result;
                Note("shots: params block at " + Hex(result));
            } else {
                malloc_failed_ = true;
            }
            break;
        case Pending::call:
            if (const auto it = entries_.find(pending_actor_); it != entries_.end() && !it->second.queue.empty()) {
                Entry& e = it->second;
                const Todo t = e.queue.front();
                e.queue.pop_front();
                const bool took = Took(t.shot.kind, result);
                if (t.put_back) {
                    ++counts_.put_back;
                } else {
                    ++(took ? counts_.made : counts_.refused);
                    const RecordKey record{t.shot.key, t.shot.kind == Kind::prepare ? t.shot.index : t.shot.asked};
                    if (t.shot.kind == Kind::prepare) {
                        if (took && t.shot.index >= 0) {
                            e.records.Took(record.first, record.second, static_cast<std::int32_t>(result));
                            e.held[record] = static_cast<std::int32_t>(result);
                        }
                    } else {
                        e.held.erase(record);
                        if (t.shot.kind == Kind::sleep) {
                            e.records.Slept(record.first, record.second);
                        }
                    }
                    if (!took && counts_.refused <= 8) {
                        Note(e.member.name + ": shots: the game refused our " + Name(t.shot.kind) + " of key " +
                             Hex(t.shot.key) + " (owner seq " + std::to_string(t.shot.seq) + ")");
                    }
                }
            }
            break;
        case Pending::none:
            break;
        }
    }
    if (!on_ || main_base == 0) {
        return std::nullopt;
    }
    for (auto& [actor, e] : entries_) {
        if (e.queue.empty()) {
            continue;
        }
        // Its shooter now, and still its own.
        std::uint64_t components = 0;
        std::uint64_t shooter = 0;
        std::uint64_t maker = 0;
        if (!guest.Read(actor + addr::kActorComponents, &components, sizeof components) || components == 0 ||
            !guest.Read(components + addr::kComponentShooter, &shooter, sizeof shooter) || shooter == 0 ||
            !guest.Read(shooter + addr::kShooterOwner, &maker, sizeof maker) || maker != actor) {
            counts_.dropped += e.queue.size();
            e.queue.clear();
            e.held.clear();
            e.records.Clear();
            continue;
        }
        Todo& t = e.queue.front();
        const Kind kind = t.shot.kind;
        CoopPuppet::Call c;
        c.x[0] = shooter;
        if (t.put_back) {
            c.function = main_base + addr::kSleep;
            c.x[1] = t.shot.key;
            c.x[2] = static_cast<std::uint64_t>(static_cast<std::int64_t>(t.local));
        } else if (e.member.role != Role::copy) {
            e.queue.pop_front();  // (not ours to make any more)
            continue;
        } else {
            std::optional<std::int32_t> local;
            if (kind != Kind::prepare) {
                local = e.records.Local(t.shot.key, t.shot.asked);
                if (!local) {
                    ++counts_.not_prepared;
                    e.queue.pop_front();
                    continue;
                }
            }
            if (kind == Kind::sleep) {
                c.function = main_base + addr::kSleep;
                c.x[1] = t.shot.key;
                c.x[2] = static_cast<std::uint64_t>(static_cast<std::int64_t>(*local));
            } else {
                const auto kept = e.templates.find({kind, t.shot.key});
                if (kept == e.templates.end() || kept->second.shooter != shooter) {
                    if (t.since_ms == 0) {
                        t.since_ms = now_ms;
                    }
                    if (now_ms - t.since_ms < kWaitMs) {
                        continue;  // our own AI may build that key's block yet
                    }
                    if (++counts_.no_template <= 8) {
                        Note(e.member.name + ": shots: no " + Name(kind) + " block of key " + Hex(t.shot.key) +
                             " in this game: the owner's seq " + std::to_string(t.shot.seq) + " is not made");
                    }
                    e.queue.pop_front();
                    continue;
                }
                if (block_ == 0) {
                    if (allocating_ || malloc_failed_) {
                        return std::nullopt;
                    }
                    CoopPuppet::Call m;
                    m.function = main_base + addr::kMalloc;
                    m.x[0] = kBlockSize;
                    allocating_ = true;
                    pending_ = Pending::malloc;
                    return m;
                }
                const std::uint64_t at = block_ + (kind == Kind::prepare ? 0 : addr::kPrepareSize);
                std::vector<std::uint8_t> bytes = Rebased(kept->second.block, at);
                const bool patched = kind == Kind::prepare ? PatchPrepare(bytes, t.shot)
                                                           : PatchShoot(bytes, t.shot, *local);
                if (!patched || !guest.Write(at, bytes.data(), bytes.size())) {
                    ++counts_.dropped;
                    e.queue.pop_front();
                    continue;
                }
                c.function = main_base + (kind == Kind::prepare ? addr::kPrepare : addr::kShoot);
                c.x[1] = at;
            }
        }
        pending_ = Pending::call;
        pending_actor_ = actor;
        in_flight_ = shooter;
        return c;
    }
    return std::nullopt;
}

void Table::DropGame() {
    entries_.clear();
    block_ = 0;
    allocating_ = false;
    malloc_failed_ = false;
    pending_ = Pending::none;
    pending_actor_ = 0;
    in_flight_ = 0;
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    DropGame();
    main_base_ = 0;
    counts_ = {};
    notes_.clear();
}

Counts Table::GetCounts() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Table::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

Table& TheTable() {
    static Table table;
    return table;
}

}  // namespace CoopBossShooter

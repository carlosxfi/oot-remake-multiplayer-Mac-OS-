// SPDX-License-Identifier: GPL-3.0-or-later
//
// A boss's move state goes with the boss (row 149): coop_brain_state.h.
#include "coop_brain_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace CoopBrainState {

namespace {

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%#llx", static_cast<unsigned long long>(v));
    return text;
}

const BrainValue* ValueOf(const BrainState& board, std::uint32_t id) {
    for (const BrainValue& v : board.values) {
        if (v.key == id) {
            return &v;
        }
    }
    return nullptr;
}

std::string Text(std::span<const Key> keys, const std::vector<BrainValue>& values) {
    std::string out;
    for (const BrainValue& v : values) {
        for (const Key& k : keys) {
            if (k.id == v.key) {
                out += (out.empty() ? "" : ", ") + std::string(k.name) + "=" + std::to_string(v.value);
            }
        }
    }
    return out;
}

}  // namespace

std::vector<Key> KeysOf(std::string_view boss) {
    std::vector<Key> out;
    for (const Key& k : kKeys) {
        if (k.boss == boss) {
            out.push_back(k);
        }
    }
    std::sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.id < b.id; });
    return out;
}

std::vector<BrainValue> Plan(std::span<const Key> keys, const BrainState& theirs, const BrainState& ours) {
    std::vector<BrainValue> out;
    for (const Key& k : keys) {
        const BrainValue* want = ValueOf(theirs, k.id);
        const BrainValue* have = ValueOf(ours, k.id);
        if (want == nullptr || have == nullptr || want->type != static_cast<std::uint8_t>(k.kind)) {
            continue;  // (not sent by that owner's build or as another type, or not read here)
        }
        const std::int32_t value = k.kind == Kind::boolean ? (want->value != 0 ? 1 : 0) : want->value;
        if (value != have->value) {
            out.push_back({k.id, static_cast<std::uint8_t>(k.kind), value});
        }
    }
    return out;
}

void Table::Note(std::string text) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(text));
    }
}

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        auto it = entries_.find(m.actor);
        const bool seen = it != entries_.end();
        Entry e = seen ? std::move(it->second) : Entry{};
        if (!seen) {
            for (const Key& k : KeysOf(m.name)) {
                e.slots.push_back(Slot{k, 0, 0});
            }
            if (e.slots.empty()) {
                continue;  // nothing of this actor is carried
            }
        } else if (e.member.role != m.role) {
            e.todo.clear();
            e.planned = false;
            e.mine.reset();
            // Made ours: the old owner's last values go in before our brain decides (Step). Made a copy: the new
            // owner's come.
            e.due = m.role == Role::owner && !e.in.values.empty();
            if (m.role == Role::owner) {
                Note(m.name + ": blackboard: ours now: " +
                     (e.due ? "player " + std::to_string(e.member.owner) + "'s move state is put in first"
                            : std::string("no move state came from its old owner: our own stays")));
            }
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

void Table::Receive(std::uint64_t actor, const BrainState& board) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || board == it->second.in) {
        return;
    }
    Entry& e = it->second;
    e.in = board;
    if (e.member.role == Role::owner && !board.values.empty()) {
        // The hand-over state came after the news: put in as well.
        Note(e.member.name + ": blackboard: its old owner's move state came after it was ours: put in now");
        e.due = true;
        e.planned = false;
        e.todo.clear();
    }
}

std::optional<BrainState> Table::OwnerBoard(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.member.role != Role::owner) {
        return std::nullopt;
    }
    const Entry& e = it->second;
    if (!e.due) {
        return e.mine;
    }
    // Still to put in: the old owner's, as they came (our keys only), never our stale own.
    BrainState out;
    for (const Slot& s : e.slots) {
        if (const BrainValue* v = ValueOf(e.in, s.key.id)) {
            out.values.push_back(*v);
        }
    }
    return out;
}

bool Table::Find(CoopPuppet::Guest& guest, Entry& e) {
    std::uint64_t components = 0;
    std::uint64_t holder = 0;
    std::uint64_t blackboard = 0;
    if (!guest.Read(e.member.actor + addr::kActorComponents, &components, sizeof components) || components == 0 ||
        !guest.Read(components, &holder, sizeof holder) || holder == 0 ||
        !guest.Read(holder + addr::kHolderBlackboard, &blackboard, sizeof blackboard) || blackboard == 0 ||
        blackboard == ~std::uint64_t{0}) {
        e.blackboard = 0;
        return false;
    }
    const auto found = [&e] {
        return std::any_of(e.slots.begin(), e.slots.end(), [](const Slot& s) { return s.name_at != 0; });
    };
    if (blackboard == e.blackboard) {
        return found();
    }
    e.blackboard = blackboard;
    for (Slot& s : e.slots) {
        s.name_at = 0;
    }
    for (const Kind kind : {Kind::s32, Kind::boolean}) {
        const bool is_s32 = kind == Kind::s32;
        std::int32_t names = 0;
        std::int32_t values = 0;
        std::uint64_t names_at = 0;
        if (!guest.Read(blackboard + (is_s32 ? addr::kS32NameCount : addr::kBoolNameCount), &names, sizeof names) ||
            !guest.Read(blackboard + (is_s32 ? addr::kS32ValueCount : addr::kBoolValueCount), &values,
                        sizeof values) ||
            !guest.Read(blackboard + (is_s32 ? addr::kS32Names : addr::kBoolNames), &names_at, sizeof names_at) ||
            names <= 0 || names > addr::kMaxNames || names_at == 0) {
            continue;
        }
        std::vector<std::uint64_t> at(static_cast<std::size_t>(names));
        if (!guest.Read(names_at, at.data(), at.size() * sizeof(std::uint64_t))) {
            continue;
        }
        for (Slot& s : e.slots) {
            if (s.key.kind != kind) {
                continue;
            }
            std::string text(s.key.name.size() + 1, '\1');
            for (std::size_t i = 0; i < at.size(); ++i) {
                // The name and its end, so a longer key that starts the same is not taken for it.
                if (at[i] != 0 && guest.Read(at[i], text.data(), text.size()) &&
                    std::memcmp(text.data(), s.key.name.data(), s.key.name.size()) == 0 && text.back() == '\0') {
                    if (static_cast<std::int64_t>(i) < values) {
                        s.name_at = names_at + i * sizeof(std::uint64_t);
                        s.index = static_cast<std::uint32_t>(i);
                    }
                    break;
                }
            }
        }
    }
    return found();
}

std::optional<BrainState> Table::ReadValues(CoopPuppet::Guest& guest, const Entry& e) {
    std::uint64_t s32_at = 0;
    std::uint64_t bool_at = 0;
    if (e.blackboard == 0 || !guest.Read(e.blackboard + addr::kS32Values, &s32_at, sizeof s32_at) ||
        !guest.Read(e.blackboard + addr::kBoolValues, &bool_at, sizeof bool_at)) {
        return std::nullopt;
    }
    BrainState out;
    for (const Slot& s : e.slots) {
        if (s.name_at == 0) {
            continue;
        }
        if (s.key.kind == Kind::s32) {
            std::int32_t value = 0;
            if (s32_at != 0 && guest.Read(s32_at + s.index * sizeof value, &value, sizeof value)) {
                out.values.push_back({s.key.id, coop::protocol::kBrainS32, value});
            }
        } else {
            std::uint8_t value = 0;
            if (bool_at != 0 && guest.Read(bool_at + s.index, &value, sizeof value)) {
                out.values.push_back({s.key.id, coop::protocol::kBrainBool, value != 0 ? 1 : 0});
            }
        }
    }
    return out;
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        // A new game (or none): our block was its heap's and every blackboard its; a call in flight is not ours to
        // finish (as coop_boss_orders').
        if (main_base_ != 0) {
            Note("blackboard: a new game: block, bosses and call in flight dropped");
            DropGame();
        }
        main_base_ = main_base;
        after_call = false;
    }
    if (!after_call) {
        malloc_failed_ = false;
        if (pending_ == Pending::malloc) {
            allocating_ = false;  // last frame's call was not made: asked again
        }
        pending_ = Pending::none;  // (a value not put in stays first in its plan)
    } else {
        switch (std::exchange(pending_, Pending::none)) {
        case Pending::malloc:
            allocating_ = false;
            if (result != 0) {
                block_ = result;
                Note("blackboard: value block at " + Hex(result));
            } else {
                malloc_failed_ = true;
            }
            break;
        case Pending::set:
            if (const auto it = entries_.find(pending_actor_); it != entries_.end() && !it->second.todo.empty()) {
                Entry& e = it->second;
                const BrainValue asked = e.todo.front();
                e.todo.erase(e.todo.begin());
                const std::optional<BrainState> now = ReadValues(guest, e);
                const BrainValue* have = now ? ValueOf(*now, asked.key) : nullptr;
                if (have != nullptr && have->value == asked.value) {
                    ++counts_.put;
                    ++e.put;
                } else {
                    ++counts_.refused;  // (a locked key: the game's own rule; ours stays)
                    Note(e.member.name + ": blackboard: the game left key " + Hex(asked.key) + " alone");
                }
            }
            break;
        case Pending::none:
            break;
        }
    }
    if (main_base == 0 || entries_.empty()) {
        return std::nullopt;
    }
    if (block_ == 0 && !allocating_ && !malloc_failed_) {
        // Made while the boss is still a copy, so the values go in the frame it is made ours.
        CoopPuppet::Call c;
        c.function = main_base + addr::kMalloc;
        c.x[0] = addr::kBlockSize;
        allocating_ = true;
        pending_ = Pending::malloc;
        return c;
    }
    for (auto& [actor, e] : entries_) {
        if (e.member.role != Role::owner) {
            continue;
        }
        if (!Find(guest, e)) {
            if (!e.unread_said) {
                e.unread_said = true;
                ++counts_.unread;
                Note(e.member.name + ": blackboard: not found, or none of its keys in it: nothing carried");
            }
            continue;  // (asked again next frame: its blackboard may not be made yet)
        }
        std::vector<Key> keys;
        for (const Slot& s : e.slots) {
            keys.push_back(s.key);
        }
        if (e.due) {
            const std::optional<BrainState> ours = ReadValues(guest, e);
            if (!ours) {
                continue;
            }
            if (!e.planned) {
                e.todo = Plan(keys, e.in, *ours);
                e.planned = true;
                e.put = 0;
                e.same = 0;
                for (const BrainValue& v : e.in.values) {
                    if (ValueOf(*ours, v.key) != nullptr) {
                        ++e.same;
                    }
                }
                e.same -= static_cast<std::uint32_t>(e.todo.size());
                ++counts_.handed;
                counts_.same += e.same;
                if (!e.todo.empty()) {
                    Note(e.member.name + ": blackboard: put in: " + Text(keys, e.todo));
                }
            }
            if (!e.todo.empty() && block_ == 0) {
                continue;  // (no block this frame: asked again; our stale values are not sent meanwhile)
            }
            if (!e.todo.empty()) {
                const BrainValue& v = e.todo.front();
                const auto slot = std::find_if(e.slots.begin(), e.slots.end(),
                                               [&v](const Slot& s) { return s.key.id == v.key && s.name_at != 0; });
                if (slot == e.slots.end()) {
                    e.todo.erase(e.todo.begin());  // (its key is gone with a new blackboard)
                    return std::nullopt;
                }
                // The value: an s32, or a bool in its first byte.
                std::array<std::uint8_t, addr::kBlockSize> block{};
                std::memcpy(block.data(), &v.value, sizeof v.value);
                if (!guest.Write(block_, block.data(), block.size())) {
                    return std::nullopt;
                }
                CoopPuppet::Call c;
                c.function = main_base + (slot->key.kind == Kind::s32 ? addr::kSetS32 : addr::kSetBool);
                c.x[0] = e.blackboard;
                c.x[1] = slot->name_at;
                c.x[2] = block_;
                pending_ = Pending::set;
                pending_actor_ = actor;
                return c;
            }
            Note(e.member.name + ": blackboard: its old owner's move state is ours: " + std::to_string(e.put) +
                 " put in, " + std::to_string(e.same) + " the same");
            e.due = false;
            e.planned = false;
        }
        e.mine = ReadValues(guest, e);
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

}  // namespace CoopBrainState

// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 286's log-only probe of the Shooter component: coop_shooter_probe.h.
#include "coop_shooter_probe.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <utility>

namespace CoopShooterProbe {

namespace {

template <class T>
bool Get(const Reader& read, std::uint64_t address, T& out) {
    return address != 0 && read(address, &out, sizeof out);
}

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%#" PRIx64, v);
    return text;
}

// An address in main as main+offset, else as it is.
std::string Where(std::uint64_t v, std::uint64_t main_base) {
    return v >= main_base && v - main_base < 0x6000000 ? "main+" + Hex(v - main_base) : Hex(v);
}

std::string NameAt(const Reader& read, std::uint64_t actor) {
    std::uint64_t at = 0;
    if (!Get(read, actor + addr::kActorName, at) || at == 0) {
        return "?";
    }
    std::string name;
    for (std::uint64_t i = 0; i < 80; ++i) {
        char c = 0;
        if (!read(at + i, &c, 1) || c == 0) {
            break;
        }
        if (c < 0x20 || c > 0x7e) {
            return "?";
        }
        name += c;
    }
    return name.empty() ? "?" : name;
}

std::uint64_t ActorOfHandle(const Reader& read, std::uint64_t handle) {
    std::uint64_t holder = 0, actor = 0;
    if (!Get(read, handle + addr::kHandleHolder, holder) || !Get(read, holder + addr::kHolderActor, actor)) {
        return 0;
    }
    return actor;
}

RecordView ReadRecord(const Reader& read, std::uint64_t record) {
    RecordView v;
    std::array<std::uint8_t, 3> states{};
    if (read(record + addr::kRecordState, states.data(), states.size())) {
        v.state = states[0];
        v.asked = states[1];
        v.is_asked = states[2];
    }
    v.shot = ActorOfHandle(read, record + addr::kRecordShot);
    if (v.shot != 0) {
        std::array<std::uint32_t, 3> words{};
        if (!Get(read, v.shot, v.vtable) || !Get(read, v.shot + addr::kActorState, words)) {
            v.name = "?";
            return v;
        }
        v.actor_state = words[0];
        v.requests = words[1];
        v.more = words[2];
        Get(read, v.shot + addr::kActorPosition, v.place);
        v.name = NameAt(read, v.shot);
    }
    return v;
}

EntryView ReadEntryAt(const Reader& read, std::uint64_t entry, std::uint32_t key) {
    EntryView v;
    v.key = key;
    std::uint64_t records = 0;
    if (!Get(read, entry + addr::kEntryCount, v.count) || !Get(read, entry + addr::kEntryRecords, records) ||
        records == 0) {
        v.count = 0;
        return v;
    }
    for (std::uint32_t i = 0; i < std::min(v.count, kMaxRecords); ++i) {
        v.records.push_back(ReadRecord(read, records + i * addr::kRecordSize));
    }
    return v;
}

// Each used entry of the shooter's map.
template <class F>
void EachEntry(const Reader& read, std::uint64_t shooter, F&& f) {
    std::uint64_t map = 0;
    std::uint32_t slots = 0;
    if (!Get(read, shooter + addr::kShooterMap, map) || map == 0 ||
        !Get(read, shooter + addr::kShooterSlots, slots) || slots == 0 || slots > kMaxSlots) {
        return;
    }
    for (std::uint32_t s = 0; s < slots; ++s) {
        const std::uint64_t entry = map + s * addr::kMapEntrySize;
        std::uint32_t key = 0;
        if (Get(read, entry, key) && key != 0 && !f(entry, key)) {
            return;
        }
    }
}

const char* FnText(Fn fn) { return fn == Fn::prepare ? "PREPARE" : fn == Fn::shoot ? "SHOOT" : "SLEEP"; }

template <std::size_t N>
std::string Words(const std::array<std::uint64_t, N>& words) {
    std::string out;
    for (const std::uint64_t w : words) {
        char text[24];
        std::snprintf(text, sizeof text, "%s%016" PRIx64, out.empty() ? "" : " ", w);
        out += text;
    }
    return out;
}

}  // namespace

bool Same(const RecordView& a, const RecordView& b) {
    return a.state == b.state && a.asked == b.asked && a.is_asked == b.is_asked && a.shot == b.shot &&
           a.name == b.name && a.vtable == b.vtable && a.actor_state == b.actor_state && a.requests == b.requests &&
           a.more == b.more;
}

bool Same(const EntryView& a, const EntryView& b) {
    return a.key == b.key && a.count == b.count &&
           std::equal(a.records.begin(), a.records.end(), b.records.begin(), b.records.end(),
                      [](const RecordView& x, const RecordView& y) { return Same(x, y); });
}

std::uint64_t ShooterOf(const Reader& read, std::uint64_t actor) {
    std::uint64_t table = 0, shooter = 0;
    if (actor == 0 || !Get(read, actor + addr::kActorComponents, table) || table == 0 ||
        !Get(read, table + addr::kComponentShooter, shooter)) {
        return 0;
    }
    return shooter;
}

std::optional<EntryView> ReadEntry(const Reader& read, std::uint64_t shooter, std::uint32_t key) {
    std::optional<EntryView> out;
    if (shooter == 0 || key == 0) {
        return out;
    }
    EachEntry(read, shooter, [&](std::uint64_t entry, std::uint32_t k) {
        if (k == key) {
            out = ReadEntryAt(read, entry, k);
        }
        return !out;
    });
    return out;
}

std::vector<EntryView> ReadEntries(const Reader& read, std::uint64_t shooter) {
    std::vector<EntryView> out;
    if (shooter != 0) {
        EachEntry(read, shooter, [&](std::uint64_t entry, std::uint32_t k) {
            out.push_back(ReadEntryAt(read, entry, k));
            return out.size() < 32;
        });
    }
    return out;
}

std::string Text(const EntryView& entry, std::uint64_t main_base) {
    std::string out = "{" + Hex(entry.key) + " x" + std::to_string(entry.count);
    for (std::size_t i = 0; i < entry.records.size(); ++i) {
        const RecordView& r = entry.records[i];
        out += " [" + std::to_string(i) + " " + std::to_string(r.state) + "/" + std::to_string(r.asked) + "/" +
               std::to_string(r.is_asked);
        if (r.shot == 0) {
            out += " no bullet]";
            continue;
        }
        char place[64];
        std::snprintf(place, sizeof place, " at (%.1f, %.1f, %.1f)]", static_cast<double>(r.place[0]),
                      static_cast<double>(r.place[1]), static_cast<double>(r.place[2]));
        out += " " + r.name + " " + Hex(r.shot) + " vt " + Where(r.vtable, main_base) + " state " +
               std::to_string(r.actor_state) + " req " + Hex(r.requests) + " " + Hex(r.more) + place;
    }
    return out + "}";
}

Call Begin(const Reader& read, Fn fn, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2, std::uint64_t from,
           bool own_call) {
    Call c;
    c.fn = fn;
    c.shooter = x0;
    c.from = from;
    c.own_call = own_call;
    Get(read, x0, c.vtable);
    if (fn == Fn::sleep) {
        c.key = static_cast<std::uint32_t>(x1);
        c.index = static_cast<std::int32_t>(static_cast<std::uint32_t>(x2));
    } else if (x1 != 0) {
        c.params = x1;
        Get(read, x1, c.head);
        if (fn == Fn::prepare) {
            c.key = static_cast<std::uint32_t>(c.head[0]);
            Get(read, x1 + addr::kPrepareIndex, c.index);
            if (c.head[1] != 0) {
                Get(read, c.head[1], c.extra);
            }
            Get(read, x1 + addr::kPrepareTail, c.tail);
        } else {
            std::uint8_t uses = 0;
            Get(read, x1 + addr::kShootKey, c.key);
            Get(read, x1 + addr::kShootIndex, c.index);
            Get(read, x1 + addr::kShootUsesIndex, uses);
            c.uses_index = (uses & 1) != 0;
            Get(read, x1 + addr::kShootKey, c.tail);
        }
    }
    if (Get(read, x0 + addr::kShooterOwner, c.maker) && c.maker != 0) {
        c.maker_name = NameAt(read, c.maker);
    } else {
        c.maker_name = "?";
    }
    c.before = ReadEntry(read, x0, c.key);
    return c;
}

void Probe::SetOn(bool on) {
    std::lock_guard lock(mutex_);
    on_ = on;
}

bool Probe::On() const {
    std::lock_guard lock(mutex_);
    return on_;
}

void Probe::Publish(std::vector<Member> members) {
    std::lock_guard lock(mutex_);
    members_ = std::move(members);
}

void Probe::Line(std::string text, std::uint64_t now_ms) {
    if (now_ms / 1000 != second_) {
        second_ = now_ms / 1000;
        lines_this_second_ = 0;
    }
    if (lines_total_ >= kMaxLines || lines_this_second_ >= kMaxLinesPerSec) {
        ++dropped_;
        return;
    }
    ++lines_total_;
    ++lines_this_second_;
    lines_.push_back(std::move(text));
}

std::string Probe::RoleText(std::uint64_t actor) const {
    for (const Member& m : members_) {
        if (m.actor == actor) {
            return m.ours ? "owner" : "copy of player " + std::to_string(m.owner) + "'s";
        }
    }
    return "no boss of the walk";
}

void Probe::End(const Reader& read, const Call& c, std::uint64_t x0, std::uint64_t main_base, std::uint64_t now_ms) {
    // (the game's AI reads the answer as w0: an index, or a bool in its low byte)
    const std::int32_t answer = static_cast<std::int32_t>(static_cast<std::uint32_t>(x0));
    const bool done = c.fn == Fn::prepare ? answer >= 0 : (x0 & 0xff) != 0;
    const std::optional<EntryView> after = ReadEntry(read, c.shooter, c.key);
    // The key read again after a shoot: the function's first act is a call of the object's own (slot +0x20).
    std::uint32_t key = c.key;
    std::array<std::uint64_t, kTailWords> tail = c.tail;
    if (c.fn == Fn::shoot && c.params != 0) {
        Get(read, c.params + addr::kShootKey, key);
        Get(read, c.params + addr::kShootKey, tail);
    }
    const std::optional<EntryView> after_key = key != c.key ? ReadEntry(read, c.shooter, key) : std::nullopt;

    std::lock_guard lock(mutex_);
    ++calls_total_;
    Counts& counts = counts_[{c.fn, c.maker_name}];
    ++counts.calls;
    ++(done ? counts.done : counts.refused);

    std::string text = std::string(FnText(c.fn)) + " " + c.maker_name + " " + Hex(c.maker) + " (" +
                       RoleText(c.maker) + ")" + (c.own_call ? " OUR CALL" : "") + " key " + Hex(c.key) + " index " +
                       std::to_string(c.index) + (c.uses_index ? "" : " (first prepared)") + " -> " +
                       (c.fn == Fn::prepare ? std::to_string(answer) : done ? "yes" : "no") + ", shooter " +
                       Hex(c.shooter) + " vt " + Where(c.vtable, main_base) + " from " + Where(c.from, main_base);
    if (key != c.key) {
        text += ", key now " + Hex(key);
    }
    const bool changed = c.before.has_value() != after.has_value() || (after && !Same(*c.before, *after));
    if (!c.before) {
        text += "; no such key";
    } else {
        text += "; before " + Text(*c.before, main_base);
        text += !after ? "; after: gone" : changed ? "; after " + Text(*after, main_base) : "; after the same";
    }
    if (after_key) {
        text += "; its key's " + Text(*after_key, main_base);
    }
    if (text == last_) {
        ++repeats_;
        return;
    }
    if (repeats_ != 0) {
        Line("(the last call " + std::to_string(std::exchange(repeats_, 0)) + " more times)", now_ms);
    }
    last_ = text;
    Line(std::move(text), now_ms);
    if (c.params == 0) {
        return;
    }
    std::uint32_t& n = words_[std::string(FnText(c.fn)) + c.maker_name + Hex(c.key) + (done ? "y" : "n")];
    if (n++ >= kWordsFirst && n % kWordsEvery != 0) {
        return;
    }
    std::string words = std::string(FnText(c.fn)) + " params " + Hex(c.params) + " +0: " + Words(c.head);
    if (c.fn == Fn::prepare) {
        words += "; +8's: " + Where(c.head[1], main_base) + " = " + Where(c.extra[0], main_base) + " " +
                 Words(c.extra) + "; +0x1930: " + Words(c.tail);
    } else {
        words += "; class " + Where(c.head[0], main_base) + "; +0x1928: " + Words(c.tail);
        if (tail != c.tail) {
            words += "; after: " + Words(tail);
        }
    }
    Line(std::move(words), now_ms);
}

void Probe::LookAt(const Reader& read, std::uint64_t actor, const std::string& who, std::uint64_t main_base,
                   std::uint64_t now_ms, std::map<std::uint64_t, Seen>& seen, bool nested) {
    const std::uint64_t shooter = ShooterOf(read, actor);
    if (shooter == 0 || seen.contains(actor)) {
        return;
    }
    Seen now{shooter, ReadEntries(read, shooter)};
    const auto was = seen_.find(actor);
    for (const EntryView& e : now.entries) {
        const EntryView* old = nullptr;
        if (was != seen_.end() && was->second.shooter == shooter) {
            const auto it = std::find_if(was->second.entries.begin(), was->second.entries.end(),
                                         [&](const EntryView& x) { return x.key == e.key; });
            old = it != was->second.entries.end() ? &*it : nullptr;
        }
        if (old == nullptr || !Same(*old, e)) {
            Line("HOLDS " + who + " " + Text(e, main_base), now_ms);
        }
    }
    const std::vector<EntryView> entries = now.entries;
    seen.emplace(actor, std::move(now));
    if (nested) {
        return;
    }
    // A bullet's own shooter (its fields and guard-breaks).
    for (const EntryView& e : entries) {
        for (const RecordView& r : e.records) {
            if (r.shot != 0) {
                LookAt(read, r.shot, who + "'s " + r.name + " " + Hex(r.shot), main_base, now_ms, seen, true);
            }
        }
    }
}

void Probe::Look(const Reader& read, std::uint64_t main_base, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    if (now_ms - looked_ms_ >= kLookMs) {
        looked_ms_ = now_ms;
        std::map<std::uint64_t, Seen> seen;
        for (const Member& m : members_) {
            LookAt(read, m.actor, m.name + " " + Hex(m.actor) + " (" + RoleText(m.actor) + ")", main_base, now_ms,
                   seen, false);
        }
        seen_ = std::move(seen);
    }
    if (now_ms - counts_ms_ >= kCountsMs && calls_total_ != calls_said_) {
        counts_ms_ = now_ms;
        calls_said_ = calls_total_;
        if (repeats_ != 0) {
            Line("(the last call " + std::to_string(std::exchange(repeats_, 0)) + " more times)", now_ms);
            last_.clear();
        }
        std::string text = "COUNTS " + std::to_string(calls_total_) + " calls";
        for (const auto& [which, c] : counts_) {
            text += "; " + std::string(FnText(which.first)) + " " + which.second + " " + std::to_string(c.calls) +
                    " (" + std::to_string(c.done) + " done, " + std::to_string(c.refused) + " refused)";
        }
        if (dropped_ != 0) {
            text += "; " + std::to_string(dropped_) + " lines not logged (too many)";
        }
        lines_.push_back(std::move(text));  // (always: one each kCountsMs)
    }
}

std::vector<std::string> Probe::TakeLines() {
    std::lock_guard lock(mutex_);
    return std::exchange(lines_, {});
}

std::map<std::pair<Fn, std::string>, Counts> Probe::GetCounts() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

void Probe::Reset() {
    std::lock_guard lock(mutex_);
    members_.clear();
    seen_.clear();
    last_.clear();
    repeats_ = 0;
}

Probe& TheProbe() {
    static Probe probe;
    return probe;
}

}  // namespace CoopShooterProbe

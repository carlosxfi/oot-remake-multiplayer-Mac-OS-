// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_beaten.h"

#include <algorithm>
#include <variant>

#include "coop_boss.h"
#include "coop_quests.h"
#include "coop/protocol/codec.hpp"

namespace CoopBossBeaten {

namespace {

#include "coop_boss_beaten.inc"

bool Has(std::span<const std::uint8_t> rooms, std::uint8_t room) {
    return std::find(rooms.begin(), rooms.end(), room) != rooms.end();
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Row 138 (4): the alive value of a set's Bool for a quest at `quest_step` (a step in the set's `below`), or nothing
// when it is left alone (see Apply::HostReset).
std::optional<bool> AliveValue(const BeatenSet& set, const BeatenBool& b, std::uint32_t quest_step) {
    const std::string_view name(b.name);
    if (b.value) {
        if (EndsWith(name, "_BossIn")) {
            return std::nullopt;  // the intro: a save from after it, before the kill, has it
        }
        return false;
    }
    // `<quest>_IsBefore_<Step>`: true while the quest is before that step.
    const std::string prefix = std::string(set.quest_name) + "_IsBefore_";
    if (!StartsWith(name, prefix)) {
        return std::nullopt;
    }
    const CoopQuests::Quest* quest = CoopQuests::Find(set.quest);
    if (quest == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::int32_t> now = CoopQuests::Rank(*quest, quest_step);
    const std::optional<std::int32_t> at =
        CoopQuests::Rank(*quest, coop::protocol::flag_hash(name.substr(prefix.size())));
    if (!now || !at) {
        return std::nullopt;
    }
    return *now < *at;
}

} // namespace

std::span<const BeatenSet> Sets() {
    return kSets;
}

const BeatenSet* ForRoom(std::uint8_t room) {
    for (const BeatenSet& s : kSets) {
        if (s.room == room) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<std::string_view> DeathScene(std::uint8_t room) {
    if (ForRoom(room) == nullptr) {
        return std::nullopt;
    }
    if (const CoopBoss::Room* where = CoopBoss::RoomByIndex(room)) {
        return std::string_view(where->scene);
    }
    switch (room) {
    case 8:
        return std::string_view("InsideGanonCastle");  // Ganondorf's room (Room_26, Create_Ganondorf)
    case 9:
        return std::string_view("FinalBattleField");  // Ganon (FinalBattleField_* logic, Banc GanonCastle/)
    default:
        return std::nullopt;
    }
}

bool KeptScene(std::uint8_t room, std::string_view scene) {
    const std::optional<std::string_view> death = DeathScene(room);
    if (!death) {
        return true;  // no set: never given anywhere
    }
    if (room == 8 || room == 9) {
        return scene == "InsideGanonCastle" || scene == "CrumblingGanonCastle" || scene == "FinalBattleField";
    }
    return scene == *death;
}

bool Record::Add(std::uint8_t room) {
    if (ForRoom(room) == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (Has(rooms_, room)) {
        return false;
    }
    rooms_.push_back(room);
    return true;
}

void Record::Clear() {
    std::lock_guard lock(mutex_);
    rooms_.clear();
}

std::vector<std::uint8_t> Record::Rooms() const {
    std::lock_guard lock(mutex_);
    return rooms_;
}

std::optional<std::uint8_t> ProvenDeath(const coop::protocol::Message& m) {
    namespace proto = coop::protocol;
    const auto in_its_dungeon = [](std::uint8_t room, const std::string& scene) -> std::optional<std::uint8_t> {
        const std::optional<std::string_view> where = DeathScene(room);
        if (!where || *where != scene) {
            return std::nullopt;
        }
        return room;
    };
    const auto* s = std::get_if<proto::BossScene>(&m);
    if (s == nullptr ||
        (s->kind != proto::BossSceneKind::outro_start && s->kind != proto::BossSceneKind::outro_end)) {
        return std::nullopt;  // never BOSS_ROOM defeated: a game also sends it on a guess
    }
    return in_its_dungeon(s->boss, s->scene);
}

Record& TheRecord() {
    static Record record;
    return record;
}

bool InDungeon(std::uint8_t room, std::string_view scene) {
    if (CoopBoss::RoomByIndex(room) == nullptr) {
        return false;
    }
    const std::optional<std::string_view> death = DeathScene(room);
    return death && *death == scene;
}

std::vector<Write> Apply::Check(const std::optional<std::string>& scene, bool place_known,
                                std::span<const std::uint8_t> beaten,
                                const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read,
                                const std::function<bool(std::uint8_t)>& near,
                                const std::function<bool(std::uint8_t)>& death_here) {
    if (!scene) {
        scene_.reset();  // loading: the scene after it (the same one too) checks again
        loaded_.clear();
        checked_.clear();
        return {};
    }
    if (!scene_ || *scene_ != *scene) {
        scene_ = scene;
        loaded_.assign(beaten.begin(), beaten.end());  // a later record change waits for the next load
        checked_.clear();
    }
    if (!place_known) {
        return {};
    }
    // Elsewhere: the rooms the record held when this scene loaded. In a dungeon boss's own scene: live, the record
    // as it is now (rows 136/137).
    std::vector<std::uint8_t> due = loaded_;
    for (const std::uint8_t room : beaten) {
        if (InDungeon(room, *scene) && !Has(due, room)) {
            due.push_back(room);
        }
    }
    std::vector<Write> out;
    // Row 138 (4): the host's older save, a boss alive in it: its Bools back (HostReset).
    for (auto it = reverts_.begin(); it != reverts_.end();) {
        const std::uint8_t room = *it;
        const BeatenSet* set = ForRoom(room);
        if (set == nullptr || Has(beaten, room)) {
            it = reverts_.erase(it);  // beaten again since the reset: Apply gives the set below
            continue;
        }
        if (InDungeon(room, *scene) && near(room)) {
            ++it;  // in (or before) the boss room: once we are out of it
            continue;
        }
        const std::optional<std::uint32_t> step = read(set->quest);
        if (!step) {
            ++it;  // unread: the next frame
            continue;
        }
        it = reverts_.erase(it);
        if (std::find(set->below.begin(), set->below.end(), *step) == set->below.end()) {
            continue;  // the host's save has the boss beaten (or the quest past it): ours stays
        }
        for (const BeatenBool& b : set->bools) {
            const std::optional<bool> alive = AliveValue(*set, b, *step);
            const std::optional<std::uint32_t> now = read(b.hash);
            if (alive && now && (*now != 0) != *alive) {
                out.push_back(Write{room, b.hash, false, *alive ? 1u : 0u, b.name, true});
            }
        }
    }
    for (const std::uint8_t room : due) {
        if (!Has(beaten, room)) {
            continue;  // the record was cleared since (the host loaded a save): that boss fights again
        }
        const BeatenSet* set = ForRoom(room);
        if (set == nullptr || Has(checked_, room)) {
            continue;
        }
        if (InDungeon(room, *scene)) {
            if (near(room) && (!death_here || death_here(room))) {
                continue;  // in (or before) the boss room while our own death of it may run (D9): after it
            }
        } else if (KeptScene(room, *scene)) {
            continue;  // Ganondorf's and Ganon's end sequence: the next scene gives it
        }
        checked_.push_back(room);
        for (const BeatenBool& b : set->bools) {
            const std::optional<std::uint32_t> now = read(b.hash);
            if (now && (*now != 0) != b.value) {
                out.push_back(Write{room, b.hash, false, b.value ? 1u : 0u, b.name});
            }
        }
        const std::optional<std::uint32_t> step = read(set->quest);
        if (step && std::find(set->below.begin(), set->below.end(), *step) != set->below.end()) {
            // Two sets of one quest (Ganondorf's Step06, Ganon's Complete), both read before either is written: one
            // write, the furthest step (the one the other's step is below), whatever order the record holds them in.
            const auto same = std::find_if(out.begin(), out.end(),
                                           [&](const Write& w) { return w.is_enum && w.hash == set->quest; });
            if (same == out.end()) {
                out.push_back(Write{room, set->quest, true, set->step, set->quest_name});
            } else if (std::find(set->below.begin(), set->below.end(), same->value) != set->below.end()) {
                *same = Write{room, set->quest, true, set->step, set->quest_name};
            }
        }
    }
    return out;
}

void Apply::HostReset(bool quests_shared) {
    reverts_.clear();
    if (!quests_shared) {
        return;  // LICHII_COOP_QUESTS=0: no quest follows the host, each game's boss stays its own
    }
    for (const BeatenSet& s : kSets) {
        if (CoopQuests::Find(s.quest) != nullptr) {
            reverts_.push_back(s.room);
        }
    }
}

void Apply::Reset() {
    reverts_.clear();
    scene_.reset();
    loaded_.clear();
    checked_.clear();
}

} // namespace CoopBossBeaten

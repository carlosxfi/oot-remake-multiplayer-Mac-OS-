// SPDX-License-Identifier: GPL-3.0-or-later
// A dungeon boss beaten by the group, given to a game that did not see it die (G2, player report #241; docs/sync-coverage.md
// D9, docs/dungeons-boss.md "Beaten by the group").
//
// D9 keeps a boss's end state each game's own: every game in the boss's area runs the boss's own death setting
// itself (coop_boss_scenes.h), which sets that game's flags, quest step and warp exactly as a solo kill does. A game
// that did not (its player was elsewhere in the world or in the dungeon, or it is an old build) still holds the boss
// alive while the shared heart container is taken, and a reload brings the fight back with no container to win.
//
// So the group's boss death, only a proven one (ProvenDeath: never BOSS_ROOM `defeated`, which a game also sends on a
// guess), is kept in Record() (the server keeps it for players who join later and forgets it when the host loads a
// save), and Apply gives a game
// whose own flags lack it the boss's whole beaten set: what that boss's death setting writes in the dungeon, read from
// the game's romfs (tools/puppet_pack/boss_beaten.py, coop_boss_beaten.inc). One way only: each Bool is written to
// its beaten value when it is not there, and the dungeon quest is raised to the step the death moves it to only from
// a step before it (a finished dungeon, Complete, is never stepped back). Never in the boss's own scene (its room's
// logic is loaded there, maybe a fight): only while our Link stands in another scene, so the dungeon loads next as a
// beaten save's: no boss, the blue warp there. Pure: the hook reads and writes the flags.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopBossBeaten {

struct BeatenBool {
    std::uint32_t hash;  // flag_hash of the name
    bool value;          // what the boss's death writes
    const char* name;
};

struct BeatenSet {
    std::uint8_t room;  // BOSS_SCENE's boss index: CoopBoss::Rooms() index (BOSS_ROOM's room) for 0 to 7; 8 Ganondorf, 9 Ganon
    const char* boss;
    std::uint32_t quest;  // the dungeon quest's Enum (GameDataList)
    const char* quest_name;
    std::uint32_t step;  // flag_hash of the step the death moves the quest to
    const char* step_name;
    std::span<const std::uint32_t> below;  // the quest's steps before it: NotReady up to the boss step
    std::span<const BeatenBool> bools;
};

// The ten bosses' sets, by boss index: the eight dungeon bosses (their room) and, with no room in CoopBoss::Rooms(),
// Ganondorf (8: Defeat_Ganon to Step06) and Ganon (9: to Complete). Defeat_Ganon is each game's own (coop_quests.cpp),
// so a player who was not in the final fights got none of it before.
std::span<const BeatenSet> Sets();
const BeatenSet* ForRoom(std::uint8_t room);

// The scene the boss dies in (BOSS_SCENE's scene of its proven death): a room's dungeon scene for 0 to 7,
// InsideGanonCastle for Ganondorf, FinalBattleField for Ganon. Nothing for another index.
std::optional<std::string_view> DeathScene(std::uint8_t room);
// Whether `scene` is one the boss's set is never given in: its dungeon scene for 0 to 7 (its room's logic is loaded,
// maybe a fight); for Ganondorf and Ganon the whole end sequence, InsideGanonCastle, CrumblingGanonCastle (the
// escape, which reads Defeat_Ganon's step live) and FinalBattleField.
bool KeptScene(std::uint8_t room, std::string_view scene);
// Whether `scene` is dungeon boss `room`'s own dungeon scene (0 to 7 only; Ganondorf and Ganon have no room): where
// Apply gives the set live, outside the boss's area (rows 136/137).
bool InDungeon(std::uint8_t room, std::string_view scene);

// The room whose boss `m` proves dead, or nothing. The same rule as the server's (server/session.cpp):
// a BOSS_SCENE outro_start or outro_end: a game took the boss's death event itself (its own death setting or
// Dm_<Dungeon>_BossOut, by name; a copy's game only once its owner died) and, for outro_end, that game's death event
// ended (only the game that started it sends it). In that boss's DeathScene (not the boss rush, GanondorfStage or
// GanonStage). Its boss index 0 to 7 is the room (coop_boss_scenes.cpp kBosses), 8 and 9 are Ganondorf and Ganon. The server keeps the outro_starts and tells a joiner each as an
// outro_end from the same player (its cutscene rules do nothing with an end of a death they never saw).
// BOSS_ROOM defeated is never one: it only says that player's fight is over (maybe a guess: a long event, no
// reading). Our own game's proof is also Sync's beaten_here (the walk read the boss gone).
std::optional<std::uint8_t> ProvenDeath(const coop::protocol::Message& m);

// The rooms whose boss the group beat this session. BOSS_ROOM messages come on the network thread; the hook's frame
// reads. Cleared when the host loads a save and when we leave the session (the server tells a joiner again).
class Record {
public:
    // True when `room` is new (a boss room of the table).
    bool Add(std::uint8_t room);
    void Clear();
    std::vector<std::uint8_t> Rooms() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> rooms_;
};
Record& TheRecord();

// One write to our game: a Bool (`is_enum` false, `value` 0 or 1) or the quest's Enum (`value` a step's hash).
struct Write {
    std::uint8_t room;
    std::uint32_t hash;
    bool is_enum;
    std::uint32_t value;
    const char* name;
    bool revert = false;  // #138 (4): a Bool put back to its alive value after the host's older save (HostReset)
};

class Apply {
public:
    // At each hook frame: `scene` nothing while loading; `place_known` once Link's position reads. Only at a scene
    // load (boss-away-crash, review of c78d17c4): the rooms `beaten` holds when a scene is first seen are the ones
    // given in it, each once, once Link's place reads, in a scene that is not the boss's. A boss beaten while we stand
    // in a scene waits for the next load: some of the set is read live outside the dungeon (Gohma_DefeatBoss by
    // Kokiri Forest's Npc_Kokiri_011, HavePlayedEvent.Dm_DodongoCave_BossOut by Death Mountain Trail's
    // DeathMountainTrail_Child_Prefab_6f34, SageOfForest_IsBefore_Step08 by Kokiri Forest's Level_All_01 and
    // Level_Area_01), and in the game alone those never change while such a scene is loaded: a kill is always in
    // another scene. `read(hash)` is the flag's value now (nothing: unread, left alone). Returns what to write.
    //
    // Rows 136/137 (boss-away-0115): in a dungeon boss's own scene (0 to 7) the set is given LIVE, as soon as the
    // record has it and our Link is not in that boss's area (`near(room)`: the room and the room before its door,
    // CoopBossScenes::InArea). Read from the romfs: the boss room's logic holds its flags live and acts on them
    // (Gohma's Logic/InsideTheDekuTree_Level_Room_L_Common_865f: Create_WarpPrefab <- Gohma_DefeatBoss, Delete_Gohma <-
    // Get_StoneOfForest_IsAfter_DefeatBoss_Exp, the container while the shared InsideTheDekuTree_Heart_Container_Obtained
    // is 0), as it does when the game's own death setting writes them in that scene. Waiting for another scene's
    // load left a player who was in the dungeon at the kill with the shared quest past DefeatBoss (the boss deleted)
    // and no Gohma_DefeatBoss (no warp): a boss room with no boss, no warp and no way out, and a reload into the
    // dungeon never gave it (#136). In the area D9 runs the boss's own death there.
    //
    // Row 136 (boss-warp-136): in the area the set waits only while our game's own death of that boss may still run
    // (`death_here(room)`: D9 asked it, it plays, it waits for our copy to read dead, an outro_start not yet taken,
    // or any event runs; CoopBoss::DeathMayRun). D9 asks the death only of a game in the area when the death
    // starts, so a game that came into the area after it (walked in during the others' death cutscene, fell into
    // Gohma's room later, joined the session there, Game Over / a load into the room) or whose ask was refused (the
    // death flow alone: no quest step, no warp) had nothing to run it and waited for a walk out of a room whose only
    // way out is the warp. Read from the romfs (as above), the room's logic makes the warp and the container from
    // the flags live, as when the boss's own death setting writes them in that room: once no death of ours can run,
    // the set is given there too. With no `death_here` (empty) the area always waits (the older rule).
    std::vector<Write> Check(const std::optional<std::string>& scene, bool place_known,
                             std::span<const std::uint8_t> beaten,
                             const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read,
                             const std::function<bool(std::uint8_t)>& near,
                             const std::function<bool(std::uint8_t)>& death_here = {});
    // Row 138 (4): the host loaded a save. The beaten set's Bools are each game's own, so its FLAG_RESET never sets
    // them back, while a boss's quest that is shared (`quests_shared`: CoopQuests, Get_StoneOfForest,
    // Get_StoneOfFire, Get_StoneOfWater, SageOfForest, SageOfFire) is written to the host's step. A guest that held
    // the boss beaten (its own kill, D9 or this class's set) then had Gohma deleted by nothing and the blue warp
    // up, or the warp AND Gohma: the room's logic reads both live. So for each such boss, at the next Check where
    // the set would be given (out of its area in its own dungeon, anywhere else at once) and the quest now reads a
    // step in `below` (the boss alive in the host's save) and the record does not have it again: each Bool the death
    // raises (true) and the game holds goes back to false, and a `<quest>_IsBefore_<Step>` Bool the death clears goes
    // back to true when the quest is before that step. Left alone: the boss's intro (`..._BossIn`: a save from after
    // the intro has it), Bools the death clears with no rule for their value before (King Dodongo's BossAppear).
    // Quests each game keeps (SageOfWater, SageOfShadow, SageOfSpirit, Defeat_Ganon) are not reset by the host: the
    // guest's boss stays as its own save has it, quest and Bools together (no mixed room). On the host its own save
    // holds the alive values: nothing to write.
    void HostReset(bool quests_shared);
    void Reset();

private:
    std::vector<std::uint8_t> reverts_;  // HostReset's rooms not yet checked
    std::optional<std::string> scene_;
    std::vector<std::uint8_t> loaded_;   // the record's rooms when this scene was first seen: the only ones given here
    std::vector<std::uint8_t> checked_;  // rooms done since this scene load
};

} // namespace CoopBossBeaten

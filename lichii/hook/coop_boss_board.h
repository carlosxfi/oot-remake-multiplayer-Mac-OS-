// SPDX-License-Identifier: GPL-3.0-or-later
//
// A boss's move state goes with the boss (row 149, King Dodongo rolling in the lava; docs/dungeons-boss.md
// "Blackboard move state"). No emulator code: tested in tests/lichii/boss_board_test.cpp.
//
// A boss's AI keeps the state its moves are chosen from in the actor's blackboard, which is each game's own. King
// Dodongo's roll does not follow the walls by itself: it rolls STRAIGHT to a corner number kept there
// (DungeonBossDodongo_CurrentTargetCornerNum, the way round in DungeonBossDodongo_IsReverse; pack
// Enemy_DungeonBoss_Dodongo, AI/DungeonBossDodongo.action.spinattack.module.ainb: SelectNextCornerNumber,
// GetCornerPosFromNumber, OneShotTargetSetFromPosAndRot). The native boss sync (coop_boss_native.h) carried his
// verbs and the mover his place, but never that number: a copy counted corners by itself, and the game made his owner
// rolled to ITS stale corner from wherever he stood, a straight line across the lava pit, where no wall ends the
// roll (run 20261123-043946; row 325's fix removed one way in, not this cause).
//
// So the values go with the boss:
// - the owner's game reads the keys of kKeys its boss has, each frame, and sends them in the boss's samples
//   (OBJECT_SAMPLE component 25, coop::protocol::BossBoard; the server keeps them for HANDOVER_STATE);
// - a copy keeps the owner's last values and changes nothing in its own game (its place and verbs are the owner's);
// - a game made the owner puts those values into its own blackboard first, through the game's own setters, called
//   by our caller on the game's calc thread before the boss table's caller (kBossBoard < kBossTable), so its brain
//   decides from where the old owner's stood. Until they are in, it sends them on unchanged, never its stale own.
// Every boss with such state is in kKeys (Twinrova's battle state is carried as orders, coop_boss_orders.h).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossBoard {

using coop::protocol::BossBoard;
using coop::protocol::BossBoardValue;

namespace addr {
// Build e0f3ccfaf50065ae. An actor's blackboard: [[[actor + 0x270]] + 0x10], none when 0 or -1 (main+0xd6d110).
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kHolderBlackboard = 0x10;
// Its s32 group: name count (s32), names (char* array), value count (s32), values (s32 array).
inline constexpr std::uint64_t kS32NameCount = 0x98;
inline constexpr std::uint64_t kS32Names = 0xa0;
inline constexpr std::uint64_t kS32ValueCount = 0xa8;
inline constexpr std::uint64_t kS32Values = 0xb0;
// Its bool group, the same with one byte per value.
inline constexpr std::uint64_t kBoolNameCount = 0x188;
inline constexpr std::uint64_t kBoolNames = 0x190;
inline constexpr std::uint64_t kBoolValueCount = 0x198;
inline constexpr std::uint64_t kBoolValues = 0x1a0;
// The game's setters: x0 the blackboard, x1 a pointer to the key's name pointer, x2 a pointer to the value (s32,
// or one byte). They find the key by name, leave a locked key alone, store and count the change.
inline constexpr std::uint64_t kSetS32 = 0x264f170;  // main+
inline constexpr std::uint64_t kSetBool = 0xd6cb40;  // main+
// Our value block in guest memory (made once with the game's malloc).
inline constexpr std::uint64_t kMalloc = 0x14c6c90;  // main+, as coop_boss_native.h
inline constexpr std::size_t kBlockSize = 0x10;
// A group with more names than this is not read (a wrong pointer).
inline constexpr std::int32_t kMaxNames = 1024;
}  // namespace addr

enum class Kind : std::uint8_t { s32, boolean };

// One blackboard key that is carried. `id` is its number on the wire: never reused, never changed.
struct Key {
    std::uint16_t id;
    std::string_view boss;  // the actor's name
    std::string_view name;  // the key's name in its blackboard
    Kind kind;
};

// What a boss chooses its next move from: counters, indexes, the way round, a phase, a forced next move. Not the
// one-frame triggers its actions raise from its own contacts, not what follows from its place or animation (those
// are the copy's own, from the owner's place and verbs), no actor or perception ids (each game's own).
inline constexpr std::array kKeys{
    // King Dodongo: the corner his roll goes to and the way round; a baby Dodongo spawn owed after the roll.
    Key{1, "Enemy_DungeonBoss_Dodongo", "DungeonBossDodongo_CurrentTargetCornerNum", Kind::s32},
    Key{2, "Enemy_DungeonBoss_Dodongo", "DungeonBossDodongo_IsReverse", Kind::boolean},
    Key{3, "Enemy_DungeonBoss_Dodongo", "DungeonBossDodongo_IsAfterRollingSpawn", Kind::boolean},
    // Volvagia: the battle state, the hole it holds, its ground moves in a row, the forced sky attack, the turn.
    Key{10, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_BattleState", Kind::s32},
    Key{11, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_ContinuousGroundActionNum", Kind::s32},
    Key{12, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_HoldingAppearHoleIndex", Kind::s32},
    Key{13, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_ForceSkyAttackType", Kind::s32},
    Key{14, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsTurnLeft", Kind::boolean},
    Key{15, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsActFirstSkyAttack", Kind::boolean},
    Key{16, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsInfinitySkyAttack", Kind::boolean},
    Key{17, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsAppearFromCenter", Kind::boolean},
    Key{18, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsOnceAppearSign", Kind::boolean},
    Key{19, "Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsActFallRockOnce", Kind::boolean},
    // Gohma: the egg she lays next, the life her ground mode began at, the battle she is forced into.
    Key{30, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_EggIndex", Kind::s32},
    Key{31, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_GroundModeEnterLife", Kind::s32},
    Key{32, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsForceLandBattle", Kind::boolean},
    Key{33, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsForceClimb", Kind::boolean},
    Key{34, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsForceCeilingBattle", Kind::boolean},
    Key{35, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsClimbFromOutside", Kind::boolean},
    Key{36, "Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsJumpOffImmediately", Kind::boolean},
    // Phantom Ganon: the light ball rally, the aura, his hits taken on the ground, the forced special attack.
    Key{50, "Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_ShootLioghtBallIndex", Kind::s32},
    Key{51, "Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_CurrentDarknessAuraType", Kind::s32},
    Key{52, "Enemy_DungeonBoss_PhantomGanon", "DamageCountInLandBattle", Kind::s32},
    Key{53, "Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsCompletedFirstRally", Kind::boolean},
    Key{54, "Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsFirstAttackSecondHalfBattle", Kind::boolean},
    Key{55, "Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsDoFirstAttackAfterForceSpecialAttack",
        Kind::boolean},
    Key{56, "Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsForceSpecialAttack", Kind::boolean},
    // Ganondorf: the same common state, his own light ball index, the final phase and its attack count.
    Key{70, "Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_ShootLioghtBallIndex", Kind::s32},
    Key{71, "Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_CurrentDarknessAuraType", Kind::s32},
    Key{72, "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBossGanondorf_ShootLioghtBallIndex", Kind::s32},
    Key{73, "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBossGanondorf_FinalPhaseAttackActionNum", Kind::s32},
    Key{74, "Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsCompletedFirstRally", Kind::boolean},
    Key{75, "Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsFirstAttackSecondHalfBattle", Kind::boolean},
    Key{76, "Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsDoFirstAttackAfterForceSpecialAttack",
        Kind::boolean},
    Key{77, "Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsForceSpecialAttack", Kind::boolean},
    Key{78, "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganondorf_UseForceSpecialAttack", Kind::boolean},
    Key{79, "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganondorf_IsFinalPhaseStart", Kind::boolean},
    Key{80, "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganondorf_IsUnlockedBasicAttack", Kind::boolean},
    // Morpha: the half phase, its attack counters and which tentacle attack is next.
    Key{90, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_CheapCount", Kind::s32},
    Key{91, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_NextTentacleAttack", Kind::s32},
    Key{92, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_NowTentacleAttack", Kind::s32},
    Key{93, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsHalfPhase", Kind::boolean},
    Key{94, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsHalfPhaseStart", Kind::boolean},
    Key{95, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsHalfPhaseStartExecuted", Kind::boolean},
    Key{96, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsFirstAttackExeced", Kind::boolean},
    Key{97, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsSlamCleared", Kind::boolean},
    Key{98, "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsSpinCleared", Kind::boolean},
    // Bongo Bongo: the attack he is forced to next and with which hand.
    Key{110, "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_ForceAttackCommand", Kind::s32},
    Key{111, "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_IsForceRightHand", Kind::boolean},
    Key{112, "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_IsForceLeftHand", Kind::boolean},
    Key{113, "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_IsForceWait", Kind::boolean},
    // Ganon: his sequence, the final rush's hits, each weak point's life and whether it is broken, his charge.
    Key{130, "Enemy_DungeonBoss_Ganon", "CurrentSequenceType", Kind::s32},
    Key{131, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_FinalRushCurrentHitNum", Kind::s32},
    Key{132, "Enemy_DungeonBoss_Ganon", "LifeAtChanceWaitStart", Kind::s32},
    Key{133, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_RightHandWeakPointLife", Kind::s32},
    Key{134, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_LeftHandWeakPointLife", Kind::s32},
    Key{135, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_ThirdEyeWeakPointLife", Kind::s32},
    Key{136, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsBrokenRightHandWeakPoint", Kind::boolean},
    Key{137, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsBrokenLeftHandWeakPoint", Kind::boolean},
    Key{138, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsBrokenThirdEyeWeakPoint", Kind::boolean},
    Key{139, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsForbidChargeAttack", Kind::boolean},
    Key{140, "Enemy_DungeonBoss_Ganon", "DungeonBossGanon_CanRushAttack", Kind::boolean},
    // Barinade: its form, the jellyfish move and the arms left.
    Key{150, "Enemy_DungeonBoss_Barinade", "CurrentFormType", Kind::s32},
    Key{151, "Enemy_DungeonBoss_Barinade", "CurrentJellyfishActionType", Kind::s32},
    Key{152, "Enemy_DungeonBoss_Barinade", "RemainArmCount", Kind::s32},
    Key{153, "Enemy_DungeonBoss_Barinade", "IsBeamAttackAfterChanceTime", Kind::boolean},
    Key{154, "Enemy_DungeonBoss_Barinade", "IsKnockBackAfterChanceTime", Kind::boolean},
};

// Ids from 1, rising (the wire's order), a boss's keys in one run, and no boss with more than one component holds.
constexpr bool KeysFit() {
    std::uint16_t before = 0;
    std::size_t run = 0;
    for (std::size_t i = 0; i < kKeys.size(); ++i) {
        if (kKeys[i].id <= before) return false;
        before = kKeys[i].id;
        run = i != 0 && kKeys[i].boss == kKeys[i - 1].boss ? run + 1 : 1;
        if (run > coop::protocol::kMaxBossBoardValues) return false;
    }
    return true;
}
static_assert(KeysFit());

// The keys of one boss, in id order (none: nothing of it is carried).
std::vector<Key> KeysOf(std::string_view boss);

// The planner, without game memory (tested): which of the old owner's values a game made the owner puts in, in
// order. Only keys of this boss, each once; a value our game already has is left alone (`ours`: what our blackboard
// holds now, by key id; a key not in it could not be read and is passed over). A bool goes in as 0 or 1.
std::vector<BossBoardValue> Plan(std::span<const Key> keys, const BossBoard& theirs, const BossBoard& ours);

enum class Role : std::uint8_t { owner, copy };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;
    std::string name;
};

struct Counts {
    std::uint64_t handed = 0;    // bosses made ours whose old owner's values were looked at
    std::uint64_t put = 0;       // values put into our blackboard (they differed)
    std::uint64_t same = 0;      // values our game already had
    std::uint64_t refused = 0;   // values the game's setter left alone (a locked key)
    std::uint64_t unread = 0;    // a boss whose blackboard or keys were not found
};

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossBoard;

    // The walk's bosses this round. One no longer listed is forgotten. A copy made ours: the old owner's last
    // values are put in first (Step).
    void Publish(std::span<const Member> members);

    // The owner's values from its samples or the hand-over state (coop_objects_sync.cpp BossBoardIn). For a copy
    // they are kept; for one of ours (a hand-over state that came after the news) they are put in as well.
    void Receive(std::uint64_t actor, const BossBoard& board);

    // Ours: its values for its samples. While the old owner's are still to put in: those. Nothing: no key of it is
    // carried, or none was read yet.
    std::optional<BossBoard> OwnerBoard(std::uint64_t actor) const;

    // The caller (coop_callers.h contract): reads our bosses' values; puts the old owner's into a boss made ours.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base);

    // A new game process: every boss, the block and any call in flight are dropped.
    void Reset();

    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();

private:
    struct Slot {
        Key key;
        std::uint64_t name_at = 0;  // where the group's names array holds this key's name pointer (0: not found)
        std::uint32_t index = 0;
    };
    struct Entry {
        Member member;
        std::vector<Slot> slots;         // its keys (KeysOf), found in `blackboard`
        std::uint64_t blackboard = 0;    // the one the slots were found in (0: not yet)
        bool unread_said = false;
        BossBoard in;                    // the owner's last values (a copy's; kept when made ours)
        bool due = false;                // ours: `in` is still to put in
        std::vector<BossBoardValue> todo;  // the plan being put in, front first
        bool planned = false;
        std::optional<BossBoard> mine;   // ours: as last read
        std::uint32_t put = 0;           // this hand-over's counts, for its note
        std::uint32_t same = 0;
    };
    void Note(std::string text);
    void DropGame();  // (mutex_ held)
    // Finds the entry's blackboard and its keys in it (false: none now). Reads only.
    bool Find(CoopPuppet::Guest& guest, Entry& e);
    static std::optional<BossBoard> ReadValues(CoopPuppet::Guest& guest, const Entry& e);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    Counts counts_{};
    std::vector<std::string> notes_;
    std::uint64_t main_base_ = 0;
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    bool malloc_failed_ = false;
    enum class Pending : std::uint8_t { none, malloc, set } pending_ = Pending::none;
    std::uint64_t pending_actor_ = 0;
};

Table& TheTable();

}  // namespace CoopBossBoard

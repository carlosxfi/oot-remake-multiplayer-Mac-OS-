# Bosses built for more than one player (design notes, 2026-11-21)

The question asked of every boss stage: **is it fun with two to four players, and if not, how should it change so
the fight feels built around a group** (the way Four Swords builds its fights around several Links)? This page is
the plan; [dungeons-boss.md](dungeons-boss.md) says how the sync works today.

## What every fight has today

- One game runs the boss (its owner); the others show a copy that follows it (native boss sync). Hits from every
  player count, the death plays once in each game, the heart container is one per player.
- **Boss turns** (#308 and `boss-turns-0119`): the boss goes to the next player by number at the end of **every**
  attack in its owner's game (4 s at least with one player, `boss_turn_us`, so a quick combo is not split), so its
  AI fights each player in turn. A player who is down, dead or paused is skipped, and is in the round again at the
  next attack's end once up (their puppet is seen by enemies again then too).
- **Never stuck on a player who cannot fight** (2026-11-21, every enemy): a boss, or any enemy in its fight, leaves
  an owner who goes down or dies at once and one paused after 2 s (`fight_owner_dead_us`, `fight_owner_paused_us`;
  out of a fight the old 10 s and 6 s), for a player up and playing. With nobody up it stays where it is.
- **Plain enemies can take turns too, OFF by default** (`enemy_turns`): the same rule as a boss's, between its
  attacks, among the players within 12 m of it (`enemy_turn_m`). It stays off: a plain enemy's AI chases only its
  owner's Link, so every turn walks it over to the other player and back (the shuttle of report 138, docs/objects.md
  "Sticky target"), where solo a mob keeps the Link it fights; the chase, near and sticky rules already give it to
  the player fighting it. On, the server's `enemies:` line counts them as `enemy turns`. (Also: only a boss or part
  is listed with `object_presence::takes_turns` by the hook, so plain enemies would need that flag first.)
- **A stun hands the boss to the stunner, also right after a turn:** a stun in another player's game (Gohma's eye,
  a ball sent back into a copy Phantom Ganon or Ganondorf) asks the boss over at most every 8 s
  (`boss_hitter_cooldown_us`), counted from the last change that was no turn (`Object::hitter_changed_at`): with a
  turn at every attack, a count from every change refused nearly every stun.
- The difficulty scales by the number of players (2 players: boss life 200%, damage 120%).

What that does not give yet: **an attack aimed at more than one player at once**. The game's AI picks one target,
and a shot or a grab is real only in the owner's game (a copy's own shots are refused, the owner's come as copies),
except the split shots below.

## Tools we have or can build

| Tool | What it does | State |
|---|---|---|
| Turns | the boss fights each player in turn, between attacks | in (proof runs) |
| Turn length | per boss: every attack, every second attack, every N s | to build (a server table by boss) |
| Split shot | an attack's shot made in **every** game, aimed at that game's own Link: each player gets the ball / the rock / the beam at the same moment | in for Phantom Ganon's bolt, shockwaves and balls and Ganondorf's `DungeonBossGanondrof_Bullet_*` (#308, `CoopEnemyShots::IsSplitShot`); the rest to build |
| Half split | a split shot in half the games, the other half the next time (every other attack) | to build on split shot |
| Part targets | a boss of several bodies (Bongo Bongo's hands, Twinrova's sisters, Morpha's tentacles) gives each body its own target player | to build (owner per group member) |
| Stun for all | a stun in one game stuns the boss for everyone (the stun hand-over already moves him to the stunner) | in |

## Boss by boss

### Queen Gohma (Deku Tree, child)
- **Ceiling / wander:** she crawls toward her target and drops eggs. With turns she goes after each player; the
  eggs hatch in every game. Fun: yes, one player stuns the eye with the slingshot, the others slash.
- **Change:** none needed. Later: more eggs with more players (the enemies scale says 125%).

### King Dodongo (Dodongo's Cavern, child)
- **Inhale / fire breath:** aimed at one player; any player can throw the bomb in. **Roll:** chases one player.
- Fun with turns: yes (whoever he faces is the bait, another bombs him). **Change:** turn after **every** roll and
  every breath (turn length = every attack), so nobody waits a whole minute.

### Barinade (Jabu-Jabu, child)
- **Tentacles (3), jellyfish ring, body:** the beam is aimed at one player.
- **Change:** part targets for the tentacles' zaps (each player cuts one); the jellyfish ring spins toward its
  target, so turns every attack. Not tested yet (no save with Barinade's room before his fight).

### Phantom Ganon (Forest Temple, adult)
- **Stage 1, paintings:** he rides out of a painting toward his target; the others can shoot him. With turns each
  player gets a ride at them. Fun: yes.
- **Stage 1, the leap:** he leaps out of a painting and throws a bolt at the room's middle; four shockwaves run
  out from it in a cross. As copies flown on the owner's path the shockwaves hurt nobody but the owner. **Done
  (split shot):** every game makes its own bolt and its own four shockwaves, seen and felt in that game only, so
  each player dodges one cross, the same one, at the same moment.
- **Stage 2, the ball rally:** today one player rallies with him, the others watch. **Done (split shot):** each
  player gets their own ball at the same moment, seen and played in their own game only. The owner's Ganon sends
  it back as alone; a copy's cannot (his AI runs in the owner's game), so a ball sent back into him stuns him in
  that game and the stun hand-over gives him to that player for the sword hits.
- **Stage 2, the goal (asked for, the hardest):** he does not go down until **half of the players** have sent
  their ball back into him. Each game reports its return to the server (a new message, or a DAMAGE 0 with the
  rally's round), the server counts the players of that round, and the knockdown is held in every game (the
  owner's too: its own stun reaction waits) until the count reaches half (2 of 2 or 3, 2 of 4); then everyone sees
  him fall at once and the one who made the last return owns him. Needs: a way to hold the game's own knockdown on
  the owner's Ganon (patch 0007's Life step hold is for life, not for the reaction), and a round count per rally.

### Volvagia (Fire Temple, adult)
- **Holes / head out:** the hammer player hits the head; **flight + fire breath:** at one player; **falling rocks:**
  around one player.
- **Change:** split shot for the falling rocks (rocks around every player); turns every flight.

### Morpha (Water Temple, adult)
- **Tentacle grabs:** one target. **Nucleus:** one player hookshots it out, another slashes it: the best
  two-player moment of the game already.
- **Change:** part targets: with more players, one tentacle per player (the game makes two late in the fight);
  turns every grab.

### Bongo Bongo (Shadow Temple, adult)
- **Two hands, the eye:** both hands must be stunned, then the eye shot. Built for two players already: one takes
  the left hand, one the right.
- **Change:** part targets: the left hand goes for one player, the right for another (today both go for the
  owner). Turns between slams.

### Twinrova (Spirit Temple, adult)
- **Stage 1, Koume and Kotake:** fire and ice shots, three of one kind charge the Mirror Shield.
- **Change:** part targets: each sister has her own target player (two players each duel one sister). With more
  players the sisters swap targets every attack.
- **Stage 2, Twinrova:** one body, one target: turns every attack. Later: a charged Mirror Shield blast from any
  player stuns her for all (stun for all).

### Ganondorf (Ganon's Tower)
- **Energy ball rally, multi-ball, light arrows:** the same as Phantom Ganon's stage 2 (asked for): his ball at
  every player at once (split shot; `DungeonBossGanondrof_Bullet_` is on the list, his own `Ganondorf_Bullet_*`
  names to be read in a run), and he goes down only when half of the players have sent theirs back into him; a
  light arrow from any player stuns him for all.
- Not tested yet (the boss rush stage did not place him from this save).

### Ganon (final battle)
- **Stage 1:** he faces one player; the tail is the weak spot. With turns the player he faces is the bait and the
  others go for the tail: the most Four-Swords fight in the game as it is.
- **Stage 2 (sword lost):** same; turns every swing.

## Findings of the every-stage runs (2026-11-21, two games, the boss rush stages)

- **Synced and dying once for both, with turns at attack ends:** Phantom Ganon (stage 1; stage 2 needs real arrow hits
  in the paintings, a test hit only takes life), Volvagia, Morpha, Bongo Bongo (hands and body change owner
  together), Twinrova (both stages, once her merge was shared), Ganon (both halves).
- **Fixed:** Twinrova's merge (`SageOfSpirit_BossChange`) played in one game only, so the other had no Twinrova;
  every boss's single shots are split shots now; every enemy takes turns and leaves a dead or paused owner at once.
- **The boss rush is not harder by itself:** the bosses are the same actors with the same placement parameters as in
  their dungeons (Phantom Ganon's differ only in `DefaultDungeonRoomIndex`), and no parameter table names the rush;
  only the rush rooms' own logic graphs do. Co-op's scaling applies the same to both.
- **King Dodongo in his rush stage stays in `Wait`** with one player alone too (a single-game run): not co-op. His
  stage's logic (`DodongoStage_497d`) makes him appear through `DreamWorld_BossAppear` from a player's area trigger.
- **Volvagia at 2 players is very hard standing still:** both test Links reached 0 life several times in her fire
  (her breath and floor fire are each game's own, so every player is attacked). Worth a look in the difficulty
  review.
- **Not placed from the test saves:** Barinade (a save at Jabu-Jabu's boss is needed), Ganondorf (his stage did
  not place him).

## Whole-game survey (2026-11-21): every enemy, room event and push puzzle

Three surveys of the romfs (every map, actor pack and event flow) for places tonight's fixes apply. Applied without
a game run (unit tests only), for players to check:

| What | Where it went | Check in a game |
| --- | --- | --- |
| Never mid-attack: no enemy changes owner by a chase, the near rule or a nearer player while its owner's attack runs (a grab or hold stays on its victim), at most 8 s | server `attack_hold_max_us`, `mid_attack` | B hits a Like Like holding A: A freed, A moves after |
| Not applied (geno-fix-0119): boss-style turns for one-on-one fighters (Iron Knuckles, Poe sisters Amy/Beth/Joelle, Stalfos, Lizalfos, Dinolfos, white Wolfos, the club Moblin). Their AI is a plain enemy's (it chases only its owner's Link), so boss rules would pass them between players: the mob shuttle the owner settled off (enemy_turns off) | none: they are plain enemies | no "boss turns:" lines for them |
| Packs under one owner: Lizalfos/Dinolfos, Stalfos and Wolfos of one family within 25 m are one group (root: the lowest id), so their teamwork (messages between their AIs: `IsRequestingCoop`, `IsDisableMeleeBattle`...) runs whole in one game (never the boss bit: plain enemies, no turns; geno-fix-0119) | `PackFamily` in `GroupsOf` | one owner for both of a pair; one attacks at a time |
| Anubis keeps its target (it mirrors the player moving it) | `KeepsTarget` in `Sync::StateOf` | the mirror follows one player |
| Ganon bound for the last blow (`InsideGanonCastle_Dungeon_BossBind0`) shared like the other stage changes | `coop_boss_scenes` entry 12 | both games' Ganon bound |
| The Flare Dancer's fireballs split | `kSplit` | each player gets their own fire |
| The Goron City statue's moves shared | `IsMovable` | both see it pushed |
| Capstans' turns shared (Spirit Temple mirrors, Shadow Temple room 1 statue) | `IsCapstan` shifters | the turn shows; the beam and the "correct" check in the game that follows |
| Per-player room flags no longer shared: the Dark Link rooms' passage flags, the Shadow lifts' crush flag | `coop_dungeon.cpp` | B in room 22/23 while A passes a Dark Link door; B on the boat while A is crushed |

Found and left as they are: Octorok and Deku Scrub shots stay shared (the player shot at sends them back); the
Wallmaster carry-back and the Gerudo Fortress loss are per player; every block puzzle's answer counter is shared;
no other actor reads its state only at load (the turning floor's fix fits nowhere else); no other event turns a
room under a player (the hallway block fits nowhere else); the water level stays a change for everyone.

Still to design (need a game run first):
- Redead and Gibdo: the scream freezes only the owner's Link; every player in its range should freeze in their own
  game. The freeze is the Redead's own (`Enemy_ReDead_Shout`, `ParalysisEnableDistMin/Max`, `ParalysisEnableAngleDiff`
  and their `InTown` values). Planned: if the freeze reaches the owner's Link as a hit (its kind words), each game
  in range gives its own Link that hit (`HurtOurLinkNative`, as PvP does). Probe (debug walk, `coop: scream:`
  lines: every Redead/Gibdo animation change and every hit our Link takes with its kind words): run `st-redead`
  (Graveyard_Adult, both players 4 m from the three Redeads at 113 97 9) saw no scream in 50 s: they stayed in
  `Wait`/`Sit`. What starts the scream (time of day, the player moving or looking) is still to find. Not for this
  release.
- Meg (Forest Temple): her decoys (`Enemy_MiddleBoss_Poe_Meg_Dummy`, no BSA, MaxLife 1, own AI) are made anew
  each cycle and get ids by order (`SceneSpawnId`), so three made in one frame can match differently between games;
  each game also rolls its own layout at Appear, and a copy's decoy AI fights the Mover. Likely cause: per-game
  layouts plus decoys matched by order (not missing messages: a copy's Meg runs her action modules). Smallest fix,
  medium confidence: `MinionMaker` maps the decoy to Meg, so only Meg's owner keeps decoys and shares them (as
  Gohma's eggs); risk: the copy's Meg remakes them in a loop. Fuller: match by `MegDummy_SeatId` (blackboard layout
  to find live). First step: log decoy ids, orders and places per game in a two-game run.
- Dead Hand: its hands are boss parts of Dead Hand (`PartRules` kSummoned), so they follow his owner and grab only
  that player. Smallest fix, medium confidence: drop that rule and keep Dead Hand a boss by name in
  `IsBossOrPart`; each hand then goes to its nearest player. Risk: a hand's grab sends `MessageAppear` to its own
  game's Dead Hand, held in a game that does not own him, so he may not rise there. Later: hand Dead Hand to a
  grabbing hand's owner (a summoner link the server does not follow).
- Water Temple and others: placements whose `Presence.FlagName` is a shared flag that changes during play (the
  water level's planes and waterfall, the Shadow tower's pillar and rubble, Bottom of the Well's water, the Forest
  Temple's room 7 water and elevator, Ice Cavern pots, a Jabu-Jabu air wall) are probably read only at a scene
  load. No proven live fix: deleting or making a placement live is proven for enemies, not for terrain collision.
  First step: a log line when such a flag changes with our Link near a gated piece; then a live check.
- Shadow Temple's falling pillar (`Dm_ShadowTemple_0015`): part of the presence item above.
- Volvagia's falling rocks: made by the arena (`Obj_FallingRockManager_A_03`, a pool of `Obj_FallingRock_A_06`),
  aimed at the local Link (`QueryDungeonGetPlayerPos`), switched on by Volvagia's `FallRockActivate` /
  `FallRockEffectActivate` plugs; co-op does not touch them. So each game may already drop its own rocks at its
  own Link; whether a copy game switches them on is to check live (log rocks falling per game).

## Order of work

1. **Turns between attacks** (Dauro's `boss-turns-0119`, merged in our branch): prove in every boss's every stage
   with two games (proof runs per boss).
2. **Turn length per boss** (every attack for Dodongo's roll, Volvagia's flight, Morpha's grab; every second for
   the long ones), a server table.
3. **Split shot** for Phantom Ganon's and Ganondorf's ball, then Volvagia's rocks; half split as a setting.
4. **Part targets** for Bongo Bongo's hands, Twinrova's sisters, Morpha's tentacles, Barinade's tentacles.

## Testing every stage

Each boss gets a proof run that walks it through every stage with two games: the test `hit` command takes it to
each stage's life (as `hs-boss-turns-pg` takes Phantom Ganon 42 -> 12 into the rally), both players watch it act
there for a minute (turns, its attacks at each player), then the kill. Checked per stage: the same move on both
screens, an attack at each player, a turn at an attack's end, no stall, one death.

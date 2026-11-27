/* SPDX-License-Identifier: GPL-3.0-or-later
   The Shooter component's three verbs, on Hookshot (oot-remake-multiplayer, docs/dungeons-boss.md "Shooter probe").

   Three wrapped game functions more in the one wrap table (patch 0005's nx2/coop_world_wraps.py WRAPS; the ids are in
   the one wrap space of hos_coop_world.h). Patch 0045 adds their rows and this file (Hookshot's
   host/hos_coop_boss_shooter.h is the same file). The module's hos_coop_wrap gets them; Hookshot announces them in
   HOS_COOP_WORLD_HELLO's bits. The rows:
     (28, 0x2eb1ed0, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4, 0x910003fd, 0x52832e08)),
     (29, 0x2eb2a40, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4, 0x910003fd, 0xf9400028)),
     (30, 0x2eb26b0, (0x34000341, 0xb9402c08, 0x34000308, 0xd503237f)),
   The module only reads (row 286's probe, LICHII_COOP_SHOOTER_PROBE=1): phase 0 always answers 0. Plain C. */
#ifndef HOS_COOP_BOSS_SHOOTER_H
#define HOS_COOP_BOSS_SHOOTER_H

/* main+0x2eb1ed0: a shooter prepares a bullet it keeps (x0 the shooter, x1 the prepare params: +0 the u32 key, +0x14
   the s32 index or -1). Returns the s32 record index, < 0 when refused. Every way into main+0x2eb4c30, which wakes the
   bullet's actor, goes through it: the AI nodes ExecuteShooterPrepare and ...WithTarget, and main+0x2eb2f20. */
#define HOS_COOP_WRAP_SHOOTER_PREPARE 28u
/* main+0x2eb2a40: a shooter shoots a prepared bullet (x0 the shooter, x1 the shoot params, an object: +0x1928 the u32
   key, +0x192c the s32 index). Returns a bool. The AI node OneShotShooterShoot tail-calls it. */
#define HOS_COOP_WRAP_SHOOTER_SHOOT 29u
/* main+0x2eb26b0: a shooter puts a bullet back to sleep (x0 the shooter, w1 the key, w2 the index). Returns a bool. */
#define HOS_COOP_WRAP_SHOOTER_SLEEP 30u

#endif

// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge in the game (docs/playtest.md, "Bridge protocol"): a TCP socket on 127.0.0.1 that an agent's
// MCP client drives one game through, with the frame hold (coop_playtest_core.h). Off unless
// LICHII_COOP_PLAYTEST_PORT is set: then nothing listens and AtFrame costs one atomic load.
#pragma once

#include <filesystem>

namespace Core {
class System;
}

namespace CoopPlaytest {

// At the module's start: reads the switches and, when on, starts the bridge's thread. `game_dir`: dumps and shots
// go in <game_dir>/playtest/.
void Start(Core::System& system, const std::filesystem::path& game_dir);
// The game's calc thread, first thing at every frame hook (HOS_COOP_SITE_FRAME), before any co-op work and with
// no lock held: holds there while the bridge asks.
void AtFrame();
// At exit: lets a held game go and stops the thread.
void Stop();

}  // namespace CoopPlaytest

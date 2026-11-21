#pragma once

#include <cstdint>

struct HosCoopMacPeer {
    char name[65]; // 16 Unicode nickname characters plus the terminator.
    std::uint16_t life;
    std::uint16_t max_life; // Zero until PLAYER_LIFE arrives.
};

using hos_coop_mac_peers_fn = std::uint32_t (*)(HosCoopMacPeer*, std::uint32_t);

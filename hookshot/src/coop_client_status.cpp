#include <cstdint>
#include <cstdio>

#include "coop_hook.h"
#include "coop_hud.h"
#include "hos_coop_mac_peers.h"
#include "coop_hookshot.h"

// The native Mac panel reads the same connection line as the co-op overlay.
extern "C" COOP_HOOKSHOT_EXPORT void hos_coop_client_status(char* out, std::uint32_t size) {
    if (!out || !size) return;
    const auto lines = CoopHook::OverlayLines();
    auto text = lines.empty() ? std::string("Connecting to the host…") : lines.front().text;
    if (text == "Co-op: handshaking") {
        text = "Handshaking. If Windows asks for approval, accept the player there.";
    } else if (text == "Co-op: waiting to retry") {
        text = "Host did not respond. Retrying…";
    }
    std::snprintf(out, size, "%s", text.c_str());
}

// The Mac HUD uses the session roster and existing PLAYER_LIFE messages.
extern "C" COOP_HOOKSHOT_EXPORT std::uint32_t hos_coop_mac_peers(HosCoopMacPeer* out, std::uint32_t capacity) {
    if (!out || !capacity) return 0;
    std::uint32_t count = 0;
    for (const auto& player : CoopHook::PlayerList()) {
        if (player.self) continue;
        auto& peer = out[count++];
        peer = {};
        std::snprintf(peer.name, sizeof(peer.name), "%s", player.name.c_str());
        if (const auto life = CoopHud::PlayerLife(player.number)) {
            peer.life = life->life;
            peer.max_life = life->max;
        }
        if (count == capacity) break;
    }
    return count;
}

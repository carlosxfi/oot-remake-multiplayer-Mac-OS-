#include <cstdint>
#include <cstdio>

#include "coop_hook.h"
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

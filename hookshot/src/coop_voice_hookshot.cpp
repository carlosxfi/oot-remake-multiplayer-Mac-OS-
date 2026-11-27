// SPDX-License-Identifier: GPL-3.0-or-later
//
// Proximity voice on Hookshot (docs/voice.md): the two optional exports of
// hos_coop_voice.h. Hookshot's mixer finds them once the module is loaded
// (our patch 0009, host/coop_voice.c), hands over the microphone and libopus
// through attach, and calls render every 5 ms with the game's final mix.
// This file turns that table into CoopVoice's Backend.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "coop_hookshot.h"
#include "coop_longshot_settings.h"
#include "coop_voice.h"
#include "hos_coop_voice.h"
#include "coop_export_guard.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

const HosCoopVoiceApi* g_voice = nullptr;

constexpr int kBitrate = 24000;   // speech, wideband: clear at 3 KB a second
constexpr int kLossPercent = 10;  // in-band FEC for about this much loss

class VoiceEncoder final : public coop::voice::Encoder {
public:
    VoiceEncoder() : enc_(g_voice->opus_encoder_new(kBitrate, kLossPercent)) {}
    ~VoiceEncoder() override {
        if (enc_ != nullptr) {
            g_voice->opus_encoder_free(enc_);
        }
    }
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;
    std::size_t encode(std::span<const std::int16_t> pcm, std::span<std::uint8_t> out) override {
        if (enc_ == nullptr || pcm.size() != static_cast<std::size_t>(HOS_COOP_VOICE_FRAME)) {
            return 0;
        }
        const int n = g_voice->opus_encode(enc_, pcm.data(), out.data(), static_cast<int>(out.size()));
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

private:
    void* enc_;
};

class VoiceDecoder final : public coop::voice::Decoder {
public:
    VoiceDecoder() : dec_(g_voice->opus_decoder_new()) {}
    ~VoiceDecoder() override {
        if (dec_ != nullptr) {
            g_voice->opus_decoder_free(dec_);
        }
    }
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;
    bool decode(std::span<const std::uint8_t> packet, bool fec, std::span<std::int16_t> pcm) override {
        if (dec_ == nullptr || pcm.size() != static_cast<std::size_t>(HOS_COOP_VOICE_FRAME)) {
            return false;
        }
        return g_voice->opus_decode(dec_, packet.empty() ? nullptr : packet.data(), static_cast<int>(packet.size()),
                                    fec ? 1 : 0, pcm.data()) > 0;
    }

private:
    void* dec_;
};

// The table's microphone list (none opened), as names.
std::vector<std::string> MicList() {
    constexpr int kMax = 16, kName = 128;
    std::vector<char> names(static_cast<std::size_t>(kMax * kName));
    const int n = g_voice->mic_list(names.data(), kName, kMax);
    std::vector<std::string> out;
    for (int i = 0; i < n && i < kMax; ++i) {
        const char* at = names.data() + static_cast<std::size_t>(i * kName);
        out.emplace_back(at, std::find(at, at + kName, '\0'));
    }
    return out;
}

bool KeyDown(std::uint32_t vk) {
    if (vk == 0 || vk > 255 || CoopHookshot::KeysTaken()) {  // typing in the chat box is not talking
        return false;
    }
#ifdef _WIN32
    // Only while the game's window is in front: the key held in another
    // program is not push to talk.
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front == nullptr || (GetWindowThreadProcessId(front, &pid), pid) != GetCurrentProcessId()) {
        return false;
    }
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
#else
    return false;
#endif
}

// Row 229: one of Longshot's rows by its key, its index kept (the rows do not move within a run; checked by its key).
std::optional<CoopLongshotSettings::Setting> RowByKey(const char* key, int* at) {
    if (*at >= 0) {
        if (auto r = CoopLongshotSettings::At(*at); r && r->key == key) return r;
    }
    for (int i = 0; i < CoopLongshotSettings::kMaxRows; ++i) {
        auto r = CoopLongshotSettings::At(i);
        if (!r) break;
        if (r->key == key) {
            *at = i;
            return r;
        }
    }
    *at = -1;
    return std::nullopt;
}

} // namespace

namespace CoopVoiceGameMic {
// The game frame, no lock of ours held (each read takes Longshot's menu lock a moment): Longshot's Insert-menu
// Microphone ("microphone": 0 Off, 1 the default, 2.. one by name) and its gain ("microphone_gain", 0 to 400 %), for
// the voice's Default, at most every 3 s while voice is on. Rows not there (an older Longshot): not known.
void Poll() {
    static std::chrono::steady_clock::time_point next{};
    static int mic_at = -1, gain_at = -1;
    const auto now = std::chrono::steady_clock::now();
    if (now < next || g_voice == nullptr || !CoopVoice::Available() || !CoopLongshotSettings::Available() ||
        CoopVoice::Get().mode == coop::voice::Mode::off) {
        return;
    }
    next = now + std::chrono::seconds(3);
    const auto mic = RowByKey("microphone", &mic_at);
    const auto gain = RowByKey("microphone_gain", &gain_at);
    CoopVoice::SetGameMic(!mic ? -1 : (mic->value == 0 || (gain && gain->value == 0)) ? 0 : 1);
}
}  // namespace CoopVoiceGameMic

extern "C" {
// hos_coop_voice.h: once, from Hookshot's mixer thread, maybe before
// hos_coop_start has run (the mixer finds the module as soon as it is
// loaded): nothing here needs the game, and a 0 would be for good.
COOP_HOOKSHOT_EXPORT int hos_coop_voice_attach(const HosCoopVoiceApi* api) {
    static CoopBoundary::Site site{"hos_coop_voice_attach"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        // (a game made before the table had mic_list and mic_choose: voice as before, the Insert menu's microphone)
        if (api == nullptr || api->version != HOS_COOP_VOICE_VERSION || api->size < offsetof(HosCoopVoiceApi, mic_list) ||
            g_voice != nullptr) {
            return 0;
        }
        g_voice = api;
        CoopVoice::Backend b;
        b.mic_start = [] { return g_voice->mic_start() != 0; };
        b.mic_read = [](std::int16_t* out, int max) { return g_voice->mic_read(out, max); };
        b.mic_stop = [] { g_voice->mic_stop(); };
        b.codecs.encoder = [] { return std::make_unique<VoiceEncoder>(); };
        b.codecs.decoder = [] { return std::make_unique<VoiceDecoder>(); };
        b.now_us = [] { return g_voice->now_us(); };
        b.key_down = KeyDown;
        if (api->size >= sizeof(HosCoopVoiceApi) && api->mic_list != nullptr && api->mic_choose != nullptr) {
            b.mic_list = MicList;
            b.mic_choose = [](const std::string& name) { g_voice->mic_choose(name.c_str()); };
        }
        CoopVoice::SetBackend(std::move(b));
        return 1;
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_voice_render(float* const* out, int channels, int frames) {
    static CoopBoundary::Site site{"hos_coop_voice_render"};
    CoopExportGuard::Guard(site, [&] {
        CoopVoice::Render(out, channels, frames);
    });
}
}

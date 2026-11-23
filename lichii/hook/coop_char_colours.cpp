// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_char_colours.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CoopCharColours {
namespace {

// The colour tables, in 255ths as the mod stores them: the numbers of OoT Character Select v0.2.1
// mods/customize.py (HAIR_COLOURS, brow_colour of HAIR_TEXTURE, EYE_COLOURS), MIT, Copyright (c) 2026 snaildev99
// (THIRD_PARTY_NOTICES.md). Rows in the options' order (1 = the first colour after Default).
struct Hair {
    std::array<std::uint8_t, 3> colour0, colour1, brow;
};
constexpr std::array<Hair, kHairs> kHairTable = {{
    {{255, 100, 0}, {202, 80, 0}, {129, 90, 18}},        // Blonde
    {{102, 41, 18}, {120, 36, 33}, {19, 8, 3}},          // Brown
    {{15, 13, 13}, {31, 28, 28}, {2, 2, 1}},             // Black
    {{184, 15, 5}, {176, 23, 43}, {50, 3, 2}},           // Red
    {{201, 89, 11}, {230, 115, 26}, {114, 26, 4}},       // Orange
    {{118, 161, 25}, {117, 168, 9}, {9, 46, 3}},         // Green
    {{20, 64, 191}, {26, 102, 217}, {4, 17, 55}},        // Blue
    {{230, 56, 107}, {242, 102, 140}, {154, 41, 79}},    // Pink
    {{204, 209, 217}, {230, 230, 242}, {191, 181, 166}}, // White
}};
// ns_param_color2 of the iris: hue, saturation, brightness shifts (0.5 = as the texture), for the common iris.
constexpr std::array<std::array<std::uint8_t, 3>, kEyes> kEyeTable = {{
    {128, 128, 128},  // Blue
    {96, 128, 116},   // Green
    {59, 107, 107},   // Brown
    {128, 31, 115},   // Gray
    {242, 143, 115},  // Red
    {149, 122, 117},  // Violet
    {66, 143, 128},   // Gold
}};

std::array<float, 3> Floats(const std::array<std::uint8_t, 3>& b) {
    constexpr float k = 1.0f / 255.0f;
    return {b[0] * k, b[1] * k, b[2] * k};
}

class Dresser {
public:
    Dresser(const Game& g, State& s, Result& r, std::size_t max_calls) : g_(g), s_(s), r_(r), max_calls_(max_calls) {}
    bool Stopped() const { return r_.failed || r_.cut; }

    std::uint64_t R64(std::uint64_t a) const {
        std::uint64_t v = 0;
        return a != 0 && g_.read(a, &v, sizeof v) ? v : 0;
    }
    template <typename T>
    T Rv(std::uint64_t a) const {
        T v{};
        if (a == 0 || !g_.read(a, &v, sizeof v)) return T{};
        return v;
    }
    std::optional<std::uint64_t> Call(std::uint64_t f, std::array<std::uint64_t, 8> x) {
        if (r_.failed || r_.cut || f == 0) return std::nullopt;
        if (r_.calls >= max_calls_) {
            r_.cut = true;
            return std::nullopt;
        }
        r_.calls++;
        auto v = g_.call(f, x);
        if (!v) r_.failed = true;
        return v;
    }
    std::uint64_t Virtual(std::uint64_t object, std::uint64_t offset) const { return R64(R64(object) + offset); }
    std::uint64_t Name(std::size_t i) const { return s_.cell + kCellNames + 16 * i; }

    // A by-name call (the unit's +0xd0 / +0xe0): the name's pointer in kCellName. -1: none.
    std::int16_t ByName(std::uint64_t unit, std::uint64_t offset, std::size_t name, std::optional<std::int16_t> mat) {
        const std::uint64_t ptr = Name(name);
        if (!g_.write(s_.cell + kCellName, &ptr, sizeof ptr)) {
            r_.failed = true;
            return -1;
        }
        std::array<std::uint64_t, 8> x{unit};
        if (mat) {
            x[1] = static_cast<std::uint64_t>(static_cast<std::int64_t>(*mat)), x[2] = s_.cell + kCellName;
        } else {
            x[1] = s_.cell + kCellName;
        }
        const auto v = Call(Virtual(unit, offset), x);
        return v ? static_cast<std::int16_t>(*v & 0xFFFF) : static_cast<std::int16_t>(-1);
    }

    std::uint64_t MaterialObject(std::uint64_t unit, std::int64_t index) const {
        if (index < 0) return 0;
        const std::uint64_t ptrs = R64(unit + 0x110);
        if (ptrs != 0) return R64(ptrs + 8 * static_cast<std::uint64_t>(index));
        const std::uint64_t base = R64(unit + 0x108);
        return base != 0 ? base + 0x80 * static_cast<std::uint64_t>(index) : 0;
    }

    bool HasPalette(std::uint64_t object) const {
        const std::uint64_t m = R64(object);
        return m != 0 && Rv<std::uint8_t>(m + kMatTextures) > Rv<std::uint8_t>(m + kMatSamplers);
    }

    // Four floats on a material's parameter: `rgb`, or nothing for the file's own; the fourth is always the file's.
    void SetFloats(std::uint64_t unit, std::int16_t mat, std::int16_t param, std::optional<std::array<float, 3>> rgb) {
        if (mat < 0 || param < 0) return;
        const std::uint64_t m = R64(MaterialObject(unit, mat));
        const std::uint64_t info = R64(R64(R64(m + 0x10)) + 0x20);
        const std::uint64_t values = R64(m + 0x58);
        if (m == 0 || info == 0 || values == 0) return;
        const auto offset = Rv<std::uint16_t>(info + 0x18 * static_cast<std::uint64_t>(param) + 0x10);
        std::array<float, 4> f{};
        if (!g_.read(values + offset, f.data(), sizeof f)) return;
        if (rgb) std::copy(rgb->begin(), rgb->end(), f.begin());
        if (!g_.write(s_.cell + kCellFloats, f.data(), sizeof f)) {
            r_.failed = true;
            return;
        }
        Call(Virtual(unit, kUnitSetFloats),
             {unit, static_cast<std::uint64_t>(static_cast<std::int64_t>(mat)), static_cast<std::uint64_t>(param),
              s_.cell + kCellFloats});
    }

    // The option's palette entry in the material's first place (the engine's own routine); 0 the material's own,
    // only with put_back. Nothing when that texture is there already or not bound yet.
    void PutTexture(std::uint64_t object, std::uint8_t option, bool put_back) {
        const std::uint64_t m = R64(object);
        if (m == 0) return;
        const auto samplers = Rv<std::uint8_t>(m + kMatSamplers), textures = Rv<std::uint8_t>(m + kMatTextures);
        if (textures <= samplers) return;
        const std::uint8_t place = PalettePlace(samplers, textures, option);
        if (place == 0 && !put_back) return;
        const std::uint64_t views = R64(m + kMatViews), slots = R64(m + kMatSlots), live = R64(object + kObjViews);
        if (views == 0 || slots == 0 || live == 0) return;
        const std::array<std::uint64_t, 2> pair{R64(views + 8u * place), R64(slots + 8u * place)};
        if (pair[0] == 0 || R64(live) == pair[0]) return;
        if (!g_.write(s_.cell + kCellTexture, pair.data(), sizeof pair)) {
            r_.failed = true;
            return;
        }
        Call(g_.main_base + kSetTexture, {object, 0, s_.cell + kCellTexture});
    }

    void Unit(State::Unit& u, const Options& o, bool put_back, int iris_shift) {
        if (!u.looked_up) {
            // The unit's material count first: a number the by-name call gives is used only below it.
            const auto count = Call(Virtual(u.unit, kUnitMaterialCount), {u.unit});
            if (!count) return;
            u.materials = std::min<std::uint64_t>(*count & 0xFFFFFFFFu, 256);
            for (std::size_t i = 0; i < kNames && !Stopped(); i++) {
                u.material[i] = ByName(u.unit, kUnitMaterialByName, i, std::nullopt);
                if (u.material[i] >= 0 && static_cast<std::uint64_t>(u.material[i]) >= u.materials) {
                    u.material[i] = -1;
                }
                u.param[i] = {-1, -1, -1};
                if (u.material[i] < 0) continue;
                for (std::size_t p = (i == kHairNames.size() ? 2 : 0); p < (i == kHairNames.size() ? 3u : 2u); p++) {
                    u.param[i][p] = ByName(u.unit, kUnitParamByName, kNames + p, u.material[i]);
                }
            }
            if (Stopped()) return;
            u.looked_up = true;
        }
        if (o.hair_exact && !Stopped()) {
            // (co-op's hair colour on a Link model: the parameters alone, as the game colours its own hair)
            for (std::size_t i = 0; i < kHairNames.size() && !Stopped(); i++) {
                if (u.material[i] < 0) continue;
                const bool brow = i + 1 == kHairNames.size();
                for (int p = 0; p < 2; p++) {
                    SetFloats(u.unit, u.material[i], u.param[i][static_cast<std::size_t>(p)],
                              brow ? (*o.hair_exact)[2] : (*o.hair_exact)[static_cast<std::size_t>(p)]);
                }
            }
        } else if (o.hair != 0 || put_back) {
            for (std::size_t i = 0; i < kHairNames.size() && !Stopped(); i++) {
                if (u.material[i] < 0) continue;
                const std::uint64_t object = MaterialObject(u.unit, u.material[i]);
                // Mt_Hairs is the name the player's code asks for: a character without hair has a stand-in under it,
                // which keeps its own colours. Only real hair got a palette from the mod.
                if (i == 0 && !HasPalette(object)) continue;
                const bool brow = i + 1 == kHairNames.size();
                for (int p = 0; p < 2; p++) {
                    SetFloats(u.unit, u.material[i], u.param[i][static_cast<std::size_t>(p)],
                              brow ? BrowColour(o.hair) : HairColour(o.hair, p));
                }
                PutTexture(object, o.hair, put_back);
            }
        }
        if ((o.eyes != 0 || o.eyes_exact || put_back) && !Stopped()) {
            const std::size_t e = kHairNames.size();
            SetFloats(u.unit, u.material[e], u.param[e][2], o.eyes_exact ? o.eyes_exact : EyeColour(o.eyes, iris_shift));
        }
        const bool clothes = o.clothes[0] != 0 || o.clothes[1] != 0 || o.clothes[2] != 0;
        if ((clothes || put_back) && !Stopped()) {
            for (std::uint64_t i = 0; i < u.materials && !Stopped(); i++) {
                const std::uint64_t object = MaterialObject(u.unit, static_cast<std::int64_t>(i));
                const std::uint64_t m = R64(object);
                if (m == 0) continue;
                const int row = ClothRow(Rv<std::uint8_t>(m + kMatSamplers), Rv<std::uint8_t>(m + kMatTextures));
                if (row >= 0) PutTexture(object, o.clothes[static_cast<std::size_t>(row)], put_back);
            }
        }
    }

    void Run(const Options& o, bool put_back, int iris_shift) {
        if (!o.Colours() && !put_back) return;
        // The model component, the player's kind only (the mod's own check: its +0xf8 is the player model's).
        const std::uint64_t component = R64(R64(s_.actor + kActorComponents) + 8);
        const std::uint64_t kind = R64(g_.main_base + kModelTypeGot);
        if (component == 0 || kind == 0 || Virtual(component, 0xf8) != kind) return;
        const std::uint64_t model = R64(component + 0x18);
        const auto count = Rv<std::int32_t>(model + 0x20);
        const std::uint64_t list = R64(model + 0x28);
        if (model == 0 || list == 0 || count <= 0 || count > 64) return;
        if (!s_.names_written) {
            std::array<char, 16 * (kNames + 3)> names{};
            // (16-byte slots, zeroed above: at most 15 bytes each, the 16th stays 0; memcpy, not strncpy, since gcc's
            // -Wstringop-truncation refuses a 15-letter name copied with 15)
            const auto put = [&names](std::size_t slot, const char* s) {
                std::memcpy(&names[16 * slot], s, std::min<std::size_t>(std::strlen(s), 15));
            };
            for (std::size_t i = 0; i < kHairNames.size(); i++) put(i, kHairNames[i]);
            put(kHairNames.size(), kEyeName);
            for (std::size_t p = 0; p < 3; p++) put(kNames + p, kParamNames[p]);
            if (!g_.write(s_.cell + kCellNames, names.data(), names.size())) {
                r_.failed = true;
                return;
            }
            s_.names_written = true;
        }
        s_.units.resize(static_cast<std::size_t>(count));
        for (std::size_t i = 0; i < s_.units.size() && !Stopped(); i++) {
            const std::uint64_t p = R64(list + 8 * i);
            const std::uint64_t unit = p != 0 ? R64(p) : 0;
            if (s_.units[i].unit != unit) {
                s_.units[i] = State::Unit();
                s_.units[i].unit = unit;
            }
            if (unit == 0 || R64(unit) == 0) continue;
            r_.units++;
            Unit(s_.units[i], o, put_back, iris_shift);
        }
    }

private:
    const Game& g_;
    State& s_;
    Result& r_;
    std::size_t max_calls_;
};

}  // namespace

Options Options::Of(std::uint32_t custom) {
    const auto n = [custom](int i) { return static_cast<std::uint8_t>((custom >> (4 * i)) & 0xF); };
    Options o;
    o.hair = n(0) <= kHairs ? n(0) : 0;
    o.eyes = n(1) <= kEyes ? n(1) : 0;
    o.gear = n(2);
    for (int i = 0; i < 3; i++) o.clothes[static_cast<std::size_t>(i)] = n(3 + i) <= kCloths ? n(3 + i) : 0;
    return o;
}

std::optional<std::array<float, 3>> HairColour(std::uint8_t option, int which) {
    if (option == 0 || option > kHairs) return std::nullopt;
    const Hair& h = kHairTable[option - 1u];
    return Floats(which == 0 ? h.colour0 : h.colour1);
}

std::optional<std::array<float, 3>> BrowColour(std::uint8_t option) {
    if (option == 0 || option > kHairs) return std::nullopt;
    return Floats(kHairTable[option - 1u].brow);
}

std::optional<std::array<float, 3>> EyeColour(std::uint8_t option, int iris_shift) {
    if (option == 0 || option > kEyes) return std::nullopt;
    auto b = kEyeTable[option - 1u];
    b[0] = static_cast<std::uint8_t>(std::clamp(int{b[0]} + iris_shift, 0, 255));
    return Floats(b);
}

std::uint8_t PalettePlace(std::uint8_t samplers, std::uint8_t textures, std::uint8_t option) {
    if (option == 0 || textures <= samplers) return 0;
    const int place = int{option} + int{samplers} - 1;
    return place < int{textures} ? static_cast<std::uint8_t>(place) : 0;
}

int ClothRow(std::uint8_t samplers, std::uint8_t textures) {
    const int behind = int{textures} - int{samplers} - int{kCloths};   // 0, 1 or 3: colour 1, 2, 3; 2: the hair's
    if (behind < 0 || behind > 3 || behind == 2) return -1;
    return behind == 3 ? 2 : behind;
}

namespace {
std::atomic<std::uint16_t> g_shown{0};
}
void SetShownCharacter(std::uint16_t number) { g_shown.store(number, std::memory_order_relaxed); }
namespace {
std::atomic<bool> g_mod_present{false};
}
void SetModPresent(bool present) { g_mod_present.store(present, std::memory_order_relaxed); }
bool ModPresent() { return g_mod_present.load(std::memory_order_relaxed); }
std::uint16_t ShownCharacter() { return g_shown.load(std::memory_order_relaxed); }

Wardrobe& TheWardrobe() {
    static Wardrobe w;
    return w;
}

Result Dress(const Game& game, State& state, std::uint32_t custom, bool put_back, int iris_shift,
             std::size_t max_calls) {
    Result r;
    if (state.cell == 0 || state.actor == 0 || !game.read || !game.write || !game.call) return r;
    Dresser(game, state, r, max_calls).Run(Options::Of(custom), put_back, iris_shift);
    return r;
}

Result Dress(const Game& game, State& state, const Options& options, bool put_back, int iris_shift,
             std::size_t max_calls) {
    Result r;
    if (state.cell == 0 || state.actor == 0 || !game.read || !game.write || !game.call) return r;
    Dresser(game, state, r, max_calls).Run(options, put_back, iris_shift);
    return r;
}

std::array<std::array<float, 3>, 3> HairParam(std::uint32_t rgb) {
    const auto linear = [](std::uint32_t c) {
        const float s = static_cast<float>(c & 0xFF) / 255.0f;
        return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    const std::array<float, 3> c0{linear(rgb >> 16), linear(rgb >> 8), linear(rgb)};
    std::array<float, 3> c1{}, brow{};
    for (std::size_t i = 0; i < 3; ++i) {
        c1[i] = std::min(1.0f, c0[i] * kHairLighter);
        brow[i] = c0[i] * kBrowScale;
    }
    return {c0, c1, brow};
}

std::array<float, 3> EyeParam(std::uint32_t rgb) {
    const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                b = static_cast<float>(rgb & 0xFF) / 255.0f;
    const float hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    float h = 0.0f;
    if (d > 0.0f) {
        h = hi == r ? std::fmod((g - b) / d, 6.0f) : hi == g ? (b - r) / d + 2.0f : (r - g) / d + 4.0f;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }
    const float s = hi > 0.0f ? d / hi : 0.0f, v = hi;
    // (a grey: the iris's own hue, only less saturated)
    const float x = d > 0.0f ? 0.5f + (h - kIrisHue) / 2.0f : 0.5f;
    const auto clamp01 = [](float t) { return std::clamp(t, 0.0f, 1.0f); };
    return {clamp01(x), clamp01(0.5f + (s - kIrisSaturation) / 2.0f), clamp01(0.5f + (v - kIrisBrightness) / 2.0f)};
}

std::size_t Wardrobe::Frame(const Game& game, const std::vector<Target>& targets, std::uint64_t now_ms,
                            const std::function<void(const std::string&)>& say) {
    for (auto& k : kept_) k.seen = false;
    const bool viewer_paints = Options::Of(Viewer()).Colours();
    std::size_t calls = 0;
    // One target further each frame comes first, so a budget that runs out never leaves the same puppets last.
    first_ = targets.empty() ? 0 : (first_ + 1) % targets.size();
    for (std::size_t n = 0; n < targets.size(); n++) {
        const Target& t = targets[(first_ + n) % targets.size()];
        auto it = std::find_if(kept_.begin(), kept_.end(), [&](const Kept& k) { return k.state.actor == t.actor; });
        if (it == kept_.end()) {
            kept_.push_back({});
            it = kept_.end() - 1;
            it->state.actor = t.actor;
            it->model = t.model;
        }
        Kept& k = *it;
        k.seen = true;
        // Another block, or another actor made at this address: its model is not the one looked up.
        if (k.state.cell != t.cell || k.model != t.model) {
            k = Kept{};
            k.state.actor = t.actor;
            k.state.cell = t.cell;
            k.model = t.model;
            k.seen = true;
        }
        if (t.mods_routine || calls >= kCallsPerFrame || now_ms < k.retry_ms) continue;
        Options o = Options::Of(t.custom);
        if (t.eyes) o.eyes_exact = EyeParam(*t.eyes);
        if (t.hair) o.hair_exact = HairParam(*t.hair);
        const bool colours = o.Colours();
        bool due;
        if (viewer_paints) {
            due = true;   // the mod's code painted the viewer's options on it this frame
        } else if (!k.dressed || *k.dressed != t.custom || k.dressed_eyes != t.eyes || k.dressed_hair != t.hair) {
            due = colours || k.ever;   // a change (to none: put back what was put on)
        } else {
            due = colours && now_ms - k.at_ms >= kAgainMs;
        }
        if (!due) continue;
        const bool put_back = viewer_paints || k.ever;
        const Result r = Dress(game, k.state, o, put_back, t.iris_shift, kCallsPerFrame - calls);
        calls += r.calls;
        if (r.failed) {
            // Not again before kAgainMs (a call that cannot be made fails every frame), said once per puppet.
            if (say && !k.said_failed) {
                say("characters: a model call failed while dressing an actor at " + std::to_string(t.actor) +
                    ": tried again every " + std::to_string(kAgainMs / 1000) + " s");
            }
            k.said_failed = true;
            k.retry_ms = now_ms + kAgainMs;
            continue;
        }
        if (r.cut || r.units == 0) continue;   // the budget ran out or the model is not there yet: next frame
        if (say && (!k.dressed || *k.dressed != t.custom) && colours) {
            say("characters: Customize options " + std::to_string(t.custom) + " put on the puppet (" +
                std::to_string(r.units) + " model unit(s), " + std::to_string(r.calls) + " calls)");
        }
        k.dressed = t.custom;
        k.dressed_eyes = t.eyes;
        k.dressed_hair = t.hair;
        k.ever = k.ever || colours;
        k.at_ms = now_ms;
    }
    kept_.erase(std::remove_if(kept_.begin(), kept_.end(), [](const Kept& k) { return !k.seen; }), kept_.end());
    return calls;
}

}  // namespace CoopCharColours

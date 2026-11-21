// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's gear in the colours of its look (docs/puppet.md, "The Link
// customizer"): which textures hold which parts, and those textures painted.
//
// Each shield, sword, pair of boots and pair of gauntlets has one colour
// texture holding all its parts; the Goron and Zora tunics have a few. A
// part is told from the rest by the tone of its end colours
// (coop_look_paint.h), as read from the game's own textures: the Hylian
// Shield's frame is grey, its face blue, its bird red; a sword's blade grey.
//
// The puppet pack has the game's own texels of these textures
// (coop_look/gear.bin, tools/puppet_pack/look_live.py, the same file format as
// the tunic's colours). The module paints them in the look and hands them to
// a CoopLookTunic::Wardrobe, which finds the textures the game made
// and has Longshot draw them painted (texture overrides), as for the tunic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_look.h"
#include "coop_look_paint.h"
#include "coop_look_tunic.h"

namespace CoopLookParts {

struct GearRule {
    CoopLook::Part part;
    std::uint32_t tones;   // CoopLookPaint::Tone bits
};
struct GearTexture {
    const char* name;      // in the game ("Mt_Weapon_Shield_Hylian_Alb")
    CoopLookPaint::Format format;
    std::vector<GearRule> rules;
};
// Every gear texture (tools/puppet_pack/look_live.py GEAR lists the same names).
const std::vector<GearTexture>& Gear();

// The pack's file of the game's own gear texels, and the name a look's
// painting goes by in the wardrobe: "gear" when the look sets no gear part.
inline constexpr const char* kBase = "gear";
std::string Name(const CoopLook::Look& look);
bool AnyGear(const CoopLook::Look& look);

// The gear texels painted in `look` (same textures, same sizes); a texture
// with no part set, or not in the table, as it is. A metalness texture
// (UseMetal) has the blocks under each painted metal part set to 0.
CoopLookTunic::Texels Painted(const CoopLookTunic::Texels& gear, const CoopLook::Look& look);

// The metal parts (#202). The swords, the Hylian and Mirror Shields and the
// gauntlets have a metalness texture (`Mt_..._Mtl`, BC4) beside their colour
// one; the game draws a metal texel's colour as what it reflects, with no
// diffuse light, so a metal part painted only in its colour texture shows
// much darker than the colour picked. The pack lists, for each such texture
// and part, the byte offsets of its BC4 blocks under that part, every mip
// level (tools/puppet_pack/look_metal.py, coop_look/metal.bin; gear.bin
// holds the textures' own texels). Painted sets those blocks to 0 (no metal)
// when the part has a colour, so the part looks painted; with no colour the
// texture stays the game's own.
struct MetalBlocks {
    std::string texture;   // "Mt_Weapon_Shield_Hylian_Mtl"
    CoopLook::Part part;
    std::vector<std::uint32_t> offsets;   // of 8-byte BC4 blocks in its texels
};
using Metal = std::vector<MetalBlocks>;
// metal.bin's bytes; nothing (and `why`) when they are not one.
std::optional<Metal> ParseMetal(const std::vector<std::uint8_t>& file, std::string* why = nullptr);
// The table Painted uses from now on (nothing: no metal is painted down).
void UseMetal(std::shared_ptr<const Metal> metal);
// The blocks of `texels` (the texture `name`) under the parts `look` paints, set to 0.
// Returns how many blocks it set.
std::size_t PaintMetal(const Metal& metal, const std::string& name, std::vector<std::uint8_t>& texels,
                       const CoopLook::Look& look);

// The puppets' gear (docs/puppet.md, "Gear colours on the puppets"): the
// pack's item copies name textures of their own, "Mt_..." as "C<copy>_..."
// (tools/puppet_pack/look_items.py tex_name). One more wardrobe keeps every
// copy's textures in the look of the player it is painted for, drawn by
// Longshot's texture overrides as your own gear is (nothing of the game is
// written; the game loads and draws the copies itself).
//
// A gear texture's name in copy `copy` (1 to 9); "" for a name that is not
// a gear texture's ("Mt_..." is), or that could name another folder.
std::string SlotName(const std::string& name, std::size_t copy);
// The gear textures under copy `copy`'s names, painted in `look` (the game's
// own colours when none) and marked (Mark): what that copy's pieces are drawn
// with.
CoopLookTunic::Texels CopyTexels(const CoopLookTunic::Texels& gear, std::size_t copy,
                                 const std::optional<CoopLook::Look>& look);
// A copy's texels as the pack's copy files hold them: the image's last byte
// XOR the copy's number (tools/puppet_pack/look_live.py mark). The game keeps
// no name with a texture's texels, so only this tells a copy's textures from
// your own Link's. That byte is in the last mip level or the padding past it.
void Mark(std::vector<std::uint8_t>& texels, std::size_t copy);
// The looks the copies are painted in (index 0: copy 1; nothing: the game's own).
using CopyLooks = std::vector<std::optional<CoopLook::Look>>;
// The copies' wardrobe's base (every copy in the game's own colours), and the
// name of a painting of them: kCopiesBase when no copy has a gear part set.
inline constexpr const char* kCopiesBase = "puppets";
std::string CopiesName(const CopyLooks& looks);

// The boots in Link's own model (the adult's Kokiri boots, the child's
// leather, which holds his belts too): not an item, so no item copies. Every
// Link draws one texture of a name; the pack's puppet body models, one per
// tunic colour, each name a copy of their own, "Mt_..." as "P<letter>_..."
// (tools/puppet_pack/look_items.py BODY_BOOTS, boots_name, boots_mark: the
// same names, letters and marks). One more wardrobe keeps each colour's
// copies in the boots colour of the player who wears that colour. Your own
// Link's model (the game's, or its own copy) names the game's boots: the
// gear wardrobe paints those (Gear() lists them).
//
// The body boots textures (a subset of Gear()).
const std::vector<std::string>& BodyBoots();
// Each tunic colour's letter, by the name CoopLook::TunicName gives ("red",
// ..., "green"): tools/puppet_pack/tunic.py COLORS. Nothing for another name.
std::optional<char> LetterOf(std::string_view tunic);
// A body boots texture's name in colour `letter`'s models; "" if `name` is not one.
std::string BootsName(const std::string& name, char letter);
// Each colour's boots colour (0xRRGGBB) by its letter; a letter not in it
// keeps the game's colour.
using Wearers = std::map<char, std::uint32_t>;
// Who paints which colour's boots: your own Link first (`own`: the letter
// of the model he wears, and his boots colour), then the others in the
// order given (the first of a letter has it; nothing: the game's colour).
struct Wearer {
    std::optional<char> letter;
    std::optional<std::uint32_t> boots;
};
Wearers Assign(const Wearer& own, const std::vector<Wearer>& others);
// The boots wardrobe's base (every colour's copies in the game's colour) and
// the name of a painting: kBootsBase when no colour has boots set.
inline constexpr const char* kBootsBase = "boots";
std::string BootsPaintingName(const Wearers& wearers);
// Every colour's copies of the body boots in `gear` (renamed, painted for
// its wearer, marked with the letter's code as the pack marks them).
CoopLookTunic::Texels BootsTexels(const CoopLookTunic::Texels& gear, const Wearers& wearers);

}  // namespace CoopLookParts

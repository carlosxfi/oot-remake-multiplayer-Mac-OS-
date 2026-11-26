"""The co-op HUD's look, made from the player's own game files (docs/overlay.md).

    python -m puppet_pack.hud --romfs <.nup or extracted romfs> --out <folder>

make-puppet-pack runs it after the puppets (build.py) and writes into the puppet mod's
folder, `coop_hud/` beside `coop_puppet.json` (outside `romfs/`, so the game never sees
it). The emulator's co-op window loads it at start; when a file is missing it keeps its
plain look and logs why.

Writes:
  font-regular.otf, font-medium.otf  the game's HUD font (Normal.buif's Latin font)
  heart-0.png ... heart-4.png        a heart with 0 to 4 quarters filled, 96 x 96
  map-arrow-00.png ... -23.png       the game's map arrow (Link's own), turned 0..23 x 15 degrees
  map-arrow-fill-00.png ... -23.png  its inside, white: drawn tinted with each player's colour
  places.json                        {scene: the game's own place name}
  hud.json                           what was made, and the language

Nothing from the game is in this tool or in git: every picture, the font and the names
come from the files given.
"""
import argparse
import json
import math
import os
import re
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import nup, sarc, texture, zs  # noqa: E402

HUD_DIR = "coop_hud"
FORMAT = 1
DEFAULT_LANG = "USen"
FONT_DIR = "Font2/Scalable"
FONTS = {"font-regular.otf": "CetraDisplayPro-Regular", "font-medium.otf": "CetraDisplayPro-Medium"}
BNTX = "UI/Layout2Archive/Common.Product.*.bntx.zs"
HEART_FULL = "HeartW_00_ASTC4x4"     # the red heart
HEART_BASE = "HeartBase_00"          # the heart's shape, grey: drawn dark for an empty one
HEART_SHADOW = "HeartSh_00"
HEART_QUARTERS = ("HeartGlowBase_01", "HeartGlowBase_02", "HeartGlowBase_03")  # 1/4, 2/4, 3/4
TEXTURES = (HEART_FULL, HEART_BASE, HEART_SHADOW) + HEART_QUARTERS
# The map's player arrow (PaMapIconPlayer_00, Link's own arrow on the minimap and the Map page): _00 is the game's
# mask (white rim, black inside: the game fills the inside with the icon's colour), _01 Link's own (red inside, its
# rim's cream), Sh_00 its shadow. The other players get it with their colour inside, turned the way they face.
MAP_ARROW = "MapIconPlayer_00"
MAP_ARROW_LINK = "MapIconPlayer_01"
MAP_ARROW_SHADOW = "MapIconPlayerSh_00"
MAP_ARROW_TEXTURES = (MAP_ARROW, MAP_ARROW_LINK, MAP_ARROW_SHADOW)
MAP_ARROW_STEPS = 24    # turns, 15 degrees apart: hookshot/src/coop_draw.h kMapArrowSteps (Hookshot holds 64 pictures)
MAP_ARROW_SIZE = 80     # the square kept around the picture's centre (the arrow and its shadow, at any turn)
LOCATIONS = "LocationMsg/Location.msbt"
# The scene name's age or variant suffix: KokiriForest_Child -> KokiriForest.
SCENE_SUFFIX = re.compile(r"_(Child|Adult)(_Alternative)?$|_Alternative$")

# An empty heart: the base shape, darkened (the game draws its empty hearts dark).
EMPTY_RGB = (30, 22, 22)
EMPTY_ALPHA = 0.85
SHADOW_ALPHA = 0.55

# The obfuscated font files (.bfotf/.bfttf): 8 header bytes, then the font XORed with this
# 4-byte key (the same for every font of the game; checked on all 29).
FONT_KEY = bytes.fromhex("a6018502")
FONT_HEADER = 8


class HudError(RuntimeError):
    pass


# ---- reading the game ----

class DirReader:
    """An extracted romfs folder."""

    def __init__(self, root):
        self.root = root

    def glob(self, pattern):
        d, _, name = pattern.rpartition("/")
        try:
            names = os.listdir(os.path.join(self.root, d))
        except OSError:
            return []
        import fnmatch
        return sorted(f"{d}/{n}" for n in names if fnmatch.fnmatchcase(n, name)
                      and os.path.isfile(os.path.join(self.root, d, n)))

    def dirs(self, d):
        try:
            return sorted(n for n in os.listdir(os.path.join(self.root, d))
                          if os.path.isdir(os.path.join(self.root, d, n)))
        except OSError:
            return []

    def files(self, d):
        try:
            return sorted(n for n in os.listdir(os.path.join(self.root, d))
                          if os.path.isfile(os.path.join(self.root, d, n)))
        except OSError:
            return []

    def read(self, rel):
        with open(os.path.join(self.root, rel), "rb") as f:
            return f.read()


class NupReader:
    """A .nup game file, open."""

    def __init__(self, n):
        self.n = n

    def glob(self, pattern):
        try:
            return self.n.glob(pattern)
        except (nup.NupError, KeyError):
            return []

    def _list(self, d):
        try:
            return self.n.listdir(d)
        except (nup.NupError, KeyError):
            return {}

    def dirs(self, d):
        return sorted(k for k, (kind, _i) in self._list(d).items() if kind != "f")

    def files(self, d):
        return sorted(k for k, (kind, _i) in self._list(d).items() if kind == "f")

    def read(self, rel):
        return self.n.read(rel)


def _one(reader, pattern):
    hits = reader.glob(pattern)
    if not hits:
        raise HudError(f"the game files have no {pattern}")
    return hits[-1]


# ---- fonts ----

def deobfuscate_font(b):
    """The plain OpenType/TrueType font in a .bfotf/.bfttf (already decompressed)."""
    if len(b) < FONT_HEADER + 4:
        raise HudError("font file too short")
    size = int.from_bytes(bytes(x ^ k for x, k in zip(b[4:8], FONT_KEY)), "big")
    body = b[FONT_HEADER:]
    if size != len(body):
        raise HudError(f"font size {size} does not match the file ({len(body)})")
    key = FONT_KEY * ((len(body) + 3) // 4)
    plain = bytes(x ^ k for x, k in zip(body, key))
    if plain[:4] not in (b"OTTO", b"\0\1\0\0", b"true"):
        raise HudError("not a font after unpacking")
    return plain


# ---- place names ----

def parse_msbt(b):
    """{label: text} of an MSBT file (UTF-8 or UTF-16; tags are dropped)."""
    if b[:8] != b"MsgStdBn":
        raise HudError("not an MSBT file")
    e = "<" if b[8:10] == b"\xff\xfe" else ">"
    enc = b[0x0C]
    nsec = struct.unpack_from(e + "H", b, 0x0E)[0]
    off, sec = 0x20, {}
    for _ in range(nsec):
        magic, size = b[off:off + 4], struct.unpack_from(e + "I", b, off + 4)[0]
        sec[magic] = b[off + 16:off + 16 + size]
        off = (off + 16 + size + 15) & ~15
    lbl, txt = sec.get(b"LBL1"), sec.get(b"TXT2")
    if lbl is None or txt is None:
        raise HudError("MSBT without labels or text")
    labels = {}
    for i in range(struct.unpack_from(e + "I", lbl, 0)[0]):
        count, o = struct.unpack_from(e + "II", lbl, 4 + 8 * i)
        for _ in range(count):
            n = lbl[o]
            labels[lbl[o + 1:o + 1 + n].decode("utf-8")] = struct.unpack_from(e + "I", lbl, o + 1 + n)[0]
            o += 5 + n
    tn = struct.unpack_from(e + "I", txt, 0)[0]
    offs = [struct.unpack_from(e + "I", txt, 4 + 4 * i)[0] for i in range(tn)] + [len(txt)]
    out = {}
    for label, idx in labels.items():
        raw = txt[offs[idx]:offs[idx + 1]]
        out[label] = _msbt_text(raw, enc, e)
    return out


def _msbt_text(raw, enc, e):
    if enc == 0:  # UTF-8; tags start with 0x0E
        s = raw.split(b"\0", 1)[0]
        if b"\x0e" in s:
            s = s.split(b"\x0e", 1)[0]
        return s.decode("utf-8", "replace").strip()
    units = [struct.unpack_from(e + "H", raw, i)[0] for i in range(0, len(raw) - 1, 2)]
    chars = []
    i = 0
    while i < len(units):
        u = units[i]
        if u == 0:
            break
        if u == 0x0E:  # tag: group, type, size in bytes, then the parameters
            if i + 3 >= len(units):
                break
            i += 4 + units[i + 3] // 2
            continue
        if u == 0x0F:  # closing tag
            i += 3
            continue
        chars.append(u)
        i += 1
    return struct.pack("<" + "H" * len(chars), *chars).decode("utf-16-le", "replace").strip()


def scene_base(scene):
    return SCENE_SUFFIX.sub("", scene)


def place_names(locations, banc_dirs):
    """{scene base or Banc folder name: place name}: each location label as it is, and every
    scene of a Banc folder that has a label under that folder's name (the boss rush's
    `GohmaStage` is in `Banc/DreamWorld`)."""
    out = {k: v for k, v in locations.items() if v}
    for folder, scenes in banc_dirs.items():
        name = locations.get(folder)
        if not name:
            continue
        for s in scenes:
            out.setdefault(scene_base(s), name)
    return out


def banc_scenes(reader):
    """{Banc folder: [scene names]} from the files `<scene>_Static.bcett.byml.zs`."""
    out = {}
    for d in reader.dirs("Banc"):
        scenes = set()
        for f in reader.files(f"Banc/{d}"):
            m = re.match(r"(.+?)_(Static|Dynamic)\.bcett\.byml", f)
            if m:
                scenes.add(m.group(1))
        out[d] = sorted(scenes)
    return out


# ---- hearts ----

def _alpha(rgba, i):
    return rgba[i * 4 + 3]


def compose_hearts(px, w, h):
    """[heart with 0..4 quarters filled] as RGBA bytes, from the decoded game pictures
    {name: rgba} (all w x h)."""
    full, base, shadow = px[HEART_FULL], px[HEART_BASE], px[HEART_SHADOW]
    masks = [None] + [px[n] for n in HEART_QUARTERS] + [None]
    out = []
    for quarters in range(5):
        img = bytearray(w * h * 4)
        for i in range(w * h):
            # Shadow, then the empty heart, then the filled part over it.
            sa = shadow[i * 4 + 3] * SHADOW_ALPHA / 255.0
            r, g, b, a = 0.0, 0.0, 0.0, sa
            ba = base[i * 4 + 3] / 255.0 * EMPTY_ALPHA
            shade = base[i * 4] / 255.0
            er, eg, eb = (c * (0.6 + 0.4 * shade) for c in EMPTY_RGB)
            r, g, b, a = _over((er, eg, eb, ba), (r, g, b, a))
            if quarters == 4:
                cover = 1.0
            elif quarters == 0:
                cover = 0.0
            else:
                cover = masks[quarters][i * 4 + 3] / 255.0
            fa = full[i * 4 + 3] / 255.0 * cover
            if fa > 0:
                r, g, b, a = _over((full[i * 4], full[i * 4 + 1], full[i * 4 + 2], fa), (r, g, b, a))
            img[i * 4:i * 4 + 4] = bytes((int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5)))
        out.append(bytes(img))
    return out


def compose_map_arrow(px, w, h):
    """(rim, inside) of the game's player arrow, as RGBA bytes (w x h), from the decoded pictures {name: rgba}.
    rim: the shadow and the rim in Link's arrow's own colours, drawn as it is; inside: white, shaded as Link's red
    inside, drawn tinted with the player's colour over the rim picture (the game's own mask splits the two)."""
    mask, link, shadow = px[MAP_ARROW], px[MAP_ARROW_LINK], px[MAP_ARROW_SHADOW]
    n = w * h
    inside_a = [mask[i * 4 + 3] / 255.0 * (1.0 - mask[i * 4] / 255.0) for i in range(n)]
    peak = max((link[i * 4] for i in range(n) if inside_a[i] > 0.5), default=255) or 255
    rim, inside = bytearray(n * 4), bytearray(n * 4)
    for i in range(n):
        ra = mask[i * 4 + 3] / 255.0 * (mask[i * 4] / 255.0)
        r, g, b, a = _over((link[i * 4], link[i * 4 + 1], link[i * 4 + 2], ra),
                           (0.0, 0.0, 0.0, shadow[i * 4 + 3] * SHADOW_ALPHA / 255.0))
        rim[i * 4:i * 4 + 4] = bytes((int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5)))
        v = min(255, int(link[i * 4] * 255 / peak + 0.5))
        inside[i * 4:i * 4 + 4] = bytes((v, v, v, int(inside_a[i] * 255 + 0.5)))
    return bytes(rim), bytes(inside)


def crop_centre(rgba, w, h, size):
    """The size x size square around the picture's centre (outside it: clear)."""
    out = bytearray(size * size * 4)
    x0, y0 = (w - size) // 2, (h - size) // 2
    for y in range(size):
        sy = y0 + y
        if 0 <= sy < h:
            for x in range(size):
                sx = x0 + x
                if 0 <= sx < w:
                    out[(y * size + x) * 4:(y * size + x) * 4 + 4] = rgba[(sy * w + sx) * 4:(sy * w + sx) * 4 + 4]
    return bytes(out)


def turned(rgba, size, degrees):
    """The square picture turned clockwise on screen (y down) by degrees about its centre, bilinear on
    premultiplied alpha (outside the source: clear)."""
    t = math.radians(degrees)
    c, s = math.cos(t), math.sin(t)
    mid = size / 2.0
    out = bytearray(size * size * 4)

    def px(x, y):
        if 0 <= x < size and 0 <= y < size:
            o = (y * size + x) * 4
            a = rgba[o + 3] / 255.0
            return rgba[o] * a, rgba[o + 1] * a, rgba[o + 2] * a, a
        return 0.0, 0.0, 0.0, 0.0

    for y in range(size):
        dy = y + 0.5 - mid
        for x in range(size):
            dx = x + 0.5 - mid
            sx = dx * c + dy * s + mid - 0.5   # (the inverse turn: where this pixel comes from)
            sy = -dx * s + dy * c + mid - 0.5
            ix, iy = math.floor(sx), math.floor(sy)
            fx, fy = sx - ix, sy - iy
            acc = [0.0, 0.0, 0.0, 0.0]
            for (qx, qy, k) in ((ix, iy, (1 - fx) * (1 - fy)), (ix + 1, iy, fx * (1 - fy)),
                                (ix, iy + 1, (1 - fx) * fy), (ix + 1, iy + 1, fx * fy)):
                if k > 0:
                    p = px(qx, qy)
                    for j in range(4):
                        acc[j] += p[j] * k
            a = acc[3]
            if a > 0:
                o = (y * size + x) * 4
                out[o:o + 4] = bytes((min(255, int(acc[0] / a + 0.5)), min(255, int(acc[1] / a + 0.5)),
                                      min(255, int(acc[2] / a + 0.5)), min(255, int(a * 255 + 0.5))))
    return bytes(out)


def map_arrow_names(step):
    return f"map-arrow-{step:02d}.png", f"map-arrow-fill-{step:02d}.png"


def _over(top, under):
    tr, tg, tb, ta = top
    ur, ug, ub, ua = under
    a = ta + ua * (1 - ta)
    if a <= 0:
        return 0.0, 0.0, 0.0, 0.0
    return ((tr * ta + ur * ua * (1 - ta)) / a, (tg * ta + ug * ua * (1 - ta)) / a,
            (tb * ta + ub * ua * (1 - ta)) / a, a)


# ---- the whole thing ----

def build(reader, out_dir, lang=DEFAULT_LANG, log=print):
    """Writes the HUD files into out_dir. Raises HudError when a needed file is missing."""
    dicts = zs.Dicts(reader.read("Pack/ZsDic.pack.zs"))

    def read(rel):
        return zs.maybe_decompress(dicts, reader.read(rel))

    os.makedirs(out_dir, exist_ok=True)
    made = {"format": FORMAT, "language": lang, "fonts": [], "hearts": [], "map_arrows": 0, "places": 0}
    for out_name, font in FONTS.items():
        rel = _one(reader, f"{FONT_DIR}/{font}.*.bf?tf.zs")
        _write(os.path.join(out_dir, out_name), deobfuscate_font(read(rel)))
        made["fonts"].append(out_name)
    tex = texture.read_bntx(read(_one(reader, BNTX)), set(TEXTURES) | set(MAP_ARROW_TEXTURES))
    arrow_px = {n: texture.decode(tex.pop(n)) for n in MAP_ARROW_TEXTURES if n in tex}
    missing = [n for n in TEXTURES if n not in tex]
    if missing:
        raise HudError(f"the game's HUD pictures have no {', '.join(missing)}")
    w, h = tex[HEART_FULL].width, tex[HEART_FULL].height
    if any((t.width, t.height) != (w, h) for t in tex.values()):
        raise HudError("the heart pictures are not all the same size")
    px = {n: texture.decode(t) for n, t in tex.items()}
    for q, rgba in enumerate(compose_hearts(px, w, h)):
        name = f"heart-{q}.png"
        _write(os.path.join(out_dir, name), texture.png(w, h, rgba))
        made["hearts"].append(name)
    made["map_arrows"] = _write_map_arrows(arrow_px, out_dir, log)
    mals =sarc.read(read(_one(reader, f"Mals/{lang}.Product.*.sarc.zs")))
    if LOCATIONS not in mals:
        raise HudError(f"the {lang} text has no {LOCATIONS}")
    places = place_names(parse_msbt(mals[LOCATIONS]), banc_scenes(reader))
    _write(os.path.join(out_dir, "places.json"),
           json.dumps(places, ensure_ascii=False, indent=1, sort_keys=True).encode("utf-8"))
    made["places"] = len(places)
    _write(os.path.join(out_dir, "hud.json"), json.dumps(made, indent=1).encode("utf-8"))
    log(f"co-op HUD: font, {len(made['hearts'])} hearts, {made['map_arrows']} map arrow turns and "
        f"{len(places)} place names "
        f"({lang}) in {out_dir}")
    return made


def _write_map_arrows(arrow_px, out_dir, log):
    """map-arrow-NN.png and map-arrow-fill-NN.png for each turn; the number of turns written (0: the game has not
    the pictures, the maps then show the players' names only)."""
    if len(arrow_px) != len(MAP_ARROW_TEXTURES):
        log("co-op HUD: no map arrow pictures in this game (" +
            ", ".join(n for n in MAP_ARROW_TEXTURES if n not in arrow_px) + "): names only on the maps")
        return 0
    side = math.isqrt(len(arrow_px[MAP_ARROW]) // 4)
    if any(len(p) != side * side * 4 for p in arrow_px.values()):
        log("co-op HUD: the map arrow pictures are not all one square size: names only on the maps")
        return 0
    rim, inside = compose_map_arrow(arrow_px, side, side)
    size = min(MAP_ARROW_SIZE, side)
    rim, inside = crop_centre(rim, side, side, size), crop_centre(inside, side, side, size)
    for step in range(MAP_ARROW_STEPS):
        deg = step * 360.0 / MAP_ARROW_STEPS
        rim_name, inside_name = map_arrow_names(step)
        _write(os.path.join(out_dir, rim_name), texture.png(size, size, turned(rim, size, deg)))
        _write(os.path.join(out_dir, inside_name), texture.png(size, size, turned(inside, size, deg)))
    return MAP_ARROW_STEPS


def _write(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def build_any(romfs, out_dir, lang=DEFAULT_LANG, log=print):
    """build() from a .nup file or an extracted romfs folder."""
    if os.path.isfile(romfs) and nup.is_nup(romfs):
        with nup.Nup(romfs) as n:
            return build(NupReader(n), out_dir, lang, log)
    return build(DirReader(romfs), out_dir, lang, log)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--romfs", required=True, help="the game's .nup file, or its extracted romfs folder")
    ap.add_argument("--out", required=True, help="the folder to write (made if missing)")
    ap.add_argument("--lang", default=DEFAULT_LANG, help="the game text language, e.g. USen, EUen, EUfr")
    a = ap.parse_args(argv)
    try:
        build_any(a.romfs, a.out, a.lang)
    except (HudError, texture.TextureError, nup.NupError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

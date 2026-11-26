"""Pack files `look_cap` (@pytest.mark.needs_pack('look_cap'), run flag --pack look_cap): the puppet pack's
coop_look/cap.bin made from the TESTED sha's tools/puppet_pack/look_cap.py over the lane's romfs, as the pack build
makes it (tools/puppet_pack/build.py, the block after "The blocks of the tunics' textures that only the cap uses":
read_tex = romfs/Tex/<name>.bntx.zs, read_model = romfs/<look_cap.model_path>, both through the romfs's zstd dicts;
the same maker as coord offline/kokiri-band-c124-cap.py). Used by test_look.py::test_kokiri_hood_band.

Interface (harness-c132): make(out, ctx) -> {path relative to the game's coop-puppet mod dir: built file};
ctx.tools (on sys.path), ctx.romfs, ctx.game_nup, ctx.lane, ctx.say(line).
"""
import os

REL = "coop_look/cap.bin"


def summary(blob, look_cap):
    """'cap.bin: <bytes> bytes, <n> textures, <offsets> offsets' (the old setup's line, setup-only.txt:26; the
    module's own count is the 'Cap trim paints N blocks' line the test checks)."""
    entries = look_cap.unpack(blob)
    return "cap.bin: %d bytes, %d textures, %d offsets" % (len(blob), len(entries), sum(len(e[2]) for e in entries))


def build(out, ctx, look_cap, name="cap.bin"):
    from puppet_pack import zs
    dicts = zs.Dicts.from_romfs(ctx.romfs)

    def read(p):
        if not os.path.isfile(p):
            return None
        with open(p, "rb") as f:
            return dicts.decompress(f.read())

    def read_tex(tex):
        return read(os.path.join(ctx.romfs, "Tex", "%s.bntx.zs" % tex))

    def read_model(model):
        return read(os.path.join(ctx.romfs, *look_cap.model_path(model).split("/")))

    blob = look_cap.cap(read_tex, read_model, ctx.say)
    if blob is None:
        raise RuntimeError("look_cap: no cap.bin made from %s (see the look: lines)" % ctx.romfs)
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, name)
    with open(path, "wb") as f:
        f.write(blob)
    ctx.say(summary(blob, look_cap))
    return {REL: path}


def make(out, ctx):
    from puppet_pack import look_cap
    return build(out, ctx, look_cap)

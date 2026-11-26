"""--pack hud: the co-op HUD files (coop_hud/: fonts, hearts, the map arrow, place names) made by the TESTED sha's
tools/puppet_pack/hud.py from the base game (the same build the kit's pack runs; regress-fps-map-run.ps1:91 made the
map arrows this way by hand). For rows that need HUD files newer than the lane's pack (G2 map arrow: hud.MAP_ARROW_FILES,
which coop_hookshot.cpp LoadLook reads from coop_hud)."""
import os


def make(out, ctx):
    from puppet_pack import hud
    src = ctx.romfs if ctx.romfs and os.path.isdir(ctx.romfs) else ctx.game_nup
    if not src:
        raise RuntimeError("no romfs folder and no .nup to make the HUD files from")
    hud.build_any(src, out, log=ctx.say)
    files = {"coop_hud/" + n: os.path.join(out, n) for n in sorted(os.listdir(out))
             if os.path.isfile(os.path.join(out, n))}
    missing = [n for n in hud.MAP_ARROW_FILES if "coop_hud/" + n not in files]
    if missing:
        raise RuntimeError("hud.py made no %s (the game's map arrow textures not found)" % ", ".join(missing))
    ctx.say("pack hud: %d files from %s" % (len(files), src))
    return files

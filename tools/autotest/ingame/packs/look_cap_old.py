"""Pack files `look_cap_old` (--pack look_cap_old): coop_look/cap.bin as the pack made it BEFORE the fix, for
test_kokiri_hood_band's probe (KOKIRI_BAND_PROBE_PACK=43600: its 'cap.bin: 174844 bytes, 6 textures, 43600 offsets' over
dump/romfs): tools/puppet_pack/look_cap.py of OLD = 3d91ec10a^ (the
fix 3d91ec10a counts a smaller mip level's cap blocks by texel area), read by `git show` from the tested sha's repo and
run inside the tested puppet_pack package (its bfres / look_metal / tunic helpers); otherwise as packs/look_cap.py.
"""
import importlib.util
import os
import subprocess

OLD = "3d91ec10a^"


def _sibling():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "look_cap.py")
    spec = importlib.util.spec_from_file_location("packs_look_cap", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def old_look_cap(tools, out):
    """The OLD look_cap.py as a module of the tested puppet_pack package (its relative imports resolve there)."""
    import puppet_pack   # noqa: F401  (the package its `from . import` lines need)
    src = subprocess.run(["git", "-C", tools, "show", "%s:tools/puppet_pack/look_cap.py" % OLD],
                         capture_output=True, check=True).stdout
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, "look_cap_old_src.py")
    with open(path, "wb") as f:
        f.write(src)
    spec = importlib.util.spec_from_file_location("puppet_pack._look_cap_old", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make(out, ctx):
    ctx.say("look_cap_old: tools/puppet_pack/look_cap.py of %s (before the fix 3d91ec10a)" % OLD)
    return _sibling().build(out, ctx, old_look_cap(ctx.tools, out))

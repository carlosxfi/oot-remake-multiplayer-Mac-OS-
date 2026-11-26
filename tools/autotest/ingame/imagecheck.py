"""Compare a bridge shot with its stored reference: SSIM (scikit-image) on the grey picture, both scaled to 320 wide;
the score is the worst of 8 x 8 tiles of the SSIM map (ssim()).

Only rows about looks take shots (worker-rules TEST SYSTEM). A shot is the game's framebuffer from the bridge's
`shot` (no overlay, no window, no monitor), so the same scene from the same place gives the same picture up to
animation and lighting noise; `tolerance` is the lowest SSIM a test accepts (each test states its own).

compare() never makes a reference: a missing one is kept as <keep_dir>/new/<name>.png and the result is 'NO-REF'
(not ok). A person looks at it and copies it into tools/autotest/ingame/refs/ once it shows the right thing.
"""
import os
import shutil

WIDTH = 320
GRID = 8


def load_grey(path, width=WIDTH):
    import numpy as np
    from PIL import Image
    with Image.open(path) as im:
        im = im.convert("L")
        h = max(1, round(im.height * width / im.width))
        im = im.resize((width, h), Image.BILINEAR)
        return np.asarray(im, dtype=np.float64) / 255.0


def ssim(a_path, b_path, grid=GRID):
    """The LOWEST mean SSIM of a grid x grid split of the SSIM map: a small thing missing (a mask, a HUD block) drops
    its own tile even when the rest of the picture is the same (the whole picture's mean SSIM would hide it)."""
    from skimage.metrics import structural_similarity
    a, b = load_grey(a_path), load_grey(b_path)
    if a.shape != b.shape:
        return 0.0, "sizes differ %s vs %s (aspect ratio changed)" % (a.shape, b.shape)
    _, m = structural_similarity(a, b, data_range=1.0, full=True)
    h, w = m.shape
    tiles = [m[i * h // grid:(i + 1) * h // grid, j * w // grid:(j + 1) * w // grid].mean()
             for i in range(grid) for j in range(grid)]
    return float(min(tiles)), None


def hue_of(rgb):
    """The hue (0-360) of a 0xRRGGBB colour."""
    import colorsys
    r, g, b = ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0
    return colorsys.rgb_to_hsv(r, g, b)[0] * 360.0


def changed_hue(before, after, rgb, width=WIDTH, moved=0.15, saturated=0.25, near=30.0):
    """vest-link (colour-aware look check): the pixels that CHANGED between two shots of the same place (`before`,
    `after`), and how many of them take the hue of `rgb` in `after`. No guessed mask region: what a recolour changes is
    found by the change itself. A pixel changed when one channel moved by more than `moved` (0..1); it counts as the
    colour when it is saturated (HSV S > `saturated`; grey/dark pixels have no hue) and its hue is within `near`
    degrees of rgb's. Returns {changed, coloured, share, hue}. The thresholds are this check's tolerances, stated by
    each caller's assert, not values of the game."""
    import numpy as np
    from PIL import Image

    def load(path, size=None):
        with Image.open(path) as im:
            im = im.convert("RGB")
            if size is None:
                size = (width, max(1, round(im.height * width / im.width)))
            return np.asarray(im.resize(size, Image.BILINEAR), dtype=np.float64) / 255.0, size

    a, size = load(before)
    b, _ = load(after, size)
    moved_px = np.abs(b - a).max(axis=2) > moved
    hsv = np.asarray(Image.fromarray((b * 255).astype("uint8"), "RGB").convert("HSV"), dtype=np.float64) / 255.0
    hue, sat = hsv[..., 0] * 360.0, hsv[..., 1]
    want = hue_of(rgb)
    off = np.abs((hue - want + 180.0) % 360.0 - 180.0)
    coloured = moved_px & (sat > saturated) & (off <= near)
    n, c = int(moved_px.sum()), int(coloured.sum())
    return {"changed": n, "coloured": c, "share": round(c / n, 3) if n else 0.0, "hue": round(want, 1)}


def compare(shot, ref, tolerance, keep_dir=None):
    out = {"path": shot, "ref": ref, "score": None, "ok": False, "tolerance": tolerance}
    if not shot or not os.path.exists(shot):
        out["why"] = "no shot file (%s)" % shot
        return out
    if keep_dir:
        os.makedirs(os.path.join(keep_dir, "shots"), exist_ok=True)
        kept = os.path.join(keep_dir, "shots", os.path.basename(shot))
        shutil.copyfile(shot, kept)
        out["path"] = kept
    if not os.path.exists(ref):
        if keep_dir:
            os.makedirs(os.path.join(keep_dir, "new"), exist_ok=True)
            shutil.copyfile(shot, os.path.join(keep_dir, "new", os.path.basename(ref)))
        out["why"] = "NO-REF: no reference %s yet (candidate kept under new/)" % os.path.basename(ref)
        return out
    score, err = ssim(shot, ref)
    out["score"] = round(score, 4)
    if err:
        out["why"] = err
        return out
    out["ok"] = score >= tolerance
    out["why"] = "SSIM %.4f %s %.2f" % (score, ">=" if out["ok"] else "<", tolerance)
    return out

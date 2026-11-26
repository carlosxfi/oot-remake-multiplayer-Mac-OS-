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

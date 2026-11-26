"""Decode the kit's own randomizer presets with the pinned OOTRR and check what they promise (row 53).

The strings are read from hookshot/src/coop_rando.h (OwnPresetString: the one source), the OOTRR release from
tools/release/kit/hookshot/OOTRR.txt (release, url, the file for this system and its SHA-256). The release file is
downloaded, its SHA-256 checked, and `ootrr settings-dump --decode <string>` run on each preset. Checked:
  - every preset decodes with no warnings;
  - every preset sets door_of_time_escape "moat" (the Ocarina of Time lies in the castle moat from the start, so
    HF Ocarina of Time Item and Song from Ocarina of Time need no Spiritual Stones);
  - open and open_fast differ only in gerudo_fortress (open / fast), as docs/randomizer.md says.
Any failure, a failed download or a wrong checksum included, exits 1 with one line saying what. Never skipped.

    python tools/rando/check_presets.py [--ootrr <path to an ootrr(.exe) of the pinned release>]
"""

import argparse
import hashlib
import io
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "hookshot" / "src" / "coop_rando.h"
PIN = ROOT / "tools" / "release" / "kit" / "hookshot" / "OOTRR.txt"


class Fail(Exception):
    pass


def own_presets(header_text):
    """{name: string} from OwnPresetString's `if (name == "x") return "OOTRR1-...";` lines."""
    found = dict(re.findall(r'if \(name == "([a-z_]+)"\) return "(OOTRR1-[A-Z0-9]+)";', header_text))
    if not found:
        raise Fail("no OwnPresetString presets found in %s" % HEADER.name)
    return found


def read_pin(text):
    pin = {}
    for line in text.splitlines():
        if line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        pin[k.strip()] = v.strip()
    for k in ("release", "url", "windows", "windows_sha256", "linux", "linux_sha256"):
        if not pin.get(k):
            raise Fail("OOTRR.txt has no %s=" % k)
    return pin


def check(decoded):
    """decoded: {name: {"settings": {...}, "warnings": [...]}} -> list of failure lines (empty: all good)."""
    bad = []
    for name, d in sorted(decoded.items()):
        if d.get("warnings"):
            bad.append("%s: OOTRR warns: %s" % (name, "; ".join(d["warnings"])))
        esc = d.get("settings", {}).get("door_of_time_escape")
        if esc != "moat":
            bad.append("%s: door_of_time_escape is %r, not 'moat' (row 53)" % (name, esc))
    if "open" in decoded and "open_fast" in decoded:
        a, b = decoded["open"]["settings"], decoded["open_fast"]["settings"]
        diff = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
        if diff != ["gerudo_fortress"] or a.get("gerudo_fortress") != "open" or b.get("gerudo_fortress") != "fast":
            bad.append("open and open_fast must differ only in gerudo_fortress (open / fast); they differ in: %s"
                       % ", ".join("%s (%r / %r)" % (k, a.get(k), b.get(k)) for k in diff))
    return bad


def fetch_ootrr(pin, into):
    windows = os.name == "nt"
    if not windows and (platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64")):
        raise Fail("the pinned OOTRR has no file for %s %s" % (platform.system(), platform.machine()))
    name, want = (pin["windows"], pin["windows_sha256"]) if windows else (pin["linux"], pin["linux_sha256"])
    url = pin["url"].rstrip("/") + "/" + name
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
    except Exception as e:  # any network error is a failure, never a skip
        raise Fail("download of %s failed: %s" % (url, e))
    got = hashlib.sha256(data).hexdigest()
    if got != want.lower():
        raise Fail("%s: SHA-256 %s, OOTRR.txt pins %s" % (name, got, want))
    exe = "ootrr.exe" if windows else "ootrr"
    if name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            member = next((m for m in z.namelist() if m.replace("\\", "/").split("/")[-1] == exe), None)
            if member is None:
                raise Fail("%s has no %s" % (name, exe))
            z.extractall(into)
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            members = [m for m in t.getmembers() if m.isfile() and ".." not in m.name.split("/")]
            member = next((m.name for m in members if m.name.split("/")[-1] == exe), None)
            if member is None:
                raise Fail("%s has no %s" % (name, exe))
            t.extractall(into, members=members)
    path = pathlib.Path(into) / member
    if not windows:
        path.chmod(0o755)
    return path


def decode(ootrr, string):
    r = subprocess.run([str(ootrr), "settings-dump", "--decode", string], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise Fail("ootrr settings-dump --decode %s... exited %d: %s" % (string[:16], r.returncode, r.stderr.strip()))
    try:
        return json.loads(r.stdout)
    except ValueError as e:
        raise Fail("ootrr settings-dump output is not JSON: %s" % e)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ootrr", help="an ootrr of the pinned release (default: download it)")
    a = ap.parse_args(argv)
    try:
        presets = own_presets(HEADER.read_text(encoding="utf-8"))
        pin = read_pin(PIN.read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as tmp:
            ootrr = pathlib.Path(a.ootrr) if a.ootrr else fetch_ootrr(pin, tmp)
            decoded = {n: decode(ootrr, s) for n, s in presets.items()}
        bad = check(decoded)
    except Fail as e:
        print("check_presets: FAIL: %s" % e)
        return 1
    for line in bad:
        print("check_presets: FAIL: %s" % line)
    if bad:
        return 1
    print("check_presets: OK: %s decoded with OOTRR %s: door_of_time_escape moat; open / open_fast differ only in "
          "gerudo_fortress" % (", ".join(sorted(presets)), pin["release"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

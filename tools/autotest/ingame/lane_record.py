"""lane-server.txt for game folders lane-game-for.ps1 did not install (the short gate's packaged kit pair), so the
bridge tests run on them with --kit-ls (bt_plugin: lane_check + the boot of those folders).

    python -I tools/autotest/ingame/lane_record.py KIT_LS --lane 1 --commit SHA --server EXE [--protocol N] [--by NAME]

Writes KIT_LS/laneN-A/lane-server.txt and KIT_LS/laneN-B/lane-server.txt (pair.write_lane_server: lane-game-for.ps1's
fields), then runs pair.lane_check on them: stdout is ONE JSON object {ok, lane, commit, dll_sha256} or {ok: false,
error}; exit 0 / 2 (SETUP-FAIL).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pair as P   # noqa: E402


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("kit_ls")
    p.add_argument("--lane", type=int, default=1)
    p.add_argument("--commit", required=True)
    p.add_argument("--server", required=True)
    p.add_argument("--protocol", default="")
    p.add_argument("--by", default="lane_record")
    a = p.parse_args(argv)
    try:
        for x in ("A", "B"):
            P.write_lane_server(os.path.join(a.kit_ls, "lane%d-%s" % (a.lane, x)), a.commit, a.server, a.protocol, a.by)
        info = P.lane_check(a.kit_ls, a.lane)
    except P.SetupFail as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        return 2
    print(json.dumps({"ok": True, "lane": a.lane, "commit": info["commit"], "dll_sha256": info["dll_sha256"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())

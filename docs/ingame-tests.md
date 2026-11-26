# In-game tests (one system, over the play-test bridge)

Every in-game check is a pytest test in `tools/autotest/ingame/`, written once and run every rc. Tests drive the
games only through the play-test bridge ([playtest.md](playtest.md), "Bridge protocol"): set-up by `warp`, `place`,
`setflag`/`flag`, `give`; input by the bridge's `input` with frame steps. No OS key presses, no sleeps, no window
focus, no screen grabs, so the monitor, the display and windows on top cannot break a run.

Maintainer only, like the rest of `tools/autotest`: it needs a test lane (`tools/autotest/lanes.py`) installed by
`offline/lane-game-for.ps1` (its `lane-server.txt`).

## Running

```sh
python -I -m pytest tools/autotest/ingame --lanes 1 --expect-commit <sha> --out <dir>   # lane 1
python -I -m pytest tools/autotest/ingame --lanes 1 --expect-commit <sha> --lane-check-only --out <dir>  # setup proof, no boot
python -I -m pytest tools/autotest/ingame -n 2 --lanes 1,2 --out <dir>             # both lanes at once
python -I -m pytest tools/autotest/ingame -m smoke --lanes 1 --out <dir>           # the rc smoke tier only
python -I -m pytest tools/autotest/ingame --lanes 1 --repeat 10 --no-smoke-gate --out <dir>   # flake rate
python -I -m pytest tools/autotest/ingame --fake --out <dir>                       # no game: fake bridges
```

One game pair is booted per lane (`live.py start --lane L --playtest`, the lane's own server from `lane-server.txt`)
and every test of that lane runs on it. With `-n 2` xdist worker N takes the Nth lane of `--lanes`. Ports: lane L,
player N on `5000 + 10*L + N` (playtest.md). The boot's setup proof is `pair.lane_check` (the same rule as
`offline/lib/lane-server.ps1` + `lane-pair.ps1`: both games' module is the one installed, from the same patches
build, the server exists), then `hello` on both bridges with Longshot's control answering. `--expect-commit` (the lane's module sha, from
`lane-server.txt`'s `commit=`) is required on a real lane. `--lane-check-only` runs that proof and the collection
only (no boot, no test): one `SETUP-OK`/`SETUP-FAIL` line per lane, `<dir>/setup.json`, exit 0 only if all are OK.

## Results

One summary per run: `<dir>/summary.json` and `summary.md` (add `--junitxml <file>` for JUnit).

| Result | When |
|---|---|
| PASS | every assert held |
| FAIL | an assert failed, or the test ran past its `budget` (FAIL `timeout`); a failed test reruns ONCE |
| FLAKY | failed, then passed on the rerun: never a pass |
| SETUP-FAIL | the lane, the boot, or the test's own set-up (`with pair.setup():`, under 30 s) did not hold; not rerun |
| NO-REF | a look check has no stored reference yet; its shot is kept under `<dir>/lane<L>/new/` |

A smoke test (`@pytest.mark.smoke`) that FAILs or SETUP-FAILs stops the run: the rc is broken.

Each test has a hard deadline (`@pytest.mark.budget(seconds)`, default 120): every bridge call checks it, and every
wait is counted in game frames with a ceiling (`pair.until`, `pair.expect_log`), so a test ends by itself.
pytest-timeout (pytest.ini, 1800 s) is only the backstop for a call that never returns.

## Judging

- By state: the bridge's state JSON (`scene`, `pos`, `equips`, `coop.players`, `world`, flags by `flag_read`), the
  module's log lines (`pair.expect_log`: the game's `game-out-X.txt`, only lines written since the test began), and
  checkpoints with `desync_check.py`.
- By picture ONLY for what a player sees (worker-rules TEST SYSTEM: other players' Epona animated, menu UI, HUD UI,
  name tags, balloons, pause pages, a new or changed look): `pair.check_shot(game, name, tolerance)` on EACH game,
  twice a few frames apart where animation must show. The shot is the game's framebuffer (the bridge's `shot`), and is
  compared to `tools/autotest/ingame/refs/<name>-<game>.png` by SSIM (scikit-image): the score is the worst of 8 x 8
  tiles of the SSIM map, so one small missing thing fails its tile. Shots are kept with the result
  (`<dir>/lane<L>/shots/`). A test never writes a reference: a person checks a NO-REF candidate and copies it into
  `refs/`.

## Writing a test

```python
@pytest.mark.budget(120)
@pytest.mark.row("#91 masks on the other player's puppet")
def test_x(pair):
    with pair.setup():                         # SETUP-FAIL if anything here fails or takes > 30 s
        pair.give("A", "PouchData.Armor_Child_Mask_Skull.StockNum")
    pair.act("A", "wear head Armor_Child_Mask_Skull")
    pair.expect_log("B", r"P1 wears head 'Armor_Child_Mask_Skull'", max_frames=300)
```

Every key, flag, line and log pattern cites its source in the test (worker-rules "no guessed values"); shared tables
are imported (`ocarina_notes.KEY_NOTE`), never copied. `pair.step_both(n)` runs both games in step; never let one game
run alone. The selftests (`tools/autotest/tests/test_ingame.py`, in CI) run the whole suite on two fake bridges
(`ingame/fakegame.py`).

## Tools (yes / no)

| Tool | | Why |
|---|---|---|
| pytest, pytest-xdist, pytest-timeout | yes | owner rule; xdist workers = lanes; timeout as the backstop only (on Windows it can only end the worker) |
| scikit-image, Pillow | yes | SSIM of shots against references |
| a virtual display driver | not needed now | bridge shots come from the framebuffer, not the screen; windows still open somewhere (live.py `--display`); worth it only so no window ever lands on the owner's monitor |
| Toxiproxy / clumsy | later, yes | lag and loss tests (playtest.md "Lag on purpose" has run.ps1's relays today) |

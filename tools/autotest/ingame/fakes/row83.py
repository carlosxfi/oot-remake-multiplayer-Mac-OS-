"""Row 83 (test_unfocus.py): what a real lane game gives the back-window fps test, on the fake games.

- setup: the game's longshot_options.ini with frame_rate_cap=0 (the lanes' own: coop-kit/ls/lane1-A), and Longshot's
  'hid: game window not in front' line (ls31-src host/nn_hid.c:317): live.py opens the games behind.
- command: while the bridge's `resume` lets the game run, one 5 s present window every 0.1 s (ls31-src host/nn_vi.c
  :704-705's two lines) at BT_FAKE_FPS_<tag> fps (default 60; under 59 a third of the presents are over 22 ms);
  `pause`, `step` and `input` hold it again and stop them. Frames run as at the 30 calc rate (150 per window).
"""
import os
import threading

_RUN = {}     # game -> its real-time thread's stop event


def setup(game):
    with open(os.path.join(game.folder, "longshot_options.ini"), "w", encoding="utf-8") as f:
        f.write("frame_rate_cap=0\n")
    game.say('hid: game window not in front (foreground window: process 4242, class "FakeShell")')


def _stop(game):
    ev = _RUN.pop(game, None)
    if ev:
        ev.set()


def _start(game):
    if game in _RUN:
        return
    stop, fps = threading.Event(), float(os.environ.get("BT_FAKE_FPS_%s" % game.tag) or 60)
    _RUN[game] = stop

    def go():
        n = 0
        while not stop.wait(0.1):
            if game.srv.fileno() == -1:          # (the fake lane closed)
                return
            with game.lock:
                game.run(150)
                k = int(fps * 5)
                n += k
                late = 0 if fps >= 59 else k // 3
                game.say("vi: %d frames presented (%.1f fps over the last 5.0 s, interval 1)" % (n, fps))
                game.say("vi: present intervals: <15.5 ms 0, 15.5-17.8 %d, 17.8-22 0, 22-30 %d, 30-40 0, over 40 0"
                         % (k - late, late))
    threading.Thread(target=go, daemon=True).start()


def command(game, m):
    cmd = m.get("cmd")
    if cmd == "resume":
        _start(game)
    elif cmd in ("pause", "step", "input"):
        _stop(game)
    return None                                  # the built-in answer still runs

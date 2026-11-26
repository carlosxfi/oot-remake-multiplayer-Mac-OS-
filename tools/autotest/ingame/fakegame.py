"""Two fake games for the in-game tests' selftests (--fake): each a real TCP bridge on 127.0.0.1 speaking the play-test
protocol (docs/playtest.md "Bridge protocol"), with a module log file, frames, hold/step, and the few reactions the
pilot tests look for (a connect, puppets, masks, the ocarina, a shared flag, /teleport, the pause, the title).

It proves the runner, not the game: results, budgets, SETUP-FAIL / FLAKY / NO-REF, the summary, shots and their
compare, the lane fixture. What a reaction logs is copied from the tests' own patterns (the old rows' expect=).
"""
import json
import math
import os
import socket
import threading

GATE = "Npc_Hylia_Soldier_003_GateOpen"


class FakeGame:
    def __init__(self, tag, folder, lane):
        self.tag, self.folder, self.lane = tag, folder, lane
        os.makedirs(os.path.join(folder, "playtest", "dumps"), exist_ok=True)
        os.makedirs(os.path.join(folder, "playtest", "shots"), exist_ok=True)
        self.log_path = os.path.join(folder, "game-out-%s.txt" % tag)
        self.lock = threading.RLock()
        self.frame, self.paused, self.scene = 100, False, "KokiriForest_Child"
        self.pos, self.facing = [0.0 if tag == "A" else 4.0, 0.0, 0.0], 0.0
        self.menu_pause, self.title_until, self.at_title = False, None, False
        self.head, self.ocarina, self.flags, self.pending = None, False, {GATE: 0}, []
        self.other = None
        open(self.log_path, "w").close()
        me, them = (1, 2) if tag == "A" else (2, 1)
        self.say("coop: multiplayer tab: test: to-title.now in %s starts the way back to the title" % folder)
        self.say("coop: connected to 127.0.0.1")
        self.say("coop: puppet: P%d CoopPuppet_Child_Green spawned after 120 ms" % them)
        if tag == "A":
            self.say("coop: drawing over Hookshot's picture")
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    def say(self, line):
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write("[hos fake] %s\n" % line)

    def later(self, frames, fn):
        self.pending.append((self.frame + frames, fn))

    # -- frames -----------------------------------------------------------------------------------------------
    def run(self, n):
        for _ in range(int(n)):
            self.frame += 1
            due = [p for p in self.pending if p[0] <= self.frame]
            self.pending = [p for p in self.pending if p[0] > self.frame]
            for _, fn in due:
                fn()
            if os.path.exists(os.path.join(self.folder, "to-title.now")):
                os.remove(os.path.join(self.folder, "to-title.now"))
                self.say("coop: multiplayer tab: test: back to the title asked (to-title.now)")
                self.at_title = True

    def in_play(self):
        return not self.at_title and not self.menu_pause

    def state(self):
        live = self.in_play()
        return {"frame": self.frame, "paused": self.paused, "scene": self.scene if not self.at_title else None,
                "pos": list(self.pos) if live else None, "facing": self.facing if live else None,
                "age": "child" if live else None, "loading": False if live else None, "event": False if live else None,
                "not_ready": None if live else "no fresh Link sample", "control": True,
                "equips": {"weapon": "KokiriSword", "shield": None, "head": self.head, "armor": "Kokiri", "boots": "Normal"},
                "coop": {"players": [{"number": 1, "name": "A", "state": "connected"},
                                     {"number": 2, "name": "B", "state": "connected"}]},
                "world": {"actors": [{"id": 1, "name": "CoopPuppet_Child_Green", "kind": "player",
                                      "pos": list(self.other.pos)}]}}

    # -- commands ---------------------------------------------------------------------------------------------
    def answer(self, m):
        cmd = m.get("cmd")
        r = {"id": m.get("id"), "ok": True}
        if cmd == "hello":
            r.update(protocol=1, control=True)
        elif cmd == "pause":
            self.paused = True
        elif cmd == "resume":
            self.paused = False
        elif cmd == "state":
            r.update(self.state())
        elif cmd in ("step", "input"):
            r["from"] = self.frame
            if cmd == "input":
                self.press(m.get("buttons") or [])
            self.run(m.get("frames", 1))
            self.paused = True
            r["state"] = self.state()
        elif cmd == "warp":
            self.run(40)
            self.scene = m["scene"]
            r.update(arrived=True, via="teleport", map=m["scene"], state=self.state())
        elif cmd == "act":
            self.act(m["lines"])
            r.update(lines=1, note="ok")
        elif cmd == "give":
            r["result"] = "given"
        elif cmd == "flag":
            r["was"] = self.flags.get(m["name"], 0)
            self.flags[m["name"]] = m["value"]
            if m["name"] == GATE and m["value"] == 1 and r["was"] == 0:
                self.say("coop: story: bool 0x19898d14 (entry 3) 0 -> 1")
                o = self.other
                o.later(12, lambda: (o.flags.__setitem__(GATE, 1), o.say("coop: story: bool 0x19898d14 (entry 3) 0 -> 1")))
            r["value"] = m["value"]
        elif cmd == "flag_read":
            r.update(kind="bool", value=self.flags.get(m["name"]))
        elif cmd == "dump":
            path = os.path.join(self.folder, "playtest", "dumps", "%s.json" % m["tag"])
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"frame": self.frame, "tag": m["tag"], "state": self.state(), "flags": dict(self.flags)}, f)
            r.update(path=path, flags=len(self.flags))
        elif cmd == "shot":
            path = os.path.join(self.folder, "playtest", "shots", "%s.png" % m.get("name", "shot"))
            self.picture(path)
            self.run(1)
            r.update(path=path, width=320, height=180, advanced=1)
        else:
            r = {"id": m.get("id"), "ok": False, "error": "bad_request", "message": "fake: no %s" % cmd}
        r.setdefault("frame", self.frame)
        r.setdefault("paused", self.paused)
        return r

    def press(self, buttons):
        o, me = self.other, 1 if self.tag == "A" else 2
        if "PLUS" in buttons:
            self.menu_pause = not self.menu_pause
        if self.at_title and "A" in buttons:
            self.later(30, lambda: setattr(self, "at_title", False))
            return
        if "LEFT" in buttons:
            self.ocarina = True
            self.later(20, lambda: self.say("coop: ocarina probe: our Link plays the ocarina"))
            return
        if "B" in buttons:
            self.ocarina = False
        notes = {"L": (4, "D"), "R": (8, "F"), "Y": (11, "B"), "X": (16, "HighD"), "A": (14, "A")}
        for b in buttons:
            if self.ocarina and b in notes:
                n, name = notes[b]
                self.later(3, lambda n=n, name=name: self.say("coop: ocarina note %d (Ocarina_%s) sent" % (n, name)))
                o.later(8, lambda name=name: o.say("coop: puppet: P%d plays sound 'Ocarina_%s'" % (me, name)))

    def act(self, line):
        w = line.split()
        o, me = self.other, 1 if self.tag == "A" else 2
        if w[0] == "place":
            self.pos = [float(v) for v in w[1:4]]
        elif w[0] == "face":
            dx, dz = float(w[1]) - self.pos[0], float(w[2]) - self.pos[2]
            self.facing = math.degrees(math.atan2(dx, dz)) % 360
        elif w[0] == "wear" and w[1] == "head":
            self.head = None if w[2] == "none" else w[2]
        elif w[0] == "dress":
            head = self.head
            if head:
                o.later(10, lambda: o.say("coop: puppet: P%d wears head '%s' (returned 0x1)" % (me, head)))
            else:
                o.later(10, lambda: o.say("coop: puppet: P%d takes off head (returned 0x1)" % me))
        elif w[0] == "chat" and w[1:] == ["/teleport", "B"]:
            self.later(30, lambda: setattr(self, "pos", [o.pos[0] + 1.0, o.pos[1], o.pos[2]]))

    def picture(self, path):
        from PIL import Image, ImageDraw
        im = Image.new("RGB", (320, 180), (40, 90, 40))
        d = ImageDraw.Draw(im)
        d.rectangle([20, 20, 120, 40], fill=(200, 30, 30))             # "hearts"
        d.ellipse([140, 60, 180, 140], fill=(230, 220, 200) if not self.menu_pause else (20, 20, 20))
        d.rectangle([150 + self.frame % 3, 50, 170, 70], fill=(240, 240, 240) if self.head else (40, 90, 40))
        im.save(path)

    # -- socket -----------------------------------------------------------------------------------------------
    def serve(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            with conn, conn.makefile("rb") as rf:
                for line in rf:
                    with self.lock, self.other.lock:
                        reply = self.answer(json.loads(line))
                    try:
                        conn.sendall((json.dumps(reply) + "\n").encode())
                    except OSError:
                        break
            self.paused = False

    def close(self):
        self.srv.close()


class FakeLane:
    def __init__(self, out_dir, lane=1):
        root = os.path.join(out_dir, "fake")
        self.g = {x: FakeGame(x, os.path.join(root, "lane%d-%s" % (lane, x)), lane) for x in ("A", "B")}
        self.g["A"].other, self.g["B"].other = self.g["B"], self.g["A"]

    def games(self):
        return {x: {"port": g.port, "folder": g.folder, "log": g.log_path} for x, g in self.g.items()}

    def close(self):
        for g in self.g.values():
            g.close()

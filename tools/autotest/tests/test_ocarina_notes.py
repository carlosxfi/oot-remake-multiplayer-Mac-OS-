import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import ocarina_notes as on  # noqa: E402

ROOT = os.path.join(HERE, "..", "..", "..")
# the note numbers of the game's OcarinaMgr table, 1-based (lichii/hook/coop_ocarina_sound.cpp NoteName, docs/puppet.md)
NOTE_NUMBER = {"D": 4, "F": 7, "A": 11, "B": 13, "HighD": 16}


class OcarinaNotesTest(unittest.TestCase):
    def test_the_module_sends_the_note_the_game_plays(self):
        # the module's NoteOf against the one table (read from the game: run epona-seen-keys-fec750ad)
        with open(os.path.join(ROOT, "lichii", "hook", "coop_ocarina_sound.cpp"), encoding="utf-8") as f:
            src = f.read()
        body = re.search(r"std::uint8_t NoteOf\(std::uint64_t button\) \{(.*?)\n\}", src, re.S).group(1)
        cases = dict(re.findall(r"case kButton([A-Z]+): return (\d+);", body))
        self.assertEqual(set(cases), set(on.KEY_NOTE))
        for key, note in on.KEY_NOTE.items():
            with self.subTest(key=key):
                self.assertEqual(int(cases[key]), NOTE_NUMBER[note], "%s plays %s in the game" % (key, note))

    def test_every_song_is_five_key_notes_and_maps_back(self):
        self.assertEqual(sorted(on.NOTE_KEY), sorted(NOTE_NUMBER))
        for song, notes in on.SONGS.items():
            with self.subTest(song=song):
                self.assertEqual(len(notes), 6)
                self.assertEqual([on.KEY_NOTE[k] for k in on.keys(song)], notes)
        self.assertEqual(on.keys("EponaSong"), ["X", "Y", "A", "X", "Y", "A"])

    def test_every_song_s_keys_go_through_noteof_to_its_notes(self):
        # keys(song) through the module's NoteOf (coop_ocarina_sound.cpp) are the song's notes, note by note
        with open(os.path.join(ROOT, "lichii", "hook", "coop_ocarina_sound.cpp"), encoding="utf-8") as f:
            body = re.search(r"std::uint8_t NoteOf\(std::uint64_t button\) \{(.*?)\n\}", f.read(), re.S).group(1)
        noteof = {k: int(n) for k, n in re.findall(r"case kButton([A-Z]+): return (\d+);", body)}
        for song, notes in on.SONGS.items():
            with self.subTest(song=song):
                self.assertEqual([noteof[k] for k in on.keys(song)], [NOTE_NUMBER[n] for n in notes])




class SongStepTest(unittest.TestCase):
    # epona-seen-2: `song NAME [HOLD [GAP]]`, written out by expand() for both runners (hookshot.py parse, run.ps1)
    def test_a_song_is_its_keys_with_the_proven_timings(self):
        lines = on.expand("mark a\nsong EponaSong   # Epona\nmark b")
        self.assertEqual(lines[0], (1, "mark a"))
        self.assertEqual(lines[-1], (3, "mark b"))
        song = lines[1:-1]
        self.assertTrue(all(n == 2 for n, _ in song))
        self.assertEqual([t for _, t in song if t.startswith("press")],
                         ["press %s %d" % (k, on.SONG_HOLD) for k in on.keys("EponaSong")])
        self.assertEqual([t for _, t in song if t.startswith("wait")], ["wait %d" % on.SONG_GAP] * 5)
        self.assertEqual((on.SONG_HOLD, on.SONG_GAP), (24, 90))   # run epona-seen-keys-fec750ad's

    def test_hold_and_gap_on_the_line(self):
        texts = [t for _, t in on.expand("song SariaSong 12 40")]
        self.assertEqual(texts[:3], ["press R 12", "wait 40", "press A 12"])

    def test_bad_lines_are_refused_with_their_number(self):
        for bad in ("song", "song Nope", "song EponaSong 0", "song EponaSong x", "song EponaSong 1 2 3",
                    "song EponaSong ²", "song EponaSong 0000000001"):
            with self.subTest(line=bad):
                with self.assertRaises(on.SongError) as e:
                    on.expand("mark a\n" + bad)
                self.assertTrue(str(e.exception).startswith("line 2:"))

    def test_the_runner_plays_the_presses(self):
        import hookshot as hs
        steps = hs.parse("song EponaSong\nmark z\n")
        presses = [s for s in steps if s.text.startswith("press")]
        self.assertEqual([s.text for s in presses], ["press %s 24" % k for k in on.keys("EponaSong")])
        self.assertTrue(all(s.line == 1 for s in steps[:-1]))
        with self.assertRaises(hs.ScriptError) as e:
            hs.parse("mark a\nsong Nope\n")
        self.assertIn("line 2:", str(e.exception))

    def test_no_scenario_types_a_song_by_hand(self):
        # one table: a song is a `song` line, never its keys copied into a script; also caught: the keys the old,
        # guessed tables gave (NoteOf before 0.12.0: Y = A, X = B, A = high D; the OoT3D guess: A = D, X = B, L = high D)
        guessed = [{"L": "D", "R": "F", "Y": "A", "X": "B", "A": "HighD"},
                   {"A": "D", "R": "F", "Y": "A", "X": "B", "L": "HighD"}]
        tables = [on.KEY_NOTE] + guessed
        import glob
        for path in sorted(glob.glob(os.path.join(os.path.dirname(HERE), "scenarios", "*.txt"))):
            run = []
            for raw in open(path, encoding="utf-8").read().split("\n") + ["end"]:
                w = raw.split("#", 1)[0].split()
                if not w or w[0] in ("wait", "mark"):
                    continue
                if w[0] == "press" and len(w) >= 2 and w[1] in on.KEY_NOTE:
                    run.append(w[1])
                    continue
                for i in range(len(run) - 5):
                    for table in tables:
                        notes = [table[k] for k in run[i:i + 6]]
                        self.assertNotIn(notes, list(on.SONGS.values()),
                                         "%s: a song typed by hand (%s): use `song NAME`" % (path, " ".join(run[i:i + 6])))
                run = []

    @unittest.skipUnless(os.name == "nt", "run.ps1 is Windows PowerShell")
    def test_run_ps1_default_runtime_checks_songs_before_any_game(self):
        # review-b120: run.ps1's default (emulator) path ran the song check with no Python resolved, so every emulator
        # run died. Played here as a run is started (no -Python, no -Runtime): fake kits, -NoRestore, -NoCoop, a fake
        # game file; a bad song line must stop it with SETUP-FAIL (exit 2), a good one must pass the check.
        import shutil
        import subprocess
        import tempfile
        here = os.path.dirname(HERE)
        scenarios = os.path.join(here, "scenarios")
        with tempfile.TemporaryDirectory() as d:
            for k in ("kitA", "kitB"):
                os.makedirs(os.path.join(d, k, "emulator", "user", "config"))
            game = os.path.join(d, "game.nsp")
            open(game, "w").close()
            name = "zz-test-song-%d" % os.getpid()
            script = os.path.join(scenarios, name + ".A.txt")

            def run(text):
                with open(script, "w", encoding="ascii", newline="") as f:
                    f.write(text)
                return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                       os.path.join(here, "run.ps1"), "-Scenario", name, "-KitA", os.path.join(d, "kitA"),
                                       "-KitB", os.path.join(d, "kitB"), "-Game", game, "-NoRestore", "-NoCoop",
                                       "-Shots", os.path.join(d, "shots"), "-Display", "no-such-display"],
                                      capture_output=True, text=True, timeout=120)
            try:
                r = run("mark start\r\nsong EponaSongg\r\nmark end\r\n")
                self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
                self.assertIn("SETUP-FAIL scenario %s:" % name, r.stdout)
                self.assertIn("bad song line", r.stdout)
                r = run("mark start\r\nsong EponaSong\r\nmark end\r\n")
                self.assertIn("song lines checked: %s" % name, r.stdout, r.stdout + r.stderr)
                self.assertNotIn("SETUP-FAIL", r.stdout)
            finally:
                if os.path.exists(script):
                    os.remove(script)
                shutil.rmtree(os.path.join(d, "shots"), ignore_errors=True)

    def test_the_command_line_writes_the_script_out(self):
        # run.ps1 -Runtime emulator: `ocarina_notes.py expand SCRIPT` before the game reads the script
        import subprocess
        import tempfile
        tool = os.path.join(os.path.dirname(HERE), "ocarina_notes.py")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "s.txt")
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write("mark a\r\nsong ZeldaLullaby 12 40\r\nmark b\r\n")
            r = subprocess.run([sys.executable, "-I", tool, "expand", path], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = r.stdout.splitlines()
            self.assertEqual(out[0], "mark a")
            self.assertEqual([ln.split()[1] for ln in out if ln.startswith("press")], on.keys("ZeldaLullaby"))
            with open(path, "w", encoding="utf-8") as f:
                f.write("song Nope\n")
            r = subprocess.run([sys.executable, "-I", tool, "expand", path], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("line 1:", r.stderr)


if __name__ == "__main__":
    unittest.main()

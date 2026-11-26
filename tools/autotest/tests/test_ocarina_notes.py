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


if __name__ == "__main__":
    unittest.main()

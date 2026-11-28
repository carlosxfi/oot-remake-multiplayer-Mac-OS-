"""The Ocarina's keys and notes: the ONE table every test script and tool uses (worker-rules "one source of truth").

KEY_NOTE: which note the game plays for each key, read from the game itself, not guessed. Run epona-seen-keys-fec750ad
(2026-11-26, lane 1, scenario hs-ocarina-keys, LICHII_COOP_DEBUG_SOUND=1): each key emitted exactly one Ocarina_<note> and
its own Ocarina_Finger_<key> (coord-multiplayer/offline/epona-seen/hs-ocarina-keys/keys.txt). It is the N64 layout:
A = D, C-down = F, C-left = B, C-up = high D, C-right = A. The module's NoteOf (lichii/hook/coop_ocarina_sound.cpp) must
send the same notes: tests/test_ocarina_notes.py checks it. Three runs before this one played songs from guessed keys
and none was a song.

SONGS: each song as notes, as Ocarina of Time writes them (the game's songbook, "Check songbook"). A song is proven in
the game once a run plays it with keys(); NOT_PLAYED lists those no run has played yet.
"""

import re
import sys

# the game's note names (OcarinaMgr's table, docs/puppet.md "The note table"), the five the keys play
KEY_NOTE = {"L": "D", "R": "F", "Y": "B", "X": "HighD", "A": "A"}
NOTE_KEY = {note: key for key, note in KEY_NOTE.items()}

SONGS = {
    "ZeldaLullaby": ["B", "HighD", "A", "B", "HighD", "A"],
    "EponaSong": ["HighD", "B", "A", "HighD", "B", "A"],
    "SariaSong": ["F", "A", "B", "F", "A", "B"],
    "SunSong": ["A", "F", "HighD", "A", "F", "HighD"],
    "TimeSong": ["A", "D", "F", "A", "D", "F"],
    "StormSong": ["D", "F", "HighD", "D", "F", "HighD"],
}
NOT_PLAYED = set(SONGS)   # none yet: hs-epona-seen plays EponaSong next


def keys(song):
    """The keys that play a song, in order: keys("EponaSong") == ["X", "Y", "A", "X", "Y", "A"]."""
    return [NOTE_KEY[n] for n in SONGS[song]]

# `song NAME [HOLD [GAP]]`, the test runners' step (hookshot.py parse; run.ps1 -Runtime emulator writes it out before
# the game reads the script): the song's keys from keys(), each `press KEY HOLD`, `wait GAP` between two notes. The
# defaults are run epona-seen-keys-fec750ad's (hs-ocarina-keys.B: every key held 24 ticks, 90 between, played its
# note); a script that wants other timings says so on the line.
SONG_HOLD = 24
SONG_GAP = 90


class SongError(ValueError):
    pass


def _ticks(word):
    # 1 to 36000 (a wait's or a press's most), ASCII digits only, at most 9 of them (coop_testinput.cpp SongTicks: the same rule)
    return int(word) if re.fullmatch(r"[0-9]{1,9}", word) and 0 < int(word) <= 36000 else None


def expand(text):
    """The script's lines as (line number, text), each `song` line written out as its presses and waits (same number)."""
    out = []
    for number, raw in enumerate(text.split("\n"), 1):
        w = raw.split("#", 1)[0].split()
        if not w or w[0].upper() != "SONG":
            out.append((number, raw))
            continue
        if not 2 <= len(w) <= 4 or w[1] not in SONGS or any(_ticks(t) is None for t in w[2:]):
            raise SongError("line %d: want: song NAME [HOLD [GAP]], NAME one of %s" % (number, " ".join(sorted(SONGS))))
        hold = int(w[2]) if len(w) >= 3 else SONG_HOLD
        gap = int(w[3]) if len(w) >= 4 else SONG_GAP
        for i, key in enumerate(keys(w[1])):
            if i:
                out.append((number, "wait %d" % gap))
            out.append((number, "press %s %d" % (key, hold)))
    return out


def main(argv):
    # run.ps1 -Runtime emulator: `ocarina_notes.py expand SCRIPT` prints SCRIPT with every song line written out
    if len(argv) != 2 or argv[0] != "expand":
        sys.stderr.write("usage: ocarina_notes.py expand SCRIPT\n")
        return 2
    with open(argv[1], encoding="utf-8-sig") as f:
        text = f.read().replace("\r\n", "\n")
    try:
        sys.stdout.write("\n".join(line for _, line in expand(text)))
    except SongError as e:
        sys.stderr.write("%s: %s\n" % (argv[1], e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

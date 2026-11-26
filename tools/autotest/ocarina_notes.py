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

"""Tests for puppet_pack.start_points. No romfs here: the row's form, and the committed coop_start_points.inc
checked for what the module relies on (issue 289: a start with no entrance actor of its name has no starter for
its arrival event; the game's own entries to those scenes reserve the starter '')."""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from puppet_pack import start_points as sp  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
INC = os.path.join(REPO, "lichii", "hook", "coop_start_points.inc")
ROW = re.compile(r'^    \{"([A-Za-z0-9_]+)", "([A-Za-z0-9_]+)", (-?\d+\.\d\df), (-?\d+\.\d\df), (-?\d+\.\d\df), '
                 r'(true|false)\},$')


def rows():
    with open(INC, encoding="utf-8") as f:
        lines = [line.rstrip("\n") for line in f if not line.startswith("//")]
    out = []
    for line in lines:
        m = ROW.match(line)
        assert m, line
        out.append((m.group(1), m.group(2), m.group(6) == "true"))
    return out


class RowTest(unittest.TestCase):
    def test_a_row_says_whether_the_scene_has_an_entrance_actor_of_the_starts_name(self):
        self.assertEqual(sp.row("DodongoCavern", "Side", [1.0, -2.5, 3.25], {"Side", "Other"}),
                         '    {"DodongoCavern", "Side", 1.00f, -2.50f, 3.25f, true},\n')
        self.assertEqual(sp.row("VolvagiaStage", "VolvagiaStage", [0, 0, 0], {"DreamRematch"}),
                         '    {"VolvagiaStage", "VolvagiaStage", 0.00f, 0.00f, 0.00f, false},\n')
        self.assertEqual(sp.row("VolvagiaStage", "VolvagiaStage", [0, 0, 0], ()),
                         '    {"VolvagiaStage", "VolvagiaStage", 0.00f, 0.00f, 0.00f, false},\n')
        self.assertTrue(ROW.match(sp.row("A", "B", [0, 0, 0], ()).rstrip("\n")))


class CommittedTableTest(unittest.TestCase):
    def test_every_row_has_the_form_the_module_and_the_test_runner_read(self):
        table = rows()
        self.assertGreater(len(table), 100)
        self.assertEqual(len({(s, n) for s, n, _ in table}), len(table))  # no start twice

    def test_the_starts_with_no_entrance_actor_are_the_boss_stages_and_the_museum(self):
        without = sorted(s for s, _n, has in rows() if not has)
        self.assertEqual(len(without), 26, without)
        self.assertIn("EnemyMuseum", without)
        self.assertEqual([s for s in without if not s.endswith("Stage")], ["EnemyMuseum"])
        # The ten bosses the game's rematch enters with the starter '' (DreamBossStage's EventReserveBancChange).
        for boss in ("Gohma", "Dodongo", "Barinade", "PhantomGanon", "Volvagia", "Morpha", "BongoBongo", "Twinrova",
                     "Ganondorf", "Ganon"):
            self.assertIn(boss + "Stage", without)
        # Such a scene has just the one start, named like the scene (the museum's is the game's own spelling).
        for scene, name, has in rows():
            if not has:
                self.assertEqual(name, "EnemyMusuem" if scene == "EnemyMuseum" else scene)

    def test_no_scene_ending_in_stage_has_a_starter_and_every_other_but_the_museum_has(self):
        for scene, name, has in rows():
            if scene.endswith("Stage") or scene == "EnemyMuseum":
                self.assertFalse(has, (scene, name))
            else:
                self.assertTrue(has, (scene, name))


if __name__ == "__main__":
    unittest.main()

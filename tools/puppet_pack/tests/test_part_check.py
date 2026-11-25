"""Row 280 guard: a part the kit made never names a file neither it nor the game has (puppet_pack/part_check.py)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from puppet_pack import part_check as pc  # noqa: E402

SKEL = "Model/Player_Dark_Animation.Skeleton.bfres.zs"
PACK = "Pack/Actor/Player_Adult.pack.zs"
POUCH = "Pack/Actor/PouchPlayer_Adult.pack.zs"
INFO = "RSDB/ActorInfo.Product.111.rstbl.byml.zs"
GAME = {PACK, POUCH, INFO, "Model/Player_Dark_Animation.anim.bfres.zs",
        "Model/Player_Adult_Dark.Player_Adult_Dark.bfres.zs", "Pack/ZsDic.pack.zs"}


def names(table):
    return lambda p: set(table.get(p, ()))


class PlanTest(unittest.TestCase):
    def test_row_280_the_adult_packs_go_back_to_the_games(self):
        released = {PACK, POUCH, SKEL, "Model/Player_Adult.Player_Adult.bfres.zs"}
        kept = released - {SKEL}   # (the old NPC rule dropped Model/Player_Dark_*)
        mine = {PACK: ["Player_Dark_Animation", "Player_Adult"], POUCH: ["Player_Dark_Animation"]}
        # (the game's own adult pack names it too: the game has a Model file of that name, so it still counts)
        left, why = pc.plan(released, kept, GAME, names(mine), names({PACK: ["Player_Dark_Animation"]}))
        self.assertEqual(sorted(left), [PACK, POUCH])
        self.assertEqual(why[PACK], [SKEL])

    def test_the_fixed_part_is_kept_whole(self):
        released = {PACK, POUCH, SKEL}
        mine = {PACK: ["Player_Dark_Animation"], POUCH: ["Player_Dark_Animation"]}
        self.assertEqual(pc.plan(released, released, GAME, names(mine)), ([], {}))

    def test_a_name_the_games_copy_has_with_no_such_file_in_that_folder_does_not_count(self):
        cloth = "Phive/Cloth/Player_Adult_Dark.Nin_Ounce_NVN2.bphcl"
        released = {INFO, cloth}
        mine = {INFO: ["Player_Adult_Dark"]}
        # (the game's ActorInfo names Dark Link's cloth; the game has a Model of that name but no Phive one)
        left, _ = pc.plan(released, {INFO}, GAME, names(mine), names({INFO: ["Player_Adult_Dark"]}))
        self.assertEqual(left, [])
        # (a name only the mod's copy has does count)
        left, _ = pc.plan(released, {INFO}, GAME, names(mine), names({}))
        self.assertEqual(left, [INFO])

    def test_guarded_paths_never_count(self):
        model = "Model/Player_Adult_C115.Player_Adult.bfres.zs"
        released = {INFO, model}
        mine = {INFO: ["Player_Adult_C115"]}
        left, _ = pc.plan(released, {INFO}, GAME, names(mine), guarded=r"^Model/Player_(Adult|Child)_C\d{3}\.")
        self.assertEqual(left, [])
        left, _ = pc.plan(released, {INFO}, GAME, names(mine))
        self.assertEqual(left, [INFO])

    def test_a_left_out_file_the_game_lacks_is_missing_for_the_others(self):
        new_pack = "Pack/Actor/Player_Coop_New.pack.zs"     # (not in the game)
        mdl = "Model/Player_Coop_New.Player_Coop_New.bfres.zs"
        released = {new_pack, mdl, SKEL, "Sequence/Coop.module.ainb"}
        kept = released - {SKEL}
        mine = {new_pack: ["Player_Dark_Animation"], "Sequence/Coop.module.ainb": ["Player_Coop_New"]}
        left, why = pc.plan(released, kept, GAME, names(mine))
        self.assertEqual(left, [new_pack, "Sequence/Coop.module.ainb"])
        self.assertEqual(why["Sequence/Coop.module.ainb"], [new_pack])

    def test_data_files_are_not_read(self):
        tex = "Tex/Mt_Body_Player_Adult_Alb.bntx.zs"
        released = {tex, SKEL}
        seen = []
        pc.plan(released, {tex}, GAME, lambda p: seen.append(p) or {"Player_Dark_Animation"})
        self.assertEqual(seen, [])


class NoteTest(unittest.TestCase):
    def test_one_line_names_the_sets(self):
        self.assertEqual(pc.note_line("Character Select", [POUCH, PACK]),
                         "Character Select: the game's own Player_Adult, PouchPlayer_Adult used: "
                         "the mod's need a file it does not have.")
        many = [f"Pack/Actor/P{i}.pack.zs" for i in range(6)]
        self.assertIn("P0, P1, P2, P3 and 2 more used", pc.note_line("M", many))

    def test_check_leaves_out_writes_the_note_and_clears_it(self):
        with tempfile.TemporaryDirectory() as d:
            mod, game = os.path.join(d, "mod"), os.path.join(d, "game")
            for root, rel, data in ((mod, PACK, b"..Player_Dark_Animation.."), (game, PACK, b"vanilla"),
                                    (game, "Model/Player_Dark_Animation.anim.bfres.zs", b"a")):
                p = os.path.join(root, "romfs" if root == mod else "", rel)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(data)
            with open(os.path.join(mod, pc.RELEASE_LIST), "w") as f:
                f.write(f"{PACK}\n{SKEL}\n")
            said = []
            self.assertEqual(pc.cmd_check(mod, game, "Character Select", said.append), 0)
            self.assertFalse(os.path.exists(os.path.join(mod, "romfs", PACK)))
            with open(os.path.join(mod, pc.NOTE), encoding="utf-8") as f:
                self.assertTrue(f.read().startswith("Character Select: the game's own Player_Adult used"))
            # (made again with the file it needs: no note)
            for rel in (PACK, SKEL):
                with open(os.path.join(mod, "romfs", rel), "wb") as f:
                    f.write(b"..Player_Dark_Animation..")
            self.assertEqual(pc.cmd_check(mod, game, "Character Select", said.append), 0)
            self.assertFalse(os.path.exists(os.path.join(mod, pc.NOTE)))

    def test_list_keeps_what_the_copy_had(self):
        with tempfile.TemporaryDirectory() as mod:
            os.makedirs(os.path.join(mod, "romfs", "Pack", "Actor"))
            open(os.path.join(mod, "romfs", PACK), "wb").close()
            with open(os.path.join(mod, pc.RELEASE_LIST), "w") as f:
                f.write(SKEL + "\n")
            pc.cmd_list(mod, None)
            with open(os.path.join(mod, pc.RELEASE_LIST)) as f:
                self.assertEqual(f.read().split(), sorted([PACK, SKEL]))


if __name__ == "__main__":
    unittest.main()

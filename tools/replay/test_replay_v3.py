"""Round-trip tests for the selection log v3 reader in replay.py (R4).

Two checks, so neither end of the format is tested only against itself:

1. The C++ encoder's golden file (tests/core/fixtures/decisions/
   synthetic_v3.jsonl, pinned byte for byte by DecisionLogTests.cpp) decodes
   to the values the C++ test wrote -- every number below is copied from
   tests/core/DecisionLogTests.cpp, not from the file.
2. A synthetic session written by an encoder in this file (separate from the
   C++ one, from the schema doc) decodes back to what was written, including
   context sharing, cap reuse, a second segment and a gzip file; malformed
   files are refused.

Run:  python -I tools/replay/test_replay_v3.py     (exit 0 = pass)
Also registered with CTest (tests/CMakeLists.txt) when Python is found.
"""

from __future__ import annotations

import gzip
import importlib.util
import json
import math
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
GOLDEN = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3.jsonl"
# The golden file as 0.23.45 wrote it: no fb/cr row columns, no covered, no
# summon. The reader must keep reading files written before 0.23.46.
GOLDEN_0_23_45 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.23.45.jsonl"
# The golden file as 0.23.52 wrote it: before the old engine's score parts
# (est..wu, fl), hk, the favorite flag, ctx.old and dec.rid (0.23.53).
GOLDEN_0_23_52 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.23.52.jsonl"
# The golden file as 0.23.55 wrote it: before the shadow model's columns
# (mu, sd, lm, um, ex) and the head's "bootstrap" (0.23.56, R8.2).
GOLDEN_0_23_55 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.23.55.jsonl"
# The golden file as 0.24.8 wrote it: 243 columns, before 0.24.9 added alcohol
# after kind_light (every later column's index moved by one).
GOLDEN_0_24_8 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.24.8.jsonl"
# The golden file as 0.24.9 wrote it: that column named alcohol, before 0.24.16
# renamed it narcotic (same index).
GOLDEN_0_24_9 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.24.9.jsonl"
# The golden file as 0.24.16 wrote it: before the D2 shadow's columns (amu, ar,
# br, ak, bk; 0.24.17).
GOLDEN_0_24_16 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.24.16.jsonl"
GOLDEN_0_24_28 = REPO / "tests" / "core" / "fixtures" / "decisions" / "synthetic_v3_0.24.28.jsonl"
OLD_KEYS = ("est", "a", "n", "ucb", "rec", "prior", "cw", "fl", "wu", "hk")
SHADOW_KEYS = ("mu", "sd", "lm", "um", "ex")
SHADOW_A_KEYS = ("amu", "ar", "br", "ak", "bk")   # 0.24.17

# python -I leaves the script's folder off sys.path: load replay.py by path
# (and write no __pycache__ into the tree).
sys.dont_write_bytecode = True
_spec = importlib.util.spec_from_file_location("replay", HERE / "replay.py")
replay = importlib.util.module_from_spec(_spec)
sys.modules["replay"] = replay   # dataclasses look their module up while it loads
_spec.loader.exec_module(replay)


def close(a, b, rel=1e-3):
    return a is not None and b is not None and math.isclose(a, b, rel_tol=rel, abs_tol=1e-6)


class GoldenFile(unittest.TestCase):
    """What the C++ encoder wrote (DecisionLogTests.cpp, struct Synthetic)."""

    @classmethod
    def setUpClass(cls):
        cls.decs = replay.load_v3([GOLDEN])

    def test_count_and_order(self):
        self.assertEqual([d["seq"] for d in self.decs], [1, 2, 3, 4, 5, 6, 7])
        self.assertEqual([d["out"] for d in self.decs], ["key", "menu", "menu", "nothing", "wheel", "nothing", "key"])

    def test_head(self):
        h = self.decs[0]["head"]
        self.assertEqual(h["v"], 3)
        self.assertEqual(len(h["cols"]), 244)   # 243 until 0.24.9 added alcohol (narcotic from 0.24.16)
        self.assertEqual(h["cols"].index("narcotic"), h["cols"].index("kind_light") + 1)
        self.assertNotIn("alcohol", h["cols"])
        self.assertEqual(len(h["needs"]), 97)  # 96 until 0.23.51 added loadout_quarterstaff; 94 until 0.23.50 added time_to_edge and edge_approach; 93 until 0.23.30 added combat_regen_ok
        self.assertEqual(h["cross"][4], "stack_count")
        # 0.23.46: covered at the end of cross, two row columns, the filter names.
        self.assertEqual(h["cross"][7], "covered")
        self.assertEqual(h["row"][10:12], ["fb", "cr"])
        # 0.23.53: the old engine's score parts and the hotkey, the favorite
        # flag, the floor names.
        self.assertEqual(h["row"][12:22], list(OLD_KEYS))
        self.assertEqual(h["row"][22:27], list(SHADOW_KEYS))   # 0.23.56 (R8.2)
        self.assertEqual(h["row"][27:], list(SHADOW_A_KEYS))   # 0.24.17
        self.assertEqual(h["flags"]["favorite"], 512)
        self.assertEqual(h["floors"], ["pass", "cold", "ctx", "util", "suppressed"])
        self.assertEqual(h["filters"][8], "summon_limit")
        # 0.23.55 (R8.1): the four hard gates, appended after truncated.
        self.assertEqual(h["filters"][14:], ["truncated", "no_follower", "no_workstation", "not_dark",
                                             "favorite_suppressed", "lock_out_of_reach",
                                             "resist_weaker",    # 0.24.29: appended
                                             "no_ally_aimed"])   # 0.24.31: appended
        self.assertEqual(len(h["filters"]), 22)
        self.assertEqual(h["kinds"][7], "Armor")
        self.assertEqual(h["episode"], {"onset": 0.5, "expiry": 0.25, "minSec": 1.0, "graceSec": 4.0,
                                        "answerSlackSec": 0.5, "deathDrops": 1,
                                        "lapSec": 60.0, "lapMaxStepSec": 1.0,
                                        "lapIdleSec": 30.0})   # 0.23.51: the lap grid and its idle hold
        self.assertEqual(replay.head_lap_sec(h), 60.0)
        # 0.23.41: the [Needs] curves in force, one per need, index-aligned.
        self.assertEqual(len(h["curves"]), len(h["needs"]))
        curves = dict(zip(h["needs"], h["curves"]))
        self.assertEqual(curves["health_deficit"], ["logistic", 0.5, 10])
        self.assertEqual(curves["enemy_count"], ["linear", 1, 5])
        self.assertEqual(curves["darkness"], ["step", 0.5, 0])
        self.assertEqual(replay.head_curves(h)["enemy_count"], ("linear", 1, 5))

    def test_key_press(self):
        d = self.decs[0]
        self.assertEqual(d["form"], "0003EADE")
        self.assertEqual(d["name"], "Potion of Healing")
        self.assertEqual((d["src"], d["via"], d["how"], d["kind"]), ("Hotkey", "key 3 (s2)", "consumed", "consume"))
        self.assertEqual(d["confirmMs"], 812)
        self.assertEqual(d["ctxAgeMs"], 12)
        self.assertEqual(d["open"], [0, 31])
        self.assertEqual(d["learned"], 1)
        ctx = d["ctx"]
        self.assertEqual(ctx["why"], "press")
        self.assertTrue(close(ctx["need"]["health_deficit"], 0.8808))
        self.assertEqual(ctx["need"]["in_combat"], 1)
        self.assertTrue(close(ctx["input"]["health_deficit"], 0.7))
        self.assertEqual(set(ctx["need"]), {"health_deficit", "in_combat"})
        self.assertEqual(ctx["pipe"], {"ok": 1, "page": 0, "slots": 8, "ageMs": 40})
        self.assertEqual(ctx["race"], "NordRace")
        # 0.23.41: where and when, for the darkness fit.
        self.assertEqual(ctx["env"], {"interior": 1, "hour": 13.5, "daylight": 0, "light": 0.3})
        self.assertEqual(len(ctx["rows"]), 6)
        c = d["chosen"]
        self.assertEqual(c["form"], "0003EADE")
        self.assertEqual(c["kind"], "Potion")
        self.assertEqual(c["src"], "Potion")
        self.assertEqual(c["slot"], 2)
        self.assertTrue(close(c["util"], 1.234))
        self.assertEqual(c["flags"], 1 | 2 | 4 | 16)
        self.assertTrue(close(c["cap"]["restore_health"], 0.62))
        self.assertTrue(close(c["cap"]["restore"], 0.62))
        self.assertTrue(close(c["x"]["overshoot_health"], -0.25))
        self.assertTrue(close(c["x"]["stack_count"], 0.4553))
        rows = ctx["rows"]
        self.assertTrue(close(rows[1]["wp"], 0.0825))
        self.assertEqual(rows[1]["flags"] & replay.V3_FLAGS["wildcard"], 32)
        self.assertEqual(rows[1]["x"], {"school_fortified": 1})
        self.assertEqual(rows[1]["cap"], {"damage": 0.4, "damage_health_fire": 0.4, "school_destruction": 1})
        self.assertEqual((rows[2]["uid"], rows[2]["util"]), (3, None))
        self.assertEqual(rows[2]["cap"], {"weapon_damage": 0.55})
        self.assertEqual((rows[3]["kind"], rows[3]["src"], rows[3]["flags"]), ("Armor", None, 4 | 8))
        self.assertEqual((rows[4]["kind"], rows[4]["cap"], rows[4]["cap_id"]), (None, None, -1))
        # 0.23.46: the summon spell the summon filter dropped at the limit.
        self.assertEqual(rows[5]["form"], "000640B6")
        self.assertEqual(rows[5]["flags"], 4)
        self.assertEqual(rows[5]["fb"], "summon_limit")
        self.assertTrue(close(rows[5]["x"]["covered"], 0.8176))
        self.assertEqual(rows[5]["cr"], 8)
        self.assertEqual(rows[5]["cap"], {"summon": 0.6, "summon_creature": 0.6, "school_conjuration": 1})
        for r in rows[:5]:
            self.assertEqual((r["fb"], r["cr"]), (None, None))
            self.assertNotIn("covered", r["x"])
        self.assertEqual(ctx["summon"], {"n": 2, "limit": 2, "act": [["000640B6", "summon", 8, 52, "FF000ABC"],
                                                                     ["0007E8DF", "reanimate", -1, 10.25, "FF000ABD"]]})

    def test_old_engine_parts(self):
        # 0.23.53: every number from DecisionLogTests.cpp.
        d = self.decs[0]
        self.assertEqual(d["rid"], 41)
        old = d["ctx"]["old"]
        self.assertEqual({k: old[k] for k in ("topN", "fav", "scored")}, {"topN": 10, "fav": "boost", "scored": 2})
        for k, v in (("minCtx", 0.05), ("minUtil", 0.1), ("coldBoost", 0.2), ("beta", 0.2), ("lambdaMin", 0.5),
                     ("lambdaMax", 3.0), ("n0", 2.0), ("favMin", 1.3), ("favMax", 2.5), ("total", 37.5)):
            self.assertTrue(close(old[k], v), k)
        self.assertEqual(len(old["phi"]), 18)
        self.assertEqual((old["phi"][0], old["phi"][17]), (1, 0.25))
        self.assertTrue(close(old["phi"][1], 0.3))
        rows = d["ctx"]["rows"]

        def parts(r):
            return tuple(r[k] for k in OLD_KEYS)

        # 0: scored by the main loop.
        want = (0.42, 0.6667, 4, 0.3105, 0.1875, 0.55, 0.8)
        for k, v in zip(OLD_KEYS, want):
            self.assertTrue(close(rows[0][k], v), k)
        self.assertEqual((rows[0]["fl"], rows[0]["wu"], rows[0]["hk"]), ("pass", None, None))
        self.assertEqual(rows[0]["flags"] & replay.V3_FLAGS["favorite"], 0)
        # 1: a favourite on hotkey index 2, rescued by the cold start.
        self.assertEqual(rows[1]["flags"] & replay.V3_FLAGS["favorite"], 512)
        self.assertEqual(rows[1]["hk"], 2)
        self.assertEqual(rows[1]["fl"], "cold")
        self.assertEqual((rows[1]["est"], rows[1]["a"], rows[1]["n"], rows[1]["ucb"], rows[1]["rec"]), (0, 0, 0, 1, 0))
        self.assertTrue(close(rows[1]["cw"], 0.02))
        # 2: under the utility floor, with its would-be utility.
        self.assertEqual(rows[2]["fl"], "util")
        self.assertTrue(close(rows[2]["wu"], 0.0812))
        self.assertTrue(close(rows[2]["est"], -0.05))
        self.assertTrue(close(rows[2]["n"], 2.5))
        self.assertIsNone(rows[2]["util"])
        # 3: held, never gathered: the learner's metrics only.
        self.assertEqual(parts(rows[3]), (0, 0, 0, 1, 0, None, None, None, None, None))
        # 4: favourited on key 8, nothing else.
        self.assertEqual(parts(rows[4]), (None,) * 9 + (7,))
        self.assertEqual(rows[4]["flags"], 4 | 512)
        # 5: a filter drop: the run's parts, no floor.
        self.assertEqual(rows[5]["fb"], "summon_limit")
        self.assertIsNone(rows[5]["fl"])
        self.assertTrue(close(rows[5]["wu"], 0.9))
        self.assertTrue(close(rows[5]["prior"], 0.6))
        # The record ids: the picks the learner took carry their v2 record's.
        self.assertEqual([x["rid"] for x in self.decs], [41, None, 42, None, 43, None, 41])
        # A context without a run (the onset) has no "old"; its row no parts.
        self.assertNotIn("old", self.decs[3]["ctx"])
        self.assertEqual(parts(self.decs[3]["ctx"]["rows"][0]), (None,) * 10)
        # The armour row added at the pick has none either.
        self.assertEqual(parts(self.decs[1]["chosen"]), (None,) * 10)

    def test_menu_pick_of_armour_with_added_row(self):
        d = self.decs[1]
        self.assertEqual(d["name"], "Épée Boots")   # cp1252 bytes, escaped
        self.assertEqual((d["learned"], d["skip"], d["case"]), (0, "armour", "A (not candidate)"))
        self.assertEqual(d["ctx"]["why"], "menu")
        self.assertEqual(d["ctx"]["menu"], "InventoryMenu")
        self.assertEqual(len(d["ctx"]["rows"]), 2)
        self.assertEqual(d["row"], 2)
        self.assertEqual(d["chosen"]["form"], "000136D5")
        self.assertEqual(d["chosen"]["flags"], 4 | 256)
        # Content-addressed: the same cap id as the worn boots of context 1.
        boots_id = self.decs[0]["ctx"]["rows"][3]["cap_id"]
        self.assertEqual(d["chosen"]["cap_id"], boots_id)
        self.assertEqual(d["ctxAgeMs"], 4200)

    def test_co_pick_shares_the_menu_context(self):
        self.assertIs(self.decs[2]["ctx"], self.decs[1]["ctx"])
        self.assertEqual(self.decs[2]["repeat"], 1)
        self.assertEqual(self.decs[2]["chosen"]["slot"], -1)   # no longer shown in the menu's context
        self.assertIsNone(self.decs[2]["chosen"]["wp"])

    def test_nothing(self):
        d = self.decs[3]
        self.assertIsNone(d["form"])
        self.assertIsNone(d["chosen"])
        self.assertEqual(d["ep"], {"need": "hunger", "i": 26, "durSec": 12.5, "peak": 0.71,
                                   "onset": "2026-10-09 12:01:00.000", "why": "end", "lap": 0, "epSec": 12.5})
        self.assertEqual(replay.nothing_stretch(d), ("end", 0, 12.5))
        self.assertIsNone(replay.nothing_stretch(self.decs[0]))   # a pick
        self.assertEqual(d["ctx"]["why"], "onset")
        self.assertIsNone(d["ctx"]["race"])
        self.assertNotIn("summon", d["ctx"])   # 0.23.46: written only when valid
        self.assertEqual(d["ctx"]["rows"][0]["cap"], {"survival": 0.5, "survival_hunger": 0.5})

    def test_lap(self):
        # 0.23.51: the second stretch of a diseased episode, lapped unanswered.
        d = self.decs[5]
        self.assertEqual(d["out"], "nothing")
        self.assertEqual(d["ep"], {"need": "diseased", "i": 18, "durSec": 60.0, "peak": 1,
                                   "onset": "2026-10-09 12:02:00.000", "why": "lap", "lap": 1, "epSec": 150.0})
        self.assertEqual(replay.nothing_stretch(d), ("lap", 1, 150.0))
        self.assertEqual(d["ctx"]["why"], "lap")
        self.assertEqual(d["ctx"]["utc"], "2026-10-09 12:03:30.000")   # the lap that began the stretch
        self.assertEqual(d["ctx"]["need"], {"diseased": 1})
        self.assertEqual(d["ctxAgeMs"], 64000)

    def test_held_detail_and_pre_press_equipped(self):
        # The onset context was taken inside the post-load window (heldFull 0).
        self.assertEqual([d["ctx"]["heldFull"] for d in self.decs[:5]], [1, 1, 1, 0, 1])
        self.assertEqual([d["preEquipped"] for d in self.decs], [0, 0, 0, 0, 1, 0, 0])

    def test_wheel_reuses_the_first_context(self):
        self.assertIs(self.decs[4]["ctx"], self.decs[0]["ctx"])
        self.assertEqual(self.decs[4]["chosen"]["form"], "00012FCD")

    def test_second_segment_redefines(self):
        d = self.decs[6]
        self.assertIsNot(d["ctx"], self.decs[0]["ctx"])   # a fresh table after the second head
        self.assertEqual(d["ctx"]["rows"][0]["cap"], self.decs[0]["ctx"]["rows"][0]["cap"])

    def test_shadow_columns(self):
        # 0.23.56 (R8.2): every number from DecisionLogTests.cpp.
        d = self.decs[0]
        self.assertEqual(d["head"]["row"][22:27], list(SHADOW_KEYS))
        self.assertEqual(d["head"]["bootstrap"]["created_utc"], "2026-10-29 12:29:59")
        self.assertTrue(d["head"]["bootstrap"]["sha256"].startswith("0011"))
        rows = d["ctx"]["rows"]
        # 0: scored, explained by health_deficit x restore_health
        self.assertTrue(close(rows[0]["mu"], 1.25))
        self.assertTrue(close(rows[0]["sd"], 0.4))
        self.assertTrue(close(rows[0]["lm"], 0.3125))
        self.assertEqual(rows[0]["um"], 0)
        self.assertEqual(rows[0]["ex"], ("health_deficit", "restore_health"))
        # 3: a held row the shadow scored at the context, no positive need term
        self.assertTrue(close(rows[3]["mu"], -0.5))
        self.assertEqual((rows[3]["lm"], rows[3]["um"], rows[3]["ex"]), (0, 1, "learned"))
        # the others carry none
        for i in (1, 2, 4, 5):
            self.assertEqual(tuple(rows[i][k] for k in SHADOW_KEYS), (None,) * 5, i)

    def test_d2_shadow_columns(self):
        # 0.24.17: every number from DecisionLogTests.cpp.
        rows = self.decs[0]["ctx"]["rows"]
        # 0: A ranks it first (key 0 of A's stateless page), the live ranking second (key 2)
        self.assertTrue(close(rows[0]["amu"], 1.75))
        self.assertEqual((rows[0]["ar"], rows[0]["br"], rows[0]["ak"], rows[0]["bk"]), (1, 2, 0, 2))
        # 1: off A's page (rank 7), on the live ranking's stateless page (rank 4, key 5)
        self.assertTrue(close(rows[1]["amu"], 0.2))
        self.assertEqual((rows[1]["ar"], rows[1]["br"], rows[1]["ak"], rows[1]["bk"]), (7, 4, -1, 5))
        self.assertEqual(tuple(rows[1][k] for k in SHADOW_KEYS), (None,) * 5)   # the nulls between
        for i in (2, 3, 4, 5):
            self.assertEqual(tuple(rows[i][k] for k in SHADOW_A_KEYS), (None,) * 5, i)


class GoldenFile0_24_16(unittest.TestCase):
    """The same session as 0.24.16 wrote it (before 0.24.17): every row reads
    the D2 shadow's columns as None; the rest as before."""

    @classmethod
    def setUpClass(cls):
        cls.decs = replay.load_v3([GOLDEN_0_24_16])

    def test_defaults(self):
        h = self.decs[0]["head"]
        self.assertEqual(h["row"][-1], "ex")
        for d in self.decs:
            for r in d["rows"]:
                self.assertEqual(tuple(r[k] for k in SHADOW_A_KEYS), (None,) * 5)
        self.assertTrue(close(self.decs[0]["ctx"]["rows"][0]["mu"], 1.25))   # everything else as before
        self.assertTrue(close(self.decs[0]["chosen"]["util"], 1.234))


class GoldenFile0_24_28(unittest.TestCase):
    """The same session as 0.24.28 wrote it (19 filters, before 0.24.29
    appended lock_out_of_reach): a row's fb indexes its own head's list, so
    the decoded session is the same."""

    def test_same_session(self):
        old = replay.load_v3([GOLDEN_0_24_28])
        new = replay.load_v3([GOLDEN])
        self.assertEqual(len(old[0]["head"]["filters"]), 19)
        self.assertEqual(new[0]["head"]["filters"][:19], old[0]["head"]["filters"])
        rows = lambda decs: [(r["form"], r["fb"], r["flags"]) for d in decs for r in d["rows"]]
        self.assertEqual(rows(old), rows(new))
        self.assertTrue(any(fb for _, fb, _ in rows(new)))   # the session has filtered rows to compare
        self.assertEqual([d["out"] for d in old], [d["out"] for d in new])


class GoldenFile0_24_8(unittest.TestCase):
    """The same session as 0.24.8 wrote it (243 columns, no narcotic): its caps
    use the old indices of every column after kind_light, and decode by the
    head's own names to the same caps as this build's file."""

    def test_same_caps_by_name(self):
        old = replay.load_v3([GOLDEN_0_24_8])
        new = replay.load_v3([GOLDEN])
        self.assertEqual(len(old[0]["head"]["cols"]), 243)
        self.assertNotIn("narcotic", old[0]["head"]["cols"])
        caps = lambda decs: [r["cap"] for d in decs for r in d["rows"] + [d["chosen"] or {}] if "cap" in r]
        self.assertEqual(caps(old), caps(new))
        moved = {k for c in caps(new) if c for k in c} & set(new[0]["head"]["cols"][new[0]["head"]["cols"].index("narcotic") + 1:])
        self.assertTrue(moved)   # the session's caps reach past the inserted column
        self.assertEqual([d["out"] for d in old], [d["out"] for d in new])


class GoldenFile0_24_9(unittest.TestCase):
    """The same session as 0.24.9 wrote it: the column after kind_light named
    alcohol. The reader keys by name with no alias, so the head differs in that
    one name and the decoded session is the same."""

    def test_rename(self):
        old = replay.load_v3([GOLDEN_0_24_9])
        new = replay.load_v3([GOLDEN])
        oc, nc = old[0]["head"]["cols"], new[0]["head"]["cols"]
        at = oc.index("alcohol")
        self.assertEqual(nc.index("narcotic"), at)
        self.assertEqual(oc[:at] + oc[at + 1:], nc[:at] + nc[at + 1:])
        caps = lambda decs: [r["cap"] for d in decs for r in d["rows"] + [d["chosen"] or {}] if "cap" in r]
        self.assertEqual(caps(old), caps(new))
        self.assertEqual([d["out"] for d in old], [d["out"] for d in new])


class GoldenFile0_23_55(unittest.TestCase):
    """The same session as 0.23.55 wrote it (before 0.23.56): every row reads
    the shadow columns as None and the head has no "bootstrap"."""

    @classmethod
    def setUpClass(cls):
        cls.decs = replay.load_v3([GOLDEN_0_23_55])

    def test_defaults(self):
        h = self.decs[0]["head"]
        self.assertEqual(h["row"][-1], "hk")
        self.assertNotIn("bootstrap", h)
        for d in self.decs:
            for r in d["rows"]:
                self.assertEqual(tuple(r[k] for k in SHADOW_KEYS), (None,) * 5)
        self.assertEqual(self.decs[0]["ctx"]["rows"][0]["fl"], "pass")   # everything else as before
        self.assertTrue(close(self.decs[0]["chosen"]["util"], 1.234))


class GoldenFile0_23_52(unittest.TestCase):
    """The same session as 0.23.52 wrote it (before 0.23.53): every row reads
    the old engine's parts and hk as None, no favorite bit, no ctx.old, no rid."""

    @classmethod
    def setUpClass(cls):
        cls.decs = replay.load_v3([GOLDEN_0_23_52])

    def test_defaults(self):
        self.assertEqual([d["out"] for d in self.decs], ["key", "menu", "menu", "nothing", "wheel", "nothing", "key"])
        h = self.decs[0]["head"]
        self.assertEqual(h["row"][-2:], ["fb", "cr"])
        self.assertNotIn("floors", h)
        self.assertNotIn("favorite", h["flags"])
        for d in self.decs:
            self.assertNotIn("rid", d)
            self.assertNotIn("old", d["ctx"])
            for r in d["rows"]:
                self.assertEqual(tuple(r[k] for k in OLD_KEYS), (None,) * 10)
                self.assertEqual(r["flags"] & 512, 0)
        # Everything else decodes as before.
        self.assertEqual(self.decs[0]["ctx"]["rows"][5]["fb"], "summon_limit")
        self.assertTrue(close(self.decs[0]["chosen"]["util"], 1.234))


class GoldenFile0_23_45(unittest.TestCase):
    """The same session as 0.23.45 wrote it, before fb/cr, covered and summon:
    every row reads fb = cr = None, no row has covered, no context a summon."""

    @classmethod
    def setUpClass(cls):
        cls.decs = replay.load_v3([GOLDEN_0_23_45])

    def test_defaults(self):
        self.assertEqual([d["out"] for d in self.decs], ["key", "menu", "menu", "nothing", "wheel", "key"])
        h = self.decs[0]["head"]
        self.assertIsNone(replay.head_lap_sec(h))                          # 0.23.51: no lap grid
        self.assertEqual(replay.nothing_stretch(self.decs[3]), ("end", 0, 12.5))   # a whole episode
        self.assertEqual(len(h["row"]), 10)
        self.assertNotIn("filters", h)
        self.assertNotIn("covered", h["cross"])
        self.assertEqual(len(self.decs[0]["ctx"]["rows"]), 5)
        for d in self.decs:
            self.assertNotIn("summon", d["ctx"])
            for r in d["rows"]:
                self.assertEqual((r["fb"], r["cr"]), (None, None))
                self.assertNotIn("covered", r["x"])
        # Everything else decodes as before.
        self.assertTrue(close(self.decs[0]["chosen"]["x"]["stack_count"], 0.4553))
        self.assertEqual(self.decs[1]["case"], "A (not candidate)")


# --- An encoder written from the schema doc, independent of the C++ one ------

def encode(segments):
    """segments: list of (head, [dec]) where every dec carries its own 'ctx' dict
    (shared by identity) with rows whose 'cap' is a {col: value} dict."""
    lines = []
    for head, decs in segments:
        lines.append(json.dumps(head))
        cap_ids, done = {}, set()

        def cap_id(cap):
            if cap is None:
                return -1
            key = tuple(sorted((head["cols"].index(k), v) for k, v in cap.items()))
            if key not in cap_ids:
                cap_ids[key] = len(cap_ids)
                lines.append(json.dumps({"t": "cap", "id": cap_ids[key], "c": [list(p) for p in key]}))
            return cap_ids[key]

        def row(r):
            x = [[head["cross"].index(k), v] for k, v in r["x"].items()]
            out = [r["form"], r["uid"], None if r["kind"] is None else head["kinds"].index(r["kind"]),
                   None if r["src"] is None else head["src"].index(r["src"]), cap_id(r["cap"]),
                   r["flags"], r["slot"], r["util"], x, r["wp"]]
            if "fb" in head.get("row", []):   # 0.23.46 heads: the two appended columns
                out += [None if r.get("fb") is None else head["filters"].index(r["fb"]), r.get("cr")]
            if "est" in head.get("row", []):   # 0.23.53 heads: up to the last one set
                keys = OLD_KEYS + (SHADOW_KEYS if "mu" in head.get("row", []) else ()) \
                    + (SHADOW_A_KEYS if "amu" in head.get("row", []) else ())   # 0.23.56, 0.24.17
                tail = []
                for k in keys:
                    v = r.get(k)
                    if k == "fl" and v is not None:
                        v = head["floors"].index(v)
                    elif k == "ex" and isinstance(v, tuple):
                        v = [head["needs"].index(v[0]), head["cols"].index(v[1])]
                    tail.append(v)
                while tail and tail[-1] is None:
                    tail.pop()
                out += tail
            return out

        for d in decs:
            ctx = d["ctx"]
            if id(ctx) not in done:
                done.add(id(ctx))
                rows = [row(r) for r in ctx["rows"]]
                lines.append(json.dumps({
                    "t": "ctx", "id": ctx["id"], "utc": ctx["utc"], "why": ctx["why"],
                    "need": [[head["needs"].index(k), v] for k, v in ctx["need"].items()],
                    "in": [[head["needs"].index(k), v] for k, v in ctx["input"].items()],
                    "pipe": ctx["pipe"], "race": ctx["race"], "wc": ctx["wc"],
                    **({"env": ctx["env"]} if "env" in ctx else {}), "rows": rows}))
            rec = {k: v for k, v in d.items() if k not in ("ctx", "add")}
            rec.update({"t": "dec", "v": 3, "ctx": ctx["id"], "add": [row(r) for r in d["add"]]})
            lines.append(json.dumps(rec))
    return "\n".join(lines) + "\n"


def make_head():
    golden_head = json.loads(GOLDEN.read_text(encoding="utf-8").splitlines()[0])
    return golden_head


def make_session():
    head = make_head()
    sword = {"weapon_damage": 0.75, "weapon_type_sword": 1}
    potion = {"restore": 0.33, "restore_magicka": 0.33}

    def r(form, kind, src, cap, flags, slot=-1, util=None, x=None, wp=None, uid=0, fb=None, cr=None, **old):
        row = {"form": form, "uid": uid, "kind": kind, "src": src, "cap": cap, "flags": flags,
               "slot": slot, "util": util, "x": x or {}, "wp": wp, "fb": fb, "cr": cr}
        row.update({k: old.get(k) for k in OLD_KEYS + SHADOW_KEYS + SHADOW_A_KEYS})   # 0.23.53, 0.23.56, 0.24.17
        return row

    ctx_a = {"id": 10, "utc": "2026-10-09 15:00:00.000", "why": "press",
             "need": {"magicka_deficit": 0.9, "enemy_close": 1}, "input": {"magicka_deficit": 0.8, "enemy_close": 300.0},
             "pipe": {"ok": 1, "page": 2, "slots": 8, "ageMs": 120}, "race": "DraugrRace",
             "wc": {"base": 0.165, "max": 0.5},
             "env": {"interior": 0, "hour": 21.25, "daylight": 0, "light": 0.1},
             "rows": [r("0003EAE3", "Potion", "Potion", potion, 1 | 2 | 4 | 16, 0, 2.5,
                        {"overshoot_magicka": 0.1, "stack_count": 0.6},
                        est=0.31, a=0.75, n=6, ucb=0.25, rec=0, prior=0.5, cw=0.9, fl="pass",
                        mu=0.875, sd=0.25, lm=0.125, um=0, ex=("magicka_deficit", "restore_magicka"),
                        amu=1.125, ar=1, br=3, ak=0, bk=-1),
                      # 0.23.53: a favourite on key 1 under the context floor.
                      r("00012EB7", "Weapon", "Weapon", sword, 1 | 4 | 512, uid=12, x={"weapon_charge": 0.25},
                        est=0, a=0, n=0, ucb=1, rec=0, prior=0.4, cw=0.01, fl="ctx", wu=0.004, hk=0),
                      r("0001397D", "Ammo", "Ammo", None, 1 | 4, x={"ammo_matches_launcher": 1},
                        mu=-0.25, sd=0.5, lm=0, um=1, ex="learned"),
                      # 0.23.46: a potion on cooldown, its effect up with 12.5 s left.
                      r("0003EAE5", "Potion", None, potion, 4, x={"covered": 0.75}, fb="cooldown", cr=12.5)]}
    ctx_b = {"id": 11, "utc": "2026-10-09 15:00:30.000", "why": "onset",
             "need": {"darkness": 0.8}, "input": {"darkness": 0.9},
             "pipe": {"ok": 0, "page": -1, "slots": 0, "ageMs": 0}, "race": None,
             "wc": {"base": 0, "max": 0}, "rows": []}

    def dec(seq, out, ctx, chosen_row=-1, form=None, add=(), **kw):
        base = {"seq": seq, "utc": f"2026-10-09 15:01:{seq:02d}.000", "launch": "20261009-120000",
                "list": "py", "char": "0000000000000001", "gen": 2, "out": out, "form": form,
                "name": None, "row": chosen_row, "src": "", "via": "", "case": "", "how": "",
                "kind": "", "confirmMs": 0, "repeat": 0, "learned": 1, "skip": "", "ctxAgeMs": 0,
                "open": [], "ep": None}
        base.update(kw)
        base["ctx"] = ctx
        base["add"] = list(add)
        return base

    seg1 = [dec(1, "key", ctx_a, 0, "0003EAE3", src="Hotkey"),
            dec(2, "nothing", ctx_b, ep={"need": "darkness", "i": 63, "durSec": 4.0, "peak": 0.8,
                                         "onset": "2026-10-09 15:00:30.000"}),
            dec(3, "menu", ctx_a, 4, "00099999", add=[r("00099999", "Armor", None, sword, 4 | 256)],
                learned=0, skip="stale", ctxAgeMs=900)]
    seg2 = [dec(4, "wheel", ctx_a, 1, "00012EB7", src="Wheeler")]
    return head, seg1, seg2


class PythonRoundTrip(unittest.TestCase):
    def roundtrip(self, text, suffix=".jsonl"):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / ("s" + suffix)
            if suffix.endswith(".gz"):
                with gzip.open(p, "wt", encoding="utf-8") as f:
                    f.write(text)
            else:
                p.write_text(text, encoding="utf-8")
            return replay.load_v3([p])

    def check(self, decs, seg1, seg2):
        self.assertEqual(len(decs), 4)
        src = seg1 + seg2
        for got, want in zip(decs, src):
            for k in ("seq", "out", "form", "row", "learned", "skip", "ctxAgeMs", "ep"):
                self.assertEqual(got[k], want[k], k)
            want_rows = want["ctx"]["rows"] + want["add"]
            self.assertEqual(len(got["rows"]), len(want_rows))
            for g, w in zip(got["rows"], want_rows):
                for k in ("form", "uid", "kind", "src", "cap", "flags", "slot", "util", "x", "wp", "fb", "cr") + OLD_KEYS \
                        + SHADOW_KEYS + SHADOW_A_KEYS:
                    self.assertEqual(g[k], w[k], k)
            self.assertEqual(got["ctx"]["need"], want["ctx"]["need"])
            self.assertEqual(got["ctx"]["input"], want["ctx"]["input"])
            self.assertEqual(got["ctx"].get("env"), want["ctx"].get("env"))   # absent stays absent
        self.assertIs(decs[0]["ctx"], decs[2]["ctx"])           # shared within a segment
        self.assertIsNot(decs[0]["ctx"], decs[3]["ctx"])        # redefined in the next one
        self.assertEqual(decs[2]["chosen"]["form"], "00099999")
        self.assertEqual(decs[2]["rows"][1]["cap_id"], decs[2]["chosen"]["cap_id"])   # one cap, one id

    def test_plain_and_gzip(self):
        head, seg1, seg2 = make_session()
        text = encode([(head, seg1), (head, seg2)])
        self.check(self.roundtrip(text), seg1, seg2)
        self.check(self.roundtrip(text, ".jsonl.gz"), seg1, seg2)

    def test_old_head_rows_default_the_new_columns(self):
        # A pre-0.23.46 head: ten row columns, no filters, no covered. The rows
        # written under it decode with fb/cr None -- also when a later segment
        # of the same file has the new head.
        head, seg1, seg2 = make_session()
        old = dict(head)
        old["row"] = head["row"][:10]
        old.pop("filters")
        old["cross"] = [c for c in head["cross"] if c != "covered"]
        for d in seg1 + seg2:
            for r in d["ctx"]["rows"] + d["add"]:
                r["x"].pop("covered", None)
        text = encode([(old, seg1), (head, seg2)])
        for d in seg1:   # what the old segment reads back as
            for r in d["ctx"]["rows"] + d["add"]:
                r["fb"], r["cr"] = None, None
                r.update(dict.fromkeys(OLD_KEYS + SHADOW_KEYS + SHADOW_A_KEYS))
        decs = self.roundtrip(text)
        self.assertEqual(len(decs[0]["ctx"]["rows"]), 4)
        for d in decs[:3]:
            for r in d["rows"]:
                self.assertEqual((r["fb"], r["cr"]), (None, None))
                self.assertNotIn("covered", r["x"])
        self.assertNotIn("filters", decs[0]["head"])
        self.assertIn("filters", decs[3]["head"])
        for d in decs[:3]:
            for r in d["rows"]:
                self.assertEqual(tuple(r[k] for k in OLD_KEYS), (None,) * 10)
        self.assertEqual(decs[3]["rows"][1]["hk"], 0)   # the new segment reads them
        self.assertEqual(decs[3]["rows"][1]["fl"], "ctx")

    def test_refuses_malformed(self):
        head, seg1, seg2 = make_session()
        text = encode([(head, seg1)])
        lines = text.splitlines()
        no_ctx = "\n".join(l for l in lines if '"t": "ctx"' not in l) + "\n"
        with self.assertRaises(replay.V3FormatError):
            self.roundtrip(no_ctx)
        no_head = "\n".join(lines[1:]) + "\n"
        with self.assertRaises(replay.V3FormatError):
            self.roundtrip(no_head)
        bad_v = text.replace('"v": 3', '"v": 4', 1)
        with self.assertRaises(replay.V3FormatError):
            self.roundtrip(bad_v)

    def test_torn_line_is_skipped_and_the_next_head_resyncs(self):
        head, seg1, seg2 = make_session()
        lines = encode([(head, seg1), (head, seg2)]).splitlines()
        # Tear segment 1's first context (ctx 10) in half: a crash mid-write,
        # after which the writer started on a new line.
        i = next(k for k, l in enumerate(lines) if '"t": "ctx", "id": 10' in l)
        lines[i] = lines[i][: len(lines[i]) // 2]
        # And a torn last line with no newline at all.
        text = "\n".join(lines) + "\n" + '{"t": "dec", "v": 3, "se'
        stats = {}
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "torn.jsonl"
            p.write_text(text, encoding="utf-8")
            decs = list(replay.iter_v3([p], stats))
        # seq 1 and 3 needed ctx 10 (skipped); seq 2 (its own context) and
        # seq 4 (after the next head, which re-defines ctx 10) survive.
        self.assertEqual([d["seq"] for d in decs], [2, 4])
        self.assertEqual(stats, {"bad_lines": 2, "skipped": 2, "lost_heads": 0, "truncated": 0})
        self.assertEqual(decs[1]["chosen"]["form"], "00012EB7")

    # --- damage the second verifier probed (scratchpad r4b/torn_probe.py) -----

    def _read_bytes(self, data, suffix=".jsonl"):
        """Decode raw file bytes (already gzipped for a .gz suffix)."""
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / ("d" + suffix)
            p.write_bytes(data)
            stats = {}
            decs = list(replay.iter_v3([p], stats))
            # The summary and load_v3 must not raise on the same file either.
            import contextlib
            import io
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                replay.summarize_v3([p])
                replay.load_v3([p])
            return decs, stats

    def golden_lines(self):
        data = GOLDEN.read_bytes().replace(b"\r\n", b"\n")
        lines = data.split(b"\n")[:-1]
        heads = [i for i, l in enumerate(lines) if l.startswith(b'{"t":"head"')]
        return lines, heads

    def test_line_torn_inside_a_utf8_character(self):
        lines, heads = self.golden_lines()
        dec0 = next(i for i, l in enumerate(lines) if l.startswith(b'{"t":"dec"'))
        d = lines[dec0].replace(b'"name":"Potion of Healing"', '"name":"Épée of Healing"'.encode("utf-8"))
        cut = d.index("É".encode("utf-8")) + 1   # between the two bytes of É
        data = b"\n".join(lines[:dec0] + [d[:cut]] + lines[heads[1]:]) + b"\n"
        decs, stats = self._read_bytes(data)
        self.assertEqual(stats["bad_lines"], 1)
        self.assertEqual([x["seq"] for x in decs], [7])   # the next segment resyncs

    def test_truncated_gzip_ends_the_file_cleanly(self):
        data = gzip.compress(GOLDEN.read_bytes())[:-40]
        decs, stats = self._read_bytes(data, ".jsonl.gz")
        self.assertEqual(stats["truncated"], 1)
        self.assertGreaterEqual(len(decs), 1)

    def test_intact_gzip_stays_strict(self):
        # Only a .gz that fails to decompress whole is read leniently: an
        # intact one holding a malformed record still raises, like a .jsonl.
        lines, heads = self.golden_lines()
        bad = b"\n".join(lines[:heads[1]] + [b'{"t":"ced","id":0}'] + lines[heads[1]:]) + b"\n"
        with self.assertRaises(replay.V3FormatError):
            self._read_bytes(gzip.compress(bad), ".jsonl.gz")
        with self.assertRaises(replay.V3FormatError):
            self._read_bytes(bad)

    def test_corrupt_gzip_never_raises(self):
        # A flipped byte inside the deflate stream raises zlib.error, not
        # EOFError: every position must end the file cleanly or decode.
        packed = gzip.compress(GOLDEN.read_bytes())
        for pos in range(10, len(packed) - 8):
            data = bytearray(packed)
            data[pos] ^= 0xFF
            self._read_bytes(bytes(data), ".jsonl.gz")

    def test_torn_head_between_two_launches_skips_the_other_launch(self):
        lines, heads = self.golden_lines()
        seg2 = [l.replace(b'"launch":"20261009-120000"', b'"launch":"20261009-130000"') for l in lines[heads[1]:]]
        torn = seg2[0][:50]
        data = b"\n".join(lines[:heads[1]] + [torn] + seg2[1:]) + b"\n"
        decs, stats = self._read_bytes(data)
        # Segment 2's decision is of another launch than the head in force: not
        # joined to it.
        self.assertEqual([x["seq"] for x in decs], [1, 2, 3, 4, 5, 6])
        self.assertEqual(stats["bad_lines"], 1)
        self.assertEqual(stats["lost_heads"], 1)
        self.assertEqual(stats["skipped"], 1)

    def test_torn_head_within_a_launch_is_reported(self):
        lines, heads = self.golden_lines()
        data = b"\n".join(lines[:heads[1]] + [lines[heads[1]][:50]] + lines[heads[1] + 1:]) + b"\n"
        decs, stats = self._read_bytes(data)
        # Same launch, same head: decoded, and the lost head is counted.
        self.assertEqual(len(decs), 7)
        self.assertEqual((stats["bad_lines"], stats["lost_heads"]), (1, 1))

    def test_record_that_parses_but_is_incomplete(self):
        lines, heads = self.golden_lines()
        dec0 = next(i for i, l in enumerate(lines) if l.startswith(b'{"t":"dec"'))
        data = b"\n".join(lines[:dec0] + [b'{"t":"dec","v":3}'] + lines[dec0 + 1:]) + b"\n"
        decs, stats = self._read_bytes(data)
        self.assertEqual(stats["bad_lines"], 1)
        self.assertEqual(len(decs), 6)

    def test_nul_padding_and_crlf(self):
        lines, heads = self.golden_lines()
        whole = b"\n".join(lines) + b"\n"
        decs, stats = self._read_bytes(whole + b"\x00" * 64 + b"\n" + b"\n".join(lines[heads[1]:]) + b"\n")
        self.assertEqual(stats["bad_lines"], 1)
        self.assertEqual(len(decs), 8)
        decs, stats = self._read_bytes(whole.replace(b"\n", b"\r\n"))
        self.assertEqual((len(decs), stats["bad_lines"]), (7, 0))

    def test_summary_runs(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            replay.summarize_v3([GOLDEN])
        out = buf.getvalue()
        self.assertIn("7 decisions", out)
        self.assertIn("key=2", out)
        self.assertIn("nothing=2", out)
        self.assertIn("nothing: 1 ended episode(s), 1 lapped stretch(es)", out)


class V2Load(unittest.TestCase):
    """The v2 replay's reader (replay.load). 0.24.0..0.24.18 wrote every ranked candidate into
    the v2 record, the ones the old floors dropped with ScoreBreakdown's defaults (util, ctx,
    corr 0); load() skips them, so the policies see only the rows the old engine scored."""
    COLS = ["form", "type", "need", "rank", "util", "ctx", "est", "prior", "ucb", "alpha",
            "learn", "lambda", "rec", "corr", "potion", "fav", "wildcard", "coldStart", "pred"]

    def test_dropped_rows_are_skipped(self):
        scored = ["0003EAE3", "Potion", "HealthAny", 1, 0.6, 0.5, 0, 0, 0, 0, 0.2, 1, 0, 1.2, 1, 1, 0, 0, 0]
        dropped = ["00012EB7", "Weapon", "Melee", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0]
        # util and ctx 0 but corr set: the old scorer wrote it (corr starts at 1), so it stays
        zero_ctx = ["0001397D", "Ammo", "Ammo", 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0]
        rec = {"v": 2, "utc": "2026-10-30 12:00:00.000", "launch": "20261030-110000", "char": "C",
               "form": "0003EAE3", "src": "Huginn", "phi": [0.0], "shown": [],
               "cols": self.COLS, "cands": [dropped, scored, zero_ctx]}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "Huginn_Selections.jsonl"
            path.write_text(json.dumps(rec) + "\n", encoding="utf-8")
            recs = replay.load(path, "C", None)
        self.assertEqual([c["form"] for c in recs[0]["_cands"]], ["0003EAE3", "0001397D"])
        self.assertTrue(replay.v2_unscored(dict(zip(self.COLS, dropped))))


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)

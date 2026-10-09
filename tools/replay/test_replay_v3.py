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
        self.assertEqual([d["seq"] for d in self.decs], [1, 2, 3, 4, 5, 6])
        self.assertEqual([d["out"] for d in self.decs], ["key", "menu", "menu", "nothing", "wheel", "key"])

    def test_head(self):
        h = self.decs[0]["head"]
        self.assertEqual(h["v"], 3)
        self.assertEqual(len(h["cols"]), 239)
        self.assertEqual(len(h["needs"]), 92)
        self.assertEqual(h["cross"][4], "stack_count")
        self.assertEqual(h["kinds"][7], "Armor")
        self.assertEqual(h["episode"], {"onset": 0.5, "expiry": 0.25, "minSec": 1.0, "graceSec": 4.0,
                                        "answerSlackSec": 0.5})

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
        self.assertEqual(len(ctx["rows"]), 5)
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
                                   "onset": "2026-10-09 12:01:00.000"})
        self.assertEqual(d["ctx"]["why"], "onset")
        self.assertIsNone(d["ctx"]["race"])
        self.assertEqual(d["ctx"]["rows"][0]["cap"], {"survival": 0.5, "survival_hunger": 0.5})

    def test_wheel_reuses_the_first_context(self):
        self.assertIs(self.decs[4]["ctx"], self.decs[0]["ctx"])
        self.assertEqual(self.decs[4]["chosen"]["form"], "00012FCD")

    def test_second_segment_redefines(self):
        d = self.decs[5]
        self.assertIsNot(d["ctx"], self.decs[0]["ctx"])   # a fresh table after the second head
        self.assertEqual(d["ctx"]["rows"][0]["cap"], self.decs[0]["ctx"]["rows"][0]["cap"])


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
            return [r["form"], r["uid"], None if r["kind"] is None else head["kinds"].index(r["kind"]),
                    None if r["src"] is None else head["src"].index(r["src"]), cap_id(r["cap"]),
                    r["flags"], r["slot"], r["util"], x, r["wp"]]

        for d in decs:
            ctx = d["ctx"]
            if id(ctx) not in done:
                done.add(id(ctx))
                rows = [row(r) for r in ctx["rows"]]
                lines.append(json.dumps({
                    "t": "ctx", "id": ctx["id"], "utc": ctx["utc"], "why": ctx["why"],
                    "need": [[head["needs"].index(k), v] for k, v in ctx["need"].items()],
                    "in": [[head["needs"].index(k), v] for k, v in ctx["input"].items()],
                    "pipe": ctx["pipe"], "race": ctx["race"], "wc": ctx["wc"], "rows": rows}))
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

    def r(form, kind, src, cap, flags, slot=-1, util=None, x=None, wp=None, uid=0):
        return {"form": form, "uid": uid, "kind": kind, "src": src, "cap": cap, "flags": flags,
                "slot": slot, "util": util, "x": x or {}, "wp": wp}

    ctx_a = {"id": 10, "utc": "2026-10-09 15:00:00.000", "why": "press",
             "need": {"magicka_deficit": 0.9, "enemy_close": 1}, "input": {"magicka_deficit": 0.8, "enemy_close": 300.0},
             "pipe": {"ok": 1, "page": 2, "slots": 8, "ageMs": 120}, "race": "DraugrRace",
             "wc": {"base": 0.165, "max": 0.5},
             "rows": [r("0003EAE3", "Potion", "Potion", potion, 1 | 2 | 4 | 16, 0, 2.5,
                        {"overshoot_magicka": 0.1, "stack_count": 0.6}),
                      r("00012EB7", "Weapon", "Weapon", sword, 1 | 4, uid=12, x={"weapon_charge": 0.25}),
                      r("0001397D", "Ammo", "Ammo", None, 1 | 4, x={"ammo_matches_launcher": 1})]}
    ctx_b = {"id": 11, "utc": "2026-10-09 15:00:30.000", "why": "onset",
             "need": {"darkness": 0.8}, "input": {"darkness": 0.9},
             "pipe": {"ok": 0, "page": -1, "slots": 0, "ageMs": 0}, "race": None,
             "wc": {"base": 0, "max": 0}, "rows": []}

    def dec(seq, out, ctx, chosen_row=-1, form=None, add=(), **kw):
        base = {"seq": seq, "utc": f"2026-10-09 15:01:{seq:02d}.000", "launch": "20261009-150000",
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
            dec(3, "menu", ctx_a, 3, "00099999", add=[r("00099999", "Armor", None, sword, 4 | 256)],
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
                for k in ("form", "uid", "kind", "src", "cap", "flags", "slot", "util", "x", "wp"):
                    self.assertEqual(g[k], w[k], k)
            self.assertEqual(got["ctx"]["need"], want["ctx"]["need"])
            self.assertEqual(got["ctx"]["input"], want["ctx"]["input"])
        self.assertIs(decs[0]["ctx"], decs[2]["ctx"])           # shared within a segment
        self.assertIsNot(decs[0]["ctx"], decs[3]["ctx"])        # redefined in the next one
        self.assertEqual(decs[2]["chosen"]["form"], "00099999")
        self.assertEqual(decs[2]["rows"][1]["cap_id"], decs[2]["chosen"]["cap_id"])   # one cap, one id

    def test_plain_and_gzip(self):
        head, seg1, seg2 = make_session()
        text = encode([(head, seg1), (head, seg2)])
        self.check(self.roundtrip(text), seg1, seg2)
        self.check(self.roundtrip(text, ".jsonl.gz"), seg1, seg2)

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

    def test_summary_runs(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            replay.summarize_v3([GOLDEN])
        out = buf.getvalue()
        self.assertIn("6 decisions", out)
        self.assertIn("key=2", out)
        self.assertIn("nothing=1", out)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=2).result
    sys.exit(0 if result.wasSuccessful() else 1)

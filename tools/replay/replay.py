"""Offline replay of Huginn's selection log -- the Phase 3 harness.

Re-ranks every logged selection under a scoring policy and reports how often
the item the player actually chose would have been near the top. It was the
Phase 3 harness for learning changes (roadmap, "Learning swamps context",
review note "Replay needs randomness Huginn lacks"); since R8.4 it keeps only
the two context policies below, as R5's cheap yardstick on the v2 log.

Input: Huginn_Selections.jsonl (SKSE log folder). Each record carries the
press-time state vector `phi`, the page that was `shown`, and every scored
candidate with its full breakdown (`cols`): ctx, prior, the learner's estimate
`est`, ucb, alpha (confidence), lambda, the recency term `rec`, and the corr /
potion / fav multipliers.

What is replayed, and what is not:
- The CONTEXT side is taken from the log as it was: ctx, prior, corr, potion,
  fav. Two policies rank it: the logged utility (A*) and the context alone (B).
- R8.4 (0.24.1): the simulated old learner and its variants (the Phase 3
  policies) are gone (map Phase 4): the R6 fit reads v3 (tools/fit), and the
  choice model's replay is tests/core/ChoiceReplayTests.cpp.
- Ranking is plain utility order over the scored candidates. Overrides,
  Remembrance holds, wildcards, seating and slot classes are not modelled;
  "top 8" approximates the eight-key page.
- Selection bias: picks made FROM Huginn's page favour whatever ranked them
  there. Menu picks ("outside") are the cleaner signal and are reported
  separately.

Usage:
    python tools/replay/replay.py [path/to/Huginn_Selections.jsonl]
        [--char 3F3E2A8817962D59] [--from-launch 20261003-013602]

Selection log v3 (R4, Huginn_Selections_v3.jsonl; schema in
docs/architecture/9-selection-log-v3.md): `iter_v3(paths)` / `load_v3(paths)`
decode it -- every decision with its context, the need vector by name, every
row with cap(i) by column name and the cross-features -- and
    python -I tools/replay/replay.py --v3 [FILE ...] [--char ID]
prints counts, sizes and the old ranking's hit rate on it (one character's
decisions with --char; every character's without). The R6 fit reads
v3; the two policies above stay on the v2 log (R5's cheap test).
"""

from __future__ import annotations

import argparse
import json
import os
from collections import defaultdict

DEFAULT_LOG = os.path.expanduser(r"~/Documents/My Games/Skyrim.INI/SKSE/Huginn_Selections.jsonl")
V2_CHAR = "3F3E2A8817962D59"   # the soak run's character: the v2 replay's default --char

PAGE = 8


# --------------------------------------------------------------------------
# Policies
# --------------------------------------------------------------------------

class Policy:
    name = "?"

    def utility(self, c) -> float:
        raise NotImplementedError

    @staticmethod
    def multipliers(c):
        return c["corr"] * c["potion"] * c["fav"]


class Logged(Policy):
    """The utility the game computed (A*: live utility, ranked plainly)."""
    name = "logged (A*)"

    def utility(self, c):
        return c["util"]


class ContextOnly(Policy):
    """No learning at all (B in Huginn_AB.log)."""
    name = "context only (B)"

    def utility(self, c):
        return c["ctx"] * self.multipliers(c)


# --------------------------------------------------------------------------
# Replay
# --------------------------------------------------------------------------

def load(path, char, from_launch, to_launch=None):
    recs = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if char and r.get("char") != char:
                continue
            if from_launch and r.get("launch", "") < from_launch:
                continue
            if to_launch and r.get("launch", "") > to_launch:
                continue
            if "phi" not in r or "cands" not in r:
                continue
            cols = r["cols"]
            r["_cands"] = [dict(zip(cols, c)) for c in r["cands"]]
            recs.append(r)
    recs.sort(key=lambda r: r["utc"])
    return recs


def replay(recs, policy):
    stats = defaultdict(lambda: [0, 0, 0.0])   # group -> [selections, hits@8, sum reciprocal rank]
    for r in recs:
        cands = r["_cands"]
        chosen = next((c for c in cands if c["form"] == r["form"]), None)
        if chosen is not None:
            ranked = sorted(cands, key=policy.utility, reverse=True)
            rank = next(i for i, c in enumerate(ranked) if c["form"] == r["form"])
            groups = ["all", "outside" if r["src"] == "External" else "huginn", "type:" + chosen["type"]]
            for g in groups:
                s = stats[g]
                s[0] += 1
                s[1] += rank < PAGE
                s[2] += 1.0 / (rank + 1)
    return stats


def class_of(c):
    """The class cap's grouping (src/slot/SlotClassCap.cpp): the slot class, all food one.
    The log's "need" column IS the slot class (the name predates 0.23.8)."""
    return "FoodAny" if c["type"] == "Food" else c["need"]


def capped_page(cands, util, free, discount, page=PAGE):
    """The eight a Regular page would show under the class cap: greedy, each pick
    weighed by discount ** (items of its class already shown - free + 1)."""
    best = {}
    for c in cands:
        u = util(c)
        if c["form"] not in best or u > best[c["form"]][1]:
            best[c["form"]] = (c, u)
    pool, shown, count = list(best.values()), [], defaultdict(int)
    while pool and len(shown) < page:
        i = max(range(len(pool)),
                key=lambda j: pool[j][1] * discount ** max(0, count[class_of(pool[j][0])] - free + 1))
        c, _ = pool.pop(i)
        shown.append(c)
        count[class_of(c)] += 1
    return shown


def replay_class_cap(recs, make_policy, free, discount):
    """hit@8 when the page is filled under the class cap, and how many pages held
    five or more of one class."""
    stats = defaultdict(lambda: [0, 0, 0.0])
    crowded = 0
    policy = make_policy()
    for r in recs:
        cands = r["_cands"]
        chosen = next((c for c in cands if c["form"] == r["form"]), None)
        if chosen is not None:
            shown = capped_page(cands, policy.utility, free, discount)
            hit = any(c["form"] == r["form"] for c in shown)
            for g in ["all", "outside" if r["src"] == "External" else "huginn", "type:" + chosen["type"]]:
                stats[g][0] += 1
                stats[g][1] += hit
            per_class = defaultdict(int)
            for c in shown:
                per_class[class_of(c)] += 1
            crowded += max(per_class.values(), default=0) >= 5
    return stats, crowded


def validate(recs):
    """The logged utility must match its own formula before the policies' answers mean anything."""
    util_err, n_util = 0.0, 0
    for r in recs:
        for c in r["_cands"]:
            calc = c["ctx"] * (1 + c["lambda"] * c["learn"]) * c["corr"] * c["potion"] * c["fav"]
            util_err += abs(calc - c["util"]) / max(1e-6, abs(c["util"]))
            n_util += 1
    print(f"validation: utility formula off by {100 * util_err / max(1, n_util):.2f}% on average")


# --------------------------------------------------------------------------
# Selection log v3 (R4): docs/architecture/9-selection-log-v3.md
# --------------------------------------------------------------------------

DEFAULT_LOG_V3 = os.path.expanduser(r"~/Documents/My Games/Skyrim.INI/SKSE/Huginn_Selections_v3.jsonl")
V3_FLAGS = {"eligible": 1, "scored": 2, "held": 4, "equipped": 8, "shown": 16, "wildcard": 32,
            "override": 64, "remembered": 128, "addedAtPick": 256, "favorite": 512}
# Row columns appended within v3 after the first ten, by name (the head's `row`
# list says which a segment has, and where): 0.23.46 fb, cr; 0.23.53 the old
# engine's score parts est..wu and fl, and the favourites hotkey hk; 0.23.56
# (R8.2) the new model in shadow: mu, sd, lm (the D2 legacy feature ln learn
# mult), um (util missing) and ex (the explanation: [need index, column index]
# or "learned"); 0.24.17 the D2 engine in shadow beside the live one: amu (engine
# A's mu), ar and br (the row's rank under A and under the live ranking, 1 =
# first), ak and bk (its key on A's and on the live ranking's stateless page,
# -1 = off). A row may end before the last of them (written up to the last one
# set): a column past a row's end reads None.
V3_APPENDED = ("fb", "cr", "est", "a", "n", "ucb", "rec", "prior", "cw", "fl", "wu", "hk",
               "mu", "sd", "lm", "um", "ex", "amu", "ar", "br", "ak", "bk")


class V3FormatError(ValueError):
    """A v3 file that breaks the schema (an id used before it is defined, a
    record before any head, a version this reader does not know)."""


def _gzip_corrupt(path):
    """Does the .gz fail to decompress whole (a bad CRC, a broken deflate
    stream, a truncation)? A flipped byte can decode to text that parses as
    JSON before the stream's check fails at its end, so such a file's records
    cannot be trusted as a whole (0.23.16)."""
    import gzip
    import zlib
    try:
        with open(path, "rb") as f:
            gzip.decompress(f.read())
    except (EOFError, OSError, gzip.BadGzipFile, zlib.error):
        return True
    return False


def _raw_lines(path, stats):
    """The file's lines as bytes. A truncated or corrupt .gz (a copy taken
    mid-write, a crash while compressing, a flipped byte) ends the file where
    it breaks: counted in stats["truncated"], never raised."""
    import gzip
    import zlib
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rb") as f:
        while True:
            try:
                line = f.readline()
            except (EOFError, OSError, gzip.BadGzipFile, zlib.error):
                stats["truncated"] = stats.get("truncated", 0) + 1
                return
            if not line:
                return
            yield line


def _decode_line(raw):
    """One line as a JSON object, or None when it is not one (torn mid-record,
    torn inside a multi-byte character, NUL padding left by a crash)."""
    try:
        text = raw.decode("utf-8").strip()
    except UnicodeDecodeError:
        return None
    if not text:
        return ""
    try:
        rec = json.loads(text)
    except ValueError:
        return None
    return rec if isinstance(rec, dict) else None


def _sparse(pairs, names):
    return {names[i]: v for i, v in pairs}


def _row_extras(head):
    """Where the row columns added after the first ten sit, by the head's
    `row` list (a column may only be appended within v3): {name: index} for
    the V3_APPENDED columns this head has. 0.23.46 added fb and cr; 0.23.53
    est, a, n, ucb, rec, prior, cw, fl, wu and hk."""
    names = head.get("row") or []
    return {n: names.index(n) for n in V3_APPENDED if n in names}


def _v3_row(raw, head, caps, extras=None):
    form, uid, kind, src, cap, flags, slot, util, x, wp = raw[:10]
    if cap != -1 and cap not in caps:
        raise V3FormatError(f"cap {cap} used before it is defined")
    # 0.23.46: fb (the candidate filter that dropped a row that is not
    # eligible, by name from the head's "filters") and cr (the remaining
    # seconds behind x["covered"], -1 = no timer). Rows written before 0.23.46
    # have neither: both read None, and x has no "covered".
    # 0.23.53: the old engine's score parts (est, a, n, ucb, rec, prior, cw,
    # wu; fl the floor by name from the head's "floors") and hk. None when the
    # head has no such column or the row ends before it.
    extras = extras or {}
    col = {n: (raw[i] if i < len(raw) else None) for n, i in extras.items()}
    fb = col.get("fb")
    fl = col.get("fl")
    return {
        "form": form,
        "uid": uid,
        "kind": None if kind is None else head["kinds"][kind],
        "src": None if src is None else head["src"][src],
        "cap_id": cap,
        "cap": dict(caps[cap]) if cap != -1 else None,
        "flags": flags,
        "slot": slot,
        "util": util,
        "x": _sparse(x, head["cross"]),
        "wp": wp,
        "fb": None if fb is None else head["filters"][fb],
        "cr": col.get("cr"),
        # 0.23.53 (docs/architecture/9-selection-log-v3.md, rows)
        "est": col.get("est"),
        "a": col.get("a"),
        "n": col.get("n"),
        "ucb": col.get("ucb"),
        "rec": col.get("rec"),
        "prior": col.get("prior"),
        "cw": col.get("cw"),
        "fl": None if fl is None else head["floors"][fl],
        "wu": col.get("wu"),
        "hk": col.get("hk"),
        # 0.23.56 (R8.2): the new model in shadow; ex as (need, column) names,
        # "learned", or None
        "mu": col.get("mu"),
        "sd": col.get("sd"),
        "lm": col.get("lm"),
        "um": col.get("um"),
        "ex": _shadow_ex(col.get("ex"), head),
        # 0.24.17: the D2 engine in shadow (docs/architecture/9-selection-log-v3.md)
        "amu": col.get("amu"),
        "ar": col.get("ar"),
        "br": col.get("br"),
        "ak": col.get("ak"),
        "bk": col.get("bk"),
    }


def _shadow_ex(ex, head):
    """A row's explanation (0.23.56): [need index, column index] -> (need,
    column) by the head's names; "learned" stays; None when the row has none."""
    if isinstance(ex, list) and len(ex) == 2:
        needs, cols = head.get("needs", []), head.get("cols", [])
        k, j = ex
        if 0 <= k < len(needs) and 0 <= j < len(cols):
            return (needs[k], cols[j])
        return None
    return ex if ex == "learned" else None


def head_curves(head):
    """The [Needs] curves a head names (0.23.41), by need: {need: (kind, p1,
    p2)}. Empty for a head written before 0.23.41 (no "curves" field): those
    segments used the defaults of their build's needs.csv."""
    return {n: tuple(c) for n, c in zip(head.get("needs", []), head.get("curves", []))}


def head_lap_sec(head):
    """The lap grid a head names (0.23.51, episode.lapSec, active seconds), or
    None: a head written before 0.23.51 judged every episode whole, so its
    segment holds no lap records."""
    ep = head.get("episode") or {}
    return ep.get("lapSec")


def nothing_stretch(d):
    """For a nothing record, which stretch of its need episode it stands for
    (core/NeedEpisodes.h "lap", 0.23.51): (why, lap, epSec) -- why "end" (the
    episode expired; the stretch is its last) or "lap" (the stretch reached the
    lap grid with the need still on), lap the stretch's index (0 = from the
    onset), epSec the episode's onset to the stretch's end. A record written
    before 0.23.51 is a whole episode: ("end", 0, durSec). None for a pick."""
    ep = d.get("ep")
    if d.get("out") != "nothing" or not isinstance(ep, dict):
        return None
    return (ep.get("why", "end"), ep.get("lap", 0), ep.get("epSec", ep.get("durSec")))


def iter_v3(paths, stats=None):
    """Yield every decision of the given v3 files, in file order, resolved:
    its context (need/input by name, the rows with cap(i) by column name and
    the cross-features by name), `rows` = the context's rows then the
    decision's added rows, and `chosen` = rows[row] (None for nothing).
    Caps and contexts are per segment: a head line starts a new one.

    0.23.46 additions, defaulted for older files: a row's "fb" and "cr" (None),
    x["covered"] (absent), the context's "summon" (absent: ctx.get("summon")).
    0.23.53: a row's old-engine parts "est", "a", "n", "ucb", "rec", "prior",
    "cw", "fl" (a floor name), "wu" and its "hk" (None when absent), the
    "favorite" flag bit, the context's "old" (absent before) and the
    decision's "rid" (absent before; null when no v2 record shares it).
    0.23.51: a nothing record's ep may carry "why" ("end" / "lap"), "lap" and
    "epSec" (lapped stretches of an open episode); nothing_stretch(d) reads
    them with the defaults of an older record, head_lap_sec(head) the grid.

    Damage is skipped and counted, never raised (a crash or a full disk can
    tear the last record a launch wrote; the writer starts on a new line):
      stats["bad_lines"]   lines that are not a JSON object (torn mid-record,
                           torn inside a UTF-8 character, NUL padding) or
                           records that lack their fields;
      stats["skipped"]     records after damage that needed a cap or context
                           the damage may have taken, or that belong to
                           another launch than the head in force (a torn head
                           between two launches);
      stats["lost_heads"]  after damage, a cap id defined a second time: a
                           new segment began behind a torn head. Its records
                           are decoded against the last good head when they
                           are of its launch (a segment of the same launch has
                           the same head), and skipped otherwise;
      stats["truncated"]   .gz files that end mid-stream (read up to there).
    Without damage, a reference to an undefined id is a malformed file
    (V3FormatError), as is a version other than 3.

    Limit (0.23.16): in a .gz that fails to decompress whole (_gzip_corrupt),
    EVERY format error of that file is counted as a bad line, not raised --
    not only the ones after the break. The reader cannot tell where the
    corruption starts: the deflate output after a flipped byte can parse as
    records (a record of an unknown type was seen) until the stream's CRC
    fails at its end, and the decompression error names no earlier offset.
    The file's good records still decode; only the strictness is lost, and
    only for a corrupt .gz -- a plain .jsonl and an intact .gz stay strict."""
    if stats is None:
        stats = {}
    for key in ("bad_lines", "skipped", "lost_heads", "truncated"):
        stats.setdefault(key, 0)
    for path in paths:
        head, caps, ctxs = None, {}, {}
        extras = {}
        damaged = False   # damage since the last good head
        lost_head = False  # a lost head already counted for this damage
        # A corrupt .gz: what decodes before the break may be garbage that
        # parses (a record of an unknown type, a reference to nothing), so a
        # format error in such a file is damage, counted, never raised.
        corrupt = str(path).endswith(".gz") and _gzip_corrupt(path)
        for lineno, raw in enumerate(_raw_lines(path, stats), 1):
            try:
                rec = _decode_line(raw)
                if rec == "":
                    continue
                if rec is None:
                    stats["bad_lines"] += 1
                    damaged, lost_head = True, False
                    continue
                t = rec.get("t")
                if t == "head":
                    if rec.get("v") != 3:
                        raise V3FormatError(f"{path}:{lineno}: version {rec.get('v')} is not 3")
                    if not all(k in rec for k in ("cols", "needs", "cross", "kinds", "src")):
                        stats["bad_lines"] += 1   # a head that parses but lacks its lists
                        damaged = True
                        continue
                    head, caps, ctxs, damaged, lost_head = rec, {}, {}, False, False
                    extras = _row_extras(rec)
                    continue
                if head is None:
                    if damaged:
                        stats["skipped"] += 1
                        continue
                    raise V3FormatError(f"{path}:{lineno}: a '{t}' record before any head")
                try:
                    if t == "cap":
                        if damaged and rec["id"] in caps and not lost_head:
                            stats["lost_heads"] += 1   # a new segment behind a torn head
                            lost_head = True
                        caps[rec["id"]] = _sparse(rec["c"], head["cols"])
                    elif t == "ctx":
                        ctx = {k: v for k, v in rec.items() if k not in ("rows", "need", "in")}
                        ctx["need"] = _sparse(rec["need"], head["needs"])
                        ctx["input"] = _sparse(rec["in"], head["needs"])
                        ctx["rows"] = [_v3_row(r, head, caps, extras) for r in rec["rows"]]
                        ctxs[rec["id"]] = ctx
                    elif t == "dec":
                        if rec.get("v") != 3:
                            raise V3FormatError(f"{path}:{lineno}: decision version {rec.get('v')}")
                        if damaged and rec.get("launch") != head.get("launch"):
                            stats["skipped"] += 1   # decoded against another launch's head
                            continue
                        ctx_id = rec.get("ctx")
                        if ctx_id is not None and ctx_id not in ctxs:
                            raise V3FormatError(f"{path}:{lineno}: context {ctx_id} used before it is defined")
                        d = dict(rec)
                        d["ctx"] = ctxs[ctx_id] if ctx_id is not None else None
                        d["add"] = [_v3_row(r, head, caps, extras) for r in rec["add"]]
                        d["rows"] = (d["ctx"]["rows"] if d["ctx"] else []) + d["add"]
                        d["chosen"] = d["rows"][rec["row"]] if rec["row"] >= 0 else None
                        d["head"] = head
                        yield d
                    else:
                        raise V3FormatError(f"{path}:{lineno}: unknown record type {t!r}")
                except V3FormatError:
                    if not damaged:
                        raise
                    stats["skipped"] += 1   # it needed what the damage took
                except (KeyError, TypeError, IndexError, ValueError):
                    # Parses as JSON but is not a whole record of its type.
                    stats["bad_lines"] += 1
                    damaged = True
            except V3FormatError:
                if not corrupt:
                    raise
                stats["bad_lines"] += 1
                damaged, lost_head = True, False


def load_v3(paths, stats=None):
    """All decisions (iter_v3); torn lines are reported on stderr."""
    stats = {} if stats is None else stats
    decs = list(iter_v3(paths, stats))
    if any(stats.get(k) for k in ("bad_lines", "skipped", "lost_heads", "truncated")):
        import sys
        print(f"warning: {stats['bad_lines']} unreadable line(s), {stats['skipped']} record(s) skipped, "
              f"{stats['lost_heads']} lost head(s), {stats['truncated']} truncated file(s)", file=sys.stderr)
    return decs


def summarize_v3(paths, char=None):
    """Counts, sizes and a plain-ranking smoke number for a v3 log. With char,
    the decisions, outcomes, rows and hits are that character's only; the
    record sizes are always the whole file's."""
    sizes = defaultdict(lambda: [0, 0])   # record type -> [count, bytes]
    size_stats = {}
    for path in paths:
        for raw in _raw_lines(path, size_stats):
            rec = _decode_line(raw)
            if rec == "":
                continue
            t = str(rec.get("t")) if rec else "(torn)"
            sizes[t][0] += 1
            sizes[t][1] += len(raw)
    stats = {}
    decs = list(iter_v3(paths, stats))
    n_all = len(decs)
    if char is not None:
        decs = [d for d in decs if d.get("char") == char]
    by_out = defaultdict(int)
    nothing_laps = 0
    rows = []
    hits = defaultdict(lambda: [0, 0])
    for d in decs:
        # .get: a lenient read of a corrupt .gz can decode a record whose key
        # bytes were flipped yet still parse (seen when the golden file grew
        # by one need, 0.23.34); the summary must not raise on it.
        out = d.get("out", "?")
        by_out[out] += 1
        stretch = nothing_stretch(d)
        nothing_laps += 1 if stretch and stretch[0] == "lap" else 0
        rows.append(len(d["rows"]))
        c = d["chosen"]
        if c is None:
            continue
        # The old engine's ranking (logged util) over the eligible rows: is
        # the chosen item in the top 8? A smoke number, not the fit.
        ranked = sorted((r for r in d["rows"] if r["util"] is not None), key=lambda r: -r["util"])
        top = {(r["form"], r["uid"]) for r in ranked[:PAGE]}
        h = hits[out]
        h[0] += 1
        h[1] += (c["form"], c["uid"]) in top
    total_bytes = sum(b for _, b in sizes.values())
    print(f"{len(decs)} decisions in {len(paths)} file(s), {total_bytes} bytes"
          + (f" (character {char}: {len(decs)} of {n_all})" if char is not None else ""))
    if any(stats[k] for k in ("bad_lines", "skipped", "lost_heads", "truncated")):
        print(f"  damage: {stats['bad_lines']} unreadable line(s), {stats['skipped']} record(s) skipped, "
              f"{stats['lost_heads']} lost head(s), {stats['truncated']} truncated file(s)")
    for t, (n, b) in sorted(sizes.items()):
        print(f"  {t:5} {n:6d} lines {b:10d} bytes ({b / max(1, n):.0f} per line)")
    print("  outcomes: " + ", ".join(f"{k}={v}" for k, v in sorted(by_out.items())))
    if by_out.get("nothing"):
        print(f"  nothing: {by_out['nothing'] - nothing_laps} ended episode(s), "
              f"{nothing_laps} lapped stretch(es) (0.23.51)")
    if rows:
        print(f"  rows per decision: mean {sum(rows) / len(rows):.1f}, max {max(rows)}")
    if decs:
        print(f"  bytes per decision (all lines): {total_bytes / len(decs):.0f}")
    for out, (n, h) in sorted(hits.items()):
        print(f"  {out}: chosen in the logged-util top {PAGE}: {h} of {n}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", nargs="?", default=None)
    ap.add_argument("--v3", nargs="*", metavar="FILE",
                    help="read selection log v3 file(s) (.jsonl or .jsonl.gz; default "
                         "Huginn_Selections_v3.jsonl) and print a summary")
    ap.add_argument("--char", default=None,
                    help="keep one character (dec.char): --v3 reads every character without it; "
                         f"the v2 replay defaults to the soak run's {V2_CHAR}")
    ap.add_argument("--from-launch", default="20261003-013602", help="first launch of the run (UTC stamp)")
    ap.add_argument("--to-launch", default="20261004-235959",
                    help="last launch to include (UTC stamp); the default ends the soak run, before 0.23.0")
    args = ap.parse_args()

    if args.v3 is not None:
        summarize_v3(args.v3 or [args.log or DEFAULT_LOG_V3], args.char)
        return
    args.log = args.log or DEFAULT_LOG
    recs = load(args.log, args.char or V2_CHAR, args.from_launch, args.to_launch)
    print(f"{len(recs)} selections from {args.log}")
    validate(recs)

    policies = [Logged(), ContextOnly()]
    groups = ["all", "huginn", "outside", "type:Weapon", "type:Spell", "type:Potion", "type:Food", "type:Scroll"]
    results = {p.name: replay(recs, p) for p in policies}

    width = max(len(p.name) for p in policies)
    head = " ".join(f"{g.replace('type:', ''):>14}" for g in groups)
    print(f"\nhit@{PAGE} (chosen item in the top {PAGE}) -- n per group in brackets")
    print(f"{'policy':<{width}} {head}")
    for p in policies:
        cells = []
        for g in groups:
            n, hits, _ = results[p.name][g]
            cells.append(f"{(100 * hits / n if n else 0):5.1f}% ({n:4d})")
        print(f"{p.name:<{width}} " + " ".join(f"{c:>14}" for c in cells))

    # The class cap (src/slot/SlotClassCap.h): placement on Regular keys, under the
    # logged utility. discount 1.0 = no cap.
    shipped = Logged
    print(f"\nclass cap, logged utility: hit@{PAGE}, and pages with 5+ of one class")
    print(f"{'cap':<26} {head} {'crowded':>9}")
    for free, discount in [(3, 1.0), (1, 0.5), (3, 0.75), (3, 0.5)]:
        stats, crowded = replay_class_cap(recs, shipped, free, discount)
        label = "off" if discount >= 1.0 else f"first {free} free, x{discount}"
        cells = []
        for g in groups:
            n, hits, _ = stats[g]
            cells.append(f"{(100 * hits / n if n else 0):5.1f}% ({n:4d})")
        print(f"{label:<26} " + " ".join(f"{c:>14}" for c in cells) + f" {crowded:>9}")


if __name__ == "__main__":
    main()

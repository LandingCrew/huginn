"""Offline replay of Huginn's selection log -- the Phase 3 harness.

Re-ranks every logged selection under a scoring policy and reports how often
the item the player actually chose would have been near the top. A learning
change costs minutes here instead of play-hours (roadmap, "Learning swamps
context", review note "Replay needs randomness Huginn lacks").

Input: Huginn_Selections.jsonl (SKSE log folder). Each record carries the
press-time state vector `phi`, the page that was `shown`, and every scored
candidate with its full breakdown (`cols`): ctx, prior, the learner's estimate
`est`, ucb, alpha (confidence), lambda, the recency term `rec`, and the corr /
potion / fav multipliers.

What is replayed, and what is not:
- The CONTEXT side is taken from the log as it was: ctx, prior, corr, potion,
  fav. A policy changes only the learned side (and, optionally, the lambda
  range and recency scale).
- The LEARNER is simulated from zero over the selections in time order,
  predicting before it learns from each one -- as the game does. So a policy
  is judged on picks it had not yet seen.
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
    python -I tools/replay/replay.py --v3 [FILE ...]
prints counts, sizes and the old ranking's hit rate on it. The R6 fit reads
v3; the policies above stay on the v2 log (R5's cheap test).
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
from collections import defaultdict
from dataclasses import dataclass, field

DEFAULT_LOG = os.path.expanduser(r"~/Documents/My Games/Skyrim.INI/SKSE/Huginn_Selections.jsonl")

# The shipped constants (FeatureBanditLearner.h, ScorerSettings.h, Config.h).
LEARNING_RATE = 0.1
L2_LAMBDA = 0.01
WEIGHT_CLAMP = 10.0
CONF_MIDPOINT = 5.0
CONF_STEEPNESS = 0.3
UCB_NORM = 0.2
BETA = 0.2
PASSED_OVER_DELAY_SEC = 10   # Config::PASSED_OVER_DELAY_SEC
LAMBDA_MIN, LAMBDA_MAX = 0.5, 3.0
PAGE = 8


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def confidence(n):
    """The sigmoid the game used up to the choice target (50% at 5, ~95% at 15)."""
    return 1.0 / (1.0 + math.exp(-CONF_STEEPNESS * (n - CONF_MIDPOINT)))


def pseudo_confidence(n, n0):
    """n / (n + n0): the prior as n0 pseudo-observations (0.23.0)."""
    return n / (n + n0)


def ucb(n, total):
    if n == 0 or total == 0:
        return 1.0
    return max(0.0, min(1.0, UCB_NORM * math.sqrt(2.0 * math.log(total) / n)))


# --------------------------------------------------------------------------
# Learner
# --------------------------------------------------------------------------

@dataclass
class Learner:
    """Per-item linear model over phi, as FeatureBanditLearner."""
    dims: int = 18
    w: dict = field(default_factory=dict)
    n: dict = field(default_factory=lambda: defaultdict(int))
    total: int = 0

    def predict(self, form, phi):
        return dot(self.w.get(form, [0.0] * self.dims), phi)

    def update(self, form, phi, target, step=1.0, count=1.0):
        w = self.w.setdefault(form, [0.0] * self.dims)
        err = target - dot(w, phi)
        for i in range(self.dims):
            w[i] += step * LEARNING_RATE * err * phi[i] - step * LEARNING_RATE * L2_LAMBDA * w[i]
            w[i] = max(-WEIGHT_CLAMP, min(WEIGHT_CLAMP, w[i]))
        self.n[form] += count
        self.total += count


# --------------------------------------------------------------------------
# Policies
# --------------------------------------------------------------------------

class Policy:
    name = "?"

    def __init__(self):
        self.learner = Learner()

    def utility(self, c, phi) -> float:
        raise NotImplementedError

    def learn(self, rec, chosen, phi):
        pass

    @staticmethod
    def multipliers(c):
        return c["corr"] * c["potion"] * c["fav"]


class Logged(Policy):
    """The utility the game computed (A*: live utility, ranked plainly)."""
    name = "logged (A*)"

    def utility(self, c, phi):
        return c["util"]


class ContextOnly(Policy):
    """No learning at all (B in Huginn_AB.log)."""
    name = "context only (B)"

    def utility(self, c, phi):
        return c["ctx"] * self.multipliers(c)


class Current(Policy):
    """Today's learner, simulated: target 8 (equip) / 5 (consume) on the chosen item only."""
    name = "current, simulated"

    def __init__(self, rec_scale=1.0):
        super().__init__()
        self.rec_scale = rec_scale
        self.lambda_max = LAMBDA_MAX

    def conf(self, n):
        return confidence(n)

    def learn_score(self, c, phi):
        L = self.learner
        n = L.n[c["form"]]
        a = self.conf(n)
        learn = a * L.predict(c["form"], phi) + (1 - a) * c["prior"] + BETA * ucb(n, L.total) + self.rec_scale * c["rec"]
        lam = LAMBDA_MIN + a * (self.lambda_max - LAMBDA_MIN)
        return learn, lam

    def utility(self, c, phi):
        learn, lam = self.learn_score(c, phi)
        return c["ctx"] * (1 + lam * learn) * self.multipliers(c)

    def learn(self, rec, chosen, phi):
        # 8 / 5 by kind, not rec["reward"]: since 0.23.0 the log writes 1.0.
        self.learner.update(rec["form"], phi, 8.0 if rec.get("kind") == "equip" else 5.0)


class ChoiceTarget(Current):
    """Phase 3 #1: chosen -> 1, shown for the same slot class and passed over -> 0.

    neg_weight scales a negative's step (and how much it counts as a train);
    lambda_max caps the learned boost; rec_scale rescales the recency term
    that was sized for the 0-8 target.
    """

    def __init__(self, neg_weight=0.25, lambda_max=LAMBDA_MAX, rec_scale=1.0 / 8.0, repeat_window=0.0,
                 pseudo_n0=None, label=None):
        super().__init__(rec_scale=rec_scale)
        # None: the sigmoid confidence; a number: n / (n + pseudo_n0).
        self.pseudo_n0 = pseudo_n0
        self.neg_weight = neg_weight
        self.lambda_max = lambda_max
        # One decision, one reward: a pick of the same item within this many
        # seconds of its last pick (same launch) teaches nothing -- the main
        # weapon taken back after every scroll is not a new choice.
        self.repeat_window = repeat_window
        self._last = {}
        self._pending = []   # (due, form, phi): deferred passed-over updates
        self.name = label or (f"choice target (neg {neg_weight}, lmax {lambda_max}, rec x{rec_scale:.3g}"
                              + (f", repeat {repeat_window:g}s" if repeat_window else "")
                              + (f", pseudo-obs n0={pseudo_n0:g}" if pseudo_n0 is not None else "") + ")")

    def conf(self, n):
        return confidence(n) if self.pseudo_n0 is None else pseudo_confidence(n, self.pseudo_n0)

    def learn(self, rec, chosen, phi):
        # As the game: passed-over updates wait PASSED_OVER_DELAY_SEC, are
        # cancelled if their item is the next pick (a companion), and are
        # skipped for an item the learner holds no entry for.
        t = dt.datetime.strptime(rec["utc"][:19], "%Y-%m-%d %H:%M:%S")
        self._pending = [p for p in self._pending if p[1] != rec["form"]]
        keep = []
        for due, form, pphi in self._pending:
            if t < due:
                keep.append((due, form, pphi))
            elif form in self.learner.w:
                # A quarter step, not a train -- as BanditSubscriber does in the game.
                self.learner.update(form, pphi, 0.0, step=self.neg_weight, count=0)
        self._pending = keep
        # The repeat window covers equips only: two drinks are two decisions.
        if self.repeat_window and rec.get("kind") == "equip":
            prev = self._last.get(rec["form"])
            self._last[rec["form"]] = (t, rec["launch"])
            if prev and prev[1] == rec["launch"] and (t - prev[0]).total_seconds() < self.repeat_window:
                return
        self.learner.update(rec["form"], phi, 1.0)
        if self.neg_weight <= 0:
            return
        need = chosen["need"] if chosen else None
        by_form = {c["form"]: c for c in rec["_cands"]}
        for _slot, form, kind in rec["shown"]:
            if form == rec["form"] or kind != "Normal":
                continue
            c = by_form.get(form)
            if c is None or need is None or c["need"] != need:
                continue
            self._pending.append((t + dt.timedelta(seconds=PASSED_OVER_DELAY_SEC), form, phi))


class UsefulLife(ChoiceTarget):
    """Phase 3 #3a: confidence fades with PLAY time since the item was last chosen.

    retention(t) = 1 / (1 + exp((t - T) / s)), T = T0 + k ln(1 + n);
    n_eff = n * retention scores confidence and UCB, and a new pick restarts
    from n_eff + 1. Evicted (forgotten) below `evict`. Play time is the time
    between selections in one launch, each gap capped at GAP_CAP_SEC (menus,
    AFK); time between launches is not play.
    """
    GAP_CAP_SEC = 600

    def __init__(self, t0_h=8.0, k_h=2.0, s_h=1.0, evict=0.05, label=None):
        super().__init__(neg_weight=0.25, repeat_window=30, pseudo_n0=2)
        self.t0, self.k, self.s, self.evict = t0_h * 3600, k_h * 3600, s_h * 3600, evict
        self.play = 0.0
        self._prev = None        # (utc, launch) of the previous record
        self.chosen_at = {}      # form -> play seconds at its last pick
        self.forgotten = 0
        self.name = label or f"useful life T0={t0_h:g}h k={k_h:g}h s={s_h:g}h"

    def retention(self, form):
        n = self.learner.n[form]
        if n <= 0:
            return 1.0
        t = self.play - self.chosen_at.get(form, self.play)
        life = self.t0 + self.k * math.log1p(n)
        x = (t - life) / self.s
        return 0.0 if x > 50 else 1.0 / (1.0 + math.exp(x))

    def tick(self, rec):
        t = dt.datetime.strptime(rec["utc"][:19], "%Y-%m-%d %H:%M:%S")
        if self._prev and self._prev[1] == rec["launch"]:
            self.play += min(self.GAP_CAP_SEC, max(0.0, (t - self._prev[0]).total_seconds()))
        self._prev = (t, rec["launch"])
        L = self.learner
        for form in [f for f in L.w if L.n[f] > 0 and self.retention(f) < self.evict]:
            L.total -= L.n[form]
            del L.w[form]
            L.n[form] = 0
            self.chosen_at.pop(form, None)
            self.forgotten += 1

    def learn_score(self, c, phi):
        L = self.learner
        n_eff = L.n[c["form"]] * self.retention(c["form"])
        a = self.conf(n_eff)
        learn = (a * L.predict(c["form"], phi) + (1 - a) * c["prior"] + BETA * ucb(n_eff, L.total)
                 + self.rec_scale * c["rec"])
        return learn, LAMBDA_MIN + a * (self.lambda_max - LAMBDA_MIN)

    def learn(self, rec, chosen, phi):
        form = rec["form"]
        before = self.learner.n[form]
        n_eff = before * self.retention(form)
        super().learn(rec, chosen, phi)
        if self.learner.n[form] != before:       # a train (not a repeat inside the window)
            self.learner.total += n_eff - before
            self.learner.n[form] = n_eff + 1
            self.chosen_at[form] = self.play


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
            # The recency term on ONE scale, the pre-0.23.0 one (1.5): since
            # the choice target the game logs it already divided by 8
            # (0.1875), and the policies' rec_scale = 1/8 would divide again.
            for c in r["_cands"]:
                if 0.0 < c.get("rec", 0.0) < 1.0:
                    c["rec"] *= 8.0
            recs.append(r)
    recs.sort(key=lambda r: r["utc"])
    return recs


def replay(recs, policy):
    stats = defaultdict(lambda: [0, 0, 0.0])   # group -> [selections, hits@8, sum reciprocal rank]
    for r in recs:
        phi = r["phi"]
        cands = r["_cands"]
        chosen = next((c for c in cands if c["form"] == r["form"]), None)
        if chosen is not None:
            ranked = sorted(cands, key=lambda c: policy.utility(c, phi), reverse=True)
            rank = next(i for i, c in enumerate(ranked) if c["form"] == r["form"])
            groups = ["all", "outside" if r["src"] == "External" else "huginn", "type:" + chosen["type"]]
            for g in groups:
                s = stats[g]
                s[0] += 1
                s[1] += rank < PAGE
                s[2] += 1.0 / (rank + 1)
        policy.learn(r, chosen, phi)
        if hasattr(policy, "tick"):
            policy.tick(r)
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
        phi = r["phi"]
        cands = r["_cands"]
        chosen = next((c for c in cands if c["form"] == r["form"]), None)
        if chosen is not None:
            shown = capped_page(cands, lambda c: policy.utility(c, phi), free, discount)
            hit = any(c["form"] == r["form"] for c in shown)
            for g in ["all", "outside" if r["src"] == "External" else "huginn", "type:" + chosen["type"]]:
                stats[g][0] += 1
                stats[g][1] += hit
            per_class = defaultdict(int)
            for c in shown:
                per_class[class_of(c)] += 1
            crowded += max(per_class.values(), default=0) >= 5
        policy.learn(r, chosen, phi)
    return stats, crowded


def validate(recs):
    """The replica must reproduce what the game logged before its answers mean anything."""
    util_err, est_err, n_util, n_est = 0.0, 0.0, 0, 0
    sim = Current()
    for r in recs:
        phi = r["phi"]
        for c in r["_cands"]:
            calc = c["ctx"] * (1 + c["lambda"] * c["learn"]) * c["corr"] * c["potion"] * c["fav"]
            util_err += abs(calc - c["util"]) / max(1e-6, abs(c["util"]))
            n_util += 1
            est_err += abs(sim.learner.predict(c["form"], phi) - c["est"])
            n_est += 1
        chosen = next((c for c in r["_cands"] if c["form"] == r["form"]), None)
        sim.learn(r, chosen, phi)
    print(f"validation: utility formula off by {100 * util_err / max(1, n_util):.2f}% on average; "
          f"simulated learner estimate off by {est_err / max(1, n_est):.3f} on average "
          f"(0-8 scale; reloads of unsaved sessions and time decay make it drift)")


# --------------------------------------------------------------------------
# Selection log v3 (R4): docs/architecture/9-selection-log-v3.md
# --------------------------------------------------------------------------

DEFAULT_LOG_V3 = os.path.expanduser(r"~/Documents/My Games/Skyrim.INI/SKSE/Huginn_Selections_v3.jsonl")
V3_FLAGS = {"eligible": 1, "scored": 2, "held": 4, "equipped": 8, "shown": 16, "wildcard": 32,
            "override": 64, "remembered": 128, "addedAtPick": 256}


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


def _v3_row(raw, head, caps):
    form, uid, kind, src, cap, flags, slot, util, x, wp = raw
    if cap != -1 and cap not in caps:
        raise V3FormatError(f"cap {cap} used before it is defined")
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
    }


def iter_v3(paths, stats=None):
    """Yield every decision of the given v3 files, in file order, resolved:
    its context (need/input by name, the rows with cap(i) by column name and
    the cross-features by name), `rows` = the context's rows then the
    decision's added rows, and `chosen` = rows[row] (None for nothing).
    Caps and contexts are per segment: a head line starts a new one.

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
                        ctx["rows"] = [_v3_row(r, head, caps) for r in rec["rows"]]
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
                        d["add"] = [_v3_row(r, head, caps) for r in rec["add"]]
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


def summarize_v3(paths):
    """Counts, sizes and a plain-ranking smoke number for a v3 log."""
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
    by_out = defaultdict(int)
    rows = []
    hits = defaultdict(lambda: [0, 0])
    for d in decs:
        by_out[d["out"]] += 1
        rows.append(len(d["rows"]))
        c = d["chosen"]
        if c is None:
            continue
        # The old engine's ranking (logged util) over the eligible rows: is
        # the chosen item in the top 8? A smoke number, not the fit.
        ranked = sorted((r for r in d["rows"] if r["util"] is not None), key=lambda r: -r["util"])
        top = {(r["form"], r["uid"]) for r in ranked[:PAGE]}
        h = hits[d["out"]]
        h[0] += 1
        h[1] += (c["form"], c["uid"]) in top
    total_bytes = sum(b for _, b in sizes.values())
    print(f"{len(decs)} decisions in {len(paths)} file(s), {total_bytes} bytes")
    if any(stats[k] for k in ("bad_lines", "skipped", "lost_heads", "truncated")):
        print(f"  damage: {stats['bad_lines']} unreadable line(s), {stats['skipped']} record(s) skipped, "
              f"{stats['lost_heads']} lost head(s), {stats['truncated']} truncated file(s)")
    for t, (n, b) in sorted(sizes.items()):
        print(f"  {t:5} {n:6d} lines {b:10d} bytes ({b / max(1, n):.0f} per line)")
    print("  outcomes: " + ", ".join(f"{k}={v}" for k, v in sorted(by_out.items())))
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
    ap.add_argument("--char", default="3F3E2A8817962D59")
    ap.add_argument("--from-launch", default="20261003-013602", help="first launch of the run (UTC stamp)")
    ap.add_argument("--to-launch", default="20261004-235959",
                    help="last launch to include (UTC stamp); the default ends the soak run, before 0.23.0")
    args = ap.parse_args()

    if args.v3 is not None:
        summarize_v3(args.v3 or [args.log or DEFAULT_LOG_V3])
        return
    args.log = args.log or DEFAULT_LOG
    recs = load(args.log, args.char, args.from_launch, args.to_launch)
    print(f"{len(recs)} selections from {args.log}")
    validate(recs)

    policies = [
        Logged(),
        ContextOnly(),
        Current(),
        ChoiceTarget(neg_weight=0.0, label="choice target, positives only"),
        ChoiceTarget(neg_weight=0.25),
        ChoiceTarget(neg_weight=0.5),
        ChoiceTarget(neg_weight=1.0),
        ChoiceTarget(neg_weight=0.25, lambda_max=2.0),
        ChoiceTarget(neg_weight=0.25, rec_scale=0.0, label="choice target (neg 0.25), no recency"),
        ChoiceTarget(neg_weight=0.25, repeat_window=30),
        ChoiceTarget(neg_weight=0.25, repeat_window=60),
        ChoiceTarget(neg_weight=0.25, repeat_window=120),
        ChoiceTarget(neg_weight=0.25, repeat_window=30, pseudo_n0=1),
        ChoiceTarget(neg_weight=0.25, repeat_window=30, pseudo_n0=2, label="SHIPPED: choice target + pseudo-obs n0=2 (0.23.0)"),
        ChoiceTarget(neg_weight=0.25, repeat_window=30, pseudo_n0=3),
    ]
    # Phase 3 #3a: the useful-life curve against the shipped learner.
    policies += [
        UsefulLife(),
        UsefulLife(t0_h=4.0, k_h=1.0),
        UsefulLife(t0_h=2.0, k_h=1.0, s_h=0.5),
        UsefulLife(t0_h=1.0, k_h=0.5, s_h=0.25),
    ]
    groups = ["all", "huginn", "outside", "type:Weapon", "type:Spell", "type:Potion", "type:Food", "type:Scroll"]
    results = {p.name: replay(recs, p) for p in policies}
    for p in policies:
        if isinstance(p, UsefulLife):
            print(f"{p.name}: {p.play / 3600:.1f} play-hours, {p.forgotten} items forgotten")

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
    # shipped learner. discount 1.0 = no cap.
    shipped = lambda: ChoiceTarget(neg_weight=0.25, repeat_window=30, pseudo_n0=2)
    print(f"\nclass cap, shipped learner: hit@{PAGE}, and pages with 5+ of one class")
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

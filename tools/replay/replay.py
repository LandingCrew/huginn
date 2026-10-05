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
LAMBDA_MIN, LAMBDA_MAX = 0.5, 3.0
PAGE = 8


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def confidence(n):
    return 1.0 / (1.0 + math.exp(-CONF_STEEPNESS * (n - CONF_MIDPOINT)))


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

    def learn_score(self, c, phi):
        L = self.learner
        n = L.n[c["form"]]
        a = confidence(n)
        learn = a * L.predict(c["form"], phi) + (1 - a) * c["prior"] + BETA * ucb(n, L.total) + self.rec_scale * c["rec"]
        lam = LAMBDA_MIN + a * (LAMBDA_MAX - LAMBDA_MIN)
        return learn, lam

    def utility(self, c, phi):
        learn, lam = self.learn_score(c, phi)
        return c["ctx"] * (1 + lam * learn) * self.multipliers(c)

    def learn(self, rec, chosen, phi):
        self.learner.update(rec["form"], phi, rec["reward"])


class ChoiceTarget(Current):
    """Phase 3 #1: chosen -> 1, shown for the same need and passed over -> 0.

    neg_weight scales a negative's step (and how much it counts as a train);
    lambda_max caps the learned boost; rec_scale rescales the recency term
    that was sized for the 0-8 target.
    """

    def __init__(self, neg_weight=0.25, lambda_max=LAMBDA_MAX, rec_scale=1.0 / 8.0, repeat_window=0.0, label=None):
        super().__init__(rec_scale=rec_scale)
        self.neg_weight = neg_weight
        self.lambda_max = lambda_max
        # One decision, one reward: a pick of the same item within this many
        # seconds of its last pick (same launch) teaches nothing -- the main
        # weapon taken back after every scroll is not a new choice.
        self.repeat_window = repeat_window
        self._last = {}
        self.name = label or (f"choice target (neg {neg_weight}, lmax {lambda_max}, rec x{rec_scale:.3g}"
                              + (f", repeat {repeat_window:g}s" if repeat_window else "") + ")")

    def learn_score(self, c, phi):
        learn, _ = super().learn_score(c, phi)
        a = confidence(self.learner.n[c["form"]])
        lam = LAMBDA_MIN + a * (self.lambda_max - LAMBDA_MIN)
        return learn, lam

    def learn(self, rec, chosen, phi):
        if self.repeat_window:
            t = dt.datetime.strptime(rec["utc"][:19], "%Y-%m-%d %H:%M:%S")
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
            # A quarter step, not a train -- as BanditSubscriber does in the game.
            self.learner.update(form, phi, 0.0, step=self.neg_weight, count=0)


# --------------------------------------------------------------------------
# Replay
# --------------------------------------------------------------------------

def load(path, char, from_launch):
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
    return stats


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


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", nargs="?", default=DEFAULT_LOG)
    ap.add_argument("--char", default="3F3E2A8817962D59")
    ap.add_argument("--from-launch", default="20261003-013602", help="first launch of the run (UTC stamp)")
    args = ap.parse_args()

    recs = load(args.log, args.char, args.from_launch)
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
    ]
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


if __name__ == "__main__":
    main()

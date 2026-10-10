"""Fit the drop_ahead response curve from a Huginn debug log.

drop_ahead (needs.csv) is a logistic of the drop in front of the player, in
units. What it should anticipate is fall damage, so the curve is fitted to
the falls the player actually took: each `[Falling] end -- peak depth N`
line (state/StateManager_Position.cpp; N = the descent below the take-off
point, units) paired with the damage logged right after it.

Damage, per fall, read from the window (fall end, fall end + WINDOW_SEC]:
  * peak health_falling: the largest `health_falling=` value in the
    `[Needs]` lines (needs.csv health_falling: a logistic, c 0.03 of max
    health per second, of the net health loss rate; it saturates at 1.00
    for any big hit). This is the severity the curve is fitted to.
  * health_deficit after: the largest `health_deficit=` in the window.
  * hp drop: the last `hp=NN.N%` of a `[Context]` line at or before the fall
    end (within 60 s) minus the smallest one in the window (only when both
    exist; shown, not fitted).
A fall is EXCLUDED from the fit when
  * it landed in water: a `Water: ... swimming=true` line in
    [fall end - 0.5 s, fall end + WINDOW_SEC] (deep water is a safe landing,
    which the probe handles itself: core kSafeLandingDepth), or
  * health was already falling: the last `[Needs]` line before the fall's
    end (inside LOOKBACK_SEC) carries health_falling > 0 (combat or another
    hurt the damage cannot be told from), or
  * it is listed with --exclude HH:MM:SS (e.g. a slide down a slope that
    measured a deep "fall" without a free fall).
`[Needs]` lines are rate-limited and deadbanded (needs/NeedMonitor.cpp), and
only non-zero needs are printed, so a missing health_falling reads 0.

Fits, by grid search (std-lib only):
  1. least squares of logistic(depth; c, k) to peak health_falling (the
     severity curve; the one drop_ahead uses);
  2. maximum likelihood of logistic(depth; c, k) as P(any damage), damage =
     peak health_falling >= DAMAGE_MIN (where damage starts; for reference);
  3. with --bootstrap N: the least-squares fit on N resamples of the falls
     (with replacement, fixed seed, a coarser grid: centre step 5, slope
     step 0.001), printing the 10th / 50th / 90th percentiles of centre and
     slope -- how well a session this size pins the curve down.

Usage:
  python -I tools/needs/fit_drop_curve.py <log> [--exclude HH:MM:SS ...]
                                              [--bootstrap N]

Fall damage depends on the player's max health and on the modlist (the
damage formula and its multipliers are game settings), so a fit is only good
for the setup the log came from; say which in needs.csv.
"""

import math
import random
import re
import sys
from datetime import datetime

WINDOW_SEC = 3.0
LOOKBACK_SEC = 3.0
DAMAGE_MIN = 0.05

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]\[[^\]]*\]\[\w\]: (.*)$")
FALL_END = re.compile(r"\[Falling\] end \S+ peak depth (\d+)")
FALL_START = re.compile(r"\[Falling\] start at depth (\d+)")
NEED = re.compile(r"\b(health_falling|health_deficit)=([0-9.]+)")
HP = re.compile(r"\[Context\] .*\| hp=([0-9.]+)%")
SWIM = re.compile(r"\] Water: .*swimming=true")


def parse(path):
    events = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            t = datetime.strptime(m.group(1)[:23], "%Y-%m-%d %H:%M:%S.%f").timestamp()
            events.append((t, m.group(1)[11:19], m.group(2)))
    return events


def falls(events):
    out = []
    start_at = None
    for i, (t, clock, text) in enumerate(events):
        if FALL_START.search(text):
            start_at = t
            continue
        m = FALL_END.search(text)
        if not m:
            continue
        depth = int(m.group(1))
        fall = {"clock": clock, "t": t, "depth": depth, "start": start_at, "hf": 0.0, "hd": 0.0,
                "hp_before": None, "hp_min": None, "water": False, "pre_hurt": False}
        start_at = None
        # Backwards: the state just before the landing.
        need_seen = hp_seen = False
        for tb, _, tx in reversed(events[:i]):
            if t - tb > LOOKBACK_SEC and need_seen:
                break
            if t - tb > 60.0:
                break
            if not need_seen and "[Needs]" in tx and t - tb <= LOOKBACK_SEC:
                need_seen = True
                vals = dict(NEED.findall(tx))
                fall["pre_hurt"] = float(vals.get("health_falling", 0.0)) > 0.0
            if not hp_seen:
                h = HP.search(tx)
                if h:
                    hp_seen = True
                    fall["hp_before"] = float(h.group(1))
            if need_seen and hp_seen:
                break
        # Forwards: the damage (and the hp at the landing line itself).
        for tf, _, tx in events[i:]:
            if tf - t > WINDOW_SEC:
                break
            if SWIM.search(tx):
                fall["water"] = True
            if "[Needs]" in tx:
                vals = dict(NEED.findall(tx))
                fall["hf"] = max(fall["hf"], float(vals.get("health_falling", 0.0)))
                fall["hd"] = max(fall["hd"], float(vals.get("health_deficit", 0.0)))
            h = HP.search(tx)
            if h:
                v = float(h.group(1))
                fall["hp_min"] = v if fall["hp_min"] is None else min(fall["hp_min"], v)
        # A swim line just before the end line (same frame) counts too.
        for tb, _, tx in reversed(events[:i]):
            if t - tb > 0.5:
                break
            if SWIM.search(tx):
                fall["water"] = True
        out.append(fall)
    return out


def logistic(x, c, k):
    z = -k * (x - c)
    if z > 700:
        return 0.0
    return 1.0 / (1.0 + math.exp(z))


def frange(lo, hi, step):
    n = int(round((hi - lo) / step))
    return [lo + i * step for i in range(n + 1)]


def fit_least_squares(points, c_step=1.0, k_step=0.0005):
    best = None
    for c in frange(250.0, 800.0, c_step):
        for k in frange(0.002, 0.08, k_step):
            sse = sum((logistic(x, c, k) - y) ** 2 for x, y in points)
            if best is None or sse < best[0]:
                best = (sse, c, k)
    return best


def fit_likelihood(points):
    best = None
    eps = 1e-9
    for c in frange(250.0, 800.0, 1.0):
        for k in frange(0.002, 0.08, 0.0005):
            nll = 0.0
            for x, y in points:
                p = min(max(logistic(x, c, k), eps), 1.0 - eps)
                nll -= math.log(p) if y else math.log(1.0 - p)
            if best is None or nll < best[0]:
                best = (nll, c, k)
    return best


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    path = argv[1]
    exclude = set()
    bootstrap = 0
    rest = argv[2:]
    while rest:
        if rest[0] == "--exclude" and len(rest) >= 2:
            exclude.add(rest[1])
            rest = rest[2:]
        elif rest[0] == "--bootstrap" and len(rest) >= 2 and rest[1].isdigit():
            bootstrap = int(rest[1])
            rest = rest[2:]
        else:
            print(f"unknown argument: {rest[0]}", file=sys.stderr)
            return 2

    found = falls(parse(path))
    if not found:
        print("no [Falling] end lines in the log", file=sys.stderr)
        return 1

    print(f"{len(found)} falls in {path}")
    print(f"{'time':>8} {'depth':>6} {'dur s':>6} {'peak hf':>7} {'h def':>6} {'hp drop':>8}  use")
    points = []
    for f in sorted(found, key=lambda f: f["depth"]):
        dur = f"{f['t'] - f['start']:.2f}" if f["start"] is not None else "-"
        hp_drop = "-"
        if f["hp_before"] is not None and f["hp_min"] is not None:
            hp_drop = f"{max(f['hp_before'] - f['hp_min'], 0.0):.1f}%"
        why = ""
        if f["water"]:
            why = "excluded: landed in water"
        elif f["clock"] in exclude:
            why = "excluded: --exclude"
        elif f["pre_hurt"]:
            why = "excluded: health already falling"
        else:
            points.append((float(f["depth"]), f["hf"]))
        print(f"{f['clock']:>8} {f['depth']:>6} {dur:>6} {f['hf']:>7.2f} {f['hd']:>6.2f} {hp_drop:>8}  {why or 'fit'}")

    if len(points) < 4:
        print("too few falls to fit", file=sys.stderr)
        return 1

    sse, c, k = fit_least_squares(points)
    print()
    print(f"least squares, logistic to peak health_falling ({len(points)} falls): "
          f"centre {c:.0f}, slope {k:.4f} (rms {math.sqrt(sse / len(points)):.3f})")
    binary = [(x, y >= DAMAGE_MIN) for x, y in points]
    nll, cb, kb = fit_likelihood(binary)
    print(f"max likelihood, P(peak health_falling >= {DAMAGE_MIN}): centre {cb:.0f}, slope {kb:.4f} "
          f"(neg log-lik {nll:.2f})")
    print()
    print(f"{'drop':>6} {'severity fit':>12} {'P(damage)':>9}")
    for x in (200, 250, 300, 330, 350, 400, 450, 490, 500, 550, 600, 700, 800):
        print(f"{x:>6} {logistic(x, c, k):>12.2f} {logistic(x, cb, kb):>9.2f}")

    if bootstrap > 0:
        rng = random.Random(20261010)
        cs, ks = [], []
        for _ in range(bootstrap):
            sample = [rng.choice(points) for _ in points]
            _, bc, bk = fit_least_squares(sample, 5.0, 0.001)
            cs.append(bc)
            ks.append(bk)
        cs.sort()
        ks.sort()

        def pct(v, q):
            return v[min(len(v) - 1, int(q * len(v)))]

        print()
        print(f"bootstrap ({bootstrap} resamples), least squares: "
              f"centre p10 {pct(cs, 0.1):.0f} / p50 {pct(cs, 0.5):.0f} / p90 {pct(cs, 0.9):.0f}, "
              f"slope p10 {pct(ks, 0.1):.4f} / p50 {pct(ks, 0.5):.4f} / p90 {pct(ks, 0.9):.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

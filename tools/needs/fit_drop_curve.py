"""Fit the drop_ahead response curve from a Huginn debug log.

drop_ahead (needs.csv) is a logistic of the drop in front of the player, in
units. What it should anticipate is fall damage, so the curve is fitted to
the falls the player actually took: each `[Falling] end -- peak depth N`
line (state/StateManager_Position.cpp; N = the descent below the take-off
point, units) paired with the damage logged right after it.

The fit target, per fall, is a severity in [0, 1]. Its 0.5 is a fall costing
~7% of max health, and it saturates (0.95) near an ~11% loss:
  * measured, when the log has it (0.23.23): the `[Falling] landed -- peak
    depth N | hp A% -> B%` line written ~1 s after the end (health before
    the take-off and after the landing) gives the loss L = A - B (fraction
    of max health); severity = logistic(L; LOSS_CENTRE 0.07, LOSS_SLOPE 74).
    That mapping is calibrated on the five falls of the LoreRim session of
    2026-10-10 that had both an hp reading and a peak health_falling (2.3%
    -> 0.00, 3.4% -> 0.09, 5.7% -> 0.20, 9.8% -> 0.82, 17.2% -> 0.99; the
    mapping reads 0.03, 0.07, 0.28, 0.89, 1.00), so the two measures share a
    scale;
  * otherwise the peak health_falling: the largest `health_falling=` value in
    the `[Needs]` lines of the damage window (needs.csv health_falling: a
    logistic, c 0.03 of max health per second, of the net health loss rate;
    it saturates near 1.00 for any loss past ~11% of max).
The damage window is (fall end, fall end + WINDOW_SEC], cut short at the next
`[Falling] start` (a second fall's damage is not this one's). Also shown, not
fitted: health_deficit after (the largest `health_deficit=` in the window)
and the hp drop of the `[Context] ... hp=NN.N%` lines (the last one at or
before the fall end, within 60 s, minus the smallest one in the window).

A fall is EXCLUDED from the fit when
  * it landed in water: a `Water: ... swimming=true` line in
    [fall end - 0.5 s, fall end + WINDOW_SEC] (deep water is a safe landing,
    which the probe handles itself: core kSafeLandingDepth), or
  * other damage or healing may be mixed in, anywhere in [fall end -
    GUARD_SEC, fall end + GUARD_SEC]: a `[Needs]` line with `in_combat=`
    (combat damage credited to the fall: 2026-10-09 20:57:58) or
    `restore_pending_health=` (healing that hides the fall's loss: 20:59:33,
    21:01:21), or the `[Context]` hp rising across the window
    (healing by any other means: the last hp line of that window at least
    HP_RISE_MIN over its first), or
  * health was already falling: the last `[Needs]` line before the fall's
    end (inside LOOKBACK_SEC) carries health_falling > 0, or
  * it is listed with --exclude: HH:MM:SS, or a full date-time
    "YYYY-MM-DD HH:MM:SS" (or with a T) to pick one fall of a log that spans
    midnight or several days (e.g. a slide down a slope that measured a deep
    "fall" without a free fall).
`[Needs]` lines are rate-limited and deadbanded (needs/NeedMonitor.cpp), and
only non-zero needs are printed, so a missing health_falling reads 0.

Fits, by grid search (std-lib only; centre 250-800 units, slope
0.002-0.08):
  1. least squares of logistic(depth; c, k) to the severity (the curve
     drop_ahead uses);
  2. maximum likelihood of logistic(depth; c, k) as P(any damage), damage =
     severity >= DAMAGE_MIN (where damage starts; for reference);
  3. with --bootstrap N: the least-squares fit on N resamples of the falls
     (with replacement, fixed seed, a coarser grid: centre step 5, slope
     step 0.001), printing the 10th / 50th / 90th percentiles of centre and
     slope and how many resamples ended on the grid's edge.
An optimum on the edge of the grid is no fit -- the data did not pin the
curve down (too few damaging falls, or damage that is not the fall's) -- and
is reported with a WARNING.

Usage:
  python -I tools/needs/fit_drop_curve.py <log> [--exclude WHEN ...]
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
GUARD_SEC = 3.0
DAMAGE_MIN = 0.05
HP_RISE_MIN = 0.5  # percentage points: the window's last [Context] hp this much over its first is healing
LOSS_CENTRE = 0.07  # a measured loss of 7% of max health: severity 0.5
LOSS_SLOPE = 74.0   # 11% -> 0.95

C_LO, C_HI = 250.0, 800.0
K_LO, K_HI = 0.002, 0.08

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]\[[^\]]*\]\[\w\]: (.*)$")
FALL_END = re.compile(r"\[Falling\] end \S+ peak depth (\d+)")
FALL_START = re.compile(r"\[Falling\] start at depth (\d+)")
FALL_LANDED = re.compile(r"\[Falling\] landed \S+ peak depth (\d+) \| hp ([0-9.]+)% -> ([0-9.]+)%")
NEED = re.compile(r"\b(health_falling|health_deficit)=([0-9.]+)")
MIXED = re.compile(r"\b(in_combat|restore_pending_health)=")
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
            # (seconds, "HH:MM:SS", "YYYY-MM-DD HH:MM:SS", text)
            events.append((t, m.group(1)[11:19], m.group(1)[:19], m.group(2)))
    return events


def loss_severity(loss):
    return logistic(loss, LOSS_CENTRE, LOSS_SLOPE)


def falls(events):
    out = []
    start_at = None
    for i, (t, clock, stamp, text) in enumerate(events):
        if FALL_START.search(text):
            start_at = t
            continue
        m = FALL_END.search(text)
        if not m:
            continue
        depth = int(m.group(1))
        fall = {"clock": clock, "stamp": stamp, "t": t, "depth": depth, "start": start_at, "hf": 0.0, "hd": 0.0,
                "hp_before": None, "hp_min": None, "water": False, "pre_hurt": False, "mixed": None,
                "loss": None}
        start_at = None
        # Backwards: the state just before the landing.
        need_seen = hp_seen = False
        for tb, _, _, tx in reversed(events[:i]):
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
        # Forwards: the damage, up to WINDOW_SEC or the next fall's start.
        for tf, _, _, tx in events[i + 1:]:
            if tf - t > WINDOW_SEC or FALL_START.search(tx):
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
        # The measured loss: the landed line that follows this end (before
        # the next end; it is written ~1 s after, or at the next start).
        for tf, _, _, tx in events[i + 1:]:
            if FALL_END.search(tx) or tf - t > 10.0:
                break
            lm = FALL_LANDED.search(tx)
            if lm:
                before, after = float(lm.group(2)), float(lm.group(3))
                fall["loss"] = max(before - after, 0.0) / 100.0
                break
        # Mixed in: combat or pending healing in the guard window, or the hp rising.
        hps = []
        for tg, _, _, tx in events:
            if tg < t - GUARD_SEC:
                continue
            if tg > t + GUARD_SEC:
                break
            if fall["mixed"] is None and "[Needs]" in tx:
                mm = MIXED.search(tx)
                if mm:
                    fall["mixed"] = mm.group(1)
            h = HP.search(tx)
            if h:
                hps.append(float(h.group(1)))
        # Net over the window, not any rise: health regenerates after a hurt
        # landing too, and that must not exclude the falls that hurt.
        if fall["mixed"] is None and len(hps) >= 2 and hps[-1] >= hps[0] + HP_RISE_MIN:
            fall["mixed"] = "hp rising"
        # A swim line just before the end line (same frame) counts too.
        for tb, _, _, tx in reversed(events[:i]):
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


def on_edge(c, k):
    return math.isclose(c, C_LO) or math.isclose(c, C_HI) or math.isclose(k, K_LO) or math.isclose(k, K_HI)


def fit_least_squares(points, c_step=1.0, k_step=0.0005):
    best = None
    for c in frange(C_LO, C_HI, c_step):
        for k in frange(K_LO, K_HI, k_step):
            sse = sum((logistic(x, c, k) - y) ** 2 for x, y in points)
            if best is None or sse < best[0]:
                best = (sse, c, k)
    return best


def fit_likelihood(points):
    best = None
    eps = 1e-9
    for c in frange(C_LO, C_HI, 1.0):
        for k in frange(K_LO, K_HI, 0.0005):
            nll = 0.0
            for x, y in points:
                p = min(max(logistic(x, c, k), eps), 1.0 - eps)
                nll -= math.log(p) if y else math.log(1.0 - p)
            if best is None or nll < best[0]:
                best = (nll, c, k)
    return best


def edge_warning(what, c, k):
    if not on_edge(c, k):
        return
    print()
    print("!" * 78)
    print(f"WARNING: the {what} optimum (centre {c:.0f}, slope {k:.4f}) is on the EDGE of the")
    print(f"search grid (centre {C_LO:.0f}-{C_HI:.0f}, slope {K_LO}-{K_HI}): the falls do not pin the")
    print("curve down. Do not ship it. Check the table: too few damaging falls, or")
    print("damage that is not the fall's (combat, healing) left in the fit.")
    print("!" * 78)


def normalise_when(text):
    """--exclude value -> ("clock", "HH:MM:SS") or ("stamp", "YYYY-MM-DD HH:MM:SS")."""
    t = text.strip().replace("T", " ")
    if re.fullmatch(r"\d\d:\d\d:\d\d", t):
        return ("clock", t)
    if re.fullmatch(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", t):
        return ("stamp", t)
    return None


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
            when = normalise_when(rest[1])
            if when is None:
                print(f"--exclude wants HH:MM:SS or YYYY-MM-DD HH:MM:SS, not {rest[1]!r}", file=sys.stderr)
                return 2
            exclude.add(when)
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

    matched = set()
    measured = sum(1 for f in found if f["loss"] is not None)
    print(f"{len(found)} falls in {path} ({measured} with a measured hp loss)")
    print(f"{'time':>8} {'depth':>6} {'dur s':>6} {'peak hf':>7} {'h def':>6} {'hp drop':>8} {'loss':>6} "
          f"{'target':>6}  use")
    points = []
    for f in sorted(found, key=lambda f: f["depth"]):
        dur = f"{f['t'] - f['start']:.2f}" if f["start"] is not None else "-"
        hp_drop = "-"
        if f["hp_before"] is not None and f["hp_min"] is not None:
            hp_drop = f"{max(f['hp_before'] - f['hp_min'], 0.0):.1f}%"
        loss = "-" if f["loss"] is None else f"{f['loss'] * 100.0:.1f}%"
        target = f["hf"] if f["loss"] is None else loss_severity(f["loss"])
        hit = {("clock", f["clock"]), ("stamp", f["stamp"])} & exclude
        matched |= hit
        why = ""
        if f["water"]:
            why = "excluded: landed in water"
        elif hit:
            why = "excluded: --exclude"
        elif f["mixed"]:
            why = f"excluded: {f['mixed']} within {GUARD_SEC:.0f} s"
        elif f["pre_hurt"]:
            why = "excluded: health already falling"
        else:
            points.append((float(f["depth"]), target))
        print(f"{f['clock']:>8} {f['depth']:>6} {dur:>6} {f['hf']:>7.2f} {f['hd']:>6.2f} {hp_drop:>8} {loss:>6} "
              f"{target:>6.2f}  {why or ('fit (measured loss)' if f['loss'] is not None else 'fit')}")
    for kind, when in sorted(exclude - matched):
        print(f"note: --exclude {when} matched no fall", file=sys.stderr)

    if len(points) < 4:
        print("too few falls to fit", file=sys.stderr)
        return 1

    sse, c, k = fit_least_squares(points)
    print()
    print(f"least squares, logistic to the severity ({len(points)} falls): "
          f"centre {c:.0f}, slope {k:.4f} (rms {math.sqrt(sse / len(points)):.3f})")
    binary = [(x, y >= DAMAGE_MIN) for x, y in points]
    nll, cb, kb = fit_likelihood(binary)
    print(f"max likelihood, P(severity >= {DAMAGE_MIN}): centre {cb:.0f}, slope {kb:.4f} "
          f"(neg log-lik {nll:.2f})")
    edge_warning("least-squares", c, k)
    edge_warning("max-likelihood", cb, kb)
    print()
    print(f"{'drop':>6} {'severity fit':>12} {'P(damage)':>9}")
    for x in (200, 250, 300, 330, 350, 400, 450, 490, 500, 550, 600, 700, 800):
        print(f"{x:>6} {logistic(x, c, k):>12.2f} {logistic(x, cb, kb):>9.2f}")

    if bootstrap > 0:
        rng = random.Random(20261010)
        cs, ks = [], []
        edges = 0
        for _ in range(bootstrap):
            sample = [rng.choice(points) for _ in points]
            _, bc, bk = fit_least_squares(sample, 5.0, 0.001)
            cs.append(bc)
            ks.append(bk)
            edges += on_edge(bc, bk)
        cs.sort()
        ks.sort()

        def pct(v, q):
            return v[min(len(v) - 1, int(q * len(v)))]

        print()
        print(f"bootstrap ({bootstrap} resamples), least squares: "
              f"centre p10 {pct(cs, 0.1):.0f} / p50 {pct(cs, 0.5):.0f} / p90 {pct(cs, 0.9):.0f}, "
              f"slope p10 {pct(ks, 0.1):.4f} / p50 {pct(ks, 0.5):.4f} / p90 {pct(ks, 0.9):.4f}; "
              f"{edges} of {bootstrap} on the grid's edge")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

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
    GUARD_SEC, fall end + GUARD_SEC]: the player in combat there (combat
    damage credited to the fall: 2026-10-09 20:57:58), a `[Needs]` line with
    `restore_pending_health=` (healing that hides the fall's loss: 20:59:33,
    21:01:21), or the `[Context]` hp rising across the window
    (healing by any other means: the last hp line of that window at least
    HP_RISE_MIN over its first). Combat comes from the `[Pipeline] State
    transition (hash=N) -- scoring | ... Combat:OutOfCombat->InCombat ...`
    lines (pipeline/PipelineCoordinator.cpp LogStateTransition, the arrow
    U+2192; the published, debounced combat flag; one line on every change,
    so the state is known at any moment after the log's first State
    transition line, OutOfCombat before its first Combat change: the logged
    state starts value-initialised, and starts so again at every game load,
    the `Game load timestamp recorded` line: ResetCrossSaveState clears the
    last logged state, so a fight a reload ended logs no Combat change; the
    state starts over again at the `[StateManager] ResetTrackingState()`
    line, which ResetPipelineSubsystems writes right after that reset, so a
    stale Combat change a loading-screen tick logs in the ~20 ms between the
    two lines is dropped; `hg reset all` runs the same reset and writes the
    same line).
    Before that first line it falls back to
    `in_combat=` in a `[Needs]` line of the window, which is deadbanded and
    can miss a short fight. Or
  * health was already falling: the last `[Needs]` line before the fall's
    end (inside LOOKBACK_SEC) carries health_falling > 0, or
  * it was a slide, not a free fall (see SLIDES below), or
  * it is listed with --exclude: HH:MM:SS, or a full date-time
    "YYYY-MM-DD HH:MM:SS" (or with a T) to pick one fall of a log that spans
    midnight or several days.
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
A fit the data does not pin down is reported with a WARNING (do not ship
it), for any of:
  * an optimum on, or within EDGE_STEPS grid steps of, the grid's edge;
  * least squares rms under RMS_PERFECT: the damaging and harmless falls
    separate perfectly, so any step between them fits and the slope only
    runs to the grid's limit (2026-10-10 13:14 launch: 446 / 0.079, rms
    0.000, from 3 damaging falls of 8);
  * fewer than MIN_DAMAGING fitted falls with severity >= DAMAGE_MIN.

SLIDES. The sensor measures depth below the take-off point and re-anchors it
on a grounded poll (core/FallTracker.h), so a slide down a slope, or a run
of hops whose touches fall between polls, kept adding depth (until 0.23.29
taught the tracker this same test) without the free fall that fall damage
counts. A free fall from rest at the take-off height reaches depth d after
sqrt(2 d / G), and any downward speed at take-off (a jump coming back down)
only shortens that, so a free fall covers the gate depth d0 (the `[Falling]
start at depth d0` line) to the peak d1 (the `[Falling] end` line) in at most
    t_ff = sqrt(2 / G) * (sqrt(d1) - sqrt(d0)).
The logged duration (start line to end line; in game time where logged, see
THE CLOCK) exceeds the time between
those two depth samples by the poll that saw the landing (the end line is
the first grounded poll, d1 the poll before), and each sample's Z may be up
to a frame older than its line. A fall is a slide when
    duration - POLL_MAX_SEC - 2 * FRAME_SEC > t_ff.
G is Havok's 9.8 m/s^2 at Skyrim's 69.99 units per metre: 686 units/s^2.
The logged falls descend faster than that (the script prints their own
gravity, the median over the long ones: about 950 in the 2026-10-10 logs; the
5471-unit sea fall alone about 960), and a smaller G only lengthens t_ff, so
the test errs toward keeping a fall.

THE CLOCK. Gravity acts in game time, and the game slows its time: Wheeler
while a wheel is open (SlowTimeScale; LoreRim 0.2), the Slow Time shout, bow
zoom, LoreRim's "time slows when detected". The wall-clock duration of a fall
seen in slow motion is up to five times its game time, so it reads as a
slide. From 0.23.29 the end line logs the game time since the start line
(`game time N s`), and the test judges that. On an older log, a fall with
Wheeler open at any moment between its start and end lines (the
`WheelStateChanged: ... isOpen=` lines) is not judged: it stays in the fit
and the table marks it "unjudgeable". The other slow-time sources leave no
line in an old log, so a slide verdict there is only as good as the
assumption that none was active.

In the 2026-10-10 logs every fall the fit kept took at most 0.84 of its
budget (t_ff + slack), and five took 1.41 to 3.77 times it: 12:08:36 (645
units in 1.11 s), trace-04's 18:47:11 (549 in 1.51 s, its depth growing 136
units in 1.07 s where a free fall adds ~1000) and three harmless ones.
11:32:35 (1.06 on the wall clock: 202 -> 401 units, 3.4% of health) began
with Wheeler open (opened 11:32:32.3, closed 11:32:35.373, the fall .086 to
.616): a real fall partly in slow motion, unjudgeable and kept.

Usage:
  python -I tools/needs/fit_drop_curve.py <log> [<log> ...] [--exclude WHEN ...]
                                                           [--bootstrap N]

Several logs are read one at a time (each its own combat timeline) and their
falls pooled; a fall found in two of them (a trace's copy of a HuginnLogs
file) counts once, keyed by its end line's time stamp and depth.

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
# Slides (SLIDES in the docstring): Havok's gravity in Skyrim units; the
# longest poll (a one-poll fall, start depth == peak depth, took 0.100-0.120 s
# in the 2026-10-10 logs); a frame at the 30 fps floor.
G_UNITS = 9.8 * 69.99  # units/s^2, ~686
POLL_MAX_SEC = 0.12
FRAME_SEC = 1.0 / 30.0
SLIDE_SLACK_SEC = POLL_MAX_SEC + 2.0 * FRAME_SEC
LONG_FALL_UNITS = 250  # gate-to-peak descent from which a fall's own gravity is measured

C_LO, C_HI = 250.0, 800.0
K_LO, K_HI = 0.002, 0.08
C_STEP, K_STEP = 1.0, 0.0005  # the full fits' grid
EDGE_STEPS = 2       # an optimum this many grid steps from an edge, or closer, is not pinned down
RMS_PERFECT = 0.01   # least squares rms under this: perfect separation
MIN_DAMAGING = 5     # fewer fitted falls with severity >= DAMAGE_MIN than this: too few

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]\[[^\]]*\]\[\w\]: (.*)$")
FALL_END = re.compile(r"\[Falling\] end \S+ peak depth (\d+)")
FALL_START = re.compile(r"\[Falling\] start at depth (\d+)")
# 0.23.29: the end line's time since the start line, on the game clock too.
FALL_GAME_SEC = re.compile(r"game time ([0-9.]+) s")
# Wheeler slows the game (SlowTimeScale; LoreRim 0.2) while a wheel is open:
# wheeler/WheelerClient.cpp's callback line.
WHEEL_STATE = re.compile(r"WheelStateChanged: wheel=(\d+), isOpen=(true|false)")
FALL_LANDED = re.compile(r"\[Falling\] landed \S+ peak depth (\d+) \| hp ([0-9.]+)% -> ([0-9.]+)%")
NEED = re.compile(r"\b(health_falling|health_deficit)=([0-9.]+)")
MIXED = re.compile(r"\b(in_combat|restore_pending_health)=")
STATE_LINE = re.compile(r"\[Pipeline\] State transition \(hash=\d+\)")
COMBAT_CHANGE = re.compile(r"\bCombat:(\w+)→(\w+)")
# Main.cpp InitializeGameSystems, at every kNewGame / kPostLoadGame: the
# logged combat state starts over (PipelineCoordinator::ResetCrossSaveState).
GAME_LOAD = re.compile(r"Game load timestamp recorded \(")
# Globals.cpp ResetPipelineSubsystems, right after ResetCrossSaveState: a
# load's step 8 (~20 ms after GAME_LOAD), and `hg reset all`.
STATE_RESET = re.compile(r"\[StateManager\] ResetTrackingState\(\)")
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


def combat_timeline(events):
    """(time of the first State transition line or None, [(t, in_combat after)]).
    A game load is a change to OutOfCombat: the logged state starts over there,
    and again at the reset line after it, which drops a stale change logged
    between the two. (The reset is not under the update loop's lock, so a
    tick already inside the pipeline can still log one stale change after
    it; PipelineCoordinator.h ResetCrossSaveState. Not handled.)"""
    first = None
    changes = []
    for t, _, _, text in events:
        if GAME_LOAD.search(text) or STATE_RESET.search(text):
            changes.append((t, False))
            continue
        if not STATE_LINE.search(text):
            continue
        if first is None:
            first = t
        m = COMBAT_CHANGE.search(text)
        if m:
            changes.append((t, m.group(2) == "InCombat"))
    return first, changes


def combat_in_window(timeline, lo, hi):
    """In combat at any moment of [lo, hi] per the State transition lines:
    True / False, or None when the window starts before the log's first one."""
    first, changes = timeline
    if first is None or lo < first:
        return None
    state = False  # the logged state starts value-initialised: OutOfCombat
    for t, in_combat in changes:
        if t <= lo:
            state = in_combat
            continue
        if t > hi:
            break
        if in_combat:
            return True
    return state


def wheel_timeline(events):
    """[(t, Wheeler open after)]: its slow time is on while it is. Wheeler is
    one menu showing one wheel at a time, and it switches wheels while open
    (the logs open on wheel=2 and close on wheel=0), so the state is the last
    line's isOpen whatever its wheel. A game load closes it."""
    changes = []
    for t, _, _, text in events:
        if GAME_LOAD.search(text):
            changes.append((t, False))
            continue
        m = WHEEL_STATE.search(text)
        if m:
            changes.append((t, m.group(2) == "true"))
    return changes


def slow_time_in_window(changes, lo, hi):
    """A wheel open (slow time) at any moment of [lo, hi]."""
    state = False
    for t, is_open in changes:
        if t <= lo:
            state = is_open
            continue
        if t > hi:
            break
        return True  # a change inside the window: slow time on one side of it
    return state


def free_fall_sec(d0, d1, g=G_UNITS):
    """Longest a free fall takes from depth d0 to d1 below the take-off (from rest)."""
    return math.sqrt(2.0 / g) * (math.sqrt(max(d1, 0)) - math.sqrt(max(d0, 0)))


def judged_sec(fall):
    """The duration the slide test judges: the game time when the end line
    logs it (0.23.29), else the wall time between the lines."""
    if fall["game_sec"] is not None:
        return fall["game_sec"]
    return fall["t"] - fall["start"]


def slide_budget(fall):
    """The judged duration over the longest a free fall could take (t_ff +
    SLIDE_SLACK_SEC): over 1 is a slide. None without a start line."""
    if fall["start"] is None:
        return None
    return judged_sec(fall) / (free_fall_sec(fall["start_depth"], fall["depth"]) + SLIDE_SLACK_SEC)


def own_gravity(fall):
    """The gravity a fall's own timing implies (from rest, d0 -> d1 over the
    duration less the landing poll), or None for a short or unstarted one."""
    if fall["start"] is None or fall["depth"] - fall["start_depth"] < LONG_FALL_UNITS:
        return None
    if fall["slow_time"]:
        return None
    sec = judged_sec(fall) - POLL_MAX_SEC
    if sec <= 0:
        return None
    return 2.0 * (math.sqrt(fall["depth"]) - math.sqrt(fall["start_depth"])) ** 2 / sec ** 2


def falls(events):
    out = []
    start_at = None
    timeline = combat_timeline(events)
    wheels = wheel_timeline(events)
    for i, (t, clock, stamp, text) in enumerate(events):
        ms = FALL_START.search(text)
        if ms:
            start_at = (t, int(ms.group(1)))
            continue
        m = FALL_END.search(text)
        if not m:
            continue
        depth = int(m.group(1))
        fall = {"clock": clock, "stamp": stamp, "t": t, "depth": depth,
                "start": start_at[0] if start_at else None, "start_depth": start_at[1] if start_at else None,
                "hf": 0.0, "hd": 0.0, "hp_before": None, "hp_min": None, "water": False, "pre_hurt": False,
                "mixed": None, "loss": None}
        mg = FALL_GAME_SEC.search(text)
        fall["game_sec"] = float(mg.group(1)) if mg and fall["start"] is not None else None
        # Slow time inside the fall, on a log without the game time: the
        # wall-clock duration says nothing about the descent.
        fall["slow_time"] = (fall["game_sec"] is None and fall["start"] is not None
                             and slow_time_in_window(wheels, fall["start"], t))
        fall["slide"] = slide_budget(fall)
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
        # Combat from the [Pipeline] transitions when they cover the window,
        # else from the [Needs] in_combat= lines.
        combat = combat_in_window(timeline, t - GUARD_SEC, t + GUARD_SEC)
        fall["combat_source"] = "[Needs]" if combat is None else "[Pipeline]"
        if combat:
            fall["mixed"] = "combat ([Pipeline])"
        hps = []
        for tg, _, _, tx in events:
            if tg < t - GUARD_SEC:
                continue
            if tg > t + GUARD_SEC:
                break
            if fall["mixed"] is None and "[Needs]" in tx:
                for mm in MIXED.finditer(tx):
                    if mm.group(1) == "in_combat" and combat is not None:
                        continue  # the transitions know combat here
                    fall["mixed"] = mm.group(1) + (" ([Needs])" if mm.group(1) == "in_combat" else "")
                    break
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


def near_edge(c, k, c_step=C_STEP, k_step=K_STEP):
    """On the grid's edge or within EDGE_STEPS of its steps (a small epsilon for float grids)."""
    eps = 1e-9
    return (c - C_LO <= EDGE_STEPS * c_step + eps or C_HI - c <= EDGE_STEPS * c_step + eps
            or k - K_LO <= EDGE_STEPS * k_step + eps or K_HI - k <= EDGE_STEPS * k_step + eps)


def fit_least_squares(points, c_step=C_STEP, k_step=K_STEP):
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
    for c in frange(C_LO, C_HI, C_STEP):
        for k in frange(K_LO, K_HI, K_STEP):
            nll = 0.0
            for x, y in points:
                p = min(max(logistic(x, c, k), eps), 1.0 - eps)
                nll -= math.log(p) if y else math.log(1.0 - p)
            if best is None or nll < best[0]:
                best = (nll, c, k)
    return best


def fit_warnings(c, k, rms=None, damaging=None):
    """Why a fit is not pinned down by the falls (empty: no reason found)."""
    why = []
    if near_edge(c, k):
        where = "on" if on_edge(c, k) else f"within {EDGE_STEPS} grid steps of"
        why.append(f"the optimum is {where} the EDGE of the search grid (centre {C_LO:.0f}-{C_HI:.0f}, "
                   f"step {C_STEP:g}; slope {K_LO}-{K_HI}, step {K_STEP:g})")
    if rms is not None and rms < RMS_PERFECT:
        why.append(f"rms {rms:.3f} (< {RMS_PERFECT}): the damaging and harmless falls separate perfectly, "
                   "so any step between them fits and the slope is not measured")
    if damaging is not None and damaging < MIN_DAMAGING:
        why.append(f"only {damaging} fitted fall(s) did damage (severity >= {DAMAGE_MIN}; "
                   f"at least {MIN_DAMAGING} wanted)")
    return why


def print_warning(what, c, k, why):
    if not why:
        return
    print()
    print("!" * 78)
    print(f"WARNING: the {what} fit (centre {c:.0f}, slope {k:.4f}) is not pinned down by the")
    print("falls. Do not ship it.")
    for w in why:
        print(f"  * {w}")
    print("Check the table: too few damaging falls, or damage that is not the fall's")
    print("(combat, healing) left in the fit.")
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
    paths = []
    exclude = set()
    bootstrap = 0
    rest = argv[1:]
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
        elif not rest[0].startswith("--"):
            paths.append(rest[0])
            rest = rest[1:]
        else:
            print(f"unknown argument: {rest[0]}", file=sys.stderr)
            return 2
    if not paths:
        print("no log given", file=sys.stderr)
        return 2

    # Each log on its own (its own combat timeline); a fall two logs share
    # (a trace's copy of a HuginnLogs file) counts once.
    found = []
    seen = set()
    duplicates = 0
    for path in paths:
        for f in falls(parse(path)):
            key = (f["t"], f["depth"])
            if key in seen:
                duplicates += 1
                continue
            seen.add(key)
            found.append(f)
    if not found:
        print("no [Falling] end lines in the log(s)", file=sys.stderr)
        return 1

    matched = set()
    measured = sum(1 for f in found if f["loss"] is not None)
    from_pipeline = sum(1 for f in found if f["combat_source"] == "[Pipeline]")
    where = paths[0] if len(paths) == 1 else f"{len(paths)} logs"
    dup = f", {duplicates} duplicate(s) dropped" if duplicates else ""
    print(f"{len(found)} falls in {where}{dup} ({measured} with a measured hp loss; combat known from the "
          f"[Pipeline] transitions for {from_pipeline}, from [Needs] in_combat= for {len(found) - from_pipeline})")
    gs = sorted(g for g in (own_gravity(f) for f in found) if g is not None)
    if gs:
        print(f"the falls' own gravity (from rest, gate to peak, the {len(gs)} falls of {LONG_FALL_UNITS}+ units): "
              f"median {gs[len(gs) // 2]:.0f} units/s^2; the slide test uses {G_UNITS:.0f} "
              f"(slack {SLIDE_SLACK_SEC:.3f} s)")
    print("dur s: the judged duration, start line to end line (g: the game time the end line logs, "
          "0.23.29; else the wall time); budget ?: a Wheeler wheel (slow time) was open inside the fall")
    print(f"{'time':>8} {'depth':>6} {'dur s':>6} {'budget':>6} {'peak hf':>7} {'h def':>6} {'hp drop':>8} "
          f"{'loss':>6} {'target':>6}  use")
    points = []
    slides = []
    unjudged = []
    for f in sorted(found, key=lambda f: f["depth"]):
        dur = "-"
        if f["start"] is not None:
            dur = f"{judged_sec(f):.2f}" + ("g" if f["game_sec"] is not None else "")
        budget = "-" if f["slide"] is None else f"{f['slide']:.2f}" + ("?" if f["slow_time"] else "")
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
        elif f["slide"] is not None and f["slide"] > 1.0 and not f["slow_time"]:
            why = "excluded: slide, not a free fall"
            slides.append(f)
        elif f["mixed"]:
            why = f"excluded: {f['mixed']} within {GUARD_SEC:.0f} s"
        elif f["pre_hurt"]:
            why = "excluded: health already falling"
        else:
            points.append((float(f["depth"]), target))
        if f["slow_time"] and f["slide"] is not None and f["slide"] > 1.0:
            unjudged.append(f)
            why = (why or ("fit (measured loss)" if f["loss"] is not None else "fit")) + \
                "; slide test unjudgeable (a wheel's slow time)"
        print(f"{f['clock']:>8} {f['depth']:>6} {dur:>6} {budget:>6} {f['hf']:>7.2f} {f['hd']:>6.2f} {hp_drop:>8} "
              f"{loss:>6} {target:>6.2f}  {why or ('fit (measured loss)' if f['loss'] is not None else 'fit')}")
    for kind, when in sorted(exclude - matched):
        print(f"note: --exclude {when} matched no fall", file=sys.stderr)
    if slides:
        print()
        print(f"slides (budget = duration / (free fall from rest d0 -> d1 at {G_UNITS:.0f} units/s^2 "
              f"+ {SLIDE_SLACK_SEC:.3f} s); over 1 is slower than any free fall):")
        for f in sorted(slides, key=lambda f: f["t"]):
            ff = free_fall_sec(f["start_depth"], f["depth"])
            clock = "game" if f["game_sec"] is not None else "wall"
            print(f"  {f['stamp']}  {f['start_depth']} -> {f['depth']} units in {judged_sec(f):.2f} s ({clock}); "
                  f"a free fall takes at most {ff:.2f} + {SLIDE_SLACK_SEC:.2f} s (budget {f['slide']:.2f})")
    if unjudged:
        print()
        print("slower than a free fall on the wall clock, but a Wheeler wheel (slow time) was open inside the "
              "fall and the log has no game time: unjudgeable, not rejected:")
        for f in sorted(unjudged, key=lambda f: f["t"]):
            print(f"  {f['stamp']}  {f['start_depth']} -> {f['depth']} units in {judged_sec(f):.2f} s wall "
                  f"(budget {f['slide']:.2f})")

    if len(points) < 4:
        print("too few falls to fit", file=sys.stderr)
        return 1

    sse, c, k = fit_least_squares(points)
    rms = math.sqrt(sse / len(points))
    damaging = sum(1 for _, y in points if y >= DAMAGE_MIN)
    print()
    print(f"least squares, logistic to the severity ({len(points)} falls, {damaging} damaging): "
          f"centre {c:.0f}, slope {k:.4f} (rms {rms:.3f})")
    binary = [(x, y >= DAMAGE_MIN) for x, y in points]
    nll, cb, kb = fit_likelihood(binary)
    print(f"max likelihood, P(severity >= {DAMAGE_MIN}): centre {cb:.0f}, slope {kb:.4f} "
          f"(neg log-lik {nll:.2f})")
    print_warning("least-squares", c, k, fit_warnings(c, k, rms, damaging))
    print_warning("max-likelihood", cb, kb, fit_warnings(cb, kb, None, damaging))
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

# Long-Play Soak Testing

Endurance testing for Huginn over a 20–50 hr playthrough, played in 1–6 hr
bursts. This is **not** feature QA — it answers the questions that only appear
with time:

- **Does it hold up?** No crashes, no unbounded growth (memory, cosave, learned
  items), no performance drift over a long session.
- **Does it actually work?** Over hours of real play, does Huginn surface the
  item the player actually reaches for? (Primary goal for this pass.)
- **What's the profile shape?** Per-tick cost distribution and its tails at
  hour 1 vs hour 30.

> We do **not** test every feature one by one. We exercise the whole system
> continuously and watch aggregate signals. The only checklist is the
> **context-coverage** list below — making sure natural play hits every code
> path so none goes cold for 50 hours.

---

## One tool ≠ one run: three purposes, three builds

Running Tracy + the VSCode debugger + debug logs **simultaneously destroys the
performance profile** you're trying to measure. Split runs by purpose:

| Run type | Build | Log level | Debugger | Measures |
|----------|-------|-----------|----------|----------|
| **Perf/soak** | Release + `-DHuginn_TRACY=ON` | `info` | no | true per-tick profile, drift over hours |
| **Behavior/soak** | Debug | `debug` | attached, **no breakpoints** | recommendation correctness; debugger = crash catcher |
| **Repro** | Debug | `debug` | breakpoints | chase a specific bug a soak surfaced |

The debugger's role in a soak is to **catch the exception with a live
callstack**, not to step — you can't play 6 hours stopped on a breakpoint.

Build commands:

```sh
# Behavior soak (Debug)
cmake --preset vs2022-windows && cmake --build build --config Debug

# Perf soak (Release + Tracy)
cmake --preset vs2022-windows -DHuginn_TRACY=ON && cmake --build build --config Release
```

Log path: `C:\Users\psuba\Documents\My Games\Skyrim.INI\SKSE\`
— `_Huginn_Debug.log` (Debug) or `Huginn.log` (Release).
`basic_file_sink` truncates on launch (no rotation) — **copy the log out
before relaunching** or you lose the previous burst.

---

## The [Soak] heartbeat (v0.18.x)

Every 5 min (`Config::SOAK_HEARTBEAT_INTERVAL_MS`) the plugin emits one `info`
summary line — this is the spine of the analysis. Present in **both** Debug and
Release:

```
[Soak] up=1h23m05s | equips hit=12 near=4 miss=6 novel=8 accept=34% skipped=0 | recompute=612/42100 ticks override=14 | learn items=137 trains=4820 | tick avg=0.041 peak=0.912 ms
```

| Field | Meaning | What to watch |
|-------|---------|---------------|
| `accept=%` | of external equips Huginn attributed, how many it had **displayed** (`hit`) | **the recommendation-quality number.** Should trend up as learning warms; a flat-low accept% is the signal to investigate |
| `hit/near/miss/novel` | equip attribution buckets (E / C+D / B / A) | rising `novel` = player keeps reaching for things Huginn never scores as a candidate |
| `skipped=N (wheel/stale/spam/off)` | equips that a filter caught BEFORE attribution, so they never entered the buckets | **read this whenever `accept=n/a`.** `skipped=0` = nothing was equipped. A large `wheel=` = you played through the wheel and produced no acceptance data at all (v0.19.1+) |
| `recompute/ticks` | pipeline recomputes vs total ticks | very high ratio = state hashing thrashing (churn); near-zero = pipeline may be stuck skipping |
| `override` | recomputes where a safety override took top slot | sanity-check against how often you actually hit low-health/charge/drowning |
| `learn items/trains` | FQL learned-item count + total train count | **items must plateau, not climb linearly** across 50 hr — linear climb = unbounded weight table |
| `tick avg/peak` | per-tick cost this window | avg < 0.1 ms target; **peak stable across bursts** — a peak that grows hour-over-hour is the interesting bug |

Attribution buckets come from `ExternalEquipLearner` cases: **E** displayed
current page (hit), **D** displayed other page + **C** near-miss (near), **B**
low-ranked candidate (miss), **A** not a candidate (novel). Equips made through
Huginn's own wheel, or while the cache is stale, are excluded from the
denominator by design.

> **accept% ONLY counts equips made outside Huginn — so manual equips are a
> test requirement, not a preference.** Grading a wheel pick would ask whether
> Huginn predicted the item the player chose from Huginn's own recommendation
> list; the wheel-open filter exists to keep that circularity out. The
> consequence is that a burst played entirely through the wheel and hotkeys
> yields **no recommendation-quality signal whatsoever**, however long it runs.
> Observed 2026-08-26: a 44-minute session reported `accept=n/a` in every
> window while 21 external-equip events fired and were all filtered as
> wheel-open. Before v0.19.1 that was indistinguishable from an idle session;
> `skipped=` now makes it visible, but visibility is not data — the equips
> still have to happen.

### Tracy plots (Release+Tracy runs)

Charted time series for eyeballing drift/leaks directly in the profiler:
`Huginn/Candidates`, `Huginn/Displayed` (per recompute), `Huginn/FQL Items`,
`Huginn/Accept %` (per heartbeat). Plus the existing per-tick zones and the
`FrameMark` at `UpdateHandler.cpp` (one Tracy "frame" = one 100 ms Huginn tick,
**not** a render frame — the FPS graph is ~10 Hz by design).

---

## Context-coverage checklist

Over the full playthrough, deliberately spend real time in each so no pipeline
path stays cold. Tick these off across bursts, not per session:

- [ ] Open combat — melee
- [ ] Open combat — archery (weapon charge, ammo)
- [ ] Open combat — magic-heavy (magicka pressure)
- [ ] Sneak / stealth
- [ ] Dungeon exploration
- [ ] Town / social (no threat)
- [ ] Resource-critical — low health
- [ ] Resource-critical — low magicka / stamina
- [ ] Environmental — cold / water / darkness
- [ ] Override triggers — urgent potion, weapon charge < 25%, soul gem, drowning
- [ ] Multi-page cycling under load
- [ ] Wheeler open/close during combat (if using Wheeler)
- [ ] **Manual equips — inventory menu, favorites menu, and vanilla hotkeys.**
      The ONLY source of accept% data (see the heartbeat note above). Spread
      them across contexts rather than batching: an attribution is scored
      against whatever Huginn was showing at that moment, so twenty equips
      standing still in town measure one state twenty times
- [ ] Manual equips of things Huginn is NOT showing — the `novel`/`miss`
      buckets only fill if you sometimes reach past the recommendations

---

## Per-burst protocol

**Before:**
1. Note build variant, git hash, and the save file.
2. Verify build provenance (log line 1 stamps `vX (githash) [DEBUG/RELEASE][TRACY]`;
   the git hash bakes from HEAD at configure time, so confirm against the branch
   you mean to test — a stale DLL can pass as a branch test).
3. Copy out / clear the previous log.

**During:** play naturally, but **equip manually often enough to feed accept%**
— inventory, favorites, and vanilla hotkeys, in every context on the coverage
list. Playing purely through the wheel voids the recommendation-quality half of
the run. Only jot **timestamps** of anything that felt wrong — a wrong
recommendation, widget stutter, a freeze. Don't narrate; the heartbeat and logs
carry the rest.

**After, capture:**
- [ ] The log (`_Huginn_Debug.log` / `Huginn.log`)
- [ ] The `.tracy` capture (Release+Tracy runs)
- [ ] Any SKSE crash log (CrashLogger / NetScriptFramework)
- [ ] Cosave size (`.skse` next to the save)
- [ ] `hg status` output at end of session

---

## Pass / fail signals

| Signal | Pass | Investigate |
|--------|------|-------------|
| Crashes | zero | any — correlate crash-log timestamp with Huginn log tail |
| Memory (Tracy) | flat / bounded | rising slope over hours → Tracy memory view |
| `learn items` | plateaus | linear climb over 50 hr → unbounded weight table |
| Cosave size | plateaus | grows every save without bound |
| `tick avg` | < 0.1 ms | sustained rise across bursts |
| `tick peak` | stable across bursts | grows hour-over-hour |
| `accept %` | trends up, stabilizes | flat-low, or falling as learning accrues |
| `accept %` reported at all | populated in most windows | `n/a` with a large `skipped=(wheel=…)` means the burst produced no signal — not a pass, a **void run** for this metric |
| Log warn/error rate | → ~0 / hr | repeated same-site errors |

---

## Cross-burst analysis

Diff the `[Soak]` heartbeat lines from hour 1 vs hour 30 — that comparison *is*
the "does it hold up over 20–50 hr" answer. In Tracy, use the compare view on an
early vs late `.tracy` capture to see whether the per-tick zone distribution
shifted.

Quick log triage:

```sh
# All heartbeats from a burst
grep '\[Soak\]' _Huginn_Debug.log

# Log-spam by source site (should stay ~10 lines/sec)
grep -oE '\[[A-Za-z_]+\.(cpp|h)[ ]*:[0-9]+' _Huginn_Debug.log | sort | uniq -c | sort -rn | head

# Warnings / errors
grep -E '\]\[(warning|error|critical)\]' _Huginn_Debug.log | sort | uniq -c | sort -rn
```

---

## Future refinements (not in v0.18.x)

- Fold **consumption** events (potions/scrolls drunk) into accept% — currently
  only external equips are attributed; consumption rewards fire on a separate
  path without hit/miss classification. **Must exclude wheel-initiated
  consumption** or it reintroduces the circularity the wheel-open filter exists
  to prevent: `src=Wheeler` and `src=Hotkey` rewards are Huginn-caused and are
  not evidence Huginn predicted anything. Only a potion drunk from the vanilla
  inventory/favorites UI is attributable. On the 2026-08-26 session that would
  have added 5 of 50 reward events, not 50.
- Per-context accept% (accept rate split by combat / exploration / etc.).
- Slot-churn metric (displayed-set change rate) to quantify widget thrash
  independent of state transitions.

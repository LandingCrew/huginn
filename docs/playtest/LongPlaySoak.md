# Long-Play Soak Testing

Runs so far: [Soak-2026-10-LoreRim.md](Soak-2026-10-LoreRim.md) (9.1 h,
v0.22.11-0.22.14, the Phase 3 baseline).

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
| `skipped=N (input/stale/off)` | outside equips that a filter caught BEFORE attribution, so they never entered the buckets | **read this whenever `accept=n/a`.** `skipped=0` = nothing was equipped outside Huginn. `input=` counts consumables used with no player input behind them -- a script or another mod drinking for the player (engine equips such as the quiver refilling are not counted) (v0.22.9; before that the fields were `wheel/stale/spam/off`) |
| `recompute/ticks` | pipeline recomputes vs total ticks | very high ratio = state hashing thrashing (churn); near-zero = pipeline may be stuck skipping |
| `override` | recomputes where a safety override took top slot | sanity-check against how often you actually hit low-health/charge/drowning |
| `learn items/trains` | learned-item count + total train count | **items must plateau, not climb linearly** across 50 hr — linear climb = unbounded weight table |
| `tick avg/peak` | per-tick cost this window | avg < 0.1 ms target; **peak stable across bursts** — a peak that grows hour-over-hour is the interesting bug |
| `goals reachIns=N (candidate=M)` | **objective metric 1** (v0.22.8): every attributed external equip — the player went past Huginn to the inventory/favourites. `candidate` = the ones Huginn had scored and could have offered (hit+near+miss) | should fall over a run. `candidate` is the fixable part; the rest (`novel`) is a coverage gap |
| `presses=N (regular= labeled= offPage=)` | **objective metric 2**: slot activations through Huginn keys or a Huginn wheel. `labeled` = a classified slot (Heal, Potion...), `offPage` = any page past the first (overlaps both) | the end state is one page of Regular slots, so `labeled`+`offPage` are workarounds and should trend to zero |
| `pageFlips` | page changes that actually changed the page (keys, Wheeler, `hg page`) | the "flip to Kit" count; also a workaround |
| `slotChurn=N peak5s=… (causes) ratio(…)` | visible slot changes this window: by cause (`expired` = a lock ran out and a challenger beat the hold margin, `remembrance`, `override`, `wildcard`...), the worst single slot in 5 s, and how much challengers won by (v0.21.19) | `expired` with ratios under ~1.5 is near-ties trading places -- the hold margin's job |
| `tenure(<1s 1-3s 3-10s 10-30s 30s-2m >=2m)` | **churn speed** (v0.23.0): how long the item each change replaced had been on its slot. Page switches excluded | the target to optimise slot stability against: short tenures are juggling the player can see |
| `pressAge(<0.5s 0.5-1.5s 1.5-5s 5-30s >=30s)` | how long a pressed key had held its item (v0.23.0), Huginn keys and the shown page's wheel | a young key is a **missed** press (aimed at what was there a moment ago) or a **relevant** one (taken at once). Each press under 5 s logs `[KeyAge] page P key K pressed N s after it changed (was '…')`; the `[Selection]` outcome beside it -- confirmed, or swapped back -- tells which |

Attribution buckets come from `ExternalEquipLearner` cases: **E** displayed
current page (hit), **D** displayed other page + **C** near-miss (near), **B**
low-ranked candidate (miss), **A** not a candidate (novel). Equips made through
Huginn's own keys or wheel, or while the cache is stale, are excluded from the
denominator by design. Since v0.22.9, picks off the player's OWN Wheeler wheels
are outside selections and DO count (they were filtered as "wheel open" before).

> **accept% ONLY counts equips made outside Huginn.** Grading a Huginn key or
> Huginn-wheel pick would ask whether Huginn predicted the item the player chose
> from Huginn's own recommendation list, so those never enter it. A burst played
> entirely through Huginn's keys and wheel yields **no accept% signal**, however
> long it runs. (Before v0.22.9 the player's own wheels were filtered too: a
> 44-minute session on 2026-08-26 reported `accept=n/a` throughout while 21
> external-equip events were all skipped as wheel-open.)

### Tracy plots (Release+Tracy runs)

Charted time series for eyeballing drift/leaks directly in the profiler:
`Huginn/Candidates`, `Huginn/Displayed` (per recompute), `Huginn/Learner Items`,
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

**Do not stage equips.** Earlier versions of this checklist asked for manual
equips from the inventory, favourites and vanilla hotkeys -- including items
Huginn was not showing -- to feed accept%. That is wrong for this run, for two
reasons (review of the soak plan, 2026-10-02):
- **It inflates goal 1 directly.** Goal 1 *is* "did the player reach into the
  inventory or favourites for something Huginn could have offered"
  (`goals reachIns=`). Staged reach-ins count as failures Huginn did not cause.
- **It feeds the learner staged choices.** Since 0.22.9 an outside selection
  teaches exactly what a Huginn key press does (one selection path), so a
  staged pick trains the weights the run is meant to measure.

Reach into a menu when you would anyway -- that IS the measurement. A sparse
accept% from a run where Huginn's keys and wheel did the work is the good
outcome, not a void one.

---

## Per-burst protocol

**Before:**
1. Note build variant, git hash, and the save file.
2. Verify build provenance (log line 1 stamps `vX (githash) [DEBUG/RELEASE][TRACY]`;
   the git hash bakes from HEAD at configure time, so confirm against the branch
   you mean to test — a stale DLL can pass as a branch test).
3. Copy out / clear the previous log.

**During:** play naturally -- use Huginn's keys and wheel when they have what
you want, and the menus when they do not. Do not equip from menus to feed a
metric (see "Do not stage equips" above). Keep the page layout fixed for the
whole run: labeled versus regular presses (goal 2) move with the layout, not
just the recommender. Only jot **timestamps** of anything that felt wrong -- a
wrong recommendation, widget stutter, a freeze. Don't narrate; the heartbeat
and logs carry the rest.

**After, capture:**
- [ ] The log (`_Huginn_Debug.log` / `Huginn.log`). Since 0.22.10 each launch
      keeps the previous one as `HuginnLogs/<name>-<date-time>.log` (the newest
      20), so a forgotten copy-out no longer loses a session -- but copy them
      out before 20 more launches pass
- [ ] `Huginn_Selections.jsonl` (same folder) -- one record per confirmed
      selection with the whole scored list; the input for re-ranking offline.
      It is appended across sessions, instances and characters, so copy it
      rather than clear it. Every record says where it came from: `list` (the
      modlist folder: LoreRim-5, simonrim-essentails), `launch` (UTC start of
      the game launch), `gen` (the load within it) and, from 0.22.11, `char`
      (the character ID). `Huginn_AB.log` lines carry the same stamps
- [ ] At the first and last burst of the run: `hg dump weights`, `hg dump
      potions`, `hg dump scrolls` (the learner state the run started and ended
      with)
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
| `accept %` reported at all | sparse is fine | `n/a` with nothing skipped means no outside selections -- the GOOD case for goal 1 if `goals presses=` shows Huginn doing the work. Only worrying when presses are low too (Huginn unused) |
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

- ~~Fold consumption into accept%~~ -- done in 0.22.9 by the one selection
  path: a potion drunk from the inventory or favourites is an outside
  selection, attributed A-E like an equip and counted once it confirms.
  Huginn-key and Huginn-wheel drinks stay out, as this item required.
- Per-context accept% (accept rate split by combat / exploration / etc.).
- Slot-churn metric (displayed-set change rate) to quantify widget thrash
  independent of state transitions.

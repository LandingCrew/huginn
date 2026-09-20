# Tracy trace history

A running log of Tracy captures so we can compare hot-path costs across builds and
verify that refactors don't regress. Newest entry first.

## How to capture

- Build with Tracy on: CMake `-DHuginn_TRACY=ON` (defines `Huginn_TRACY_ENABLED`;
  zones come from `Huginn_ZONE_NAMED` / `SCOPED_TIMER` in `src/Profiling.h`).
- Play a representative slice (combat + inventory churn), then in Tracy:
  **Statistics** panel → **Timing: Self only** → sort by **MTPC** (mean time per call).
- The load line stamps the build: `Huginn vX.Y.Z (<git-sha>) [DEBUG BUILD] [TRACY]`,
  or `… [RELEASE] [TRACY]` for a release capture. Always record that SHA — it's the
  provenance for the numbers. Caveat: the SHA bakes at CMake *configure* time from
  HEAD, so a capture built from an uncommitted tree stamps the commit *before* the
  work. Record what the tree actually contained when that matters.
- **Load a save before you stop the capture.** `OnUpdate` early-returns on
  `IsWorldLoaded` (`src/UpdateLoop.cpp:510`), so a capture taken at the main menu
  or on a load screen contains exactly one zone and is worthless. See §4 of the
  profiling guide.

## How to read these numbers

- **Check the build flavour on each entry before comparing.** Entries dated
  2026-07-25 and earlier are **DEBUG + Tracy**: absolute times are inflated vs. a
  release build (no inlining/opt, plus Tracy capture overhead), so use them for
  **relative ranking and cross-build comparison**, never as absolute frame-budget
  claims. The 2026-09-19 entry is the first **RELEASE + Tracy** capture and *is*
  quotable as an absolute cost — but it is ~40x cheaper per call than the Debug
  entries, so **never rank a Debug zone against a Release one.** Compare within a
  flavour only.
- **MTPC** = mean time per call = per-invocation cost → what causes a visible *spike*.
- **Total time** = MTPC × Counts over the capture → cumulative CPU. A big total with a
  huge Count (e.g. a poll at ~1,941 ticks) is spread across every tick on the ~100 ms
  update budget, so it's cheap *per tick* even when the total looks large.
- Accessors on the registries are intentionally **un-zoned** — if you see no
  `QueryTopK`/`FindBest`/`Get*` zones, that's expected, not missing data.

## Entry template

```
## YYYY-MM-DD — <build sha> — <one-line context>
- Session: <exe @ capture time>, Tracy <ver>, <frames>, ~<program time>, <mem>
- Save: <inventory scale — items/spells/weapons/scrolls/ammo>
- Frame sampled: <ms> (<fps>), CPU ~<%>
- Notes: DEBUG+TRACY (relative only)

| Zone | MTPC | Count | Total | Note |
Top hot zones + analysis + finding mapping.
```

---

## 2026-09-19 (20:46) — `f5fa110` + uncommitted (v0.20.23) — MATCHED quiet-town capture; closes the ally question

- Session: SkyrimSE.exe @ 2026-09-19 20:46:43, Tracy 0.14.1, 74,949 frames,
  ~9:02 program time, 83.87 MB (0.13%).
- Notes: **DEBUG + TRACY (relative only)**. Quiet town, no combat — the scene the
  20:42 entry could not provide. 41 of 41 zones.

### Result: ally hysteresis works, and does not finish the job

Two quiet-town debug logs, same metric, same scene type:

| | Before (20:20, 266 s) | After (20:46, 565 s) | |
|---|---|---|---|
| `Ally:` transitions | 12 | 11 | |
| rate | 0.0451/s | **0.0195/s** | **-57%** |
| tightest gap | **0.21 s** (4 inside 1.1 s) | 0.31 s (2 pairs) | burst gone |

The pathological burst — four transitions in 1.1 s, add and prune fighting over
the same 512 threshold — does not recur. But **two 0.31 s pairs survive**
(20:52:53.014→.324, 20:55:09.174→.487), which the distance band cannot explain
on its own: crossing the 128-unit release margin in 0.31 s needs ~413 units/s,
about a sprint. So either a running NPC, or a cause that is not distance at all
— `Get3D()` going null, the actor leaving `highActorHandles`, hostility
flickering. Distance hysteresis cannot cover those.

A **time-based hold** would: keep an ally present for N ms after it was last
seen, the way `CrosshairHysteresis::PERSISTENCE_TIMEOUT_SEC` already does for
the crosshair target. Cheaper still, given `allyStatus` has no consumer at all
(written by `StateEvaluator.cpp:54`, read only by `GetHash`/`ToString`/the
diff): drop it from the hash until a rule needs it, and all 11 go away.

### The crosshair target is now the dominant flap

| Transition | Before (266 s) | After (565 s) |
|---|---|---|
| `Dist:Ranged↔Melee, Target:None↔Humanoid` | 6 (32%) | **34 (74%)** |
| `Ally:` | 12 (63%) | 11 (24%) |
| total | 19 | 46 |

The crosshair rate rose 2.7x (0.023/s → 0.060/s). That is **player camera
behaviour, not a regression** — the primary target is whatever the crosshair is
on (`StateManager_Targets.cpp`, Priority 1, no hostility filter) with a 0.3 s
sticky window, so sweeping the view across townspeople produces exactly this.
Recorded because it relocates the remaining churn: the ally sensor is no longer
where the work is.

### Skip funnel

```
OnUpdate 5,102  →  Update::PipelineCheck 1,701  →  RunPipeline 266  →  ScoreCandidates 65
```

Two thirds of ticks never reached the subsystem block (`IsWorldLoaded` false —
paused or menu; only 3 wheel events logged, so it was not the wheel). That is
another reason these Counts are not differenceable against the 20:32 and 20:42
entries, on top of the scene difference.

Full passes: 65 over 542 s = **0.12/s**, against 0.384/s at the 20:32 town
baseline. Suggestive of the ally fix paying for itself, not proof — this session
carried 2.7x the crosshair activity, which pushes the other way.

**Zero reward events all session**, so the learner latch contributed nothing
here. Which settles the other half: it cannot be inflating the pipeline count,
because it never fired.

### Cost

| Zone | MTPC | Count | Total |
|---|---|---|---|
| `Display::Wheeler` | 1.56 ms | 65 | 101.32 ms (0.02%) |
| `PollTargets` | 118.12 µs | 1,702 | **201.04 ms** (0.04%) |
| `PollPlayerMagicEffects` | 109.42 µs | 1,702 | **186.24 ms** (0.03%) |
| `OnUpdate` | 34.24 µs | 5,102 | 174.67 ms (0.03%) |
| `Inventory::DeltaScan` | 135.68 µs | 340 | 46.13 ms (0.01%) |
| `Pipeline::ScoreCandidates` | 178.18 µs | 65 | 11.58 ms |
| `Pipeline::AllocateAndLock` | 258.11 µs | 65 | 16.78 ms |
| `RunPipeline` | 80.79 µs | 266 | 21.49 ms |

Ranking unchanged: `PollTargets`, then `PollPlayerMagicEffects`. ✅ `Display::Wheeler`
MTPC drifted up to 1.56 ms on 65 calls — still the biggest per-call cost and
still critique #14, but 65 calls is not a measurement (§2.1).

---

## 2026-09-19 (20:42) — `f5fa110` + uncommitted (v0.20.23) — AFTER capture; A/B **confounded**, read the caveat

- Session: SkyrimSE.exe @ 2026-09-19 20:42:15, Tracy 0.14.1, 17,170 frames,
  ~2:19.7 program time, 81.69 MB (0.12%).
- Notes: **DEBUG + TRACY (relative only)**. Pair to the 20:32 entry below, which
  is the before.
- Zones seen: 41 of 41.

### Caveat first: the two sessions are not the same scene

The 20:32 baseline was a town with NPCs walking. This one contains **combat** —
`Enemies:None→One`, `Combat:OutOfCombat→InCombat`, and HP running down through
`High → Med → Low → Critical`. The skip funnel is activity-driven, so the Counts
below cannot be differenced against the baseline to measure the fixes.

Recorded anyway, because it answers the question the capture existed for (below)
and because a confounded pair is worth knowing about before someone quotes it.

### Skip funnel

```
OnUpdate 1,255  →  Update::PipelineCheck 893  →  RunPipeline 447  →  ScoreCandidates 68
```

vs baseline `1,307 → 1,067 → 336 → 56`. Full passes went UP (56 → 68), the
opposite of the predicted ~50, and the cause is the combat, not the changes:
**the session contains exactly one reward event** (`Reward 00013982 +8.0` at
20:43:07), so the learner latch can account for at most 1 of the 111 extra
`RunPipeline` entries.

### What it does establish

**The latch is not stuck.** That failure mode is `RunPipeline` climbing toward
`Update::PipelineCheck` (893 here) with `ScoreCandidates` behind it. Observed:
447 and 68. Ruled out, which is the only result this capture actually needed to
produce.

**The learner latch fires immediately.** From the debug log:

```
20:43:07.049  [BanditSubscriber] Reward 00013982 +8.0 (src=Hotkey)
20:43:07.049  [SlotAllocator] Candidates: 8 damage, 7 weapon, 0 buff
```

Same millisecond. Before the fix the same event waited 6.7 s and 1.6 s (20:24:25
→ 20:24:32, 20:23:55 → 20:23:57) for an unrelated hash change to release it. No
`VisualState` line follows here because the assignments did not change on that
pass — the run happened and had nothing new to publish, which is correct.

**Sub-second ally flapping is gone.** Baseline: four transitions inside 1.1 s
(gaps 0.52 / 0.21 / 0.42 s). Here the tightest gap is 1.04 s, then 1.87 s, and
the remainder run 2.6 s to 58.6 s.

### What it does NOT establish

That the ally transition RATE dropped. 8 in 167 s here against 12 in 266 s at
baseline is the same rate inside noise, and several of these are bundled with
genuine combat changes (`Enemies:None→One, Ally:Present→None,
Combat:OutOfCombat→InCombat` — an NPC turning hostile really does leave the ally
set). A combat scene is the wrong place to measure a loitering-NPC boundary.
**Open: one matched quiet-town capture.**

### Cost

| Zone | MTPC | Count | Total | vs baseline |
|---|---|---|---|---|
| `Display::Wheeler` | 1.14 ms | 68 | 77.86 ms (0.06%) | 1.24 ms / 56 / 69.51 ms |
| `Pipeline::AllocateAndLock` | 268.72 µs | 68 | 18.27 ms | 290.23 µs / 56 |
| `Pipeline::ScoreCandidates` | 182.2 µs | 68 | 12.39 ms | 199.23 µs / 56 |
| `Inventory::DeltaScan` | 139.88 µs | 179 | 25.04 ms | 143.09 µs / 213 |
| `PollTargets` | 131.48 µs | 894 | **117.54 ms** (0.08%) | 137.75 µs / 1,068 |
| `PollPlayerMagicEffects` | 107.02 µs | 894 | **95.67 ms** (0.07%) | 114.16 µs / 1,068 |
| `RunPipeline` | 57.15 µs | 447 | 25.54 ms | 65.22 µs / 336 |
| `OnUpdate` | 32.26 µs | 1,255 | 40.49 ms | 36.94 µs / 1,307 |

Every per-call figure drifted slightly DOWN. Nothing in either fix touches those
paths — it is warm-up and run-to-run noise, and is recorded here only so the next
reader does not mistake it for an optimisation. Cumulative ranking unchanged:
`PollTargets` then `PollPlayerMagicEffects`, both still Tier 3. ✅

---

## 2026-09-19 (20:32) — `f5fa110` + uncommitted — A/B BASELINE for the ally-flap and learner-latch fixes

- Session: SkyrimSE.exe @ 2026-09-19 20:32:22, Tracy 0.14.1, 17,442 frames,
  ~2:25.9 program time, 81.81 MB (0.12%).
- Save: 7 weapon stacks, 1 ammo type, 2 spells, 2 items, 0 scrolls, 2 craft apparel
  (from the `hg status` dump of the same character 20 minutes earlier).
- Notes: **DEBUG + TRACY (relative only)** — v0.20.22, the weapon instance-identity
  work with the DPS tie-break, WITHOUT the two fixes this entry is the baseline for.
  Do not rank these numbers against the RELEASE entry below; ~40x apart by flavour.
- Zones seen: **41 of 41** — the first capture to exercise every zone name.

### Why this entry exists

It is deliberately a **before**, not a ranking. §2.1 of the profiling guide puts
the bar for ranking at ~45 minutes; this is 2:26, so every MTPC here is
cold-inflated and the ordering is not trustworthy on its own. What a short
capture CAN do is give matched Counts for an A/B, provided the after-capture is
the same flavour and roughly the same length.

The two changes it baselines:

1. A learner reward now forces the next pipeline run (`NeedsForcedRun()`'s fifth
   latch). **Adds** a full pipeline pass per equip/consume event.
2. Ally range acquired at 512 and released at 640 (`RANGE_RELEASE_MARGIN`).
   **Removes** the full pipeline passes that `Ally:None<->Present` flapping was
   forcing — 12 of them in a 4½-minute debug log the same evening, 10 of which
   changed no slot at all.

### Skip funnel

```
OnUpdate 1,307  →  Update::PipelineCheck 1,067  →  RunPipeline 336  →  ScoreCandidates 56
                   (240 ticks bailed before        (68.5% skipped      (83.3% bailed at
                    the subsystem block)            at the dirty flag)  the hash compare)
```

56 full passes → 56 `AllocateAndLock` → 56 `PushDisplay` → 56 `Display::Wheeler`
and 56 `Display::Intuition`. No orphaned stages.

Far busier than the 14:27 RELEASE capture (7 `RunPipeline`, 2 `ScoreCandidates`)
— that one was a quiet session, this one is a town with NPCs moving. The
comparison to make later is against THIS entry, not that one.

### Biggest per-call cost (spike risk), by MTPC — Self only

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `Display::Wheeler` | **1.24 ms** | 56 | **69.51 ms** (0.05%) | Biggest per-call by 22x. Once per full pipeline pass — so cutting passes is the only lever on it that does not touch Wheeler itself. Still critique **#14**. |
| `ApparelRegistry::Reconcile` | 801.76 µs | 4 | 3.21 ms | Four calls — treat as unmeasured, not expensive (§2.1). Was 73.92 µs on the RELEASE capture. |
| `Pipeline::AllocateAndLock` | 290.23 µs | 56 | 16.25 ms | |
| `ReconcileWeapons::ExtractMetadata` | 245.2 µs | 5 | 1.23 ms | Now emits one record per ExtraDataList, not per base form. |
| `ReconcileWeapons::Apply` | 243.6 µs | 5 | 1.22 ms | |
| `Pipeline::ScoreCandidates` | 199.23 µs | 56 | 11.16 ms | |
| `Inventory::DeltaScan` | 143.09 µs | 213 | 30.48 ms | #13. |
| `PollTargets` | 137.75 µs | 1,068 | **147.12 ms** (0.10%) | **Biggest cumulative.** #12. ✅ ranking holds. |
| `PollPlayerMagicEffects` | 114.16 µs | 1,068 | **121.92 ms** (0.08%) | Second cumulative. Tier 3 O3. ✅ |
| `WeaponRegistry::ReconcileWeapons` | 112.24 µs | 5 | 561.21 µs | |
| `ItemRegistry::Reconcile` | 98.85 µs | 3 | 296.56 µs | |
| `ReconcileWeapons::GetInventory` | 98.42 µs | 5 | 492.08 µs | |
| `Display::Intuition` | 87.88 µs | 56 | 4.92 ms | |
| `WeaponRegistry::RefreshCharges` | 68.16 µs | 213 | 14.52 ms | |
| `RunPipeline` | 65.22 µs | 336 | 21.91 ms | Self time — the 280 hash-skipped entries are most of the Count. |
| `SpellRegistry::Reconcile` | 58.87 µs | 21 | 1.24 ms | |
| `OnUpdate` | 36.94 µs | **1,307** | 48.28 ms | Self time. Same preamble finding as the RELEASE entry. |
| `PollPlayerVitals` | 27.04 µs | 1,068 | 28.88 ms | |
| `ScrollRegistry::Reconcile` | 22.84 µs | 3 | 68.52 µs | |
| `SpellRegistry::RefreshFavorites` | 22.66 µs | 213 | 4.83 ms | |
| `PollPlayerEquipment` | 20.34 µs | 1,068 | 21.73 ms | |
| `PollPlayerPosition` | 16.62 µs | 1,068 | 17.75 ms | |
| `PollPlayerSurvival` | 15.31 µs | 107 | 1.64 ms | |
| `PollHealthTracking` | 14.89 µs | 1,068 | 15.9 ms | |
| `PollWorldObjects` | 12.54 µs | 1,068 | 13.39 ms | |
| `Pipeline::GatherState` | 12.28 µs | 336 | 4.13 ms | |

Rows below 12 µs MTPC not transcribed; `Pipeline::PushDisplay` (2.23 µs × 56)
and `Update::PipelineCheck` (2.15 µs × 1,067) are the ones worth knowing are
trivial.

### What the fixes should do to these numbers

The cost of one full pipeline pass is ~1.82 ms of self time, and **68% of it is
`Display::Wheeler`**. So the arithmetic is simple:

- Fix 2 removes an estimated **6-7 of the 56** passes (12 ally flaps per 4½ min
  scales to ~6.5 here) → about **-12 ms**, ~12% of all full-pipeline work.
- Fix 1 adds one pass per reward event; there were 2 equips in that 4½-minute
  window, so ~1 here → about **+1.8 ms**.

Net expectation: `ScoreCandidates` Count drops from 56 to roughly **50**, with
`AllocateAndLock`, `PushDisplay`, `Display::Wheeler` and `Display::Intuition`
tracking it one-for-one. Nothing else should move.

In absolute terms this is ~0.008% of runtime. It is worth measuring anyway, for
the failure mode rather than the win.

### The failure mode to look for

If the learner latch ever fails to clear, `NeedsForcedRun()` returns true every
tick and `RunPipeline` climbs toward `Update::PipelineCheck`'s Count with
`ScoreCandidates` behind it. At 1,067 passes, `Display::Wheeler` alone would be
1.24 ms x 1,067 = **1.3 s**, ~20x the entire current Huginn cost. It is a loud
failure, and Counts show it immediately — no need to read times at all.

Traced one path where a forced tick can bypass the clear: the outer gate in
`RunPipelineIfNeeded` passes, then the `g_spellRegistry->IsLoading()` guard
rejects the call, so `CheckHashSkip` never runs and never clears. It self-heals
once the registry finishes, because the same guard suppresses the work too.

---

## 2026-09-19 — `1516c34` (tree = `6953c6d`) — first RELEASE capture; Tracy 0.14.1 baseline

- Session: SkyrimSE.exe @ 2026-09-19 14:27:45, Tracy 0.14.1, 24,925 frames, ~3:09.5 program time, 65.06 MB (0.10%).
- Save: scale not recorded (see caveat below).
- Notes: **RELEASE + TRACY** — the first non-Debug entry in this file. PR #117
  (`extern/tracy` v0.13.1 → v0.14.1). Provenance caveat: the load line stamps
  `1516c34` because the binary was configured before the branch was committed; the
  source tree it was built from is exactly `6953c6d`. Version stamp `v0.20.16`.
- Zones seen: **38 of 41** distinct zone names. The 3 absent are paths this session
  never exercised.

### Headline: the whole system costs **0.021% of one core**

`OnUpdate` inclusive total is **39.97 ms across 189.5 s**. That figure bounds
everything nested under it. Tick cadence measured 1,742 ticks / 189.5 s = 9.19/s
≈ **109 ms**, matching the documented ~100 ms loop.

### Biggest per-call cost (spike risk), by MTPC — Self only

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `Pipeline::ScoreCandidates` | **75.83 µs** | 2 | 151.65 µs | Biggest per-call, and still 0.45% of a 16.7 ms frame. |
| `ApparelRegistry::Reconcile` | 73.92 µs | 1 | 73.92 µs | New in #114. One call — treat as unmeasured, not cheap (§2.1). |
| `Pipeline::AllocateAndLock` | 71.02 µs | 2 | 142.03 µs | |
| `ReconcileWeapons::Apply` | 30.98 µs | 2 | 61.97 µs | |
| `PollPlayerMagicEffects` | 25.58 µs | 212 | **5.42 ms** | Biggest cumulative — still roadmap Tier 3 O3. ✅ ranking holds. |
| `RunPipeline` | 23.03 µs | 7 | 161.23 µs | |
| `Inventory::DeltaScan` | 22.68 µs | 42 | 952.53 µs | #13. Scales with inventory — save scale not recorded here. |
| `PollTargets` | 22.68 µs | 212 | **4.81 ms** | #12. Second-biggest cumulative. ✅ |
| `WeaponRegistry::ReconcileWeapons` | 17.31 µs | 2 | 34.62 µs | |
| `PollPlayerSurvival` | 16.85 µs | 21 | 353.75 µs | |
| `SpellRegistry::Reconcile` | 16.01 µs | 4 | 64.04 µs | |
| `PollPlayerVitals` | 15.25 µs | 212 | 3.23 ms | |
| `Display::Intuition` | 13.05 µs | 2 | 26.1 µs | |
| `ReconcileWeapons::GetInventory` | 11.28 µs | 2 | 22.55 µs | |
| `OnUpdate` | 9.41 µs | **1,742** | **16.4 ms** | Self time. See below — this is the real cost centre. |
| `PollPlayerPosition` | 7.23 µs | 212 | 1.53 ms | |
| `SpellRegistry::RefreshFavorites` | 6.28 µs | 42 | 263.68 µs | |
| `PollPlayerEquipment` | 6.16 µs | 212 | 1.31 ms | |
| `WeaponRegistry::RefreshCharges` | 4.99 µs | 42 | 209.49 µs | |

Rows below 4.99 µs MTPC were not transcribed; the 39.97 ms inclusive figure bounds them.

### Finding: 41% of all cost is `OnUpdate`'s own preamble

Tracy's Find Zone puts `OnUpdate` self time at **41.03%** of its inclusive time —
16.4 ms of 39.97 ms. It outweighs every child zone, because it runs 1,742 times
while everything downstream runs 212× or fewer.

That body (`src/UpdateLoop.cpp:464-522`) is only: `SCOPED_TIMER`, two
`steady_clock::now()` calls, a singleton fetch, `IsWorldLoaded` (two `IsMenuOpen`
string-hash lookups + `Get3D`), and `SoakMetrics::RecordTick`. **If per-tick cost
ever matters, that preamble is the target — not `ScoreCandidates`,** which is
where the MTPC ranking points you. This is the one place where the guide's
"MTPC is the only signal" rule needs MTPC × Count to see the answer.

Not worth acting on at 0.021%. Recorded so the next person does not re-derive it.

### Tail: bimodal, as the skip architecture intends

`OnUpdate` inclusive distribution: mean 22.95 µs, median 8.42 µs, mode 7.96 µs,
**σ 47.78 µs (208% of mean)**, P75 11.22 µs, P90 82.15 µs, P99 184.17 µs,
**P99.9 334.5 µs**.

Mode ≈ 8 µs is a skipped tick; P90 ≈ 82 µs is a tick doing real work. Worst case
334.5 µs is 2% of a 16.7 ms frame — **no hitch risk**.

### Skip funnel is internally consistent

```
OnUpdate 1,742  →  RunPipeline 7  →  ScoreCandidates 2  →  AllocateAndLock 2  →  Display::Intuition 2
                   (99.6% skipped     (5 of 7 bailed at
                    at dirty flag)     the hash compare)
```

Two full pipeline runs produced exactly two display pushes — no orphaned stages.
Note the skip rate is far higher than the 2026-06-07 capture (64% / 96.7%); this
was a quiet session, not evidence of a change.

### Memory

Peak **193.07 KB**, 4,494 data points, flat after a single step at save-load. No
growth across ~3 minutes — but that window says **nothing** about the 20–50 hr
soak. Its real value: it confirms the `src/TracyMemory.cpp` `operator new`/`delete`
overrides work under 0.14.1, which the discarded menu-only capture could not show.

## 2026-07-25 — `99cbb48` — critique #9 (display abstraction) complete

- Session: SkyrimSE.exe @ 2026-07-25 14:19:22, Tracy 0.13.1, 53,835 frames, ~17:29 program time, 100.7 MB.
- Save: real playthrough (~78 learner items; poll counts ~6,916 ticks — a long, dense session).
- Notes: DEBUG + TRACY (relative only). `99cbb48` = PR #56 HEAD (all four review passes). **0 page switches
  this session**, so the #9 race bail was not exercised (`pageBail=0` in the heartbeat, as expected).

### Biggest per-call cost (spike risk), by MTPC

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `Display::Wheeler` | **2.54 ms** | 176 | 447 ms | Still the biggest per-call — critique **#14** (re-allocates every non-current page each push). Unchanged by #9. |
| `Pipeline::ScoreCandidates` | 1.31 ms | 176 | 231 ms | Scoring pass (bigger candidate set on the real save). |
| `Inventory::DeltaScan` | **685 µs** | 1,118 | **766 ms** | Biggest cumulative — critique **#13** (`GetInventorySafe` deep-copy), scales with the hoarder inventory. |
| `ItemRegistry::Reconcile` | 610 µs | 20 | 12 ms | Consolidated registry (#8) — still cheap. ✅ |
| `Pipeline::AllocateAndLock` | 325 µs | 176 | 57 ms | Now includes the #9 `AllocateSlotsForPage` + mid-tick race check. Reasonable. |
| `WeaponRegistry::ReconcileWeapons` | 227 µs | 22 | 5 ms | #8 — cheap. ✅ |
| `PollPlayerMagicEffects` | 113 µs | 6,916 | **780 ms** | Biggest cumulative poll — **#12**-adjacent. |
| `PollTargets` | 94 µs | 6,916 | 649 ms | Under-lock scan — critique **#12**. |
| `Display::Intuition` | 84 µs | 176 | 15 ms | Intuition push. |
| `Pipeline::PushDisplay` | **1.85 µs** | 176 | 0.33 ms | **#9 win** — was doing potential mid-tick re-allocation; now just hands the context to backends. Trivial. |

### Takeaways

- **#9 is perf-positive.** `Pipeline::PushDisplay` dropped to **1.85 µs/call** — the mid-tick re-sync + raw
  `AllocateSlotsForPage` that used to live there is gone; page resolution now happens once in the (cheap)
  `AllocateAndLock`/`ResolveDisplayPage` path. The race-bail check adds nothing measurable.
- **#8 still perf-neutral** on a real, dense save — the consolidated `*Registry::Reconcile` zones stay in the
  hundreds-of-µs range.
- **Hot paths are unchanged and still the Tier-3 targets:** `Display::Wheeler` 2.54 ms/call (**#14**),
  `Inventory::DeltaScan` 766 ms cumulative (**#13**), `PollPlayerMagicEffects` + `PollTargets` (**#12**).
  On the larger inventory, `#13`'s per-call cost grew (161 µs → 685 µs vs the 2026-07-24 baseline), as predicted.
- Frame tooltip read 21.2 s (a paused/loading frame — ignore); CPU ~46%. No pathological steady-state cost.

---

## 2026-07-24 — `4791318` — post-Scroll/Item/Spell consolidation, pre-Weapon

- Session: SkyrimSE.exe @ 2026-07-24 21:08:30, Tracy 0.13.1, 12,990 frames, ~11:17 program time, 98.3 MB.
- Save: small (3 items / 0 spells / 2 weapons — the tiny test save; poll counts ~1,941 ticks).
- Frame sampled: 10.07 ms (99.3 FPS), CPU ~40%.
- Notes: DEBUG + TRACY, `4791318` = FormRegistry migration status commit (Scroll/Item/Spell
  migrated; Weapon accessors still pre-Option-C). Baseline for the registry-consolidation work.

### Biggest per-call cost (spike risk), by MTPC

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `Display::Wheeler` | **3.65 ms** | 3 | 10.96 ms | **Biggest per-call.** Wheeler push — critique **#14** (re-allocates every non-current page each push). |
| `Pipeline::ScoreCandidates` | 1.22 ms | 3 | 3.66 ms | Scoring pass. |
| `Pipeline::AllocateAndLock` | 443.7 µs | 3 | 1.33 ms | Slot allocation + locks. |
| `ReconcileWeapons::Apply` | 194.6 µs | 5 | 973 µs | Weapon reconcile write section. |
| `WeaponRegistry::ReconcileWeapons` | 176.3 µs | 5 | 882 µs | 30 s reconcile. |
| `Inventory::DeltaScan` | 161.3 µs | 157 | **25.32 ms** | `GetInventorySafe` deep-copy — critique **#13**. |
| `Display::Intuition` | 106.0 µs | 3 | 318 µs | Intuition push — **#14** (no change-gating). |
| `Pipeline::PushDisplay` | 93.9 µs | 3 | 282 µs | |
| `ItemRegistry::Reconcile` | 92.3 µs | 3 | 277 µs | Migrated registry — cheap. ✅ |
| `PollPlayerMagicEffects` | 73.9 µs | 1,941 | **143.4 ms** | Biggest cumulative — **#12**-adjacent per-tick actor scan. |
| `PollTargets` | 55.3 µs | 1,941 | **107.4 ms** | Full scan under write lock, up to 3×/tick — critique **#12**. |
| `PollPlayerVitals` | 35.4 µs | 1,941 | 68.6 ms | |
| `WeaponRegistry::RefreshCharges` | 34.4 µs | 157 | 5.41 ms | |
| `SpellRegistry::Reconcile` | 43.6 µs | 17 | 741 µs | Migrated — cheap. ✅ |
| `ScrollRegistry::Reconcile` | 20.7 µs | 3 | 62 µs | Migrated — cheap. ✅ |

(Full 40-zone list not reproduced; the tail is all sub-20 µs MTPC — `StateManager::Update`
8.9 µs, the vital polls 7–17 µs, `Update::*` orchestration 1.8–2.6 µs, `WeaponRegistry::Refresh`
791 ns, `EquippedWeapons::Query` 749 ns.)

### Takeaways

- **System is healthy** — sampled frame 10 ms / 99 FPS, nothing blowing the budget. Everything
  below is efficiency/debt, not a fire.
- **Registry consolidation is perf-neutral** — the migrated `*Registry::Reconcile` zones are all
  sub-100 µs, and no new accessor zones appeared (accessors are deliberately un-zoned).
- **Where the measurable cost actually is**, mapped to the critique:
  - `Display::Wheeler` 3.65 ms/call → **#14** (push-path redundant work). *Biggest spike, cheapest fix.*
  - `Inventory::DeltaScan` 25 ms cumulative → **#13** (per-item `InventoryEntryData` deep-copy).
  - `PollTargets` 107 ms + `PollPlayerMagicEffects` 143 ms cumulative → **#12** (under-lock scans).
- Caveat: cumulative poll totals are spread across ~1,941 ticks on a 100 ms budget → <1% per
  tick each. They dominate *aggregate* CPU but are not hitching frames today.

### Follow-up captures wanted

- A capture on a **large real save** (the 2026-07-24 in-game soak used 54 items / 13 spells /
  10 weapons) — hoarder inventories are where `#13`'s deep-copy and `#12`'s target scan scale worst.
- A **before/after** capture bracketing any `#14` change, comparing `Display::Wheeler` /
  `Display::Intuition` MTPC.

---

## 2026-06-13 — `slot-cleanup` branch — pre-O1 baseline (folded in from `refactor/optimizations.md`)

- Session: ~13.8 min, 44,854 frames, ~4,743 update ticks. DEBUG + TRACY, *Self-only*.
- Context: slot-subsystem cleanup applied. Recorded here because it is the **before** side of
  the O1 (`RefreshCharges`) fix — the 2026-07-24 capture is the after.

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `WeaponRegistry::Refresh` | **1.25 ms** | 736 | 918 ms | #1 cost — full inventory walk @ 2 Hz. **This is what O1 removed.** |
| `Pipeline::ScoreCandidates` | 759 µs | 65 | 49 ms | Rare path, expected. |
| `ItemRegistry::Reconcile` | 663 µs | 13 | 8.6 ms | Rare full reconcile. |
| `ItemRegistry::RefreshCounts` | 634 µs | 728 | 462 ms | O2 → now tracked as critique **#13**. |
| `Pipeline::AllocateAndLock` | 338 µs | 65 | 22 ms | Slightly lower than 06-07's 354 µs — no Tier-3 regression. |
| `PollPlayerMagicEffects` | 81 µs | 4,743 | 385 ms | O3 → now tracked as its own Tier-3 item. |
| `PollTargets` | 57 µs | 4,743 | 269 ms | #12. |
| `OnUpdate` (self) | 12 µs | 4,742 | 56 ms | ✅ |

Skip-check: 4,743 ticks → 1,195 `RunPipeline` (~75% skipped at dirty flag) → 65 `ScoreCandidates`
(~94.5% of pipeline runs skipped at hash compare).

Instrumentation added in this session is what made O1 actionable: `WeaponRegistry::Refresh` had been
one opaque zone wrapping `EquippedWeapons::Query` + `RefreshCharges` + `ReconcileWeapons`, with the
inner calls on log-based `SCOPED_TIMER` rather than Tracy. Nested `Huginn_ZONE_NAMED` zones were added
(`Query` at the `UpdateLoop.cpp` call site via a lambda wrapper, to avoid forcing `Profiling.h` into the
header where `Query` is inline; the other two inside the methods).

## 2026-06-07 — `learning-cleanup` branch — first stage-zone attribution

- Sessions: a quiet ~6.5 min capture and an active ~11.5 min capture (4,336 update ticks).
  DEBUG + TRACY, *Self-only*. Huginn ≈ 0.13% CPU overall.
- Context: adding stage zones to `OnUpdate` (`Update::Subsystems` / `Update::Registries` /
  `Update::PipelineCheck`) resolved a previously-unattributed 164 µs/call of `OnUpdate` self-time
  almost entirely into registry maintenance. `OnUpdate` self dropped to ~13 µs.

| Zone | MTPC | Count | Total | Note |
|---|---|---|---|---|
| `WeaponRegistry::Refresh` | **1.23 ms** | 705 | 866 ms | #1 cost. Lines up with the ~1 s orange sawtooth in the Memory plot (per-call `InventoryEntryData` map allocation). |
| `Pipeline::ScoreCandidates` | 981 µs | 52 | 51 ms | Rare (96.7% skip), expected. |
| `ItemRegistry::RefreshCounts` | 618 µs | 694 | 429 ms | Consumption detector needs the snapshot. |
| `Pipeline::AllocateAndLock` | 354 µs | 52 | 18 ms | ✅ |
| `PollPlayerMagicEffects` | 117 µs | 4,337 | 505 ms | Top poll, every tick — it is not gated by the skip-check, it *feeds* it. |
| `PollTargets` | 75 µs | 4,337 | 327 ms | ✅ |
| `PollPlayerVitals` | 28 µs | 4,337 | 123 ms | ✅ |
| `OnUpdate` (self) | 13 µs | 4,336 | 57 ms | ✅ resolved — was 164 µs. |
| stage zones (`Update::*`) | 2–3 µs | 4,336 | ~35 ms | ✅ instrumentation overhead negligible. |

Skip-check: 4,336 ticks → 1,559 `RunPipeline` (64% skipped at sensor dirty flag) → 52
`ScoreCandidates` (96.7% skipped at discretized-hash compare). Full scoring ran 52 times in 11.5 min.

Also confirmed healthy at the time: Tier-3 changes held up (`StateManager::Update` self 8.3 µs);
instrumentation overhead safe to leave in (no-op when `Huginn_TRACY` is off); learning pipeline
(consumption + external-equip attribution) fired correctly and exactly once per action, log-verified.

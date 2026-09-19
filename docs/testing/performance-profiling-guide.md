# Huginn Performance Profiling Guide

**Applies to:** v0.19.x (verified against v0.19.10)
**Last verified:** 2026-08-29
**Status:** Current — every command below was checked against `CMakePresets.json`,
`CMakeLists.txt` and `src/`.

> **History.** The file that used to live here was a v1.0-refactor benchmarking
> plan dated 2026-02-14. It described a `scripts/` directory, a
> `.github/workflows/performance-tests.yml` CI job, a gtest target
> (`tests/performance/perf_suite.cpp` / `HuginnTests.exe`), and a
> baseline/refactor CSV corpus under `docs/testing/`. **None of those exist in
> this repository, and no evidence was found that they ever did.** They were
> removed rather than carried forward. What is documented below is what the repo
> actually has.

---

## 1. What this document covers

| Question | Where to look |
|---|---|
| How do I get a Tracy capture and read it correctly? | This document |
| What did previous captures measure? | [../profiling/tracy-traces.md](../profiling/tracy-traces.md) |
| What are the current hot-path work items? | [../roadmap.md](../roadmap.md), Tier 3 |
| How do I run a multi-hour endurance session? | [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) |
| What are the update-loop tiers, intervals and budgets? | [../reference/Performance.md](../reference/Performance.md) |
| What unit tests exist and how do they run? | [TESTING-INDEX.md](TESTING-INDEX.md) |

This guide is about **measurement**. It deliberately does not restate the timing
targets in `../reference/Performance.md` or the soak protocol in
`../playtest/LongPlaySoak.md`.

---

## 2. The two lessons that matter most

Read these before drawing a conclusion from any capture. Both were learned the
expensive way, and both invalidate figures that look perfectly reasonable.

### 2.1 Short captures produce badly wrong per-call figures

A 5–15 minute capture is dominated by **cold calls** — first-touch page faults,
lazily-built registries, uninitialised caches, cold branch predictors. Those
inflate MTPC (mean time per call) for anything that runs rarely and barely move
it for anything that runs every tick, so the *ranking* a short capture produces
is systematically wrong.

This is not hypothetical. The Tier 3 ordering in [../roadmap.md](../roadmap.md)
was rebuilt on that basis:

> "Nothing in this tier exceeds 0.10% of runtime on the 44:40 capture of
> 2026-08-26, which is the only capture long enough to trust — the 5-15 minute
> runs that set the original ranking were dominated by cold calls."

Concretely, the 5–15 minute captures ranked `Display::Wheeler` at the top on
MTPC. The 44:40 capture put `PollPlayerMagicEffects` first by total time
(136.3 µs × 19,179 = 2.61 s, 0.10% of runtime), `PollTargets` second
(113.34 µs × 19,179 = 2.17 s), and `Display::Wheeler` at 862 ms — a third of the
leader. One Tier-3 item (#14) was archived on that measurement, and #13
(`Inventory::DeltaScan`, 267.78 µs × 3,848 = 1.03 s) moved *ahead* of
`Display::Wheeler` by total despite a smaller MTPC, purely because it runs ~4×
as often.

**Rule: capture at least ~45 minutes of real play before ranking anything.**
Anything shorter is a smoke test — useful to confirm zones are firing, useless
for prioritisation.

<!-- UNVERIFIED: the 2026-08-26 44:40 capture has no entry in
../profiling/tracy-traces.md (newest entry there is 2026-07-25). Its numbers are
quoted here from the Tier 3 section of ../roadmap.md, which is the only record of
them under version control. The raw .tracy files live in an untracked `traces/`
directory. -->

### 2.2 Zone Count is never a diagnostic — MTPC is the only signal

`Huginn_ZONE_NAMED` is placed at the **top of the function, ahead of every
early-out**. Verify it yourself: `src/state/StateManager_MagicEffects.cpp:71`
opens `PollPlayerMagicEffects` with the zone macro, and the null-player and
null-`AsMagicTarget` bails come *after* it. `src/display/WheelerBackend.cpp:33`
and `src/pipeline/PipelineCoordinator.cpp:301` do the same.

Consequences:

- A zone's **Count is the number of times the function was entered**, including
  every invocation that bailed in the first few instructions. It says nothing
  about how often the work actually happened.
- Two zones with the same Count can have done wildly different amounts of work.
- "This zone fired 19,179 times, that's a lot" is not a finding.

**Use MTPC for spike risk (what causes a visible hitch) and MTPC × Count = Total
for cumulative CPU.** Count on its own is context for those two numbers, never a
conclusion.

The one place Count *is* informative is the pipeline skip rate, and only because
the skip is structural — see §6.

---

## 3. Building a profiling build

Tracy is already vendored as a submodule (`extern/tracy`, per `.gitmodules`;
currently Tracy **0.14.1**). **Do not run `git submodule add`** — the old version
of this guide told you to, and it will fail.

What you **do** need on an existing clone is a submodule sync — git does not
update submodule working trees on checkout, so a clone made before the 0.14.1
bump silently keeps 0.13.1 in `extern/tracy`:

```sh
git submodule update --init extern/tracy
```

Skipping this fails in the worst possible way: the build succeeds, the load line
still shows ` [TRACY]`, and the 0.14.x GUI simply never lists the client — the
0.13.1 protocol (76) cannot handshake with a 0.14.x server (82), and nothing
reports an error. If §4 shows no client, check the submodule before anything
else:

```sh
git -C extern/tracy describe --tags   # must print v0.14.1
```

```sh
# Ordinary Debug build (no Tracy) — the CLAUDE.md build line
cmake --preset vs2022-windows && cmake --build build --config Debug

# Debug + Tracy — zones on, but times inflated by the unoptimised build
cmake --preset vs2022-windows -DHuginn_TRACY=ON && cmake --build build --config Debug

# Release + Tracy — the profiling build. Use this for anything you will quote.
cmake --preset vs2022-windows -DHuginn_TRACY=ON && cmake --build build --config Release
```

Notes on the above, all verified:

- `-DHuginn_TRACY=ON` sets the `Huginn_TRACY` option (`CMakeLists.txt:34`), which
  forces `TRACY_ENABLE`/`TRACY_STATIC`, adds `extern/tracy`, links `TracyClient`
  and defines `Huginn_TRACY_ENABLED`. **`Huginn_TRACY_ENABLED` is the macro the
  code tests** (`src/Profiling.h`); `TRACY_ENABLE` on its own will not turn zones
  on in Huginn source.
- Configure prints `Tracy profiler: ENABLED` (or `disabled (use -DHuginn_TRACY=ON
  to enable)`). Check that line — it is the cheapest possible confirmation you
  built what you meant to.
- The preset uses a **Visual Studio multi-config generator**, so the config is
  chosen at *build* time via `--build --config X`, not at configure time. The
  preset's `CMAKE_BUILD_TYPE: Debug` cache entry is inherited from the `cmake-dev`
  base preset and is ignored by the VS generator. Passing
  `-DCMAKE_BUILD_TYPE=RelWithDebInfo` does nothing.
- `CMakeLists.txt` globs sources at configure time. After switching branches,
  reconfigure — do not just rebuild.
- The build **deploys automatically** to `$CompiledPluginsPath` (a POST_BUILD copy
  in `src/CMakeLists.txt`), so a build silently changes what you are about to
  play-test. Bump the version at `CMakeLists.txt:5` and confirm line 1 of the log
  before trusting a session.
- If `SkyrimSE.exe` is running, the link step fails with LNK1201 (the PDB is
  locked by the crash logger). Compilation succeeds; only relinking needs the game
  closed.

### Why not RelWithDebInfo?

The old guide's baseline procedure built `RelWithDebInfo`. The soak protocol in
[../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) standardises on
**Release + Tracy** for perf runs and Debug for behaviour runs. Follow that split
so captures stay comparable with the recorded history.

---

## 4. Capturing

1. Launch the **Tracy profiler GUI** (0.14.x, matching the vendored client) and
   click *Connect*.
2. Launch Skyrim. The load line stamps the build (`src/Main.cpp:817–826`):
   `Huginn vX.Y.Z (<git-sha>) [RELEASE] [TRACY] Loading`, or
   `… [DEBUG BUILD] [TRACY] Loading` for a Debug+Tracy build. The ` [TRACY]` tag
   is present only when `Huginn_TRACY_ENABLED` is defined — **if it is missing,
   you are not capturing anything.** **Record that SHA** — it is the provenance
   for every number you are about to quote. Caveat: the SHA is baked at
   configure/build time from HEAD, so uncommitted changes stamp the base commit.
3. Play a representative slice — use the context-coverage checklist in
   [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) so no code path
   goes cold. **At least ~45 minutes** (§2.1).
4. In Tracy: **Statistics** panel → **Timing: Self only** → sort by **MTPC**.
5. Record the result as a new entry in
   [../profiling/tracy-traces.md](../profiling/tracy-traces.md), newest first,
   using the entry template in that file.

**One tool at a time.** Tracy + the VSCode debugger + `debug`-level logging
simultaneously destroys the profile you are trying to measure. See "One tool ≠ one
run" in `../playtest/LongPlaySoak.md`.

### If Statistics shows one zone, you captured a menu

Check **Statistics → Total zone count** before you read anything else. If it says
**1** — only `OnUpdate` — throw the capture away and take another.

This is not a broken build, and the ` [TRACY]` tag will not warn you. `OnUpdate`
and the three zones nested directly inside it (`Update::Subsystems` :193,
`Update::Registries` :249, `Update::PipelineCheck` :388) live in the *same*
translation unit, so if `OnUpdate` emitted, the others were compiled too. The only
way to get one without the rest is the early return at `src/UpdateLoop.cpp:510`:

```cpp
if (!loaded) return;   // IsWorldLoaded: LoadingMenu / MainMenu / Get3D() == nullptr
```

Everything downstream was skipped for every tick. The `OnUpdate` MTPC you are
looking at is the cost of the early-return path — quoting it as Huginn's tick cost
understates the real figure by a wide margin.

Two tells, both visible without leaving the capture:

- **Frame rate far above the in-game cap.** A menu renders uncapped; in-world sits
  at the cap. A 2026-09-19 discard ran 207 fps against 131 fps for the good capture.
- **Memory usage near-flat at a few KB.** The `operator new`/`delete` overrides in
  `src/TracyMemory.cpp` flood during real gameplay. The discard showed 2 data
  points / 7.8 KB; the good capture showed 4,494 points / 193 KB.

Fix: connect, **then load a save**, play a representative slice, and stop the
capture without returning to the main menu.

### Frames are ticks, not render frames

`Huginn_FRAME_MARK` is emitted once per Huginn update tick
(`src/update/UpdateHandler.cpp:81`), so one Tracy "frame" is one ~100 ms Huginn
tick. The FPS graph reads ~10 Hz by design — that is not a performance problem.

---

## 5. Reading the numbers

- **MTPC** (mean time per call) = per-invocation cost → what causes a visible
  *spike*.
- **Total** = MTPC × Count over the capture → cumulative CPU. A large total spread
  over ~19,000 ticks on a 100 ms budget is cheap *per tick* even when the total
  looks alarming.
- **Self only.** Always sort with *Timing: Self only*, or nested zones
  double-count into their parents.
- **Debug + Tracy numbers are inflated** (no inlining/optimisation, plus capture
  overhead). Use them for relative ranking and cross-build comparison, never as
  absolute frame-budget claims. Release + Tracy is the build to quote.
- **Registry accessors are deliberately un-zoned.** If you see no `QueryTopK` /
  `FindBest` / `Get*` zones, that is expected, not missing data.

---

## 6. Zone inventory

**46** `Huginn_ZONE_NAMED` call sites resolving to **41 distinct zone names** exist
as of v0.20.16 (some names are hit from more than one site). A healthy in-world
capture shows most of them — the 2026-09-19 capture saw 38. The ones you will
actually look at:

| Zone | Source | What it wraps |
|---|---|---|
| `OnUpdate` | `src/UpdateLoop.cpp:465` | The whole ~100 ms tick |
| `Update::Subsystems` | `src/UpdateLoop.cpp:193` | Per-tick subsystem updates |
| `Update::Registries` | `src/UpdateLoop.cpp:249` | Registry reconcile/refresh block |
| `Update::PipelineCheck` | `src/UpdateLoop.cpp:388` | Skip decision |
| `RunPipeline` | `src/pipeline/PipelineCoordinator.cpp:74` | Pipeline past the dirty-flag skip |
| `Pipeline::GatherState` | `PipelineCoordinator.cpp:138` | State snapshot |
| `Pipeline::ScoreCandidates` | `PipelineCoordinator.cpp:301` | Candidate generation + utility scoring |
| `Pipeline::AllocateAndLock` | `PipelineCoordinator.cpp:407` | Slot allocation + locks |
| `Pipeline::PushDisplay` | `PipelineCoordinator.cpp:488` | Hand-off to display backends |
| `Display::Wheeler` | `src/display/WheelerBackend.cpp:33` | Wheeler push |
| `Display::Intuition` | `src/display/IntuitionBackend.cpp:19` | Scaleform widget push |
| `StateManager::Update` | `src/state/StateManager.cpp:77` | Interval-gated poll dispatch |
| `PollPlayerMagicEffects` | `src/state/StateManager_MagicEffects.cpp:71` | Per-tick active-effect scan |
| `PollTargets` | `src/state/StateManager_Targets.cpp:98` | Per-tick target scan |
| `PollPlayerVitals`, `PollPlayerEquipment`, `PollPlayerPosition`, `PollWorldObjects`, `PollPlayerSurvival`, `PollPlayerResistances`, `PollHealthTracking`, `PollMagickaTracking`, `PollStaminaTracking` | `src/state/StateManager_*.cpp` | The remaining sensors |
| `Inventory::DeltaScan` | `src/UpdateLoop.cpp:281` | Consumption-detector inventory snapshot |
| `SpellRegistry::Reconcile`, `ItemRegistry::Reconcile`, `ScrollRegistry::Reconcile` | `src/UpdateLoop.cpp` | Registry reconciles |
| `WeaponRegistry::Refresh`, `::RefreshCharges`, `::ReconcileWeapons` | `src/UpdateLoop.cpp`, `src/weapon/WeaponRegistry.cpp` | Weapon registry work |

**`StateManager::PollAll()` has no zone.** It is the force-everything path
(startup, `hg refresh`), not the steady-state one; `StateManager::Update` is what
runs every tick. Older docs that gave a `PollAll()` budget were describing a zone
that does not exist.

### Skip-rate arithmetic

Compare Counts down the chain:

```
OnUpdate  →  RunPipeline  →  Pipeline::ScoreCandidates
  ticks       past the         past the discretized-
              sensor dirty     hash compare
              flag
```

Worked example from the 2026-06-07 capture in
[../profiling/tracy-traces.md](../profiling/tracy-traces.md): 4,336 ticks → 1,559
`RunPipeline` (64% skipped at the dirty flag) → 52 `ScoreCandidates` (96.7% of
pipeline runs skipped at the hash compare). Full scoring ran 52 times in 11.5
minutes.

This is the *one* legitimate use of Count, and only because the ratio between two
zones stays meaningful even when each zone's own Count includes early-outs.

---

## 7. Time-series plots (drift and leak watching)

`Huginn_PLOT` charts four series in Tracy, emitted from
`src/telemetry/SoakMetrics.cpp`:

| Plot | Cadence |
|---|---|
| `Huginn/Candidates` | per pipeline recompute |
| `Huginn/Displayed` | per pipeline recompute |
| `Huginn/Learner Items` | per heartbeat |
| `Huginn/Accept %` | per heartbeat |

`Huginn/Learner Items` is the leak watch: the learned-item count must **plateau**
across a long session, not climb linearly. See
[../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) for the full heartbeat
field guide.

`Huginn_ALLOC` / `Huginn_FREE` exist in `src/Profiling.h` for targeted allocation
tracking but have no call sites in `src/` today.

---

## 8. `SCOPED_TIMER` — the log-based fallback

`src/util/ScopedTimer.h` defines `SCOPED_TIMER(name)`, which logs
`PERF: <name> took <N>us` on scope exit and compiles to `((void)0)` when `NDEBUG`
is defined.

Two things the old guide got wrong about it:

1. **It logs at `trace`, not `info`.** Per the logging principles in CLAUDE.md,
   `trace` is "effectively off" — you will not see these lines unless the log
   level is turned down to `trace`. A procedure that greps a default log for
   `PERF:` finds nothing.
2. **Debug-only means Debug-build timings**, inflated the same way Debug+Tracy is,
   without any of Tracy's aggregation.

Current call sites (11, verified at v0.20.16): `src/UpdateLoop.cpp:464`
(`MainUpdate`), `src/update/UpdateHandler.cpp:127`,
`src/learning/UtilityScorer.cpp:35`, `src/state/StateEvaluator.cpp:45`,
`src/learning/item/ItemRegistry.cpp:112,248`,
`src/scroll/ScrollRegistry.cpp:80,137`, `src/spell/SpellRegistry.cpp:185`,
`src/weapon/WeaponRegistry.cpp:112,257`, plus one in `src/Tests.cpp` that only
asserts the macro compiles and runs.

**Use Tracy.** `SCOPED_TIMER` is worth reaching for only when you want a single
number correlated with surrounding log lines and do not want to attach a profiler.

Log location (Debug: `_Huginn_Debug.log`, Release: `Huginn.log`) is whatever
CommonLibSSE's `log_directory()` resolves to for your install — see `OpenLog()` at
`src/Main.cpp:625`, and the concrete path recorded in
[../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md). `basic_file_sink`
truncates on launch with no rotation: **copy the log out before relaunching.**

---

## 9. The `[Soak]` heartbeat as a coarse perf signal

Every 5 minutes the plugin emits one `info` line carrying, among other fields,
`tick avg=… peak=… ms`. It is present in **both** Debug and Release, needs no
profiler, and is the cheapest way to notice performance drift across a long
session — a `peak` that grows hour-over-hour is the interesting bug.

Full field reference: [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md).
Do not use it for ranking; it has no per-zone attribution.

---

## 10. Where the cost currently is

Do not re-derive this from scratch — [../roadmap.md](../roadmap.md) Tier 3 is the
maintained ranking, with per-capture history for each item. Its headline as of
2026-08-26:

> Nothing in Tier 3 exceeds **0.10% of runtime**. On CPU grounds it is a budget
> list, not a work list; treat a felt stutter, not a µs figure, as the trigger to
> pick any of it up.

The order there (O3 `PollPlayerMagicEffects` → #12 `PollTargets` → #13
`Inventory::DeltaScan` → #11 spell-cost caching) is the one to trust. If a new
capture disagrees with it, check your capture length against §2.1 before
concluding the roadmap is stale.

Note that #12 and #13 **scale with inventory size** — a small test save and a
hoarder save are not comparable. Always record the save scale with a capture.

The 2026-09-19 **Release + Tracy** capture is the first absolute measurement of the
whole system rather than a relative ranking: `OnUpdate` inclusive totalled 39.97 ms
over 189.5 s, i.e. **0.021% of one core**. It confirms the Tier 3 order above
(`PollPlayerMagicEffects` ahead of `PollTargets` ahead of `Inventory::DeltaScan`)
and adds one item the MTPC ranking hides — **41% of all cost is `OnUpdate`'s own
per-tick preamble**, which no zone attributes to a subsystem because it is self
time spread over 1,742 ticks. Still inside the "budget list, not a work list"
headline.

---

## 11. Recording a capture

Append a new entry to
[../profiling/tracy-traces.md](../profiling/tracy-traces.md), newest first, with:

- date, build SHA, one-line context
- session stats (exe timestamp, Tracy version, frames, program time, memory)
- save scale (items / spells / weapons / scrolls / ammo)
- build flavour (Debug+Tracy vs Release+Tracy)
- the MTPC / Count / Total table for the top zones, plus takeaways

That file's own "Entry template" section is authoritative for the format.

Raw `.tracy` files are kept locally under an untracked `traces/` directory; they
are **not** in version control, so never link to one from a doc.

---

## 12. Related documentation

- [TESTING-INDEX.md](TESTING-INDEX.md) — unit tests and how they run
- [../profiling/tracy-traces.md](../profiling/tracy-traces.md) — capture history
- [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) — long-play soak protocol
- [../reference/Performance.md](../reference/Performance.md) — update-loop tiers, intervals, targets
- [../roadmap.md](../roadmap.md) — Tier 3 hot-path backlog
- [../architecture/0-pipeline.md](../architecture/0-pipeline.md) — what the pipeline zones wrap
- Tracy: <https://github.com/wolfpld/tracy>

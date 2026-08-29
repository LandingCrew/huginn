# Huginn Testing Index

**Applies to:** v0.19.x (verified against v0.19.10)
**Last verified:** 2026-08-29
**Status:** Current — the suite list below was read out of `src/Tests.cpp` and
`src/Main.cpp`, not carried over from an older doc.

> **History.** The file that used to live here indexed a v1.0-refactor
> benchmarking plan: `scripts/parse_perf_logs.py`, `scripts/compare_perf.py`, a
> `.github/workflows/performance-tests.yml` CI job, a gtest binary
> (`HuginnTests.exe`), `docs/testing/baseline_data/`, `refactor_data/` and
> `reports/`, and a `docs/refactor/staged-implementation.md` stage checklist.
> **None of those paths exist in this repository.** Everything below is verified
> against the current tree.

---

## 1. How Huginn's tests actually run

There is **no test target and no test binary.** `cmake --preset vs2022-windows`
prints `Build Tests: OFF`, but that line comes from a *dependency*: Huginn's root
`CMakeLists.txt:35` does `set(BUILD_TESTS OFF)` to suppress CommonLibSSE-NG's own
test target, and CommonLibSSE-NG echoes the value back
(`$CommonLibSSEPath_NG/CMakeLists.txt:13`). It says nothing about Huginn's tests,
and there is no way to turn a Huginn test target on, because there isn't one.

Instead: **`src/Tests.cpp` (~5,200 lines) is compiled into the plugin DLL in
Debug configurations only, and its suites run inside the game.**

- `src/CMakeLists.txt` marks `Tests.cpp` `HEADER_FILE_ONLY` for every non-Debug
  config, so Release does not even parse it.
- Every suite body is additionally wrapped in `#ifndef NDEBUG`.
- The `#ifndef NDEBUG` guard around the `RunUnitTests()` call site in
  `src/Main.cpp` **must stay in sync** with that `HEADER_FILE_ONLY` property — a
  config that leaves `NDEBUG` undefined while excluding `Tests.cpp` produces an
  unresolved external. There is a comment saying so at `src/Main.cpp:521`.

### Running them

```sh
# Build Debug and deploy (the POST_BUILD copy writes to $CompiledPluginsPath)
cmake --preset vs2022-windows && cmake --build build --config Debug
```

Then launch Skyrim. Two trigger points:

| When | What runs | Where |
|---|---|---|
| **`kDataLoaded`** (main menu, once per process) | `RunUnitTests()` | `src/Main.cpp:526` |
| **`kPostLoadGame`** — loading a save, **not** a new game | the nine game-data suites | `InitializeGameSystems()`, `src/Main.cpp:364–378` |

The second group is gated on `!isNewGame` because it needs real form data
(spells, items, weapons) in the player's inventory. Starting a new game runs
none of it.

There is **no console command to re-run the tests.** `hg rebuild` rebuilds
registries, not tests. To re-run, reload the save.

### Reading the results

Tests report to the log — Debug builds write `_Huginn_Debug.log` in CommonLibSSE's
`log_directory()` (see `OpenLog()`, `src/Main.cpp:625`). **A failing test logs an
error and returns early from its suite; nothing asserts, nothing crashes, and the
game keeps running.** That means a silent suite is a *failed* suite, and the only
reliable check is to grep:

```sh
# Any failure at all (259 TEST FAIL sites + 9 cosave-specific ones)
grep -E "TEST FAIL|\[Cosave Test\] FAIL" _Huginn_Debug.log

# The three terminal PASS markers — all three must be present
grep -E "All unit tests passed!|Regression Test Suite PASSED|Cosave Serialization Tests PASSED" _Huginn_Debug.log
```

Because a failure aborts the rest of its suite, the *absence* of a suite's
terminal marker is as much a signal as an explicit `TEST FAIL` line. Two
`TEST SKIP` sites also exist for cases that need data a save may not have.

---

## 2. Suite inventory

### 2.1 Runs at `kDataLoaded` (no save data needed)

**`RunUnitTests()`** — `src/Tests.cpp:1484`. Terminal marker:
`=== All unit tests passed! ===`.

| Group | Contents |
|---|---|
| GameState hash | Test 1 minimum hash, Test 2 maximum hash, Test 3 uniqueness across all `GameState::kTotalStates` = 72,576 states (6×6×3×7×4×3×2×2×2), Test 3b stamina is excluded from the hash |
| SpellRegistry | Registry starts empty (basic construction; the real coverage is the integration suite) |
| PriorCalculator context independence | Tests 1–8: healing and damage priors identical in/out of context; magnitude, scarcity, spell cost, weapon charge, ammo matching and scroll magnitude *do* affect the prior. This is the guard on the `ContextRuleEngine` / `PriorCalculator` separation |
| Optimization + engine | Test 1 partial-sort correctness, Test 3 `SCOPED_TIMER` compiles and runs, Tests 4–9 `ContextRuleEngine` vital / elemental / environmental / combat / target / equipment rules, Test 10 end-to-end `ContextRuleEngine` → `UtilityScorer` (subtests 1a/1b/2–5: forge, enchanter, resist-fire, healing at 30% HP, AOE damage, soul gem), Test 11 `TargetCollection` cache invariant, Test 12 `PipelineStateCache` rank clamping, Test 13 `EquipSourceTracker` FormID keying, Test 14 `UsageMemory` snapshot reader, Test 15 dedup equivalence (`IsFavorited`, fortify-school parity), Test 16 `FeatureQLearner` batch decay, Test 17 `ContextReason` derivation, and an unnumbered wildcard-page-cache block |

The wildcard-page-cache block (`src/Tests.cpp:4187` ff.) is the regression guard
for issue #70 and its two siblings: it pins roll probabilities to 1.0 and the
refractory to 0 so the rolls are deterministic, then asserts per-page bounds — it
asserts bounds, not randomness.

### 2.2 Runs at `kPostLoadGame` (needs a loaded save)

| Suite | Entry point | What it covers |
|---|---|---|
| `RunSpellRegistryTests()` | `Tests.cpp:329` | Spell registry against real form data |
| `RunItemClassifierTests()` | `Tests.cpp:416` | Item classification against real forms |
| `RunItemRegistryTests()` | `Tests.cpp:527` | Item registry against real inventory |
| `RunWeaponRegistryTests()` | `Tests.cpp:715` | Weapon registry against real inventory |
| `RunMultiplicativeScoringTests()` | `Tests.cpp:44` | 6 tests: zero context gates utility, adaptive lambda vs confidence, learning amplification, correlation compounding, full integration, favorites boost by rank |
| `RunRegressionTests()` | `Tests.cpp:4370` | See below |
| `RunCosaveTests()` | `Tests.cpp:4980` | 4 tests: `FeatureQLearner` export/import round-trip, empty round-trip, import clears existing data, feature-count migration (pad / truncate / equal / reject) |
| `RunStateFeaturesTests()` | `Tests.cpp:872` | 8 tests: default state, low-health combat, one-hot correctness across all 7 target types, distance normalisation, `ToArray` round-trip, normalisation bounds, no-enemy fallback, vital clamping |
| `RunFeatureQLearnerTests()` | `Tests.cpp:1199` | 8 tests: cold start, convergence, weight interpretability, regularisation prevents explosion, weight clamping, generalisation across states, item independence, `Clear()` |

**`RunRegressionTests()`** carries numbered `TC-*` cases (numbering has gaps —
TC-04, 06, 08, 09 and 13 are not present). Terminal marker:
`=== Regression Test Suite PASSED ===`.

| Case | Guards against |
|---|---|
| TC-01 | The 10× cliff at the 50% health threshold — 49% vs 51% must differ by <0.05 on the smooth quadratic curve |
| TC-02 | 10% HP still produces critical healing urgency |
| TC-03 | 100% HP produces zero urgency (so the multiplicative formula gates learning) |
| TC-05 | Multi-tag spells accumulate with `std::max()`, not last-match-wins assignment |
| TC-07 | Resist Fire stays relevant while on fire |
| TC-10 | At a forge, Fortify Smithing is highly relevant |
| TC-11 | Looking at a lock makes Unlock critical |
| TC-12 | An enemy casting raises ward relevance |
| TC-14 | Summon is suppressed when a summon is already active |
| TC-15 | Anti-undead weight vs a draugr target; **15b** that weight reaching a silvered weapon and not a steel one (#80); **15c** the silver name-match being word-bounded (Quicksilver excluded); **15d** `SpellTagExt` reaching the unlock / slow-fall / anti-dragon / waterbreathing weights for both spells and scrolls (#79) |
| TC-16 | Multiplicative formula: zero context gates learning, end to end |

### 2.3 Terminology

The learning system is a **contextual bandit** — see
[../architecture/4-contextual-bandits.md](../architecture/4-contextual-bandits.md).
The identifiers `FeatureQLearner`, `QLearnerSerializer`, the `FQLW` cosave record
and `hg reset qvalues` keep their historical names and are **not** renamed; read
"QLearner" in an identifier as "the learner". Suite names above use the real
identifiers.

---

## 3. Known open items

Both are recorded in [../roadmap.md](../roadmap.md) under *Follow-ups*.

1. **`Context::WeightForCandidate` is hand-reimplemented in two tests instead of
   being called.** The roadmap cites `Tests.cpp:2656/3374`; those line numbers
   have since drifted. The live sites are `src/Tests.cpp:3183` ("Extract weight
   using UtilityScorer's `GetContextWeight` logic", inside unit test 10) and
   `src/Tests.cpp:4516` ("Simulate `GetContextWeight` logic with `max()`
   accumulation", inside TC-05). Both should call
   `Context::WeightForCandidate()` from
   `src/context/ContextWeightForCandidate.h`, which most of the rest of the file
   already does (18 call sites). `DominantReason` / `ReasonLabel` are covered by
   unit test 17 and are not part of this gap.

2. **The cosave decode negative test logs a real-looking error every Debug
   startup.** `[E] DecodeV2EntryBlob: byteLen 83 != stride 84` is the assertion
   firing, not a failure — the "length mismatch must reject the decode" block at
   `src/Tests.cpp:5195` deliberately feeds a short blob. The roadmap cites
   `Tests.cpp:5159` (drifted). It should be silenced so a genuine rejection stays
   visible; it has cost triage time twice.

---

## 4. Other kinds of testing

| Kind | Document |
|---|---|
| **Profiling** (Tracy, zones, capture methodology) | [performance-profiling-guide.md](performance-profiling-guide.md) |
| **Capture history** (what previous traces measured) | [../profiling/tracy-traces.md](../profiling/tracy-traces.md) |
| **Long-play soak** (20–50 hr endurance, `[Soak]` heartbeat, accept%) | [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) |
| **Timing targets and update-loop tiers** | [../reference/Performance.md](../reference/Performance.md) |
| **Console commands** for manual poking (`hg refresh`, `hg recs`, `hg status`, `hg weights`) | [../reference/ConsoleCommands.md](../reference/ConsoleCommands.md) |

### In-game verification gotchas

These bite repeatedly; they are not test-suite issues but they invalidate manual
test results.

- **Verify build provenance before trusting a log.** Line 1 stamps
  `Huginn vX.Y.Z (<git-sha>)`, but the SHA bakes at CMake configure/build time
  from HEAD, so uncommitted changes stamp the base commit. Bump the version at
  `CMakeLists.txt:5` per PR and check line 1 for the version you expect.
- **The build deploys automatically** to `$CompiledPluginsPath`, so a build
  silently changes what you are play-testing.
- **Reconfigure after switching branches** — `CMakeLists.txt` globs sources at
  configure time (`GLOB_RECURSE`), so a branch with new files fails to link
  otherwise.
- **Release logs to `Huginn.log`, Debug to `_Huginn_Debug.log`**, in the same
  folder. Check `LastWriteTime` and the line-1 timestamp before trusting either.
- **Seeing an unhashed sensor fire in-game is not proof the pipeline runs for
  it.** `CheckHashSkip` compares `GameState::GetHash()`, which covers only the
  nine fields listed in `src/state/GameState.h`. A sensor outside that set only
  gets a tick through when a hashed field happens to move at the same moment. To
  verify an unhashed signal, isolate it: trigger it with every hashed field held
  still.

---

## 5. What does not exist

Listed so nobody reintroduces a reference to it:

- No `scripts/` directory — no `parse_perf_logs.py`, no `compare_perf.py`
- No `.github/` directory — no CI, no `performance-tests.yml`
- No `tests/` directory, no gtest dependency, no `HuginnTests.exe`
- No `docs/testing/baseline_data/`, `refactor_data/` or `reports/`
- No `docs/refactor/staged-implementation.md`, no `docs/reviews/SESSION-SUMMARY.md`,
  no `docs/testing/performance-issues.md`
- No `docs/ROADMAP.md` — the roadmap is [../roadmap.md](../roadmap.md), with
  [../roadmap-archive.md](../roadmap-archive.md) beside it
- No `huginn.reload` console command — the command is registered as `Huginn`
  with the short alias `hg`, so the syntax is `hg reload`; see [../reference/ConsoleCommands.md](../reference/ConsoleCommands.md)

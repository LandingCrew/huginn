# Huginn Testing Index

**Applies to:** v0.19.x (verified against v0.19.10); sections 0 and 1a added
for v0.23.9 (R1, host tests and the unattended in-game run)
**Last verified:** 2026-08-29 (suite inventory), 2026-10-08 (sections 0, 1a)
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

## 0. Host tests (`huginn_core_tests`)

Pure code -- no `RE::`, no SKSE, no CommonLib -- lives in `src/core/` and is
tested on the host, without the game. The rule for the engine rewrite: every
new piece of math goes there with host tests; the game layer only reads forms
and calls it. See [src/core/README.md](../../src/core/README.md).

| What | Where |
|---|---|
| The pure code | `src/core/` (compiled into the plugin too, by its `GLOB_RECURSE`) |
| The tests | `tests/core/*Tests.cpp`, [doctest](https://github.com/doctest/doctest) from vcpkg |
| The target | `huginn_core_tests` (`tests/CMakeLists.txt`), registered with CTest |

```sh
# Configure with deploy OFF if you do not want the Debug DLL copied into the game
cmake --preset vs2022-windows -DCOPY_OUTPUT=OFF
cmake --build build --config Debug --target huginn_core_tests
ctest -C Debug --test-dir build --output-on-failure
```

The executable (`build/tests/Debug/huginn_core_tests.exe`) exits non-zero when
any check fails, and so does `ctest`. It compiles `src/core/` **without** the
plugin's PCH, and compiles every core header on its own, so a core header that
leans on `PCH.h` fails here. `cmake/CheckCorePurity.cmake` keeps `src/core/`
free of the game (quoted includes only within `src/core/`, angle includes only
from the standard library, no `RE`/`REL`/`REX`/`SKSE`/`logger`/`spdlog` scope);
it runs at configure and as a build step of both this target and the plugin.
Rules and limits: [src/core/README.md](../../src/core/README.md).
`-DHUGINN_CORE_TESTS=OFF` drops the target (and the build-step check).

**Adding or removing a file** under `src/core/` or `tests/core/`: the globs use
`CONFIGURE_DEPENDS`, and with the Visual Studio generator the first build after
the change only re-runs the configure and still builds the old file list.
Build twice, or reconfigure first, before trusting a result.

Covered so far: the actor-type classifier (`core/ActorTypeClassifier.h`, with
`core/TargetType.h`) against all 539 rows of `race_map.csv`, 20 actor-keyword
cases and made-up races that pin each rule part (`ActorTypeClassifierTests.cpp`;
it reads the CSVs through `HUGINN_REPO_ROOT`); the slot class cap's arithmetic
(`core/SlotClassCapMath.h`, the pattern
port, checked bit for bit against the loop it replaced) and `core/RingBuffer.h`.

---

## 1. How Huginn's in-game tests run

Beyond `huginn_core_tests` (section 0), there is no host binary for the game
code. `cmake --preset vs2022-windows` prints `Build Tests: OFF`, but that line
comes from a *dependency*: Huginn's root `CMakeLists.txt` does
`set(BUILD_TESTS OFF)` to suppress CommonLibSSE-NG's own test target, and
CommonLibSSE-NG echoes the value back. It says nothing about Huginn's tests.

**`src/Tests.cpp` is compiled into the plugin DLL in Debug configurations
only, and its suites run inside the game.**

- `src/CMakeLists.txt` marks `Tests.cpp` `HEADER_FILE_ONLY` for every non-Debug
  config, so Release does not even parse it.
- Every suite body is additionally wrapped in `#ifndef NDEBUG`.
- The `#ifndef NDEBUG` guard around the `RunUnitTests()` call site in
  `src/Main.cpp` **must stay in sync** with that `HEADER_FILE_ONLY` property — a
  config that leaves `NDEBUG` undefined while excluding `Tests.cpp` produces an
  unresolved external. There is a comment saying so at `src/Main.cpp:677`.

### Running them

```sh
# Build Debug and deploy (the POST_BUILD copy writes to $CompiledPluginsPath)
cmake --preset vs2022-windows && cmake --build build --config Debug
```

Then launch Skyrim. Two trigger points:

| When | What runs | Where |
|---|---|---|
| **`kDataLoaded`** (main menu, once per process) | `RunUnitTests()` | `src/Main.cpp:682` |
| **`kPostLoadGame`** — loading a save, **not** a new game | the 18 after-load suites | `InitializeGameSystems()`, `src/Main.cpp:448–465` |

The second group is gated on `!isNewGame` because it needs real form data
(spells, items, weapons) in the player's inventory. Starting a new game runs
none of it.

There is **no console command to re-run the tests.** `hg rebuild` rebuilds
registries, not tests. To re-run, reload the save.

### 1a. Counts, the sentinel, and the unattended run

Each suite runs through `TestHarness::RunSuite` (`src/TestHarness.h`): a suite
**failed** if it logged at error level or above on its own thread (how every
suite reports a failure) or threw; **skipped** if it called
`TestHarness::MarkSkipped(reason)` and logged no error; else **passed**. Every
early return or skipped block in `Tests.cpp` calls `MarkSkipped` next to its
log line (a registry not ready, a slot setting off, too few potions): a new
skip must do the same, or it counts as a pass. One line per suite, then one
per batch:

```
[HuginnTest] suite RunSlotClassCapTest passed (0 error line(s))
[HuginnTest] suite RunHomeKeyTest SKIPPED (0 error line(s); skipped: needs seating, the hold and home keys)
[HuginnTest] RESULT phase=load suites=18 passed=17 failed=0 skipped=1 fail_lines=0 failed_suites=- skipped_suites=RunHomeKeyTest
```

`phase=menu` is `RunUnitTests()` at `kDataLoaded`; `phase=load` is the
after-load batch. These lines are logged in every Debug session.

**A behaviour change in every Debug session (0.23.9):** `RunSuite` catches
exceptions, test mode or not. A suite that throws is logged FAILED and the
next suite runs; before, the exception went up into the SKSE message handler.

**Test mode** (Debug only, off unless asked for) makes the run unattended:
after the main-menu suites Huginn loads a named save, runs the after-load
suites, logs

```
[HuginnTest] DONE result=PASS suites=19 passed=19 failed=0 skipped=0 fail_lines=0 failed_suites=- skipped_suites=- reason=-
```

and ends the process (exit code 0 on PASS, 1 on FAIL). `reason` is `-`,
`load-failed` (kPostLoadGame reported failure), `load-timeout` (no load within
`iLoadTimeoutSec`, e.g. a misspelt save) or `no-ui`/`no-save-manager`. Turn it
on with either:

- a one-shot `Huginn_TestMode.ini` in the SKSE log folder (what the runner
  uses; Huginn deletes it when it reads it, and ignores it past `iExpiresUnix`):
  ```ini
  [Test]
  bEnabled=1
  sSaveName=HuginnTest      ; no .ess; empty = main-menu suites only
  iExpiresUnix=1791500000   ; optional
  iLoadTimeoutSec=300       ; optional
  ```
- or `HUGINN_TEST_MODE=1` (and `HUGINN_TEST_SAVE=<name>`) in the game's
  environment. MO2 hands a shortcut to an already-running MO2, which launches
  with its own environment, so the file is the reliable way.

The save is loaded with `RE::BGSSaveLoadManager::Load(name, checkForMods=false)`
on the main thread, 3 s after the main menu opens: no missing-plugins dialog
to block an unattended run, and no console or Papyrus round trip. The name is
the file name without `.ess` (proven on simonrim, 2026-10-08; Huginn strips a
`.ess` if one is given).

**The runner**, `tools/ingame/run_tests.py`:

```sh
python -I tools/ingame/run_tests.py                     # simonrim, HuginnTest.ess or the newest save
python -I tools/ingame/run_tests.py --save HuginnTest   # a named save (no .ess)
python -I tools/ingame/run_tests.py --list lorerim      # LoreRim-5, profile Ultra, executable LoreRim
python -I tools/ingame/run_tests.py --no-save --dry-run # check, print the MO2 command, launch nothing
```

Before it writes or launches anything it refuses (exit 2) unless:

- the list's `overwrite/SKSE/Plugins/Huginn.dll` contains the harness strings
  (`Huginn_TestMode.ini`, `[HuginnTest] DONE`). A Release build or a Debug
  build older than 0.23.9 lacks them and would only time out. It prints the
  DLL's MD5 either way;
- `SkyrimSE.exe` is not running;
- no `ModOrganizer.exe` is running. The shortcut would be handed to it, and an
  MO2 of the same instance open on another profile would launch that profile.
  `--multiple` passes MO2's unsupported `--multiple`, and is still refused if
  a running MO2 is this instance's or its path cannot be read;
- the MO2 executable title, the profile and the save exist.

Then it launches `ModOrganizer.exe -p <profile> "moshortcut://:<executable>"`,
waits for a `_Huginn_Debug.log` started by this launch (the first line's UTC
launch stamp and the file's mtime), fails at once when Huginn ran its suites
without seeing the flag, waits for `DONE` (`--timeout`, default 600 s), and
prints each suite's result. A game that exits counts as a crash unless this
launch's log holds a `DONE`. Whatever starts the game closes it: when the run
ends, whatever the verdict, any `SkyrimSE.exe` still running 15 s later is
killed (none was running at launch, so it is the launch's; after `DONE` Huginn
has normally ended it already), and the `ModOrganizer.exe` the runner started
is closed if still open (asked first, then `/F /T` after 30 s). Exit 0 = PASS; 1 = a failed or skipped suite
(`--allow-skips` accepts skips), timeout, crash or unread flag; 2 = refused.
It reads MO2's config and never writes it, and it does **not** deploy the DLL:
copy the Debug `Huginn.dll`/`.pdb` into the list first (simonrim:
`overwrite/SKSE/Plugins/`, by hand). Make a dedicated save once, in game:
console, `save HuginnTest`. Agents launch the game only when the user has said
the machine is free.

### Reading the results

Tests report to the log — Debug builds write `_Huginn_Debug.log` in CommonLibSSE's
`log_directory()` (see `OpenLog()`, `src/Main.cpp:864`). **A failing test logs an
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
terminal marker is as much a signal as an explicit `TEST FAIL` line. Skips
(data a save may not have, settings that are off) log a line and call
`TestHarness::MarkSkipped`; the `[HuginnTest]` lines of section 1a count them.

---

## 2. Suite inventory

### 2.1 Runs at `kDataLoaded` (no save data needed)

**`RunUnitTests()`** — `src/Tests.cpp:1484`. Terminal marker:
`=== All unit tests passed! ===`.

| Group | Contents |
|---|---|
| GameState hash | Test 1 minimum hash, Test 2 maximum hash, Test 3 uniqueness across all `GameState::kTotalStates` = 48,384 states (6×6×3×7×4×2×2×2×2), Test 3b stamina is excluded from the hash, Test 3c allyStatus hashes as its injured bit — None == Present, and Injured differs, Test 3d the distance bucket is the closest living hostile's (a townsperson leaves it Ranged, a corpse is ignored), Test 3e target type is seen only for a living hostile primary |
| SpellRegistry | Registry starts empty (basic construction; the real coverage is the integration suite) |
| PriorCalculator context independence | Tests 1–8: healing and damage priors identical in/out of context; magnitude, scarcity, spell cost, weapon charge, ammo matching and scroll magnitude *do* affect the prior. This is the guard on the `ContextRuleEngine` / `PriorCalculator` separation |
| Optimization + engine | Test 1 partial-sort correctness, Test 3 `SCOPED_TIMER` compiles and runs, Tests 4–9 `ContextRuleEngine` vital / elemental / environmental / combat / target / equipment rules, Test 10 end-to-end `ContextRuleEngine` → `UtilityScorer` (subtests 1a/1b/2–5: forge, enchanter, resist-fire, healing at 30% HP, AOE damage, soul gem), Test 11 `TargetCollection` cache invariant, Test 12 `PipelineStateCache` rank clamping, Test 13 `EquipSourceTracker` FormID keying, Test 14 `UsageMemory` snapshot reader, Test 15 dedup equivalence (`IsFavorited`, fortify-school parity), Test 16 `FeatureBanditLearner` batch decay, Test 17 `ContextReason` derivation, and an unnumbered wildcard-page-cache block |

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
| `RunCosaveTests()` | `Tests.cpp:4980` | 4 tests: `FeatureBanditLearner` export/import round-trip, empty round-trip, import clears existing data, feature-count migration (pad / truncate / equal / reject) |
| `RunStateFeaturesTests()` | `Tests.cpp:872` | 8 tests: default state, low-health combat, one-hot correctness across all 7 target types, distance normalisation, `ToArray` round-trip, normalisation bounds, no-enemy fallback, vital clamping |
| `RunFeatureBanditLearnerTests()` | `Tests.cpp:1199` | 8 tests: cold start, convergence, weight interpretability, regularisation prevents explosion, weight clamping, generalisation across states, item independence, `Clear()` |

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

The learning system is a linear contextual bandit — see
[../architecture/4-contextual-bandits.md](../architecture/4-contextual-bandits.md).
Suite names above use the real identifiers.

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
| **Console commands** for manual poking (`hg refresh`, `hg recs`, `hg status`, `hg weights`) | [Console commands](https://github.com/LandingCrew/huginn/wiki/Console-Commands) |

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
- No gtest dependency, no `HuginnTests.exe`. (`tests/` exists since v0.23.9:
  it holds `huginn_core_tests`, the doctest host target of section 0.)
- No `docs/testing/baseline_data/`, `refactor_data/` or `reports/`
- No `docs/refactor/staged-implementation.md`, no `docs/reviews/SESSION-SUMMARY.md`,
  no `docs/testing/performance-issues.md`
- No `docs/ROADMAP.md` — the roadmap is [../roadmap.md](../roadmap.md), with
  [../roadmap-archive.md](../roadmap-archive.md) beside it
- No `huginn.reload` console command — the command is registered as `Huginn`
  with the short alias `hg`, so the syntax is `hg reload`; see [Console commands](https://github.com/LandingCrew/huginn/wiki/Console-Commands)

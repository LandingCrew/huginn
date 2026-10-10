# Huginn Testing Index

**Applies to:** v0.19.x (verified against v0.19.10); sections 0 and 1a added
for v0.23.9 (R1, host tests and the unattended in-game run); the selection log
v3 tests and `--decision-session` for v0.23.15 (R4)
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

| Test | What it pins |
|---|---|
| `RingBufferTests.cpp` | `core/RingBuffer.h`: starts empty, insertion order until full, overwrites the oldest, `pop_front`, `clear` |
| `SlotClassCapMathTests.cpp` | `core/SlotClassCapMath.h`: the free allowance, one more discount per item, a discount of 1 or more (and NaN) turns the cap off, a negative one clamps to a hard cap, bit for bit against the loop it replaced, the early stop |
| `SlotScoreMathTests.cpp` | `core/SlotScoreMath.h` (R7, sign-safe slot scores): the bridge ln(utility) and its inverse, the class cap as ln of the old factor (it lowers a negative score), the hold's margin as a difference, tie epsilon, sentinels, churn buckets |
| `SlotSnapshotIOTests.cpp` | `core/SlotSnapshotIO.h`, the slot snapshot text format: names survive escaping, read-back equality, the item dictionary across snapshots, malformed text refused with a line number |
| `SlotAllocGoldenTests.cpp` | R7 golden: `core/SlotAllocCore.h` with the old arithmetic reproduces the recorded `fixtures/slots/*-old.txt` pages; the new game code's `*-new.txt` pages replay; old and new arithmetic give identical pages on recorded and synthetic snapshots, differing only at float-rounding boundaries; the full sort characterised |
| `SlotAllocAdversarialTests.cpp` | R7 named cases under both arithmetics: ties keep list order, zero/denormal utilities rank last, the class cap with a tie, overrides and pinned vitals, remembered-only rows, the job-key pull, past the sorted prefix, a challenger on the hold's margin, rounding boundaries, negative scores |
| `SignatureDedupTests.cpp` | `core/SignatureDedup.h` (0.23.16), the inventory duplicate warning's dedup: once per (key, signature), again when the signature changes; 8 threads released together on a latch, each inserting 4000 keys of its own while all hit one shared key: the shared key warns once and no key is lost (0.23.17). With the lock removed it crashes (heap corruption / SIGSEGV), so it relies on the CTest TIMEOUT (`tests/CMakeLists.txt`) if a run ever hangs instead |

**The effect extractor (R2, 0.23.12).** `core/EffectRules`, `core/EffectMapper`,
`core/MiniRegex`, `core/CrossFeatures.h`:

| Test | What it pins |
|---|---|
| `EffectColumnsTests.cpp` | `core/EffectColumns.h` equals `docs/architecture/9-data/effects.csv` (ids, order, levels, families); every rule pattern compiles |
| `EffectFixtureTests.cpp` | rows sampled from three `hg dump all` CSVs (`tests/core/fixtures/effects_{vanilla,simonrim,lorerim}.csv`): each effect row's column, each item's scope and its effect columns; named LoreRim food (0.23.16): raw vs roasted Mammoth Snout (hunger size, cooked riders, Weak Stomach as `self_harm_stamina`), Apple Pie and Honey Nut Treat (`fortify_vital_magicka`, not a restore), Strange Meat |
| `MiniRegexTests.cpp` | the regex subset; and `fixtures/regex_oracle.csv`: every rule pattern against Python's `re` on real names, descriptions and keyword lists (match, span, group 1) |
| `EffectMapperTests.cpp` | values, percentiles, sentinels, the hidden-row whitelist, payloads, scope, item features, cross-features, the deviations from the reference extractor; the side-effect rule (0.23.16: harm rows on food and potions to `self_harm*`, poisons and spells untouched, `hostile` and the primary row) |

The fixtures' expectations were written by the Python reference extractor
that measured doc 9's coverage (adapted to the effects.csv names and to the
deviations listed in `core/EffectRules.h`, each flagged in `expectNote`), not
by the C++. Changing a rule table means regenerating `regex_oracle.csv` (the
test fails until then: it checks every pattern is covered); changing a
mapping means the fixture expectations must be re-derived the same way and
the change listed in `EffectRules.h`. The generators are in `tools/effects/`
(`make_fixtures.py`, `make_regex_oracle.py`; the reference extractor in
`tools/effects/reference/`; `compare_mgef.py` compares C++ and reference on
every MGEF of a dump); run them with `python -I`. They need the dumps, which
are user data and not checked in; re-running them on R2's dumps reproduces
the checked-in fixtures byte for byte.

**The need vector (R3, 0.23.14).** `core/ResponseCurve.h`, `core/NeedIds.h`
(generated from `needs.csv`), `core/NeedSnapshot*`, `core/NeedEvaluator.*`,
`core/NeedSensorMath.h`, `core/DropAhead.h`, `core/TargetFamilies.h`:

| Test | What it pins |
|---|---|
| `NeedIdsTests.cpp` | `core/NeedIds.h` equals `needs.csv` (ids, order, group, priority, default curve, deferred flag); every need has an input |
| `ResponseCurveTests.cpp` | each curve kind against hand-worked values; [0,1] for any input; parse/format round trip; bad INI values refused |
| `NeedEvaluatorTests.cpp` | named cases for the `r3_input` formulas (gates, NEVER, families, ammo by launcher...), the 0.05 signature, `Advance`, `TimeDriven`, the text record |
| `NeedSensorTests.cpp` | drop-ahead geometry (points, direction, the 20 units/s threshold, cliff, slope, no hit, bridge over water, wading; unknown probes: uphill, a wall, stairs, a crowd); water (0.23.19): deep water a safe landing and shallow water a drop to its surface, the deepest water ahead (`MeasureAhead`: a cliff into the sea, a stream, a lake shore, no hit over water, water only below the ray's bottom, unknown probes, the `kSafeLandingDepth` edge); the 16000-unit down ray (0.23.22) and the LoreRim sea cliff of 2026-10-10 it now reaches (the 4000 ray read a 3936 void there); the probe sequence (`ProbeAll`) over a scripted world: the picks it casts, a cliff, a parapet blocked at the knee, an invisible wall, walkable slopes of 5-30 degrees, a crest then a cliff, a step up, stairs, a crowd; the teleport test, the decaying sum, the time-to-kill estimate; the drop reading's unpaused age (0.23.16); the bench kind by workbench keyword (0.23.16) |
| `WaterSelfCheckTests.cpp` | the drop-ahead probe's water self-check (0.23.23, `core/WaterSelfCheck.h`): a plane more than 150 units over the feet of a dry, grounded player contradicts the engine (the false water of 2026-10-10 11:23: 234 and 364), swimming / under water / airborne / mounted / no water never do; the 0.5 s hold (fires once; a break, another plane, a gap over 0.5 s restart it; a slow frame does not); the per-load blacklist (cell + height within 4 units, Known / Full, removal after a swim proves a plane real); the swim check against the engine's water (the 13:51 cave pool, -1040 both) |
| `TargetFamiliesTests.cpp` | the multi-hot family reading on all 539 `race_map.csv` rows (family \| also) and named cases |
| `NeedFixtureTests.cpp` | every snapshot in `tests/core/fixtures/needs/*.txt` gives the vector in its `.expected.csv` |
| `ObviousPairsTests.cpp` | the obvious need x effect pairs (`effects.csv` `obvious_needs`, `needs.csv` `obvious_effects`): `cold` pairs with soups only (`survival_warmth`), never with Resist Frost, warm apparel or warming spells, and `warmth_deficit` answered by warm apparel, warming spells and warm food, never by Resist Frost (0.23.17); `diseased` never with Resist Disease (0.23.16); `deep_water_ahead` answered by Waterbreathing (0.23.19); every pair names a need; doc 9's count (264) |

The fixtures' expectations come from `tools/needs/expected_vectors.py`, an
oracle written from `needs.csv`, `NeedSnapshot.h` and the curve formulas by an
agent that did not see the evaluator; regenerate an expectation only with it.
`synthetic.txt` (67 snapshots) makes every need with a sensor fire;
`captured_vanilla.txt` (84: the monitor's with a measured `dropAhead`, the suite's two -1 as the hook starts after the suites) was recorded in game with
`run_tests.py --capture-needs 400 --capture-slots 90` (Debug, test mode;
`Huginn_NeedSnapshots.txt` in the log folder). After editing the csv's curve
or input columns: `python -I tools/needs/make_need_ids.py` (header),
`--ini` (paste into `configs/Huginn.ini`; `--check` verifies both), then the
oracle on every fixture. `drop_ahead`'s curve is fitted from a log, not set by
hand: `python -I tools/needs/fit_drop_curve.py <_Huginn_Debug.log> [--exclude
HH:MM:SS or "YYYY-MM-DD HH:MM:SS"] [--bootstrap N]` pairs each fall with the
damage after it (the measured `[Falling] landed` hp loss when logged, 0.23.23,
else the peak `health_falling`), leaves out falls near combat or healing, and
prints the table and the fit, with a WARNING when the optimum is on the grid's
edge (0.23.22: centre 466, slope 0.029 from the LoreRim session of 2026-10-10
with `--exclude 12:08:36`; the 0.23.23 script gives 465 / 0.028 there).

**The selection log v3 (R4, 0.23.15).** `core/DecisionLog.*` (the records and
their text form), `core/NeedEpisodes.h` (when a need episode starts, ends and is
answered); schema: [9-selection-log-v3.md](../architecture/9-selection-log-v3.md):

| Test | What it pins |
|---|---|
| `DecisionLogTests.cpp` | the JSON helpers (escaping, numbers, FormIDs); caps and contexts defined once per segment, content-addressed; a synthetic session (key, menu with an added row, a co-pick sharing the context, nothing, wheel, a second segment) equals `fixtures/decisions/synthetic_v3.jsonl` byte for byte (with `heldFull` and `preEquipped`); the run gate that keeps a previous save's cached run out of a context after a load (two loads in one launch) |
| `NeedEpisodesTests.cpp` | onset 0.5 / expiry 0.25 hysteresis, the 1 s minimum, a selection inside answers and one outside does not, the grace, the 0.5 s slack for a press that ended its episode, payloads, reset |
| `tools/replay/test_replay_v3.py` (CTest `replay_v3_roundtrip`) | `replay.py`'s v3 reader decodes the golden file to the values the C++ test wrote, round-trips a session written by an encoder of its own (plain and gzip), refuses malformed files, skips a torn line (and what depended on it) and resyncs at the next head, and never raises on damage (torn head, a line torn inside a UTF-8 character, a truncated or corrupt gzip, NUL padding) |

After a deliberate format change: run the tests, copy
`<build>/tests/synthetic_v3.actual.jsonl` over the fixture, update the Python
test's expectations (they are copied from `DecisionLogTests.cpp`, not from the
file), and bump the version if the change is not an addition (schema doc,
"Versioning").

**Coverage on a whole load order: `huginn_effect_report`** (a host tool built
with the tests, not run by ctest: it needs a dump, which is user data):

```sh
cmake --build build --config Release --target huginn_effect_report
build/tests/Release/huginn_effect_report.exe <Huginn_All.csv> --min-coverage 99 --diff-out diff.csv
```

It prints coverage (visible effect rows mapped, helper rows and wrappers with
nothing to read not counted) twice -- companion carrier rows left out, and
counted as unmapped (`--carriers-out` lists every carrier for audit) -- the
regex step-budget hits, coverage by route and kind, the top unmapped effects, the
coverage diff (in-scope items that have a slot class today and no description
in cap(i); the class comes from the dump's `slotClass` column, or `--classes`
for an older dump), and on a 0.23.12 dump a check that the in-game catalog
equals what the host mapper makes of the same rows (exit 1 if not).

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
| **`kPostLoadGame`** — loading a save, **not** a new game | the 19 after-load suites | `InitializeGameSystems()`, `src/Main.cpp` (the `HUGINN_RUN_SUITE` list) |

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
`iLoadTimeoutSec`, e.g. a misspelt save) or `no-ui`/`no-save-manager`.

Test mode may change the loaded game, because it never saves (the process is
ended after `DONE`). One suite uses that: `RunItemRegistryTests` gives the
player enough single-effect healing potions for its magnitude-sort check when
the save holds fewer than two, and takes them back when the suite ends. In a
normal Debug session it still skips that check (and the suite reports
SKIPPED).

Turn it on with either:

- a one-shot `Huginn_TestMode.ini` in the SKSE log folder (what the runner
  uses; Huginn deletes it when it reads it, and ignores it past `iExpiresUnix`):
  ```ini
  [Test]
  bEnabled=1
  sSaveName=HuginnTest      ; no .ess; empty = main-menu suites only
  iExpiresUnix=1791500000   ; optional
  iLoadTimeoutSec=300       ; optional
  sDumpAll=Huginn_All_x.csv ; optional (0.23.12): `hg dump all` to this file
                            ; in the log folder after the save's suites (needs a save)
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
python -I tools/ingame/run_tests.py                     # vanilla+ (simonrim instance), HuginnTest.ess or the newest save
python -I tools/ingame/run_tests.py --save HuginnTest   # a named save (no .ess)
python -I tools/ingame/run_tests.py --list simonrim     # the simonrim instance's "Simonrim Essentials" profile
python -I tools/ingame/run_tests.py --list lorerim      # LoreRim-5, profile Ultra, executable LoreRim
python -I tools/ingame/run_tests.py --no-save --dry-run # check, print the MO2 command, launch nothing
python -I tools/ingame/run_tests.py --dump-all Huginn_All_vanilla.csv   # also write hg dump all (0.23.12+)
python -I tools/ingame/run_tests.py --capture-needs 400 --capture-slots 90  # record need snapshots (0.23.14+)
python -I tools/ingame/run_tests.py --dump-recs 8      # `hg recs 40` after 8 idle s, then end (0.23.14+)
python -I tools/ingame/run_tests.py --coc "RiverwoodSleepingGiantInn;Riverwood"  # cross cells, then end (0.23.14+;
                                                      # a coc that fails fails the run; not with --capture-slots)
python -I tools/ingame/run_tests.py --decision-session # the selection log v3 session, then end (0.23.15+)
# Every test-mode run that loads a save (0.23.14+) fails unless the drop-ahead probe took a measured reading
# (DropAheadProbe::MeasuredCount); a plain run waits up to 10 s for one.
```

`--decision-session` (R4) plays a scripted session after the load suites
(`learning/SelectionLogV3Session.cpp`): health dropped to 30% for 4 s with
nothing pressed (a `nothing` record), wildcards forced on, three Huginn keys
pressed (`key`), an item of the page equipped and Huginn's own Wheeler
activation handler run (`wheel`; Wheeler's UI is not driven; skipped when
Wheeler is not connected), the inventory opened and an unworn armour piece and
an off-page weapon (or potion) equipped from it (`menu`, the armour one
`learned: 0`), the menu closed. It writes `Huginn_Selections_v3_test.jsonl`
(never the player's `Huginn_Selections_v3.jsonl`), logs
`[HuginnTest] decision session: key=.. wheel=.. menu=.. nothing=.. ...` with
the sizes and the tick and context costs, and fails the run when a key, menu or
nothing record (or a wheel record after a wheel pick) was not **written**, or
the writer did not drain: the reason reaches the DONE line
(`reason=decision-no-nothing`, `decision-flush-timeout`...), as a failed `coc`
does. The runner then decodes the file with `tools/replay/replay.py` and fails
on any damage count (unreadable lines, skipped records, lost heads, a truncated file) or a missing outcome on its own. (Proven by a local build that
dropped every `nothing` record: `DONE result=FAIL ... reason=decision-no-nothing`,
and the runner's own check on that file: `no nothing record in the v3 file`.) The menu sink's
`[SelectionV3] InventoryMenu was open ... update tick(s) inside` line is the
measurement of whether the update loop ticks in menus. Read the file with
`python -I tools/replay/replay.py --v3 <file>`.

`--dump-all NAME` (a plain file name) makes Huginn write `hg dump all` into the
SKSE log folder after the save's suites (so after the keyword distributors and
the catalog's build); the runner prints the
`[EffectCatalog]` and `dump all` lines. Use a name of its own: the log folder
may be shared by several lists, and `Huginn_All.csv` is the player's own dump.

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
launch's log holds a `DONE` (taken at once when found). A game up for
`--log-start-timeout` (90 s) without this launch's log fails fast. The log
folder's Documents part comes from the Windows known-folder API, so a
OneDrive-redirected Documents is found.

The game it tracks is its own only: an image named `SkyrimSE.exe` under the
chosen instance, created after the launch (5 s of slack for a clock step),
held open by handle from first sight (so PID reuse cannot redirect the
kill). Whatever starts the game closes
it: when the run ends, whatever the verdict, every game of its own still
running 15 s later is ended through that handle (after `DONE` Huginn has
normally ended it already), and the `ModOrganizer.exe` the runner started is
closed if still open (asked first, then `/F /T` after 30 s). Throughout, and
for 20 s after MO2 is gone (3 minutes at most), it keeps scanning for and
ending games of its own, so a game MO2 was still starting when the run ended
(`--timeout 0`) is not orphaned; `/T` cannot reach it once its parent
`skse64_loader` has exited (a game appearing more than 20 s after MO2 is gone is
not caught; nothing of the run is left to start one). Another instance's game is
never touched. The shutdown takes up to about 3 minutes and prints its
progress; a Ctrl+C during it is noted and the shutdown finishes first.

**Do not launch anything from the same instance while the runner runs.** A
game you start from it -- the runner's MO2 window, the instance's MO2
shortcut, or its Stock Game `skse64_loader` -- cannot be told apart from the
runner's and will be ended, and the last stage of closing MO2 (`taskkill /F
/T`) ends everything started from that MO2 window, xEdit included.

Exit 0 = PASS; 1 = a failed or skipped
suite (`--allow-skips` accepts skips), timeout, crash or unread flag;
2 = refused.
It reads MO2's config and never writes it, and it does **not** deploy the DLL:
copy the Debug `Huginn.dll`/`.pdb` into the list first (simonrim/vanilla+:
the instance's `overwrite/SKSE/Plugins/`, by hand). Make a dedicated save once, in game:
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

Suites are named by function, not line: `grep -n "^void Run" src/Tests.cpp`
finds each one (line numbers drift with every edit, so this index no longer
carries them). The run order is the `HUGINN_RUN_SUITE` list in `src/Main.cpp`.

### 2.1 Runs at `kDataLoaded` (no save data needed)

**`RunUnitTests()`**. Terminal marker:
`=== All unit tests passed! ===`.

| Group | Contents |
|---|---|
| GameState hash | Test 1 minimum hash, Test 2 maximum hash, Test 3 uniqueness across all `GameState::kTotalStates` = 48,384 states (6×6×3×7×4×2×2×2×2), Test 3b stamina is excluded from the hash, Test 3c allyStatus hashes as its injured bit — None == Present, and Injured differs, Test 3d the distance bucket is the closest living hostile's (a townsperson leaves it Ranged, a corpse is ignored), Test 3e target type is seen only for a living hostile primary |
| SpellRegistry | Registry starts empty (basic construction; the real coverage is the integration suite) |
| PriorCalculator context independence | Tests 1–8: healing and damage priors identical in/out of context; magnitude, scarcity, spell cost, weapon charge, ammo matching and scroll magnitude *do* affect the prior. This is the guard on the `ContextRuleEngine` / `PriorCalculator` separation |
| Optimization + engine | Test 1 partial-sort correctness, Test 3 `SCOPED_TIMER` compiles and runs, Tests 4–9 `ContextRuleEngine` vital / elemental / environmental / combat / target / equipment rules, Test 10 end-to-end `ContextRuleEngine` → `UtilityScorer` (subtests 1a/1b/2–5: forge, enchanter, resist-fire, healing at 30% HP, AOE damage, soul gem), Test 11 `TargetCollection` cache invariant, Test 12 `PipelineStateCache` rank clamping, Test 13 `EquipSourceTracker` FormID keying, Test 14 `UsageMemory` snapshot reader, Test 15 dedup equivalence (`IsFavorited`, fortify-school parity), Test 16 `FeatureBanditLearner` batch decay, Test 17 `ContextReason` derivation, and an unnumbered wildcard-page-cache block |

The wildcard-page-cache block (search `wildcardSlots` in `src/Tests.cpp`) is the regression guard
for issue #70 and its two siblings: it pins roll probabilities to 1.0 and the
refractory to 0 so the rolls are deterministic, then asserts per-page bounds — it
asserts bounds, not randomness.

### 2.2 Runs at `kPostLoadGame` (needs a loaded save)

The 19 suites, in run order:

| Suite | What it covers |
|---|---|
| `RunSpellRegistryTests()` | Spell registry against real form data |
| `RunItemClassifierTests()` | Item classification against real forms |
| `RunItemRegistryTests()` | Item registry against real inventory. Its potion-sort check needs 2+ health potions; in test mode it supplies them (section 1a), otherwise it marks the suite skipped |
| `RunWeaponRegistryTests()` | Weapon registry against real inventory |
| `RunMultiplicativeScoringTests()` | 6 tests: zero context gates utility, adaptive lambda vs confidence, learning amplification, correlation compounding, full integration, favorites boost by rank |
| `RunRegressionTests()` | See below |
| `RunCosaveTests()` | 4 tests: `FeatureBanditLearner` export/import round-trip, empty round-trip, import clears existing data, feature-count migration (pad / truncate / equal / reject) |
| `RunStateFeaturesTests()` | 8 tests: default state, low-health combat, one-hot correctness across all 7 target types, distance normalisation, `ToArray` round-trip, normalisation bounds, no-enemy fallback, vital clamping |
| `RunFeatureBanditLearnerTests()` | Feature-based learner: cold start, convergence, weight interpretability, regularisation, clamping, generalisation, item independence, `Clear()`, passed-over updates |
| `RunOverrideNamespaceTests()` | `Huginn_Overrides.ini` section namespacing (`[Spell:...]` / `[Item:...]`) |
| `RunSlotLockerResetTest()` | `SlotLocker::Reset()` clears every `LockedSlot` field (throwaway) |
| `RunSlotLockerInstanceLockTest()` | Per-stack lock breaking (throwaway) |
| `RunSlotSeatingTest()` | Anti-juggling seating; skipped when `bKeepSlotPositions` is off |
| `RunFillJobKeysTest()` | `bFillJobKeysFromRegular` |
| `RunSlotClassCapTest()` | The slot class cap: free items, then x discount per item; food counts as one class |
| `RunSlotClassCapHoldTest()` | The class cap through the slot hold; skipped unless seating, the hold and the cap at 3 free are on |
| `RunHomeKeyTest()` | Home keys: a returner takes its key back; skipped unless seating, the hold and home keys are on |
| `RunBuffElementResistTest()` | A buff element is not a resist (throwaway) |
| `RunNeedVectorTests()` | R3: the live need vector (values in [0,1], logged as `hg needs` prints it), `health_deficit` against the player's health, the drop-ahead probe's state (its rays are cast from the PlayerCharacter::Update hook, which starts after the load; the live readings are in `[DropAhead]` lines and the captured snapshots), the LOS layer's collision set, the encumbrance ratio, and (test mode only, so SKIPPED in an ordinary session) a 30% hit landing in a damage sum that then decays |

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

Recorded in [../roadmap.md](../roadmap.md) under *Follow-ups*.

1. **`Context::WeightForCandidate` is hand-reimplemented in two tests instead of
   being called.** The sites, by their comments in `src/Tests.cpp`: "Extract
   weight using UtilityScorer's GetContextWeight logic" (inside unit test 10)
   and the TC-05 block that simulates `GetContextWeight` with `max()`
   accumulation. Both should call
   `Context::WeightForCandidate()` from
   `src/context/ContextWeightForCandidate.h`, which most of the rest of the file
   already does (18 call sites). `DominantReason` / `ReasonLabel` are covered by
   unit test 17 and are not part of this gap.

2. *(Resolved.)* The cosave decode negative test used to log a real-looking
   `[E] DecodeV2EntryBlob: byteLen 83 != stride 84`. The "length mismatch must
   reject the decode" block now passes `unitTest=true`, so the rejection logs
   at info and names itself a test; it no longer counts as a failure line for
   the harness (section 1a).

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

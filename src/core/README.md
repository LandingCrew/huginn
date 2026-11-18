# src/core -- pure code, host-tested

**The rule for the engine rewrite:** every new piece of rewrite math lives in
`src/core/` and has host tests in `tests/core/`. Response curves, the learner
update, σ_Δ and the challenger rule, the effect mapper over plain records: all
of it. The game layer only reads forms and calls into `src/core/`.

What "pure" means here:

- Standard library only. `cmake/CheckCorePurity.cmake` enforces it on every
  file here whatever its extension (a quoted include can pull in a `.inc`),
  except Markdown:
  - a quoted include must resolve to a file under `src/core/` (against the
    including file's folder, then against `src/`), so `"Globals.h"`,
    `"state/GameState.h"` and `"../PCH.h"` are rejected; and it must name a
    source file (`.h .hpp .hxx .inl .ipp .inc`), so the unscanned Markdown
    (or a `.txt`) cannot be pulled into a translation unit;
  - an angle include must be a C++ standard library header (an allow-list in
    the script), so `<RE/...>`, `<REX/...>`, `<SKSE/...>`, `<spdlog/...>`,
    `<SimpleIni.h>`, `<Windows.h>` are rejected;
  - no backslash in an include path;
  - no `RE`, `REL`, `REX`, `SKSE`, `logger` or `spdlog` followed by `::`
    (spaces or a line break allowed, any column); no `using namespace` of, no
    namespace alias to, and no reopening of `RE`/`REL`/`REX`/`SKSE`, with or
    without a leading `::` (`using namespace ::RE;`, `namespace G = ::SKSE;`,
    `namespace RE {`).

  It runs at configure time and again as a build step whenever a core file
  changes; `huginn_core_tests` and the plugin both depend on that step, so an
  edit made after the configure still fails the build. The scan is textual: a
  comment that names `RE::` or a game header trips it too. Say "the game's
  types" in comments instead.

  **Known limits of a textual guard.** It is a tripwire, not a parser. These
  get past it, and are not chased: an include through a macro
  (`#include HEADER`), `__has_include`, `#pragma include_alias`, a comment
  inside the directive (`# /**/ include`), digraphs and trigraphs, a macro that
  expands to `RE` or `RE::...`, and a namespace reached only through such a
  macro. **The real check is the compile:** `huginn_core_tests` builds every
  core header on its own and every core source, with no PCH, no CommonLib
  and only `src/` on the include path, so anything that actually needs the
  game (CommonLib, the plugin's headers) fails to compile there. The one gap
  in that backstop: the vcpkg include folder reaches the test target through
  doctest, so `spdlog` or `SimpleIni` smuggled in by a macro would compile;
  the guard's angle-include allow-list is the stop for those.
- Do not rely on `PCH.h`. The plugin force-includes it into everything it
  compiles, these files included, but the test target compiles them without
  it, and it compiles every core header on its own (a generated one-line TU per
  header, including it twice), so a header that uses `std::string_view`
  without `<string_view>` fails there. Include what you use.
- Plain inputs and outputs: floats, ints, small structs. A function that needs
  a form takes the few fields it reads, and the game-side caller pulls them out.

Where things go:

| What | Where |
|------|-------|
| The pure code | `src/core/*.h` (and `*.cpp` if it needs one) |
| Its host tests | `tests/core/*Tests.cpp` (doctest) |
| The game-side caller | wherever it was; it calls `Huginn::Core::...` |

New code goes in `Huginn::Core`. Code moved here whole keeps its namespace, so
its callers do not change: `ActorTypeClassifier.h` (from `src/state/`, 0.23.9)
and `TargetType.h` (from `state/GameState.h`, which includes it) stay in
`Huginn::State`.

Both targets compile `src/core/`: the plugin through its `GLOB_RECURSE` over
`src/`, and `huginn_core_tests` directly. Tests live outside `src/` so the
plugin never compiles them.

Build and run (from the repo root, after the usual configure):

```sh
cmake --build build --config Debug --target huginn_core_tests
ctest -C Debug --test-dir build --output-on-failure
```

The executable is `build/tests/Debug/huginn_core_tests.exe`. It exits non-zero
when a test fails; `--help` lists doctest's filters (`-tc="slot class cap*"`).

Adding or removing a file: the globs use `CONFIGURE_DEPENDS`, and with the
Visual Studio generator the first build after the change only re-runs the
configure, still building the old file list. Build a second time (or
reconfigure first) before trusting the result.

The pattern, ported first: `SlotClassCapMath.h` holds the arithmetic of the
slot class cap as it was (`Core::ClassCapFactor`, the d^k multiplier).
`SlotClassCapMathTests.cpp` includes a check that the port matches the old loop
bit for bit, including past the denormal fixed point the product reaches for
discounts above 0.5. Since R7 the game no longer multiplies: the multiplier
is the old arithmetic the golden test replays (below).

The slot allocator (R7): `SlotAllocCore.h` is the whole decision of
`Slot::SlotAllocator` over plain records -- overrides, Remembrance holds, the
slot hold, the fill under the class cap, seating and home keys -- templated on
the score arithmetic. The game runs `LogScorePolicy` (scores of any sign,
`SlotScoreMath.h`); `SlotAllocator` builds the input (`src/slot/SlotSnapshot.*`),
keeps the seating memory and turns the events into logs and telemetry.
`SlotSnapshotIO.*` is the text record of one allocation. The wildcard key
(R10): `SlotAllocCore.h` places the cached wildcard on the page's wildcard key
(`SeatOf`) and draws a new one there; `WildcardPick.h` is the draw's math
(relevance x sd, the mixture with a uniform share, P(roll), the exact
propensity); `WildcardGate.h` is each page's cached wildcard and its clocks
(one roll per refractory window, only the displayed page rolls, combat,
reshape, refusal), which `Scoring::WildcardManager` wraps. The tests:

| File | What it proves |
|------|----------------|
| `SlotAllocGoldenTests.cpp` | the core with the OLD arithmetic (`LegacySlotPolicy.h`) replays what the old game code recorded in play -- pages, seating memory and events, under the shipped [SlotLocker] settings and varied ones -- bit for bit; the sign-safe arithmetic gives identical pages and events on those and on synthetic snapshots (which show only that the two arithmetics agree on a path, not that the path is right); what the new game code recorded replays exactly; recorded names are text |
| `SlotAllocAdversarialTests.cpp` | named cases: ties, zeros, the cap's 2x tie, overrides, remembered-only rows, the sort, negative scores, the hold's tie band (a challenger on the margin holds), and the float-rounding boundaries where the arithmetics can still part (the cap's scan at any discount, underflow) |
| `SlotScoreMathTests.cpp` | the bridge, the cap term, the margin, the sentinels, the churn buckets |
| `SlotSnapshotIOTests.cpp` | the snapshot format round-trips |
| `WildcardTests.cpp` | R10: the draw's math, the wildcard key in the allocation (pinned and never moved by seating, the roll's pool and propensity with seating on and off, name twins kept out), and the gate's rules |

Recorded snapshots live in `tests/core/fixtures/slots/` (`*-old.txt` from the
old code, `*-new.txt` from the new). The old code's were recorded by branch
`r7-capture-old`: b941d90 with only the capture and an event recorder added.
b941d90 is the 0.23.9 decision code except one change it made itself:
ApplySeating and RecordSeating share one clock reading per allocation
(unconditional, Release too) instead of each reading the clock. To record more: a Debug build deployed to
simonrim, then `python -I tools/ingame/run_tests.py --capture-slots 160`, then
`python -I tools/slots/trim_snapshots.py <log folder>/Huginn_SlotSnapshots.txt
tests/core/fixtures/slots/<name>.txt --max 150`.
The effect extractor (R2): `EffectRules` classifies a magic effect,
`EffectMapper` maps and grades items into cap(i), `CrossFeatures.h` holds the
runtime features. The rule tables are regular expressions run by `MiniRegex`,
a small backtracking matcher for the subset the tables use (Python `re`
semantics on bytes; `regex_oracle.csv` is Python's answer for every table
pattern on real names and descriptions). Known limits: a search has a step
budget (4M instructions) past which it answers "no match" and is counted
(`MiniRegex::BudgetExceeded`, logged by the catalog); and it is still slower
than Python's `sre` on some nested quantifiers -- `.{0,40}.{0,40}.{0,40}z` on
20k characters took 16 s against Python's 1.8 s before the budget, and
`(a|b)*c` on 100k characters 270 s, which the budget now stops. No rule
pattern has that shape; the whole LoreRim classification runs in seconds.

The need vector (R3): `ResponseCurve.h` (the curve kinds), `NeedIds.h`
(generated from `needs.csv` by `tools/needs/make_need_ids.py`),
`NeedSnapshot.h` (the plain record of every reading a need is computed from)
and `NeedSnapshotIO` (its text form), `NeedEvaluator` (the inputs of
`needs.csv`'s `r3_input` column, the skip-gate signature, `TimeDriven`),
`NeedSensorMath.h`, `DropAhead.h` and since 0.23.50 `EdgeApproach.h`
(time_to_edge and edge_approach from the probe passes) (the sensors'
arithmetic; the game side only reads forms and casts rays), `TargetFamilies.h` (the multi-hot family
reading), and since 0.23.41 `TargetTracking.h` (when a tracked actor leaves
the list, in seconds, and the player's side), `LightHold.h` (the light the
darkness need reads, held against flicker) and `EffectSourceLog.h` (the
transition lines naming the effect behind drain_health and regen_suppressed).
Tests and the replayed fixtures: docs/testing/TESTING-INDEX.md section 0.

A diagnostic (0.23.44): `HandModelCheck.h` (whether the drawn weapon in a hand is
on the skeleton, judged by pose, and when a missing model is worth a log line;
the game side is state/HandModelProbe).

The effect catalog's late entries (0.23.41): `LateCatalog.h` (which held forms
the catalog maps after its build -- player-made potions, spells no tome
teaches -- and the per-load entry table).

The selection log v3 (R4): `DecisionLog` (the records -- context, rows,
decision -- and the encoder that turns them into JSON Lines, defining each cap
and context once per file segment) and `NeedEpisodes.h` (when a need episode
starts, ends and is answered: the "nothing pressed" outcome; since 0.23.51 an
open episode is also cut into 60 s stretches of active time, each judged the
same way: the laps).

`WeaponLoadout.h` (0.23.51): which two-handed weapon is a quarterstaff (by
keyword: the engine types Requiem / LoreRim quarterstaffs as warhammers), so
the need vector reads it as `loadout_quarterstaff` instead of
`loadout_two_handed`. The game side
(`learning/SelectionLogV3*`) builds the records and writes the lines on a
background thread. Schema: docs/architecture/9-selection-log-v3.md.

The new model (R8.2, 0.23.56, in shadow then; it ranks since R8.3, 0.24.0):

- `MiniJson.*`: a strict JSON reader in MiniRegex's pattern (vcpkg.json has
  no JSON library and this folder is standard library only). Numbers parse with
  `std::from_chars` (correctly rounded: the same double Python's `float()`
  reads); strings, arrays and objects keep their contents behind one pointer,
  so the bootstrap's ~70k numbers own no container (0.7 s to 0.12 s in a
  Debug build).
- `Bootstrap.*`: the reader of the R8 bootstrap file, format version 1
  (docs/architecture/9-bootstrap-format.md): reader rules 1-10 exactly as
  `tools/fit/bootstrap.validate` checks them; names mapped to the build (an
  unknown name skipped, a missing pair at its effects.csv prior from
  `ObviousPairs.h`, the same for recent rows, u0 and curves); items resolved
  through the caller's resolver with the kind and name cross-check;
  `[Learning] sBootstrapBias` (`UseFileBias`); the stable keys the learner's
  entries carry (`StableKey`: a hash of names, never a position).
- `ObviousPairs.h`: GENERATED (`tools/effects/make_obvious_pairs.py`) from
  effects.csv's `obvious_needs`, with fitlib's signs.
- `ChoiceScore.h`: the score -- v = theta^T n once per tick, mu_i over a(i)
  (graded, presence, presence+strength), b, the features, theta_covered,
  kappa off the page, sigma_i^2 under the diagonal approximation, the
  explanation (the largest theta n cap term), doc 11's greedy upgrade gate,
  and the live recent_<family> tracker.
- `ChoiceUpdate.h`: the update in fitlib.Model.info's form (precision +=
  n_k^2 Var_p, step = variance), fitlib.build's choice set with Plackett-Luce
  removal, the per-item b moves, the b battery, the onset counter.
- `Battery.h`: the useful-life curve, moved here from FeatureBanditLearner
  (which calls it with the same arithmetic) and shared with b.
- `Sha256.h`: the bootstrap's digest for the v3 head.
- `ChoiceCosave.*`: the cosave records THTA (theta by stable key), BIAS (b
  per item) and DISC (the discovery boost's sets, 0.24.14) as byte blobs, and
  the load's merge of THTA onto the bootstrap.
- `DiscoveryBoost.h` (0.24.14): a newly gained spell or item reads b's
  variance x m(age) in the wildcard draw only (m decays by a half-life of
  play, ends at the first pick); the tracker of what the character has held
  (present at a load: never boosted) and what it gained since.
- `LearningClock.h` (0.24.22): the one learning clock both learners and the
  resets tick (saved in HCID), the table of which learning events tick it
  (what play taught and the resets; not the forget sweep, whose erasures
  re-occur on their own), and the load's keep-or-replace decision
  (`DecideLoad`).

Their tests: `MiniJsonTests.cpp`, `BootstrapTests.cpp` (the golden fixtures, a
negative case per reader rule, the prior fill), `ChoiceModelTests.cpp` (the
score per grading, kappa, the explanation, recent and its exclusions, covered
and the gate, the update, Plackett-Luce, onsets, the battery, the bias modes),
`DiscoveryBoostTests.cpp` (the boost's math and decay, the tracker's rules
-- an item present at load gets no boost --, the DISC codec),
`LearningClockTests.cpp` (ticks under threads, the table, every decision,
the choice-only-change regression, a load's steps adding no ticks, a forget
sweep adding none, a reset holding over an older save),
`ChoiceReplayTests.cpp` (fitlib's per-row logits under the shipped bootstrap
to 1e-5, from `tools/fit/export_scores.py`'s fixture; the real log through
`HUGINN_REPLAY_FIXTURE`), `ChoiceCostTests.cpp` (the parse and one 500-row
pass, reported), and `LegacyFloorPassTests.cpp` (the old floor loop moved to
`learning/legacy/LegacyLearnFeature.h` builds R8.1's list bit for bit; that
header is not core, but it is a template over plain types and compiles here).

The classifiers retired from the live engine (R11, 0.24.4):

- `CapFacets.h`: the facets -- each yes/no question the live engine asks of an
  item ("restores health?", "for someone else?", "has a side effect?"), read
  off the nonzero columns of cap(i) -- and the 24 slot classes' `Matches` and
  `Classify` order over them. The slot classes, the candidate filters, the
  override finders, the label and the widget styling all ask here; the game
  side is `effect/Facets.h`.
- `SlotClassification.h`: the slot class enum, moved whole from
  `slot/SlotConfig.h` (namespace kept) so `CapFacets.h` can name it.
- `OverrideFiles.h`: the effect override directory (doc 9's layer 2) --
  parsing a `<plugin>|<local FormID> = <column>` line, the file order (the
  `EffectOverrides/*.ini` alphabetically, the player's file last), the format
  version and the merge; the game side is `EffectCatalog::LoadOverrides`.
- `ThetaDrift.h`: how far the live θ moved from the bootstrap (means moved,
  mean / max / rms |Δμ|, mean ln variance ratio), for the `[Soak]` heartbeat
  and `hg theta`.

Their tests: `CapFacetsTests.cpp` (each facet's columns, each class arm, no
dead class, real items from the three dump fixtures), `OverrideFilesTests.cpp`
and `ThetaDriftTests.cpp`.

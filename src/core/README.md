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
`SlotSnapshotIO.*` is the text record of one allocation. The tests:

| File | What it proves |
|------|----------------|
| `SlotAllocGoldenTests.cpp` | the core with the OLD arithmetic (`LegacySlotPolicy.h`) replays what the old game code recorded in play -- pages, seating memory and events, under the shipped [SlotLocker] settings and varied ones -- bit for bit; the sign-safe arithmetic gives identical pages and events on those and on synthetic snapshots (which show only that the two arithmetics agree on a path, not that the path is right); what the new game code recorded replays exactly; recorded names are text |
| `SlotAllocAdversarialTests.cpp` | named cases: ties, zeros, the cap's 2x tie, overrides, remembered-only rows, the sort, negative scores, the hold's tie band (a challenger on the margin holds), and the float-rounding boundaries where the arithmetics can still part (the cap's scan at any discount, underflow) |
| `SlotScoreMathTests.cpp` | the bridge, the cap term, the margin, the sentinels, the churn buckets |
| `SlotSnapshotIOTests.cpp` | the snapshot format round-trips |

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

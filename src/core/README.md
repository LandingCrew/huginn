# src/core -- pure code, host-tested

**The rule for the engine rewrite:** every new piece of rewrite math lives in
`src/core/` and has host tests in `tests/core/`. Response curves, the learner
update, σ_Δ and the challenger rule, the effect mapper over plain records: all
of it. The game layer only reads forms and calls into `src/core/`.

What "pure" means here:

- Standard library only. `cmake/CheckCorePurity.cmake` enforces it on every
  `.h .hpp .hxx .inl .ipp .cpp .cxx .cc` here:
  - a quoted include must resolve to a file under `src/core/` (against the
    including file's folder, then against `src/`), so `"Globals.h"`,
    `"state/GameState.h"` and `"../PCH.h"` are rejected;
  - an angle include must be a C++ standard library header (an allow-list in
    the script), so `<RE/...>`, `<REX/...>`, `<SKSE/...>`, `<spdlog/...>`,
    `<SimpleIni.h>`, `<Windows.h>` are rejected;
  - no backslash in an include path;
  - no `RE`, `REL`, `REX`, `SKSE`, `logger` or `spdlog` followed by `::`
    (spaces allowed, any column), no `using namespace` of and no namespace
    alias to `RE`/`REL`/`REX`/`SKSE`.

  It runs at configure time and again as a build step whenever a core file
  changes; `huginn_core_tests` and the plugin both depend on that step, so an
  edit made after the configure still fails the build. The scan is textual: a
  comment that names `RE::` or a game header trips it too. Say "the game's
  types" in comments instead.
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

Both targets compile `src/core/`: the plugin through its `GLOB_RECURSE` over
`src/`, and `huginn_core_tests` directly. Tests live outside `src/` so the
plugin never compiles them.

Build and run (from the repo root, after the usual configure):

```sh
cmake --build build --config Debug --target huginn_core_tests
ctest -C Debug --test-dir build --output-on-failure
```

The executable is `build/tests/Debug/huginn_core_tests.exe`. It exits non-zero
when a test fails; `--help` lists doctest's filters (`-tc="need cap*"`).

Adding or removing a file: the globs use `CONFIGURE_DEPENDS`, and with the
Visual Studio generator the first build after the change only re-runs the
configure, still building the old file list. Build a second time (or
reconfigure first) before trusting the result.

The pattern, ported first: `NeedCapMath.h` holds the arithmetic of the slot
need cap; `Slot::NeedCap` (`src/slot/NeedCap.*`) keeps the counting and the
classification and calls `Core::NeedCapFactor`. `NeedCapMathTests.cpp`
includes a check that the port matches the old loop bit for bit, including
past the denormal fixed point the product reaches for discounts above 0.5.

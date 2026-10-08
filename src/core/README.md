# src/core -- pure code, host-tested

**The rule for the engine rewrite:** every new piece of rewrite math lives in
`src/core/` and has host tests in `tests/core/`. Response curves, the learner
update, σ_Δ and the challenger rule, the effect mapper over plain records: all
of it. The game layer only reads forms and calls into `src/core/`.

What "pure" means here:

- No `RE::`, `REL::`, `SKSE::`, and no CommonLib, SimpleIni or spdlog
  includes. Standard library only. The configure step fails if a file here
  includes `RE/`, `REL/`, `SKSE/`, `PCH.h`, `SimpleIni` or `spdlog/`, or names
  `RE::`, `REL::` or `SKSE::` (`tests/CMakeLists.txt`).
- Do not rely on `PCH.h`. The plugin force-includes it into everything it
  compiles, these files included, but the test target compiles them without
  it. Include what you use (`<algorithm>`, `<cstdint>`, ...).
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

The pattern, ported first: `NeedCapMath.h` holds the arithmetic of the slot
need cap; `Slot::NeedCap` (`src/slot/NeedCap.*`) keeps the counting and the
classification and calls `Core::NeedCapFactor`. `NeedCapMathTests.cpp`
includes a check that the port matches the old loop bit for bit.

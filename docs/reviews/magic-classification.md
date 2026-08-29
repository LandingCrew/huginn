# Magic Classification - Data-Driven Spell Patterns

## Problem

The SpellClassifier fails to classify 68 spells from a Lorerim stress test (68/236 = 29% unknown). These include 6 vanilla spells and ~62 mod spells from Apocalypse, Mysticism/Odin, and Triumvirate.

The classifier uses a 3-stage pipeline: **overrides → API checks → hardcoded name-pattern tag fallback**. Most unknowns fail at both stage 2 (API doesn't recognize archetype) and stage 3 (name doesn't match hardcoded keywords).

## Solution: Two-Layer Data Files

Rather than hardcoding more name patterns in C++, the system becomes **data-driven** by shipping a default spell patterns INI file that users can tweak without rebuilding from source.

```
Priority (highest → lowest):
  1. User overrides    (Huginn_Overrides.ini)      — exact name/FormID matches
  2. Default patterns  (Huginn_SpellPatterns.ini)   — substring patterns + specific overrides
  3. Hardcoded C++     (DetermineSpellTags)        — existing patterns, unchanged
  4. API checks        (DetermineSpellType)         — SKSE archetype inspection
```

## Implementation Status

### Completed
- None yet

### TODO
- [ ] Extend `SpellOverrides` with `m_patterns` vector, `MatchPattern()`, `HasPattern()`
- [ ] Update `LoadFromFile()` to handle `pattern=true` flag
- [ ] Update `ClassifySpell()` to check patterns between exact override and API checks
- [ ] Load two files in SpellRegistry constructor (patterns first, then user overrides)
- [ ] Create `Huginn_SpellPatterns.ini` with substring patterns + mod spell overrides

### Files to modify
| File | Change |
|------|--------|
| `src/spell/SpellOverrides.h` | Add `m_patterns` vector, `MatchPattern()` method, `HasPattern()` |
| `src/spell/SpellOverrides.cpp` | Parse `pattern=true` flag in `LoadFromFile()`, implement `MatchPattern()` |
| `src/spell/SpellClassifier.cpp` | Add `MatchPattern()` call in `ClassifySpell()` between exact override and API checks |
| `src/spell/SpellRegistry.cpp` | Load two files: default patterns then user overrides |
| `Huginn_SpellPatterns.ini` (**NEW**) | Default patterns + mod spell overrides (ships with plugin) |

## Build Note

After setting or changing environment variables (`VCPKG_ROOT`, `CommonLibSSEPath_NG`, `CompiledPluginsPath`), restart VS Code. It inherits env vars at launch time so changes in System Settings won't take effect until the editor is restarted.

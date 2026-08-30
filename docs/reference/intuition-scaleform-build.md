# Intuition Scaleform Widget — Build Guide

> **Sources:** `src/swf/` (AS2 + build), `src/ui/IntuitionMenu.{h,cpp}` (C++ side)
> **Verified against:** v0.19.10

How to build, modify, and deploy the Intuition HUD widget SWF for Skyrim.

## Toolchain

Three open-source tools replace Adobe Flash CS6:

<!-- UNVERIFIED: tool versions and install commands cannot be checked from the
     repo — build.sh only requires `swfmill` and `mtasc` to be on PATH and does
     not pin versions. Treat the version column as "what was used", not a floor. -->

| Tool | Version | Purpose | Install |
|------|---------|---------|---------|
| **JPEXS FFDec** | 24.1.1 | Inspect/edit SWF files (decompile, view shapes, modify scripts) | `winget install JPEXS.FFDec` (requires Java) |
| **MTASC** | 1.15 (Community Fork) | Compile ActionScript 2.0 → SWF bytecode | [SourceForge](https://sourceforge.net/projects/mtasc/files/) — unzip, add to PATH |
| **swfmill** | 0.3.3+ | Assemble SWF structure from XML (sprites, shapes, fonts) | [GitHub](https://github.com/djcsdy/swfmill) or [Softpedia](https://www.softpedia.com/get/Internet/WEB-Design/Flash/swfmill.shtml) — unzip, add to PATH |

**Why not Adobe Animate?** Skyrim's Scaleform runtime uses ActionScript 2.0
(Flash Player 8). Adobe Animate dropped AS2 support entirely. Flash CS6 was the
last version that worked, but it's discontinued. MTASC + swfmill is the viable
free path.

### How the tools work together

```
intuition.xml → swfmill → Intuition.swf (empty SWF shell with library symbols)
                                  │
Intuition.as  → mtasc  ───────────┘ (injects compiled AS2 bytecode into the SWF)
                                  │
                               Intuition.swf (final, ready to deploy)
```

- **swfmill** creates the SWF container with the clip/symbol structure (like a linker)
- **MTASC** compiles AS2 source code and writes the bytecode into that SWF (like a compiler)
- **JPEXS** is for inspection, debugging, and visual editing after the fact

## Project Structure

```
src/swf/
  intuition.xml     ← swfmill project definition (clip IDs, stage size)
  Intuition.as      ← ActionScript 2.0 widget class (all logic + drawing)
  build.sh          ← Build script (runs swfmill then mtasc)
  Intuition.swf     ← Build output — COMMITTED, not gitignored
```

`Intuition.swf` is tracked in git. The C++ build does not compile it; CMake only
copies the committed artifact. **A change to `Intuition.as` is not live until you
re-run `build.sh` and commit the regenerated `Intuition.swf`.**

## Building

From Git Bash (or any bash shell with swfmill + mtasc on PATH):

```sh
cd src/swf
bash build.sh
```

Output: `src/swf/Intuition.swf`

### What the build does

**Step 1 — swfmill:** `swfmill simple intuition.xml Intuition.swf` creates an
empty SWF with:

- Stage: 1280x720, 24 fps, SWF version 8, black background
- Library symbol: `IntuitionWidget` (an empty MovieClip, `class="Intuition"`)

The stage size is a canvas the widget is drawn onto at `_root` level, not the
widget's own size — the background auto-sizes to its content at runtime
(`recalcWidth()` / `resizeBackground()`).

**Step 2 — mtasc:** `mtasc -cp . -swf Intuition.swf -main Intuition.as` compiles
`Intuition.as` and injects it into the SWF:

- The `-main` flag generates a frame 1 `DoInitAction` that calls `Intuition.main()`
- `main()` calls `Object.registerClass("IntuitionWidget", Intuition)` to link the class to the symbol
- `main()` then calls `_root.attachMovie("IntuitionWidget", "widget", 1)` to instantiate it
- The constructor builds the entire UI programmatically (background, slots, page dots)

`build.sh` runs with `set -e`, so either step failing aborts the build.

### Verifying the build

<!-- UNVERIFIED: requires running JPEXS FFDec against the built SWF, which
     cannot be done from the repository. Character IDs in particular are
     swfmill/mtasc output details, not repo facts. -->

Open `Intuition.swf` in JPEXS FFDec. You should see:

```
sprites/
  DefineSprite (chid: 1, exp: "IntuitionWidget")     ← Library symbol
  DefineSprite (chid: 20480, exp: __Packages.Intuition)  ← Compiled class
scripts/
  __Packages/
    Intuition    ← Decompiled AS2 (should match Intuition.as)
```

The preview shows a black rectangle (the background drawn at partial alpha over
the black stage).

## Deploying to Skyrim

**The normal C++ build deploys it for you.** When `COPY_OUTPUT` is on,
`src/CMakeLists.txt` adds a post-build step that copies
`src/swf/Intuition.swf` to `${CompiledPluginsPath}/Interface/Huginn/Intuition.swf`,
alongside the DLL, `Huginn.ini` and the dMenu JSON. There is also an `install()`
rule putting it at `Interface/Huginn` in the packaged component.

To place it by hand, the destination is:

```
<Skyrim Data>/Interface/Huginn/Intuition.swf
```

The C++ side loads it via `BSScaleformManager::LoadMovie(this, uiMovie, FILE_NAME)`
where `IntuitionMenu::FILE_NAME` is `"Huginn/Intuition"`, which resolves to
`Data/Interface/Huginn/Intuition.swf`. The menu itself is registered under
`IntuitionMenu::MENU_NAME` = `"IntuitionMenu"`.

## Architecture

### SWF Structure

The widget is built entirely in code (no Flash IDE authoring). The `Intuition`
class extends `MovieClip` and constructs all visual elements in its constructor:

```
_root
 └─ widget (Intuition class, placed by attachMovie in main())
     ├─ background (MovieClip, depth 0)   ← Drawn via beginFill/lineTo
     ├─ slotContainer (MovieClip, depth 1)
     │   ├─ slot0 (MovieClip)
     │   │   ├─ keyLabel (TextField)   "1"
     │   │   └─ itemName (TextField)   "Fast Healing"
     │   ├─ slot1 ... slot9            ← MAX_SLOTS = 10
     ├─ pageIndicator (MovieClip, depth 2)
     │   ├─ dot0 ... dot9 (diamond shapes)  ← MAX_DOTS = 10, matches SlotSettings::MAX_PAGES
     │   └─ pageLabel (TextField)      ← inline page name after the active dot
```

The page indicator is only shown when more than one page exists.

### Entry point: `-main` flag

MTASC's `-main` flag injects a `DoInitAction` on frame 1 that calls
`Intuition.main()`. This is the bootstrapping mechanism:

```actionscript
static function main():Void
{
    Object.registerClass("IntuitionWidget", Intuition);
    _root.attachMovie("IntuitionWidget", "widget", 1);
}
```

`registerClass` links the AS2 class to the library symbol so the constructor runs
when `attachMovie` creates the instance. Without this, the MovieClip would have
no code attached.

### C++ → AS2 Public API

The C++ side grabs `_root.widget` once in the `IntuitionMenu` constructor, right
after `LoadMovie`, and invokes methods on it:

```cpp
RE::GFxValue widget;
uiMovie->GetVariable(&widget, "_root.widget");

// setSlot(0, "Fast Healing", 2, 0.95, "low health", 0)
std::array<RE::GFxValue, 6> args;
args[0] = 0.0;                                  // index
uiMovie->CreateString(&args[1], "Fast Healing"); // name (must be CreateString)
args[2] = 2.0;                                  // type (kSpell)
args[3] = 0.95;                                 // confidence
uiMovie->CreateString(&args[4], "low health");   // detail subtext
args[5] = 0.0;                                  // visual state (Normal)
widget.Invoke("setSlot", nullptr, args.data(), args.size());
```

| AS2 Method | Args | Purpose |
|------------|------|---------|
| `setSlot(index, name, type, confidence, detail, visualState)` | int, string, int, float, string, int | Update slot content and animation state |
| `clearSlot(index)` | int | Hide a slot, reset its animation state |
| `setSlotCount(count)` | int | Set visible count, resize background |
| `setPage(current, total, name)` | int, int, string | Update page indicator dots + inline label |
| `setUrgent(index, active)` | int, bool | Legacy pulse flag (superseded by `visualState`) |
| `setWidgetAlpha(alpha)` | float | Overall opacity (0-100) |
| `setChildAlpha(alpha)` | float | Secondary-element opacity (0-100), e.g. page label |
| `setRefreshEffect(mode)` | int | 0 = none, 1 = flash, 2 = tint |
| `setSlotEffect(mode)` | int | 0 = slide, 1 = fade, 2 = instant |
| `setRefreshStrength(pct)` | float | Effect strength, 0-100 |
| `tick(dt)` | float | Per-frame animation step, called from `AdvanceMovie` with the engine delta (AS2 `onEnterFrame` is unreliable here) |

`confidence` is accepted and queued through the animation path but is not
currently rendered — there are no confidence pips in the AS2.

`setChildAlpha`, `setRefreshEffect`, `setSlotEffect` and `setRefreshStrength` are
pushed from the `[Widget]` INI section on load and on every `hg reload`.

### Slot type enum

The value sent as `type` is `UI::IntuitionSlotType` (`src/ui/IntuitionMenu.h`),
which must match the AS2 `TYPE_*` constants:

| Value | AS2 Constant | C++ `IntuitionSlotType` | Color |
|-------|-------------|--------------------------|-------|
| 0 | `TYPE_EMPTY` | `kEmpty` | Gray `#808080` |
| 1 | `TYPE_NOMATCH` | `kNoMatch` | Gray `#808080` (dimmed) |
| 2 | `TYPE_SPELL` | `kSpell` | Warm white `#FFD4A0` (slot 0) / white `#FFFFFF` |
| 3 | `TYPE_WILDCARD` | `kWildcard` | Soft blue `#7EB8FF` |
| 4 | `TYPE_HEALTH_POTION` | `kHealthPotion` | Soft red `#FF6666` |
| 5 | `TYPE_MAGICKA_POTION` | `kMagickaPotion` | Soft blue `#6699FF` |
| 6 | `TYPE_STAMINA_POTION` | `kStaminaPotion` | Soft green `#66FF66` |
| 7 | `TYPE_MELEE_WEAPON` | `kMeleeWeapon` | Warm gold `#E6B84D` |
| 8 | `TYPE_RANGED_WEAPON` | `kRangedWeapon` | Warm gold `#E6B84D` |

The pipeline's own `UI::SlotContentType` (`src/ui/SlotTypes.h`) is wider than
this and is narrowed by `IntuitionMenu::MapSlotContentType()`: generic `Potion`
falls back to the health-potion visual, `Ammo` uses the ranged-weapon visual, and
`SoulGem` uses the spell visual.

### Visual state enum

The sixth `setSlot` argument is `Slot::SlotVisualState`
(`src/slot/SlotAssignment.h`), matched by the AS2 `STATE_*` constants:

| Value | AS2 Constant | C++ | Effect |
|-------|-------------|-----|--------|
| 0 | `STATE_NORMAL` | `Normal` | No special effect |
| 1 | `STATE_CONFIRMED` | `Confirmed` | Single flash (re-evaluated, same item) |
| 2 | `STATE_EXPIRING` | `Expiring` | Slow pulse (lock about to expire) |
| 3 | `STATE_OVERRIDE` | `Override` | Slide + flash |
| 4 | `STATE_WILDCARD` | `Wildcard` | Same visual as Override |

### Font handling

Skyrim provides fonts at runtime via Scaleform's font system. The widget uses
`$EverywhereMediumFont` (Skyrim's standard Futura Condensed UI font) for both the
key label and the item name format, with `embedFonts = false` on every TextField.
The `$` prefix tells Scaleform to resolve the font name at runtime from
`Data/Interface/fonts_en.swf`. Fonts won't render in JPEXS preview — this is
expected.

## Editing Workflow

### Changing widget behavior (AS2 code)

1. Edit `src/swf/Intuition.as`
2. Run `bash build.sh`
3. Rebuild the plugin (CMake copies the SWF), or copy it to
   `Data/Interface/Huginn/` by hand
4. Restart Skyrim
5. Commit the regenerated `Intuition.swf` alongside the `.as` change

### Changing widget structure (new clips/symbols)

1. Edit `src/swf/intuition.xml` to add new `<clip>` entries in `<library>`
2. Reference new symbols in `Intuition.as` via `attachMovie()` or `createEmptyMovieClip()`
3. Run `bash build.sh`

### Adding a C++ → AS2 call

1. Add the `public function` to `Intuition.as` and rebuild the SWF
2. Add the matching wrapper to `IntuitionMenu`. It **must** defer the GFx work
   via `SKSE::GetTaskInterface()->AddUITask()` — the update thread must never
   touch GFx directly — and **must** pass strings through
   `uiMovie->CreateString()` rather than a raw `.data()` pointer

### Inspecting or debugging

1. Open `Intuition.swf` in JPEXS FFDec
2. Browse the sprites tree to see the clip hierarchy
3. Check scripts to verify decompiled AS2 matches source
4. Edit shapes/scripts directly in JPEXS for quick experiments
5. Saving a modified SWF from JPEXS bypasses the build pipeline — treat it as
   throwaway, since the next `build.sh` overwrites it

## Reference Projects

<!-- UNVERIFIED: external repositories; the two "local clone" entries were paths
     on the original author's machine and are not part of this repo. -->

These repos were studied during development:

| Project | What we used | Location |
|---------|-------------|----------|
| **QuickLootIE** | SWF structure, CLIK wrappers, `ListItemRenderer` row pattern, 9-slice background, settings injection, `LootMenu.as` architecture | `<local clone>` |
| **ImmersiveHUD-SKSE** | Multi-track alpha fading, contextual visibility logic, Scaleform `DisplayInfo` manipulation, HUD mode awareness | `<local clone>` |
| **SkyUI** | `WidgetBase.as` class, HUD widget system, `meter.fla` template | [GitHub](https://github.com/schlangster/skyui) |
| **moreHUDSE** | C++ HUD injection pattern, `loadMovie` into HUDMenu | [GitHub](https://github.com/ahzaab/moreHUDSE) |

## Key Constraints

- **ActionScript 2.0 only** — Skyrim's Scaleform GFx uses AS2 (Flash Player 8 compatible). No AS3.
- **No embedded fonts** — Use `$EverywhereMediumFont` or other `$`-prefixed Skyrim font names. Don't embed fonts in the SWF.
- **SWF version 8** — `version="8"` in `intuition.xml`. Higher versions may cause Scaleform compatibility issues.
- **Keep the enums in sync** — `TYPE_*` with `IntuitionSlotType`, `STATE_*` with `SlotVisualState`, and `MAX_DOTS` with `SlotSettings::MAX_PAGES`. Nothing checks these at build time.
- **`gfxfontlib.swf` warning is harmless** — JPEXS warns about this missing file. Scaleform provides it at runtime.
- **`skyui/buttonart.swf` warning is harmless** — Same situation, SkyUI provides it at runtime.

## Status

The original "next steps" list is complete as of v0.19.x:

- `IntuitionMenu` is a registered `RE::IMenu` that loads the SWF and drives the
  full AS2 API (`src/ui/IntuitionMenu.cpp`)
- The pipeline pushes real recommendations through `Display::IntuitionBackend`;
  the old ImGui `SpellRecommendationWidget` is gone (the remaining ImGui widgets
  under `src/ui/` are Debug-build diagnostics)
- Visibility is context-driven: `HudVisibilityManager` hides the menu on
  `MenuOpenCloseEvent` and shows it during gameplay
- Position, scale, opacity, display mode and the refresh/slot effects are
  configurable from the `[Widget]` INI section, hot-reloadable via `hg reload`
  and editable in dMenu

Not implemented: context icons (health, fire, frost, forge glyphs) and confidence
pips. `setSlot` already carries a `confidence` argument the AS2 ignores.

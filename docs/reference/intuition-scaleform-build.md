# Intuition Scaleform Widget — Build Guide

How to build, modify, and deploy the Intuition HUD widget SWF for Skyrim.

## Toolchain

Three open-source tools replace Adobe Flash CS6:

| Tool | Version | Purpose | Install |
|------|---------|---------|---------|
| **JPEXS FFDec** | 24.1.1 | Inspect/edit SWF files (decompile, view shapes, modify scripts) | `winget install JPEXS.FFDec` (requires Java) |
| **MTASC** | 1.15 (Community Fork) | Compile ActionScript 2.0 → SWF bytecode | [SourceForge](https://sourceforge.net/projects/mtasc/files/) — unzip, add to PATH |
| **swfmill** | 0.3.3+ | Assemble SWF structure from XML (sprites, shapes, fonts) | [GitHub](https://github.com/djcsdy/swfmill) or [Softpedia](https://www.softpedia.com/get/Internet/WEB-Design/Flash/swfmill.shtml) — unzip, add to PATH |

**Why not Adobe Animate?** Skyrim's Scaleform runtime uses ActionScript 2.0 (Flash Player 8). Adobe Animate dropped AS2 support entirely. Flash CS6 was the last version that worked, but it's discontinued. MTASC + swfmill is the viable free path.

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
  Intuition.swf     ← Build output (gitignored)
```

## Building

From Git Bash (or any bash shell with swfmill + mtasc on PATH):

```sh
cd src/swf
bash build.sh
```

Output: `src/swf/Intuition.swf`

### What the build does

**Step 1 — swfmill:** Reads `intuition.xml` and creates an empty SWF with:
- Stage: 300x300, 24fps, SWF version 8
- Library symbol: `IntuitionWidget` (an empty MovieClip in the export table)

**Step 2 — mtasc:** Compiles `Intuition.as` and injects it into the SWF:
- The `-main` flag generates a frame 1 `DoInitAction` that calls `Intuition.main()`
- `main()` calls `Object.registerClass("IntuitionWidget", Intuition)` to link the class to the symbol
- `main()` then calls `_root.attachMovie("IntuitionWidget", "widget", 1)` to instantiate it
- The constructor builds the entire UI programmatically (background, slots, page dots)

### Verifying the build

Open `Intuition.swf` in JPEXS FFDec. You should see:

```
sprites/
  DefineSprite (chid: 1, exp: "IntuitionWidget")     ← Library symbol
  DefineSprite (chid: 20480, exp: __Packages.Intuition)  ← Compiled class
scripts/
  __Packages/
    Intuition    ← Decompiled AS2 (should match Intuition.as)
```

The preview shows a black rectangle (the background drawn at 40% alpha over the black stage).

## Deploying to Skyrim

Copy the compiled SWF to:

```
<Skyrim Data>/Interface/Huginn/Intuition.swf
```

The C++ side loads it via `BSScaleformManager::LoadMovie(this, uiMovie, "Huginn/Intuition")` which resolves to `Data/Interface/Huginn/Intuition.swf`.

## Architecture

### SWF Structure

The widget is built entirely in code (no Flash IDE authoring). The `Intuition` class extends `MovieClip` and constructs all visual elements in its constructor:

```
_root
 └─ widget (Intuition class, placed by attachMovie in main())
     ├─ background (MovieClip)    ← Drawn via beginFill/lineTo
     ├─ slotContainer (MovieClip)
     │   ├─ slot0 (MovieClip)
     │   │   ├─ keyLabel (TextField)   "1"
     │   │   └─ itemName (TextField)   "Fast Healing"
     │   ├─ slot1 ... slot7
     ├─ pageIndicator (MovieClip)
     │   ├─ dot0 ... dot3 (diamond shapes)
```

### Entry point: `-main` flag

MTASC's `-main` flag injects a `DoInitAction` on frame 1 that calls `Intuition.main()`. This is the bootstrapping mechanism:

```actionscript
static function main():Void
{
    Object.registerClass("IntuitionWidget", Intuition);
    _root.attachMovie("IntuitionWidget", "widget", 1);
}
```

`registerClass` links the AS2 class to the library symbol so the constructor runs when `attachMovie` creates the instance. Without this, the MovieClip would have no code attached.

### C++ → AS2 Public API

The C++ side communicates with the widget by invoking methods on `_root.widget`:

```cpp
RE::GFxValue widget;
uiMovie->GetVariable(&widget, "_root.widget");

// Call setSlot(0, "Fast Healing", 2, 0.95)
RE::GFxValue args[4];
args[0] = 0;               // index
args[1] = "Fast Healing";  // name
args[2] = 2;               // type (TYPE_SPELL)
args[3] = 0.95;            // confidence
widget.Invoke("setSlot", nullptr, args, 4);
```

| AS2 Method | Args | Purpose |
|------------|------|---------|
| `setSlot(index, name, type, confidence)` | int, string, int, float | Update slot content |
| `clearSlot(index)` | int | Hide a slot |
| `setSlotCount(count)` | int | Set visible count, resize background |
| `setPage(current, total, name)` | int, int, string | Update page indicator dots |
| `setUrgent(index, active)` | int, bool | Enable/disable pulse animation |
| `setWidgetAlpha(alpha)` | float | Overall opacity (0-100) |

### Slot type enum

Must match between AS2 and C++:

| Value | AS2 Constant | C++ Equivalent | Color |
|-------|-------------|----------------|-------|
| 0 | `TYPE_EMPTY` | `SlotContentType::Empty` | Gray 50% alpha |
| 1 | `TYPE_NOMATCH` | `SlotContentType::NoMatch` | Gray 50% alpha |
| 2 | `TYPE_SPELL` | `SlotContentType::Spell` | Warm white (slot 0) / White |
| 3 | `TYPE_WILDCARD` | `SlotContentType::Wildcard` | Soft blue `#7EB8FF` |
| 4 | `TYPE_HEALTH_POTION` | `SlotContentType::HealthPotion` | Soft red `#FF6666` |
| 5 | `TYPE_MAGICKA_POTION` | `SlotContentType::MagickaPotion` | Soft blue `#6699FF` |
| 6 | `TYPE_STAMINA_POTION` | `SlotContentType::StaminaPotion` | Soft green `#66FF66` |
| 7 | `TYPE_MELEE_WEAPON` | `SlotContentType::MeleeWeapon` | Warm gold `#E6B84D` |
| 8 | `TYPE_RANGED_WEAPON` | `SlotContentType::RangedWeapon` | Warm gold `#E6B84D` |

### Font handling

Skyrim provides fonts at runtime via Scaleform's font system. The widget uses `$EverywhereMediumFont` (Skyrim's standard Futura Condensed UI font). The `$` prefix tells Scaleform to resolve the font name at runtime from `Data/Interface/fonts_en.swf`. Fonts won't render in JPEXS preview — this is expected.

## Editing Workflow

### Changing widget behavior (AS2 code)

1. Edit `src/swf/Intuition.as` in VS Code
2. Run `bash build.sh`
3. Copy SWF to Skyrim `Data/Interface/Huginn/`
4. Restart Skyrim (or reload save if using hot-reload)

### Changing widget structure (new clips/symbols)

1. Edit `src/swf/intuition.xml` to add new `<clip>` entries in `<library>`
2. Reference new symbols in `Intuition.as` via `attachMovie()` or `createEmptyMovieClip()`
3. Run `bash build.sh`

### Inspecting or debugging

1. Open `Intuition.swf` in JPEXS FFDec
2. Browse sprites tree to see clip hierarchy
3. Check scripts to verify decompiled AS2 matches source
4. Edit shapes/scripts directly in JPEXS if needed (for quick experiments)
5. Save modified SWF from JPEXS (bypasses the build pipeline)

## Reference Projects

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
- **SWF version 8** — Set `version="8"` in swfmill XML. Higher versions may cause Scaleform compatibility issues.
- **`gfxfontlib.swf` warning is harmless** — JPEXS warns about this missing file. Scaleform provides it at runtime.
- **`skyui/buttonart.swf` warning is harmless** — Same situation, SkyUI provides it at runtime.

## Next Steps

- [ ] Wire up C++ `IntuitionMenu` class (register as `RE::IMenu`, load SWF, call API)
- [ ] Hello world: hardcoded slots visible in-game
- [ ] Replace ImGui `SpellRecommendationWidget` calls with `IntuitionMenu` calls in update loop
- [ ] Add fade-in/fade-out behavior (ImmersiveHUD-style alpha transitions)
- [ ] Add context icons (health, fire, frost, forge glyphs)
- [ ] Add confidence pips for spells
- [ ] MCM/INI configuration for position, scale, opacity

# Huginn

> **Alpha** — Experimental. For testers and tinkerers, not stable playthroughs.

Huginn is an SKSE plugin that watches what you can see — health, enemies, environment — and surfaces the right spell, potion, weapon, or item before you need to dig through menus. It learns your preferences over time through lightweight reinforcement learning. It never acts on its own.

<!-- TODO: replace placeholder paths with actual gifs -->

| Combat | Workstation | Learning |
|:------:|:-----------:|:--------:|
| ![Combat recommendations](docs/media/combat.gif) | ![Workstation awareness](docs/media/workstation.gif) | ![Learning adaptation](docs/media/learning.gif) |
| *Healing and resistances surface under pressure* | *Fortify potions appear at the right crafting station* | *Recommendations adapt to your playstyle* |

## Features

- **Context-aware recommendations** — analyzes health, magicka, stamina, distance, enemy type, combat state, and sneak to suggest relevant equipment
- **Learns as you play** — observes what you equip in each situation, bootstrapped with sensible defaults
- **Scaleform HUD widget** — minimal overlay with keybinds, auto-hides outside combat
- **Wheeler integration** — optional [Wheeler](https://www.nexusmods.com/skyrimspecialedition/mods/97345) radial menu support
- **Multi-page slots** — organize recommendations by role (up to 10 pages, 10 slots each)
- **Workstation awareness** — Fortify Smithing at forges, Fortify Enchanting at enchanters
- **INI-configurable** — context weights, scoring, slot layout, keybindings, display mode

## Requirements

- Skyrim SE 1.5.39+ or Skyrim AE
- SKSE
- Address Library for SKSE Plugins

## Building

### Prerequisites

- Visual Studio 2022 with C++ desktop development workload
- [vcpkg](https://github.com/microsoft/vcpkg) package manager
- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) ~v3.7.0+

### Environment Variables

```powershell
$env:CommonLibSSEPath_NG = "C:\path\to\CommonLibSSE-NG"
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
$env:CompiledPluginsPath = "C:\path\to\Skyrim\Data"
```

### Build

```powershell
cmake --preset vs2022-windows
cmake --build build --config Release
```

Debug builds enable extra logging and unit tests:
```powershell
cmake --build build --config Debug
```

### Dependencies (vcpkg)

spdlog, imgui (Win32 + DX11), simpleini, toml11, rapidcsv, freetype, rsm-binary-io

## Compatibility

- **ENB/ReShade** — independent D3D11 rendering, no conflicts
- **Wheeler** — native integration via [WHEELER - Refined](https://www.nexusmods.com/skyrimspecialedition/mods/167380)
- **Other UI mods** — compatible, independent overlay

## Credits

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) by CharmedBaryon
- [Wheeler](https://github.com/D7ry/wheeler) / [dMenu](https://github.com/D7ry/dMenu) by D7ry
- SKSE team
- LamasTinyHUD, [Dear ImGui](https://github.com/ocornut/imgui), [iEquip](https://www.nexusmods.com/skyrimspecialedition/mods/27008)

## License

GNU General Public License v3.0

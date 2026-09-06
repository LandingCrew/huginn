# Huggin

Huginn is an SKSE plugin that observes the player state and build a recommendations to continously upsert contextually relevent items, spells, weapons, into your configured hotkeys. The mod only watches what you can see — health, enemies, environment — and surfaces the right spell, potion, weapon, or item before you need to dig through menus. It learns your preferences over time through lightweight reinforcement learning and Huggin never acts on its own.

Huggin provides a UI element to show the rolling status of hotkey population.

## Features

* **Context-aware recommendations** — analyzes health, magicka, stamina, distance, enemy type, combat state, and sneak to suggest relevant equipment
* **Learns as you play** — observes what you equip in each situation, bootstrapped with sensible defaults
* **Scaleform HUD widget** — minimal overlay with keybinds, auto-hides outside combat
* **Wheeler integration** — optional [Wheeler](https://www.nexusmods.com/skyrimspecialedition/mods/97345) radial menu support
* **Multi-page slots** — organize recommendations by role (up to 10 pages, 10 slots each)
* **Workstation awareness** — Fortify Smithing at forges, Fortify Enchanting at enchanters
* **INI-configurable** — context weights, scoring, slot layout, keybindings, display mode

## Configuration and Usage

### Huggin

**Configuration Location:** SKSE\Plugins\Huggin.ini

The majority of the recommendation algorithm is configured from this file. Keybindings

### Dmenu UI controls

**Configuration Location:** SKSE\Plugins\\Huginn_Overrides.ini

This controls ingame intuition UI usability and debuging (if using a debugging build)

### Classifer Overrides 

**Configuration Location:** SKSE\Plugins\dmenu\customSettings\ini\Huginn.ini

### Console commands


## Optional Extension

### WheelerAPI

Huggin can connect to the Wheeler plugin 

#### Install

1. Download the orginal wheeler mod.
2. Install the WheelerAPI after wheeler and allow overwrites 

#### Usage

Huggin will attempt to auto connect to wheeler and it will automatically manage wheels based on the configuration settings in the ini.

* **Auto-focus skips past your own wheels.** With `bAutoFocusOnOpen = true` (the default) under `[Wheeler]`, opening Wheeler on any wheel that isn't Huginn's jumps straight to Huginn's first wheel.

* **Wheel order is remembered for the rest of the session.** Huginn deletes and recreates its wheels on every save load. It recreates them where you last dragged them to, not at `sWheelPosition` — so reordering in Wheeler's edit mode survives loading a save. Two limits worth knowing: the memory is per-session, so restarting Skyrim starts from `sWheelPosition` again; and Huginn's wheels are restored as one contiguous block, so if you interleaved your own wheels between clears the memory and puts them where the INI says.

This version of wheeler is 

### Features


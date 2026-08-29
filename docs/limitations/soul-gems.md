# Soul Gem Recharging - Known Limitations

## Overview

Huginn can recommend filled soul gems when an enchanted weapon's charge is low, and supports one-click recharging via hotkey. However, Wheeler integration has limitations due to Skyrim's form type restrictions.

## What Works

- **Intuition widget**: Soul gems display correctly with "Low Charge" subtext
- **Hotkey recharge**: Pressing the assigned hotkey recharges the equipped enchanted weapon, consumes the soul gem, and awards Enchanting XP
- **INI toggle**: `bEnableSoulGemRecharge` in `[Candidates]` section controls the feature

## What Doesn't Work

### Wheeler: Soul gems cannot be added to wheels

**Symptom**: Soul gem slots appear as empty entries in Wheeler (dark circles with no icon).

**Root cause**: Wheeler's `AddItemByFormID` API does not support `TESSoulGem` form types. The API returns result code `-6` (unsupported form type) when attempting to add a soul gem.

This is a **game engine limitation** — Skyrim does not natively support hotkeying soul gems. Wheeler inherits this restriction because it uses the game's internal form type system to render wheel entries with appropriate icons and activation behavior.

**Workaround**: Huginn zeroes out soul gem FormIDs before passing them to Wheeler, preventing error spam and retry loops. Soul gem slots appear as empty entries in Wheeler rather than broken ones.

### Why hotkeys work but Wheeler doesn't

Huginn's hotkey path bypasses the game's equip system entirely:

```
Hotkey → EquipManager::UseSoulGem()
  → player->AsActorValueOwner()->ModActorValue(kRightItemCharge, amount)
  → player->RemoveItem(soulGem, 1, kRemove)
  → player->AddSkillExperience(kEnchanting, xp)
```

This is the same approach used by [AutoUseSoulgemsSSE](https://github.com/neogulcity/AutoUseSoulgemsSSE) — directly modifying the weapon charge ActorValue rather than going through the game's recharge UI.

Wheeler, however, needs to *display* the item in its radial menu using the game's form rendering, which doesn't support soul gem forms.

## Charge Values by Soul Level

| Soul Level | Charge Restored | Enchanting XP |
|-----------|----------------|---------------|
| Petty     | 250            | 1.0           |
| Lesser    | 500            | 1.5           |
| Common    | 1,000          | 2.0           |
| Greater   | 1,500          | 3.0           |
| Grand     | 3,000          | 5.0           |

## Configuration

```ini
[Candidates]
; Set to false to disable soul gem recommendations entirely
bEnableSoulGemRecharge = true
```

## Technical References

- `src/input/EquipManager.cpp` — `UseSoulGem()` implementation
- `src/candidate/CandidateGenerator.cpp` — `GatherSoulGemCandidates()` (limited to 1 best gem)
- `src/display/WheelerBackend.cpp` — Soul gem FormID zeroing for Wheeler
- `src/state/StateManager_Equipment.cpp` — Weapon charge reading via `kRightItemCharge` ActorValue

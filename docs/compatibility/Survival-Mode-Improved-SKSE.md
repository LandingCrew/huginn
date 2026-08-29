# Survival Mode Compatibility

## Overview

Huginn's vitals tracking system needs to display "true base max" HP/MP/SP values - the maximum values before survival debuffs like hunger, cold, and fatigue are applied. This document explains how we achieve compatibility with multiple survival mod implementations.

## The Problem

Skyrim's actor value system has multiple layers:
- **Base Value**: The racial/level-up base (e.g., 100 Health)
- **Permanent Modifiers**: From perks, abilities, leveling (+65 from leveling)
- **Temporary Modifiers**: From potions, spells, enchantments
- **Damage Modifiers**: From diseases, damage taken, **survival penalties**

When a player has 165 base Health but 88 effective Health due to survival mode hunger penalty:
- `GetActorValue()` returns current value (e.g., 29 if damaged)
- `GetPermanentActorValue()` returns base + permanent mods **including survival penalty** (88)
- We need to show the "true" value: 165 (before survival penalty)

## Different Survival Mod Implementations

### 1. Survival Mode Improved SKSE
- **Tracking Method**: Uses Variable Actor Values (kVariable02/03/04)
- **How it works**:
  - Applies penalties via `RestoreActorValue(kPermanent, ...)`
  - Stores the penalty amount in Variable AVs
  - `kVariable02` = Stamina penalty (hunger)
  - `kVariable03` = Magicka penalty (fatigue)
  - `kVariable04` = Health penalty (cold)
- **Detection**: Check if Variable AVs contain non-zero values
- **Formula**: `TrueBaseMax = Temporary + Permanent + VariableAV`

**Source**: [Survival Mode Improved SKSE](https://github.com/colinswrath/Survival-Mode-Improved-SKSE)

### 2. Vanilla Creation Club Survival Mode
- **Tracking Method**: Unknown (may use different implementation)
- **Compatibility**: Graceful fallback - if Variable AVs are 0, formula becomes `Temporary + Permanent`
- **Status**: Partially compatible (may not recover full base max)

### 3. Other Survival Mods (Frostfall, iNeed, Campfire, etc.)
- **Compatibility**: Depends on implementation
- **If they use damage modifiers**: Will work with effective max display
- **If they use Variable AVs**: Will work with full base max recovery

## Our Implementation

### Formula

```cpp
TrueBaseMax = Temporary + Permanent + SurvivalPenalty
```

Where:
- **Temporary** = `GetActorValueModifier(kTemporary, AV)` - potion/spell buffs
- **Permanent** = `GetPermanentActorValue(AV)` - base + permanent mods (with survival penalty already applied)
- **SurvivalPenalty** = `GetActorValue(kVariableNN)` - the amount subtracted by survival mod

### Code Location

**File**: `src/context/ContextSensor.cpp` (lines 190-235)

```cpp
// Get permanent actor value (base + permanent mods, including survival penalties as negatives)
float permHealth = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
float permMagicka = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kMagicka);
float permStamina = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kStamina);

// Get survival mode penalty amounts (stored in Variable AVs by some survival mods)
float survivalHealthPenalty = actorValueOwner->GetActorValue(RE::ActorValue::kVariable04);  // Cold
float survivalMagickaPenalty = actorValueOwner->GetActorValue(RE::ActorValue::kVariable03); // Fatigue
float survivalStaminaPenalty = actorValueOwner->GetActorValue(RE::ActorValue::kVariable02); // Hunger

// True base max = Temporary + Permanent + StoredPenalty
newState.baseMaxHealth = tempHealth + permHealth + survivalHealthPenalty;
newState.baseMaxMagicka = tempMagicka + permMagicka + survivalMagickaPenalty;
newState.baseMaxStamina = tempStamina + permStamina + survivalStaminaPenalty;
```

### Automatic Detection

The system automatically detects if survival penalties are being tracked:

```cpp
bool hasSurvivalPenalties = (survivalHealthPenalty > 0.1f ||
                              survivalMagickaPenalty > 0.1f ||
                              survivalStaminaPenalty > 0.1f);
```

**Debug output when detected**:
```
[Vitals Debug] Survival mod detected: Using Variable AV penalty tracking
```

### Graceful Fallback

If no Variable AV tracking is detected (values are 0):
- Formula becomes: `TrueBaseMax = Temporary + Permanent + 0`
- This still works correctly for most cases
- No special handling needed
- No error messages or warnings

## Example Debug Output

### With Survival Mode Improved SKSE (hunger penalty active)

```
[Vitals Debug] Permanent (base+mods): HP=165, MP=202, SP=88
[Vitals Debug] Temporary buffs: HP=0, MP=0, SP=0
[Vitals Debug] Survival penalties (Variable AVs): HP=0, MP=73, SP=58
[Vitals Debug] Survival mod detected: Using Variable AV penalty tracking
[Vitals Debug] True Base Max: HP=165, MP=275, SP=146
[Vitals Debug] Effective Max: HP=165, MP=202, SP=88
```

**Explanation**:
- Permanent values already have penalties applied (SP=88 instead of 146)
- Variable AVs store the penalty amounts (SP penalty = 58)
- True base max recovers the original value (88 + 58 = 146)
- Effective max shows what the game HUD displays (88)

### Without Survival Mod

```
[Vitals Debug] Permanent (base+mods): HP=165, MP=275, SP=146
[Vitals Debug] Temporary buffs: HP=0, MP=0, SP=0
[Vitals Debug] Survival penalties (Variable AVs): HP=0, MP=0, SP=0
[Vitals Debug] True Base Max: HP=165, MP=275, SP=146
[Vitals Debug] Effective Max: HP=165, MP=275, SP=146
```

**Explanation**:
- No survival penalties, so Permanent = True Base Max
- All Variable AVs are 0 (no penalty tracking)
- No detection message logged
- Base and effective max are the same

## UI Display

The debug widget shows both values:

```
HP: 100% (165/165)                  ← No debuff, base = effective
MP: 73%  (202/275) [202 eff]        ← Debuffed, shows both values
SP: 20%  (29/146) [88 eff]          ← Debuffed, shows both values
```

**Format**:
- `(current/baseMax)` - always shows base max as denominator
- `[X eff]` - only shown when effective max differs from base max

## Compatibility Matrix

| Mod | Variable AV Tracking | Base Max Recovery | Effective Max Display |
|-----|---------------------|-------------------|----------------------|
| Survival Mode Improved SKSE | ✅ Yes | ✅ Full | ✅ Accurate |
| Vanilla CC Survival Mode | ❓ Unknown | ⚠️ Partial | ✅ Accurate |
| No Survival Mod | N/A | ✅ Full | ✅ Accurate |
| Custom Survival Mods | ❓ Depends | ⚠️ Varies | ✅ Accurate |

**Legend**:
- ✅ Full support
- ⚠️ Partial support (may not show true base max, but won't break)
- ❓ Unknown/untested

## Testing Recommendations

When testing with different survival mods:

1. **Check debug log** for "Survival mod detected" message
2. **Verify Variable AVs** contain non-zero penalty values
3. **Compare base max** with expected values (check character screen)
4. **Test with buffs** (potions, enchantments) to verify Temporary modifiers work
5. **Test with debuffs** (diseases) to verify Damage modifiers are separate

## Future Improvements

Potential enhancements for better compatibility:

1. **Config option** to force Variable AV tracking on/off
2. **Alternative detection methods** for vanilla CC Survival Mode
3. **Custom penalty calculation** for mods that don't use Variable AVs
4. **MCM integration** to let users specify their survival mod <-dmenu?

## References

- [CommonLibSSE-NG ActorValueOwner API](https://ng.commonlib.dev/_actor_value_owner_8h_source.html)
- [Survival Mode Improved SKSE Source](https://github.com/colinswrath/Survival-Mode-Improved-SKSE)
- [UESP: Skyrim Survival Mode](https://en.uesp.net/wiki/Skyrim:Survival_Mode)
- [UESP: Actor Value Indices](https://en.uesp.net/wiki/Skyrim_Mod:Actor_Value_Indices)

## Survival State Detection (v0.6.7+)

### Overview

Huginn detects survival states (cold, hunger, fatigue levels) to provide contextual recommendations. For example, recommending Resist Frost potions when the player is freezing.

### Supported: CC Survival Mode Only

**Huginn reads survival state directly from CC Survival Mode globals** (`ccqdrsse001-survivalmode.esl`). This is more reliable than parsing magic effect names.

**FormIDs used** (derived from [Survival Mode Improved SKSE](https://github.com/colinswrath/Survival-Mode-Improved-SKSE)):
- Cold need value: `0x81B`
- Hunger need value: `0x81A`
- Exhaustion need value: `0x816`
- Survival mode enabled: `0x826`

### Cold Level Thresholds

Based on [UESP: Cold](https://en.uesp.net/wiki/Skyrim:Cold):

| Raw Value | Level | Display Name |
|-----------|-------|--------------|
| 0-49 | 0 | Warm |
| 50-119 | 1 | Comfortable |
| 120-299 | 2 | Chilly |
| 300-499 | 3 | Very Cold |
| 500-799 | 4 | Freezing |
| 800-1000 | 5 | Numb |

### Not Supported

The following survival mods are **not supported** for state detection:

| Mod | Reason |
|-----|--------|
| **Frostfall** | Uses its own global variables, not CC Survival globals |
| **iNeed** | Uses different tracking system |
| **Campfire** | Companion mod to Frostfall |
| **Sunhelm** | Uses its own implementation |
| **Custom survival mods** | Unless they modify CC Survival globals |

### Graceful Fallback

If CC Survival Mode is not installed:
- `LookupForm` returns `nullptr` for all globals
- Cold/hunger/fatigue levels default to 0 (Warm/Satisfied/Refreshed)
- `survivalModeActive` stays `false`
- No errors or warnings - the system simply reports neutral survival state

### Debug Logging

On first poll, the system logs what globals were found:

```
[StateManager] Survival globals cache: Cold=found, Hunger=found, Exhaustion=found, Enabled=found
```

If CC Survival Mode is not installed:
```
[StateManager] Survival globals cache: Cold=null, Hunger=null, Exhaustion=null, Enabled=null
```

### Adding Support for Other Mods

To add support for Frostfall or other survival mods:
1. Research the mod's global variable FormIDs
2. Add cached pointers in `StateManager.h`
3. Add lookup logic in `CacheSurvivalGlobals()`
4. Add reading logic in `PollPlayerSurvival()` with appropriate thresholds

PRs welcome for additional survival mod support.

## SMI-Specific Integration (v0.12.x+)

### Overview

When Survival Mode Improved SKSE (`SurvivalModeImproved.esp`) is detected, Huginn reads **pre-computed stage globals** instead of converting raw 0-1000 values with hardcoded thresholds. This is more reliable because SMI may adjust internal thresholds or value scaling.

### SMI Globals Used

| FormID | Global | Type | Description |
|--------|--------|------|-------------|
| `0xA14` | `SMI_CurrentHungerNeedStage` | int (0-5) | Pre-computed hunger stage |
| `0xA1C` | `SMI_CurrentExhaustionNeedStage` | int (0-5) | Pre-computed exhaustion stage |
| `0xD1E` | `SMI_CurrentColdNeedStage` | int (0-5) | Pre-computed cold stage |
| `0xF27` | `SMI_HungerShouldBeEnabled` | float (0/1) | Per-need enable flag |
| `0xF28` | `SMI_ColdShouldBeEnabled` | float (0/1) | Per-need enable flag |
| `0xF29` | `SMI_ExhaustionShouldBeEnabled` | float (0/1) | Per-need enable flag |

### Per-Need Enable/Disable

SMI allows players to disable individual survival needs (e.g., disable hunger while keeping cold). Huginn respects these flags:

- If `SMI_*ShouldBeEnabled` is `0.0`, the corresponding need is treated as NEUTRAL (level 0)
- If the enable flag global is not found (null), the need is assumed enabled (safe fallback)
- This prevents showing food recommendations when hunger tracking is turned off

### Detection Logic

```
CacheSurvivalGlobals():
  1. Look up CC SM globals (always, for the Survival_ModeEnabled toggle)
  2. Look up SMI globals from SurvivalModeImproved.esp
  3. Set m_smiInstalled = true if ANY stage global is found

PollPlayerSurvival():
  1. Check Survival_ModeEnabled global (single source of truth)
  2. If SMI installed: read stage globals, respect enable flags
  3. If SMI not installed: convert raw values via UESP thresholds
```

### Log Output

On game load with SMI installed:
```
[StateManager] Survival globals cache: Cold=found, Hunger=found, Exhaustion=found, Enabled=found
[StateManager] SMI detected: HungerStage=found, ColdStage=found, ExhaustionStage=found
[StateManager] Native warmth function cached
```

Without SMI:
```
[StateManager] Survival globals cache: Cold=found, Hunger=found, Exhaustion=found, Enabled=found
[StateManager] SMI not installed
[StateManager] Native warmth function cached
```

## Key Takeaways

1. **Variable AVs (kVariable02/03/04)** are used by some survival mods to track penalties
2. **Automatic detection** makes the system work with or without Variable AV tracking
3. **Graceful fallback** ensures compatibility even with unknown mods
4. **Debug logging** helps diagnose compatibility issues
5. **No configuration needed** - works out of the box with most setups
6. **Survival state detection** supports CC Survival Mode + SMI (v0.12.x+)
7. **SMI per-need flags** are respected — disabled needs show as neutral

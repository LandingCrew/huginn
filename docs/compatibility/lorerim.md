# Huginn - Lorerim Compatibility

This document provides Lorerim-specific compatibility information and plugin configurations for Huginn.

---

## Overview

Lorerim is a comprehensive modpack built on **Requiem** with **Magic Redone** layered on top. This fundamentally changes how magic works compared to vanilla Skyrim.

### Key Differences from Vanilla

| Aspect | Vanilla | Lorerim (Requiem + Magic Redone) |
|--------|---------|----------------------------------|
| Magicka Regen | Always regenerates | **Stops in combat** |
| Spell Costs | Fixed by spell level | Scales with skill level (high at low skill) |
| Perk Gating | Minor bonuses | **Perks required for effectiveness** |
| Spell Balance | Per-spell | Entire magic system rebalanced |
| Survival | Optional CC | Integrated survival needs |

---

## Core Systems

### Requiem Detection

```cpp
bool IsRequiemLoaded() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    return dh->LookupModByName("Requiem.esp") != nullptr;
}

bool IsLorerimLoaded() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    return dh->LookupModByName("Requiem.esp") != nullptr &&
           dh->LookupModByName("Requiem - Magic Redone.esp") != nullptr;
}
```

### Magicka Weight Adjustments

Since magicka doesn't regenerate in combat, remaining magicka is more valuable:

```cpp
float GetMagickaValueMultiplier(const ContextState& ctx) {
    if (!IsLorerimLoaded()) return 1.0f;
    if (!ctx.inCombat) return 1.0f;

    // In combat, magicka is finite - weight remaining pool heavily
    if (ctx.magickaPercent < 0.2f) return 2.5f;  // Critical
    if (ctx.magickaPercent < 0.4f) return 1.8f;  // Low
    if (ctx.magickaPercent < 0.6f) return 1.3f;  // Moderate
    return 1.0f;
}
```

### Melee Fallback

Melee weapons become important earlier when magicka won't regenerate:

```cpp
float GetWeight_MeleeWeapon_Lorerim(const ContextState& ctx) {
    if (!ctx.inCombat) return 0.1f;

    // Earlier threshold than vanilla (40% vs 10%)
    if (ctx.magickaPercent < 0.15f) return 8.0f;  // Almost empty
    if (ctx.magickaPercent < 0.30f) return 5.0f;  // Low
    if (ctx.magickaPercent < 0.45f) return 2.0f;  // Getting low

    return 0.1f;
}
```

---

## Relevant Mods

### Framework Mods (Runtime Distribution)

These mods distribute spells, keywords, or items at runtime - Huginn needs to be aware of dynamically added content.

- **Spell Perk Item Distributor (SPID)** - Distributes spells/perks to NPCs at runtime
- **Keyword Item Distributor (KID)** - Distributes keywords at runtime
- **Keyword Patch Collection** - Adds keywords to items
- **FormList Manipulator (FLM)** - Modifies formlists at runtime
- **SkyPatcher** - Runtime patching framework
- **Object Categorization Framework** - Categorizes objects with keywords

### Perk/Magic Overhauls

| Mod | Notes |
|-----|-------|
| Requiem - The Roleplaying Overhaul | Complete gameplay overhaul |
| Requiem - Magic Redone | Major magic system changes |
| Requiem - Alchemy Redone | Changes potion effects |
| Ordinator - Perks of Skyrim | Perk overhaul affecting magic perks |
| Spell Absorption Rework | Changes spell absorption mechanics |

### Spell Mods

#### Major Spell Packs

| Mod | ESP Name | Notes |
|-----|----------|-------|
| Apocalypse | `Apocalypse - Magic of Skyrim.esp` | 100+ spells, all schools |
| Triumvirate | `Triumvirate - Mage Archetypes.esp` | Class-based spell packs |

#### Specialized Magic Mods

| Mod | ESP Name | Magic Type |
|-----|----------|------------|
| Runemaster | `Runemaster.esp` | Rune-based |
| Wizarding Traversal | `WizardingTraversal.esp` | Movement/teleport |
| Holy Templar | `HolyTemplarMagic.esp` | Paladin/Restoration |
| Elemental Mastery | `ElementalMastery.esp` | Elemental |
| Wildwaker | `WildwakerMagic.esp` | Nature/beast |
| Frostbitten Dreams | `FrostbittenDreams.esp` | Ice-themed |
| Ancient Blood Magic II | `AncientBloodII.esp` | Blood magic (health cost) |
| Dark Hierophant | `DarkHierophant.esp` | Death/dark |
| Obscure Magic | `ObscureMagic.esp` | Miscellaneous |
| Sonic Magic | `SonicMage.esp` | Sound-based |
| Constellation Magic | `ConstellationMagic.esp` | Star-themed |
| Abyssal Tides | `AbyssalTides.esp` | Water/ocean |
| Survival Spells | `SurvivalSpells.esp` | Utility |

### Vampire/Werewolf

Lorerim uses **Sacrilege** (vampires) and **Growl** (werewolves).

```cpp
// Vampire sun damage is enhanced - Resist Fire/Sun spells more valuable
float GetWeight_ResistSun_Vampire(const ContextState& ctx) {
    if (!IsPlayerVampire()) return 0.0f;
    if (ctx.isInterior) return 0.0f;

    // Check time of day
    if (ctx.timeOfDay > 6.0f && ctx.timeOfDay < 20.0f) {
        return 8.0f;  // Daytime = high priority
    }
    return 0.0f;
}
```

---

## Creature Classification

Lorerim adds many creatures via Mihail mods that need proper classification.

### Mihail Creature Mods

- Mammoth Expansion, Giants Overhaul, Kagoutis and Guars, Nix-Hounds
- Undead Werewolves, Wraiths, Sea Giants, Minotaurs SE, Cliff Racers

### New Creature Types

```json
{
  "actor_types": {
    "races": {
      "MihailSeaGiantRace": "Beast",
      "MihailMinotaurRace": "Beast",
      "MihailGoblinRace": "Beast",
      "MihailWraithRace": "Undead",
      "MihailUndeadWerewolfRace": "Undead",
      "MihailCliffRacerRace": "Beast",
      "MihailNixHoundRace": "Beast",
      "MihailKagoutiRace": "Beast",
      "MihailGuarRace": "Beast",
      "ForswornMinotaurRace": "Beast",
      "GRAHLIceTrollRace": "Beast",
      "BloodHorkerRace": "Beast",
      "CannibalDraugrRace": "Undead",
      "FireTonguedDaedrothRace": "Construct",
      "FairyRace": "Construct"
    }
  }
}
```

---

## Survival Integration

Lorerim uses **Survival Mode Improved** (enhanced CC Survival).

### Survival State Detection

```json
{
  "context": {
    "survival": {
      "active_when": {
        "global": "Survival_ModeEnabled",
        "value": "> 0"
      },
      "needs": {
        "hunger": {
          "actor_value": "Survival_HungerNeed",
          "buckets": [
            { "name": "Full", "max": 100 },
            { "name": "Satisfied", "max": 300 },
            { "name": "Peckish", "max": 500 },
            { "name": "Hungry", "max": 700 },
            { "name": "Starving", "max": 999999 }
          ]
        },
        "cold": {
          "actor_value": "Survival_ColdNeed",
          "buckets": [
            { "name": "Warm", "max": 100 },
            { "name": "Chilly", "max": 300 },
            { "name": "Cold", "max": 500 },
            { "name": "Freezing", "max": 999999 }
          ]
        },
        "fatigue": {
          "actor_value": "Survival_ExhaustionNeed",
          "buckets": [
            { "name": "Rested", "max": 100 },
            { "name": "Tired", "max": 400 },
            { "name": "Exhausted", "max": 999999 }
          ]
        }
      }
    }
  }
}
```

---

## Weight Adjustment Summary

| Context | Vanilla | Lorerim | Reason |
|---------|---------|---------|--------|
| Melee fallback (MP < 15%) | +4.0 | +8.0 | No regen, critical |
| Melee fallback (MP < 30%) | 0.0 | +5.0 | Earlier threshold |
| Melee fallback (MP < 45%) | 0.0 | +2.0 | Even earlier |
| Restore Magicka potion | +2.0 | +5.0 | Much more valuable |
| Low-cost spells in combat | ×1.0 | ×1.5 | Efficiency critical |
| High-cost spells (can't afford) | ×0.1 | ×0.0 | Don't show |
| Sun resistance (vampire, day) | +8.0 | +10.0 | Enhanced sun damage |

---

## Lorerim Plugin File

Complete plugin configuration for Lorerim:

```json
{
  "plugin_name": "Lorerim",
  "plugin_version": "1.0.0",
  "Huginn_min_version": "0.6.0",
  "required_esp": "Requiem - Magic Redone.esp",
  "depends_on": ["Requiem.esp"],

  "settings": {
    "magicka_regen_in_combat": false,
    "melee_fallback_threshold": 0.45,
    "melee_fallback_weight_base": 2.0,
    "melee_fallback_weight_critical": 8.0,
    "restore_magicka_weight_boost": 2.5,
    "spell_cost_efficiency_multiplier": 1.5,
    "unaffordable_spell_weight": 0.0
  },

  "spell_mods": [
    "Apocalypse - Magic of Skyrim.esp",
    "Triumvirate - Mage Archetypes.esp",
    "AncientBloodII.esp",
    "HolyTemplarMagic.esp",
    "WildwakerMagic.esp",
    "Runemaster.esp",
    "ConstellationMagic.esp",
    "SonicMage.esp",
    "AbyssalTides.esp",
    "DarkHierophant.esp",
    "ObscureMagic.esp",
    "FrostbittenDreams.esp",
    "WizardingTraversal.esp",
    "ElementalMastery.esp",
    "SurvivalSpells.esp"
  ],

  "actor_types": {
    "races": {
      "MihailSeaGiantRace": "Beast",
      "MihailMinotaurRace": "Beast",
      "MihailGoblinRace": "Beast",
      "MihailWraithRace": "Undead",
      "MihailUndeadWerewolfRace": "Undead",
      "ForswornMinotaurRace": "Beast",
      "GRAHLIceTrollRace": "Beast",
      "CannibalDraugrRace": "Undead",
      "FireTonguedDaedrothRace": "Construct"
    }
  },

  "items": {
    "food": {
      "keywords": ["VendorItemFood", "VendorItemFoodRaw"],
      "weight_overrides": {
        "hunger_hungry": 6.0,
        "hunger_starving": 10.0
      }
    },
    "warming": {
      "keywords": ["MAG_FortifyResistFrost", "Survival_WarmingItem"],
      "weight_overrides": {
        "cold_cold": 6.0,
        "cold_freezing": 10.0
      }
    },
    "restore_magicka": {
      "keywords": ["VendorItemPotion"],
      "effects": ["AlchRestoreMagicka"],
      "weight_multiplier": 2.5
    }
  }
}
```

---

## Framework Considerations

### SPID (Spell Perk Item Distributor)

Lorerim uses SPID to distribute spells at runtime. Huginn needs to:
- Refresh spell registry after game load (1-2 second delay)
- Re-scan when spell list changes

### KID (Keyword Item Distributor)

Keywords may be added at runtime. Huginn should:
- Not cache keyword lookups permanently
- Refresh classification on reconciliation

---

## Integration Notes

1. **Requiem** is the core overhaul - all magic costs, regeneration, and spell scaling flows through it
2. **SPID/KID** mean spells may be added to NPCs/player at runtime - static form detection may miss these
3. **Survival Mode Improved** provides the survival state hooks
4. **Mihail creatures** add many new creature types needing classification
5. **Wintersun** blessings provide passive magical effects that may affect recommendations
6. **Sacrilege/Growl** transformations provide alternate ability sets
7. **Magic Redone** patches affect most spell mod integrations

---

## See Also

- [mod-compatibility.md](mod-compatibility.md) - General mod compatibility
---

*This document is specific to Lorerim. For other modpacks, create similar compatibility files.*

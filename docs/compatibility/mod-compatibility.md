# Huginn Mod Compatibility Guide

This document provides compatibility information for popular Skyrim mods and INI override templates.

---

## Compatibility Overview

| Mod Category | Risk Level | Mitigation Strategy |
|--------------|------------|---------------------|
| Spell Mods | Medium-High | Multi-effect analysis, keyword fallback, override INI |
| Perk Overhauls | Low-Medium | Player-relative cost calculation |
| Combat Mods | Low | Binary combat state is transparent |
| Enemy Mods | Medium | Keyword-based TargetType detection |
| UI Mods | Low | Wheeler API handles UI layer |
| Framework Mods | Medium | Event hooks, cache invalidation |

---

## Spell Mods

### Apocalypse - Magic of Skyrim

**Impact:** Major - 100+ new spells across all schools

**Compatibility Notes:**
- Most spells use standard archetypes and will auto-classify
- Some unique spell types need manual override
- Concentration spells work normally

**Override Template:**
```ini
[Apocalypse_Overrides]
; Alteration
"Entomb"=type:Utility,tags:Crowd Control
"Deep Storage"=type:Utility,tags:Storage
"Fabricate Object"=type:Utility,tags:Crafting

; Conjuration - Unique summons
"Conjure Ash Guardian"=type:SummonCreature,tags:Summon,Defensive
"Conjure Avenger"=type:SummonCreature,tags:Summon,Offensive
"Conjure Herne"=type:SummonCreature,tags:Summon,Beast
"Conjure Xivilai"=type:SummonCreature,tags:Summon,Daedric

; Destruction - Special damage types
"Bolide"=type:DamageFire,tags:AOE,Ranged,Projectile
"Forbidden Sun"=type:DamageFire,tags:AOE,Ranged
"Incendiary Flow"=type:DamageFire,tags:AOE,Hazard
"Twister"=type:DamagePhysical,tags:AOE,Crowd Control
"Sleet Storm"=type:DamageFrost,tags:AOE,Slow
"Electrosphere"=type:DamageShock,tags:AOE,Sustained

; Illusion
"Pale Shadow"=type:Illusion,tags:Summon,Clone
"Evil Twin"=type:Illusion,tags:Summon,Clone
"Compelling Whispers"=type:Illusion,tags:Crowd Control

; Restoration
"Circle of Strength"=type:RestoreStamina,tags:AOE,Buff
"Leech Seed"=type:RestoreHealth,tags:Drain,DOT
"Welling Blood"=type:RestoreHealth,tags:Buff
```

---

### Triumvirate - Mage Archetypes

**Impact:** Major - Archetype-specific spell packs (Druid, Warlock, Shaman, etc.)

**Compatibility Notes:**
- Nature spells may not match standard archetypes
- Some spells have multiple effect types
- Archetype-specific mechanics


**Override Template:**
```ini
[Triumvirate_Overrides]
; Druid spells
"Overgrowth"=type:SummonCreature,tags:Nature,Summon
"Wild Spirits"=type:SummonCreature,tags:Nature,Summon
"Ensnaring Vines"=type:Utility,tags:Nature,Crowd Control
"Nature's Bounty"=type:RestoreHealth,tags:Nature,HOT

; Shaman spells
"Ancestral Guardian"=type:SummonCreature,tags:Spirit,Summon
"Spirit Walk"=type:Utility,tags:Spirit,Movement
"Totem of Wrath"=type:DamageMagic,tags:Spirit,AOE

; Warlock spells
"Eldritch Blast"=type:DamageMagic,tags:Eldritch,Ranged
"Hex"=type:Debuff,tags:Curse,DOT
"Dark Pact"=type:Buff,tags:Blood,Self
```

---

### Mysticism - A Magic Overhaul

**Impact:** Major - Overhauls vanilla magic, adds new spells

**Compatibility Notes:**
- Modifies vanilla spell costs and effects
- New spell archetypes (Sun damage, etc.)
- Better balanced for vanilla+ gameplay

**Override Template:**
```ini
[Mysticism_Overrides]
; Sun damage spells (anti-undead)
"Sun Fire"=type:DamageSun,tags:Holy,Anti-Undead
"Vampire's Bane"=type:DamageSun,tags:Holy,Anti-Undead
"Stendarr's Aura"=type:Cloak,tags:Holy,Anti-Undead

; New utility spells
"Spelldancer"=type:Buff,tags:Movement,Alteration
```

---

### Ancient Blood Magic II

**Impact:** Major - Unique blood magic school

**Compatibility Notes:**
- Blood spells cost health instead of magicka
- Self-damage mechanics require special handling
- Override health cost detection

**Override Template:**
```ini
[BloodMagic_Overrides]
; Blood damage spells
"Blood Bolt"=type:DamageMagic,tags:Blood,Ranged,HealthCost
"Hemorrhage"=type:DamageMagic,tags:Blood,DOT,HealthCost
"Blood Storm"=type:DamageMagic,tags:Blood,AOE,HealthCost

; Blood healing (drains enemies)
"Sanguine Drain"=type:RestoreHealth,tags:Blood,Drain,HealthCost
"Blood Pact"=type:RestoreHealth,tags:Blood,Self,HealthCost

; Blood buffs
"Blood Frenzy"=type:Buff,tags:Blood,Damage,HealthCost
"Crimson Aegis"=type:ArmorBuff,tags:Blood,Defensive,HealthCost
```

---

### Other Spell Mods

| Mod | Notes | Override Priority |
|-----|-------|-------------------|
| **Odin** | Vanilla+ style, mostly auto-classifies | Low |
| **Elemental Destruction Magic** | New elements (Earth, Water, Wind) | Medium |
| **Arcanum** | Complex multi-school spells | High |
| **Forgotten Magic Redone** | Leveling spells, special effects | Medium |
| **Phenderix Magic Evolved** | Many spells, mixed quality | Medium |
| **Lost Grimoire** | Standard spell types | Low |

---

## Perk Overhauls

### Requiem - The Roleplaying Overhaul

**Impact:** Critical - Complete game overhaul

**Compatibility Notes:**
- Spell costs completely reworked
- Magicka regeneration stops in combat
- Perks dramatically affect spell effectiveness
- Level-gated spell effectiveness

**Required Adaptations:**
```cpp
// Requiem-specific cost calculation
float GetRequiemMagickaCost(RE::SpellItem* spell) {
    auto* player = RE::PlayerCharacter::GetSingleton();

    // Requiem modifies costs based on skill level and perks
    // Standard CalculateMagickaCost should capture this
    float cost = spell->CalculateMagickaCost(player);

    // Requiem stops magicka regen in combat - factor this in
    if (IsInCombat() && !HasRegenPerk()) {
        // Weight spells by remaining magicka more heavily
        cost *= 1.5f;  // Effective cost higher when no regen
    }

    return cost;
}

// Detect Requiem presence
bool IsRequiemLoaded() {
    return RE::TESDataHandler::GetSingleton()
        ->LookupModByName("Requiem.esp") != nullptr;
}
```

**Override Template:**
```ini
[Requiem_Settings]
; Requiem-specific weight adjustments
MagickaRegenInCombat=false
SpellCostMultiplier=1.5
LowMagickaPenaltyThreshold=0.3

; Requiem makes melee fallback more important
MeleeWeaponWeightBoost=2.0
```

---

### Ordinator - Perks of Skyrim

**Impact:** Major - Complete perk overhaul

**Compatibility Notes:**
- Dynamic cost modifiers (Quadratic Wizard, etc.)
- Conditional bonuses not detectable
- Dual-cast multipliers changed
- Perk-added spell effects

**Notes:**
- `CalculateMagickaCost(player)` captures most perk reductions
- Some perks add effects at cast time (undetectable)
- Vancian Magic completely changes magicka system

---

### Adamant / Vokrii

**Impact:** Medium - Lighter perk touches

**Compatibility Notes:**
- Mostly compatible with standard cost calculation
- Minor conditional bonuses

---

### Lorerim (Requiem + Magic Redone)

**Impact:** Critical - Complete overhaul stack

Lorerim uses Requiem as its base with **Requiem - Magic Redone** layered on top, plus numerous patches for spell mods.

**Key Differences from Vanilla:**
- Magicka regeneration **stops in combat** (Requiem)
- Spell costs are **dramatically higher** at low skill levels
- Perks **gate spell effectiveness** (novice spells weak without perks)
- Magic Redone rebalances all spell schools
- Many spell mods have specific Magic Redone patches

**Required Adaptations:**

```cpp
// Lorerim-specific detection
bool IsLorerimLoaded() {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    return dataHandler->LookupModByName("Requiem.esp") != nullptr &&
           dataHandler->LookupModByName("Requiem - Magic Redone.esp") != nullptr;
}

// Lorerim magicka is precious - weight by remaining pool more heavily
float GetLorerimMagickaWeight(float magickaPercent, bool inCombat) {
    float weight = 1.0f;

    // No regen in combat = magicka is finite resource
    if (inCombat) {
        if (magickaPercent < 0.3f) weight = 2.0f;      // Very precious
        else if (magickaPercent < 0.5f) weight = 1.5f; // Conserve
    }

    return weight;
}

// Melee fallback is more important in Lorerim
float GetLorerimMeleeWeight(const ContextState& ctx) {
    if (!ctx.inCombat) return 0.1f;

    // Earlier fallback threshold since magicka won't regen
    if (ctx.magickaPercent < 0.2f) return 6.0f;  // Higher than vanilla
    if (ctx.magickaPercent < 0.4f) return 3.0f;

    return 0.1f;
}
```

**Plugin Example:**

```json
{
  "plugin_name": "Lorerim",
  "required_esp": "Requiem - Magic Redone.esp",
  "depends_on": ["Requiem.esp"],

  "settings": {
    "magicka_regen_in_combat": false,
    "melee_fallback_threshold": 0.4,
    "melee_fallback_weight_boost": 2.0,
    "spell_cost_weight_multiplier": 1.5
  },

  "spell_mods": {
    "comment": "These spell mods have Magic Redone patches in Lorerim",
    "patched": [
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
      "WizardingTraversal.esp"
    ]
  }
}
```

**Lorerim-Specific Weight Adjustments:**

| Context | Vanilla Weight | Lorerim Weight | Reason |
|---------|---------------|----------------|--------|
| Melee fallback (magicka < 20%) | +4.0 | +6.0 | No regen in combat |
| Melee fallback (magicka < 40%) | 0.0 | +3.0 | Earlier threshold |
| Restore Magicka potion | +2.0 | +4.0 | More valuable |
| Low-cost spells | ×1.0 | ×1.3 | Efficiency matters |
| High-cost spells (can't afford) | ×0.1 | ×0.0 | Don't tease |

---

## Combat Mods

### MCO / Precision / Chocolate Poise

**Impact:** Major for combat state detection

**Compatibility Notes:**
- Combat state uses vanilla `IsInCombat()` - still works
- Stagger states may be more frequent
- Hit detection unchanged for spell feedback

**Detection Strategy:**
```cpp
// Combat mods don't change these APIs
bool inCombat = player->IsInCombat();  // Still works
float health = player->GetActorValue(RE::ActorValue::kHealth);  // Still works

// Poise mods add stagger - we detect via taking damage
bool isStaggered = player->IsStaggered();  // May fire more often
```

---

### TK Dodge / DMCO

**Impact:** Low - Adds dodge mechanics

**Compatibility Notes:**
- Stamina tracking may need adjustment
- Dodge i-frames don't affect recommendations

---

## Enemy Mods

### Keyword-Based Detection

All enemy mods should work with keyword detection. Standard keywords:

```cpp
// Vanilla keywords - work with all mods that follow convention
constexpr RE::FormID ActorTypeDragon   = 0x00035D59;
constexpr RE::FormID ActorTypeUndead   = 0x00013797;
constexpr RE::FormID ActorTypeDaedra   = 0x000131F8;
constexpr RE::FormID ActorTypeAnimal   = 0x00013795;
constexpr RE::FormID ActorTypeCreature = 0x00013794;
constexpr RE::FormID ActorTypeNPC      = 0x00013794;

// Detection order matters - check specific before general
TargetType GetTargetType(RE::Actor* actor) {
    if (actor->HasKeywordID(ActorTypeDragon))   return TargetType::Dragon;
    if (actor->HasKeywordID(ActorTypeUndead))   return TargetType::Undead;
    if (actor->HasKeywordID(ActorTypeDaedra))   return TargetType::Construct;
    if (actor->HasKeywordID(ActorTypeAnimal))   return TargetType::Beast;
    if (actor->HasKeywordID(ActorTypeCreature)) return TargetType::Beast;
    return TargetType::Humanoid;
}
```

### Mod-Specific Creatures

| Mod | Creatures | Classification |
|-----|-----------|----------------|
| **Mihail Monsters** | Goblins, Sea Giants, Wraiths | Use vanilla keywords |
| **Immersive Creatures** | Many new types | Most use vanilla keywords |
| **OBIS** | Bandit variants | Humanoid (default) |
| **Diverse Dragons** | Dragon variants | Dragon keyword present |

---

## UI Mods

### ImmersiveHUD SKSE
compatible

### Wheeler Integration

Huginn uses Wheeler for its action wheel.

**API Version Differences:**

| Feature | Wheeler v1 | Wheeler v2 |
|---------|------------|------------|
| Managed wheels | ✓ | ✓ |
| WheelStateCallback | ✓ | ✓ |
| ItemActivatedCallback | ⚠️ May not fire | ✓ |
| Entry subtext | ✗ | ✓ |
| Custom styling | ✗ | ✓ |

**Important:** The `ItemActivatedCallback` is required for contextual bandit learning feedback. If using Wheeler v1 (C0kAdam's version), Huginn may not receive notifications when players select items from the wheel, which means:
- Positive rewards for selecting recommended spells won't be applied
- Skip penalties may incorrectly trigger when scrolling between wheels

**Recommendation:** Use Wheeler v2 (dTry's version) for full Huginn integration.

**Potential Conflicts:**
- Other Wheeler clients sharing wheel space
- HUD position overlap with TrueHUD

**Resolution:**
- Huginn creates its own managed wheels (primary + alternate)
- Position configurable via dMenu (future)

### TrueHUD

**Impact:** Low - Separate HUD layer

**Compatibility Notes:**
- Boss bars don't conflict
- Target info doesn't conflict
- Widget positions may need adjustment

### Casting Bar Mods

**Impact:** Medium - Visual overlap potential

**Compatibility Notes:**
- Huginn's spell recommendations are separate from casting UI
- May want to hide Huginn widget while casting

---

## Framework Mods

### SPID (Spell Perk Item Distributor)

**Impact:** Medium - Distributes spells at runtime

**Compatibility Notes:**
- Spells distributed after game load
- 5-second reconciliation interval catches new spells
- May want faster initial scan

**Detection:**
```cpp
// Force spell registry refresh after SPID distribution
void OnDataLoaded() {
    // SPID runs during data load
    // Schedule reconciliation after short delay
    ScheduleReconciliation(1.0f);  // 1 second after load
}
```

### KID (Keyword Item Distributor)

**Impact:** Medium - Adds keywords at runtime

**Compatibility Notes:**
- Keywords may change after initial classification
- Cache invalidation needed if keywords change

---

## Survival Mode Detection

Huginn detects survival states (cold, hunger, fatigue) to provide contextual recommendations.

### Supported

| Mod | Support Level | Notes |
|-----|---------------|-------|
| **CC Survival Mode** | Full | Reads globals directly from `ccqdrsse001-survivalmode.esl` |
| **Survival Mode Improved** | Full | Uses same globals as CC Survival Mode |

### Not Supported

| Mod | Reason |
|-----|--------|
| **Frostfall** | Uses different global variables |
| **Sunhelm** | Uses different global variables |
| **iNeed** | Uses different tracking system |
| **Other survival mods** | Unless they modify CC Survival globals |

### Graceful Fallback

If CC Survival Mode is not installed:
- Cold/hunger/fatigue default to neutral (Warm/Satisfied/Refreshed)
- `survivalModeActive` stays `false`
- No errors or warnings

See [Survival-Mode-Improved-SKSE.md](Survival-Mode-Improved-SKSE.md) for detailed technical information.

---

## Extension System

Huginn supports two approaches for mod compatibility:

1. **INI Overrides** - Simple text-based spell classification (user-editable)
2. **Classification Plugins** - Community-maintained JSON plugins (auto-loaded)

### Design Philosophy

Rather than maintaining hardcoded checks for every mod, Huginn loads external classification data. This allows:
- Mod authors to ship Huginn compatibility files
- Community patches without Huginn updates
- User overrides for edge cases

### Plugin Architecture

```
Data/SKSE/Plugins/Huginn/
├── overrides/                    # User INI overrides (highest priority)
│   └── Huginn_Overrides.ini
├── plugins/                      # Community JSON plugins (auto-loaded)
│   ├── apocalypse.Huginn.json
│   ├── triumvirate.Huginn.json
│   ├── mysticism.Huginn.json
│   └── survival_mode.Huginn.json
└── Huginn.ini                     # Main config
```

### Plugin JSON Format

```json
{
  "plugin_name": "Apocalypse",
  "plugin_version": "1.0.0",
  "Huginn_min_version": "0.6.0",
  "required_esp": "Apocalypse - Magic of Skyrim.esp",

  "spells": {
    "by_name": {
      "Entomb": { "type": "Utility", "tags": ["CrowdControl"] },
      "Bolide": { "type": "DamageFire", "tags": ["AOE", "Ranged", "Projectile"] }
    },
    "by_formid": {
      "0x12345678": { "type": "DamageFrost", "tags": ["AOE"] }
    }
  },

  "keywords": {
    "add": {
      "MagicDamageNature": { "maps_to": "DamageMagic" }
    }
  },

  "actor_types": {
    "races": {
      "MyModGoblinRace": "Beast",
      "MyModWraithRace": "Undead"
    }
  },

  "context": {
    "survival_mode": {
      "detection": "global_variable",
      "form_id": "0x000ABC12",
      "hunger_av": "Survival_HungerNeed",
      "cold_av": "Survival_ColdNeed",
      "fatigue_av": "Survival_ExhaustionNeed"
    }
  }
}
```

### Plugin Loading Priority

1. **Built-in defaults** (lowest) - Vanilla spell classification
2. **Community plugins** (medium) - `plugins/*.Huginn.json`
3. **User INI overrides** (highest) - `overrides/*.ini`

### Conditional Loading

Plugins only load if their required ESP is present:

```cpp
void LoadPlugins() {
    auto* dataHandler = RE::TESDataHandler::GetSingleton();

    for (const auto& pluginFile : GetPluginFiles()) {
        auto plugin = LoadJSON(pluginFile);

        // Skip if required ESP not loaded
        if (!plugin.required_esp.empty()) {
            if (!dataHandler->LookupModByName(plugin.required_esp)) {
                SKSE::log::info("Skipping {} - {} not loaded",
                    pluginFile, plugin.required_esp);
                continue;
            }
        }

        RegisterPlugin(plugin);
    }
}
```

### Feature Flags

For optional features (like Survival Mode), plugins can expose feature flags:

```json
{
  "features": {
    "survival_mode": {
      "enabled_when": {
        "type": "global_variable",
        "form_id": "0x000ABC12",
        "condition": "> 0"
      },
      "context_fields": ["hunger", "cold", "fatigue"]
    }
  }
}
```

### Survival Mode Plugin Example

```json
{
  "plugin_name": "Survival Mode (CC)",
  "required_esp": "ccqdrsse001-survivalmode.esl",

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
  },

  "items": {
    "food": {
      "keywords": ["VendorItemFood", "VendorItemFoodRaw"],
      "survival_weight": {
        "hunger_hungry": 6.0,
        "hunger_starving": 10.0
      }
    },
    "warming": {
      "keywords": ["MAG_FortifyResistFrost", "Survival_WarmingItem"],
      "survival_weight": {
        "cold_cold": 6.0,
        "cold_freezing": 10.0
      }
    }
  }
}
```

### Sunhelm/Frostfall Plugin Example

```json
{
  "plugin_name": "Sunhelm",
  "required_esp": "SunhelmSurvival.esp",

  "context": {
    "survival": {
      "active_when": {
        "global": "SH_SurvivalEnabled",
        "value": "> 0"
      },
      "needs": {
        "hunger": { "actor_value": "SH_HungerLevel" },
        "cold": { "actor_value": "SH_ColdLevel" },
        "fatigue": { "actor_value": "SH_FatigueLevel" }
      }
    }
  }
}
```

---

## Override INI Format

### File Location

```
Data/SKSE/Plugins/Huginn/overrides/
├── Huginn_Overrides.ini          # User overrides (highest priority)
├── Apocalypse_Overrides.ini     # Mod-specific overrides (legacy)
├── Triumvirate_Overrides.ini
└── ...
```

### INI Syntax

```ini
[ModName_Overrides]
; By spell name (partial match)
"Spell Name"=type:EffectType,tags:Tag1,Tag2,Tag3

; By FormID (exact match, higher priority)
0x12345678=type:EffectType,tags:Tag1,Tag2

[ModName_Settings]
; Mod-specific settings
SettingName=value
```

### Effect Types

| Type | Description |
|------|-------------|
| `DamageFire` | Fire damage |
| `DamageFrost` | Frost damage |
| `DamageShock` | Shock damage |
| `DamageSun` | Sun/holy damage (anti-undead) |
| `DamageMagic` | Generic magic damage |
| `DamagePoison` | Poison damage |
| `DamagePhysical` | Physical/force damage |
| `RestoreHealth` | Health restoration |
| `RestoreMagicka` | Magicka restoration |
| `RestoreStamina` | Stamina restoration |
| `ArmorBuff` | Armor/flesh spells |
| `ResistFire` | Fire resistance |
| `ResistFrost` | Frost resistance |
| `ResistShock` | Shock resistance |
| `ResistMagic` | Magic resistance |
| `SummonCreature` | Conjuration summons |
| `BoundWeapon` | Bound weapon spells |
| `Cloak` | Cloak spells |
| `Illusion` | Calm/Fear/Frenzy |
| `Buff` | Generic buff |
| `Debuff` | Generic debuff |
| `Utility` | Utility spells |
| `Unknown` | Unclassified (uses contextual bandit learning) |

### Tags

Tags provide additional context for slot filtering:

| Tag | Description |
|-----|-------------|
| `AOE` | Area of effect |
| `DOT` | Damage over time |
| `HOT` | Heal over time |
| `Ranged` | Long range |
| `Melee` | Close range |
| `Self` | Self-targeted |
| `Summon` | Summons something |
| `Crowd Control` | CC effects |
| `Anti-Undead` | Effective vs undead |
| `HealthCost` | Costs health (blood magic) |

---

## Lorerim Modlist Reference

The following mods are included in the Lorerim modlist and have been analyzed:

### Spell Mods (Require Override INI)

| Mod | Priority | Status |
|-----|----------|--------|
| Apocalypse | High | Template provided |
| Triumvirate | High | Template provided |
| Ancient Blood Magic II | High | Template provided |
| Mysticism | Medium | Template provided |
| Runemaster Magic | Medium | TODO |
| Holy Templar Magic | Medium | TODO |
| Wildwaker Magic | Medium | TODO |
| Constellation Magic | Low | TODO |
| Sonic Magic | Low | TODO |

### Frameworks (Auto-Compatible)

- SPID - Works with reconciliation
- KID - Works with keyword detection
- OAR - No interaction
- PapyrusUtil - No interaction

### Combat (Auto-Compatible)

- MCO/Precision - Binary combat state works
- Chocolate Poise - No interaction
- TK Dodge - No interaction

---

## Troubleshooting

### Spell Not Appearing in Recommendations

1. Check if spell is in player's spell list
2. Check if spell cost exceeds current magicka
3. Check spell classification in logs
4. Add manual override in INI if needed

### Wrong Classification

1. Check spell name in logs
2. Add override INI entry with correct type
3. Report to Huginn team for future default

### Performance Issues

1. Check total spell count (200+ may slow scoring)
2. Enable spell caching in config
3. Increase update interval if needed

---

## See Also

- [../architecture/pipeline.md](../architecture/0-pipeline.md) - Classification pipeline
- [../architecture/slots.md](../architecture/5-slots.md) - Slot types and filtering

# Huginn Slot Architecture

This document describes the multi-page slot system for organizing and displaying recommendations (v0.12.x+).

> **Related documentation:**
> - [0-pipeline.md](0-pipeline.md) - Overall pipeline flow including slot allocation
> - [1-states.md](1-states.md) - State models used for override detection

---

## Overview

**Current Implementation (v0.12.x+):**
- Multi-page slot system (up to 10 pages, 10 slots per page)
- Effect-based slot classification (DamageAny, HealingAny, etc.)
- Item-type-based classification (PotionsAny, ScrollsAny, WeaponsAny, school-specific spells)
- Override system with priority-based urgent replacements
- SlotLocker for temporal stability (prevents flicker)
- Wildcard support for exploration
- INI-configurable slot layouts via `[Pages]`/`[PageN]`/`[PageN.SlotM]` sections

**Planned Features:**
- Archetype-based classification (Necromancer, Paladin, Druid)
- Dynamic threshold adjustment based on player behavior
- User profiles with saved layouts

---

## Multi-Page Slot System

```
+-------------------------------------------+
|              Pages (up to 10)             |
|  +------+ +------+ +------+     +------+ |
|  |Page 0| |Page 1| |Page 2| ... |Page 9| |
|  |N slot| |N slot| |N slot|     |N slot| |
|  +------+ +------+ +------+     +------+ |
|      |        |        |            |     |
|      +-----cycle keys 4/5----------+     |
+-------------------------------------------+
           |
           v
+-------------------------------------------+
|          Display Outputs                  |
|  IntuitionMenu  |  Wheeler  |  ImGui     |
|  (Scaleform HUD)|  (Radial) |  (Debug)   |
+-------------------------------------------+
```

**Key Features:**
- Each page has its own slot configuration (classification, priority, overrides)
- Slot count is per-page (configurable via `iSlotCount`, up to 10)
- Wheeler creates one radial wheel per page
- Page cycling via keys 4/5 (configurable)
- Each page can have different slot layouts (e.g., Page 0 = combat, Page 1 = utility)
- Code default: 1 page with 7 slots; shipped INI provides 10 named preset pages

**Constants** (`SlotSettings.h`):
```cpp
inline constexpr size_t MAX_PAGES = 10;
inline constexpr size_t MAX_SLOTS_PER_PAGE = 10;
```

---

## Slot Classification System

Determines what type of items can be assigned to a slot. Classifications are hierarchical:

### Effect-Based Classifications

| Classification | Matches | Examples |
|----------------|---------|----------|
| **DamageAny** | Fire, frost, shock, poison damage | Fireball, Ice Spike, Lightning Bolt, poisoned arrows |
| **HealingAny** | RestoreHealth spells/potions | Fast Healing, Close Wounds, health potions |
| **BuffsAny** | Armor, cloak, invisibility, muffle, fortify | Oakflesh, Flame Cloak, Invisibility, Muffle |
| **DefensiveAny** | Ward, resist potions, armor spells | Lesser Ward, Resist Fire potion, Stoneflesh |
| **SummonsAny** | Conjuration summons, bound weapons | Flame Atronach, Raise Zombie, Bound Sword |
| **Utility** | Light, detect, unlock, transmute, waterbreathing | Candlelight, Detect Life, Unlock, Waterbreathing |

### Item-Type-Based Classifications

| Classification | Matches | Examples |
|----------------|---------|----------|
| **PotionsAny** | Any potion (restore, resist, buff -- not food) | Health potion, Resist Fire, Fortify Smithing |
| **ScrollsAny** | Any scroll | Scroll of Fireball, Scroll of Heal Other |
| **SpellsAny** | Any spell (all schools) | Fireball, Fast Healing, Muffle, Conjure Familiar |
| **SpellsDestruction** | Destruction spells only | Fireball, Ice Spike, Chain Lightning |
| **SpellsRestoration** | Restoration spells only | Fast Healing, Lesser Ward, Turn Undead |
| **SpellsConjuration** | Conjuration spells only | Flame Atronach, Bound Sword, Raise Zombie |
| **SpellsIllusion** | Illusion spells only | Muffle, Invisibility, Calm, Frenzy |
| **SpellsAlteration** | Alteration spells only | Oakflesh, Detect Life, Candlelight, Paralysis |
| **WeaponsAny** | Any weapon (melee + ranged + staff) | Iron Sword, Hunting Bow, Staff of Fireballs |
| **WeaponsMelee** | Melee weapons only | Swords, axes, maces, daggers |
| **WeaponsRanged** | Ranged weapons only | Bows, crossbows, staves |
| **FoodAny** | Food items (Survival Mode) | Bread, cheese, stew |
| **AmmoAny** | Ammunition | Arrows, bolts |

### Regular (Unrestricted)

**Regular** classification accepts any candidate -- this is the legacy behavior before slot classification was added.

---

## Slot Configuration Structure

**SlotConfig Fields** (`SlotConfig.h`):

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `classification` | `SlotClassification` | `Regular` | What type of items can appear |
| `wildcardsEnabled` | `bool` | `true` | Allow wildcard exploration picks |
| `overrideFilter` | `OverrideFilter` | `Any` | Which override categories accepted (None, Any, HP, MP, SP) |
| `skipEquipped` | `bool` | `false` | Skip candidates already equipped (show alternatives only) |
| `priority` | `int8_t` | `0` | Allocation order (higher = filled first) |

---

## Slot Assignment Architecture

The slot allocation pipeline produces `SlotAssignment` objects, not raw content types. There are two separate type systems:

### AssignmentType (allocator output)

What the SlotAllocator produces (`SlotAssignment.h`):

| Type | Meaning | Notes |
|------|---------|-------|
| **Empty** | Slot has no content | Slot hidden in UI |
| **Normal** | Standard utility-based assignment | Most common |
| **Override** | Forced by override condition | Critical health, drowning, etc. |
| **Wildcard** | Exploration pick | Blue styling in widget |

### SlotVisualState (animation hints)

Drives per-slot animation effects, computed by the pipeline based on slot lifecycle:

| State | Meaning | Visual |
|-------|---------|--------|
| **Normal** | Default -- no special effect | Standard rendering |
| **Confirmed** | Re-evaluated, same item assigned again | Single flash |
| **Expiring** | Lock about to expire, content will change | Slow pulse |
| **Override** | Override triggered | Slide + flash (highest priority) |
| **Wildcard** | Wildcard exploration | Same visual as Override |

### SlotContentType (display layer)

The `SlotContentType` enum (`SlotTypes.h`) is a **display-layer concern** used only by `IntuitionMenu` and Wheeler after converting from `SlotAssignment`. It provides fine-grained type information for rendering:

| Type | Displayed | Notes |
|------|-----------|-------|
| **Empty** | Nothing | Slot hidden in UI |
| **NoMatch** | "(No damage)" etc. | Shows slot classification, dimmed |
| **Spell** | Spell name | Normal recommendation |
| **Wildcard** | Spell name | Exploration pick, blue highlight |
| **HealthPotion** | Potion name | Red styling |
| **MagickaPotion** | Potion name | Blue styling |
| **StaminaPotion** | Potion name | Green styling |
| **Potion** | Potion name | Generic fallback (maps to HealthPotion icon in IntuitionMenu) |
| **MeleeWeapon** | Weapon name | Favorited melee |
| **RangedWeapon** | Weapon name | Favorited ranged |

---

## Override System

Urgent conditions bypass normal scoring and force-update slots.

### Override Rules

| Priority | Condition | Threshold | Hysteresis | Item | INI Toggle |
|----------|-----------|-----------|------------|------|------------|
| 100 | **CRITICAL_HEALTH** | HP < 10% | 15% | Best health potion (pure preferred) | `bEnableCriticalHealth` |
| 70 | **CRITICAL_MAGICKA** | MP < 10% | 15% | Best magicka potion (pure preferred) | `bEnableCriticalMagicka` |
| 60 | **CRITICAL_STAMINA** | SP < 10% | 15% | Best stamina potion (pure preferred) | `bEnableCriticalStamina` |
| 50 | **DROWNING** | Underwater, no buff | N/A | Waterbreathing potion | `bEnableDrowning` |
| 40 | **LOW_AMMO** | < 10 arrows/bolts | 15 | Best ammo from inventory | `bEnableLowAmmo` |
| 35 | **WEAPON_EMPTY** | Charge = 0% | 5% | Best filled soul gem | `bEnableWeaponCharge` |

**Override Category** (`OverrideConditions.h`):
```cpp
enum class OverrideCategory : uint8_t {
    HP,      // CriticalHealth
    MP,      // CriticalMagicka
    SP,      // CriticalStamina
    Other    // Drowning, LowAmmo, WeaponCharge
};
```

**Potion Selection:**
- Prefer pure restore potions (no side effects)
- Fall back to impure potions (Skooma, etc.) if `bAllowImpurePotions=true` (default: true)
- Return no override if no suitable potions available

**Hysteresis:**
Once activated, override stays active until value crosses the higher deactivation threshold AND minimum duration (`fMinOverrideDurationMs`, default 2000ms) has elapsed.

```
Time ->
Health %:  12% -> 9%  -> 11% -> 9%  -> 16%
            |     |      |      |      |
            |   ACTIVATE |      |    DEACTIVATE
            |  (below 10%)|     |   (above 15%)
            |     |------hold---hold---|
```

### Override Filter

Slots can restrict which override categories they accept via `OverrideFilter`:

| Filter | Accepts |
|--------|---------|
| **None** | No overrides (slot never shows urgent items) |
| **Any** | All override categories (HP, MP, SP, Other) |
| **HP** | Only CriticalHealth overrides |
| **MP** | Only CriticalMagicka overrides |
| **SP** | Only CriticalStamina overrides |

**Note:** DROWNING, LOW_AMMO, and WEAPON_EMPTY are category `Other` and only match slots with `overrideFilter = Any`. They cannot be pinned to HP/MP/SP slots.

---

## Slot Allocation Pipeline

```
Scored Candidates     Override       Slot Classification
(from UtilityScorer)  Injection      Filter
        |                |               |
        v                v               v
   +----------+    +----------+    +----------+
   | Override  |--->| Classify |--->| Dedupe   |
   | Check     |    | & Match  |    | (per     |
   |           |    |          |    |  priority)|
   +----------+    +----------+    +----------+
                                        |
                                        v
                                   +----------+    +----------+
                                   | Allocate |--->| SlotLock |---> Widget/Wheeler
                                   | Top Per  |    | (temporal|
                                   | Slot     |    |  stable) |
                                   +----------+    +----------+
```

### Stage 1: Override Injection

```cpp
// OverrideManager evaluates all override conditions
OverrideCollection overrides = OverrideManager::EvaluateOverrides(
    playerState, worldState);

// Sort by priority (highest first) — already done by OverrideCollection::SortByPriority()

// Inject into slot allocator — find a slot that accepts this override category
for (const auto& override : overrides.activeOverrides) {
    for (auto& slot : slots) {
        if (AcceptsOverride(slot.config.overrideFilter, override.category)) {
            slot.ForceUpdate(override.candidate, override.priority);
            break;  // Override consumed
        }
    }
}
```

### Stage 2: Classification Filter

```cpp
// For each slot, filter candidates by classification
for (auto& slot : slots) {
    // MatchesClassification checks both effect-based and item-type-based rules
    // School-specific spells (SpellsDestruction, etc.) check the spell's MagicSchool
    auto best = FindBestCandidate(candidates, slot.config.classification,
                                  assignedFormIDs, slot.config.skipEquipped, &player);
}
```

### Stage 3: Duplicate Removal

```cpp
// Track used FormIDs across all slots (processed in priority order)
std::set<RE::FormID> assignedFormIDs;

// Higher-priority slots get first pick of candidates.
// Lower-priority slots receive filtered candidates (high-priority picks removed).
// Duplicates are simply filtered out — no learner penalty.
```

### Stage 4: Top Candidate Allocation

```cpp
// Pick top-scoring candidate per slot
for (auto& slot : sortedSlots) {
    if (slot.hasOverride) continue;  // Already set in Stage 1

    auto best = FindBestCandidate(candidates, slot.classification,
                                  assignedFormIDs, slot.skipEquipped);
    if (best) {
        slot.assignment = SlotAssignment::FromCandidate(index, classification, *best);
        assignedFormIDs.insert(best->GetFormID());
    } else {
        slot.assignment = SlotAssignment::Empty(index, classification);
    }
}
```

---

## Slot Priority System

Slots are processed in priority order for allocation. Priority values are **user-defined integers** (typically 0-6 in the default config), not fixed tiers. Higher priority slots get first pick of candidates.

**Important:** Slot allocation priority (user-set `int8_t`, range -128 to 127) is completely separate from override priority (fixed constants like CRITICAL_HEALTH=100). They are different systems:
- **Slot priority:** Determines which slot gets first pick of candidates during allocation
- **Override priority:** Determines which urgent condition takes precedence and whether it can break locks

### Default Slot Layout (Code Default)

The code default is **1 page with 7 slots** (`Defaults::PAGE_COUNT = 1`, `Defaults::SLOTS_PER_PAGE = 7`):

| Slot | Classification | Priority | Overrides | Wildcards | SkipEquipped |
|------|----------------|----------|-----------|-----------|--------------|
| 0 | DamageAny | 6 | Any | Yes | No |
| 1 | WeaponsAny | 5 | Any | Yes | No |
| 2 | BuffsAny | 4 | Any | Yes | No |
| 3 | Regular | 3 | None | Yes | No |
| 4 | Regular | 2 | None | Yes | No |
| 5 | Regular | 1 | None | Yes | No |
| 6 | Regular | 0 | None | Yes | No |

### Shipped INI Presets (not the code default)

The shipped `Huginn.ini` provides **10 named preset pages** with 8 slots each:

| Page | Name | Focus |
|------|------|-------|
| 0 | Smart | General-purpose (DamageAny, WeaponsAny, BuffsAny, Regular...) |
| 1 | Inventory | Item-focused (PotionsAny, ScrollsAny, WeaponsAny...) |
| 2 | Regulars | All Regular slots (pure learning-driven) |
| 3 | Fighter | Melee combat (WeaponsMelee, DamageAny, BuffsAny...) |
| 4 | Archer | Ranged combat (WeaponsRanged, AmmoAny, DamageAny...) |
| 5 | Mage | Magic-focused (SpellsDestruction, SpellsRestoration, SpellsConjuration...) |
| 6 | Rogue | Stealth (SpellsIllusion, WeaponsMelee, PotionsAny...) |
| 7 | Paladin | Healing + combat (SpellsRestoration, WeaponsMelee, DamageAny...) |
| 8 | Healer | Support (HealingAny, SpellsRestoration, BuffsAny...) |
| 9 | Random | All Regular with wildcards (exploration/learning) |

---

## SlotLocker: Temporal Stability

Prevents UI flicker from brief state oscillations by holding slot content for a configurable duration.

**Pipeline position:** `SlotAllocator (stateless)` -> `[SlotLocker (stateful)]` -> `Widget/Wheeler`

```
State Diagram:
  [Empty] --new content--> [Locked]
  [Locked] --same content--> [Locked] (refresh timer)
  [Locked] --different content, lock active--> [Locked] (hold old content)
  [Locked] --lock expired + content changed--> [Unlocked] -> [Locked] (new content)
  [Locked] --override >= immediateBreakPriority--> [Unlocked] (break lock)

  Default lock: 3000ms (fLockDurationMs)
  Min lock before break: 500ms (fMinLockDurationMs)
  Override bypass: priority >= 50 (iImmediateBreakPriority)
```

**Lock Behavior:**

| Scenario | Lock Status | Displayed Content | Notes |
|----------|-------------|-------------------|-------|
| New content, lock expired | Lock | New content | Normal update |
| New content, lock active | Hold | Old content | Stability -- wait for timer |
| Same content | Refresh lock | Same content | Reset timer to prevent expiry |
| Override (P >= 50) | Break lock | Override content | Urgent bypass |
| Content changes rapidly | Hold | First stable | Prevents rapid oscillation |
| Item activated (Sticky policy) | Lock 10s | Activated item | `LockSlotForActivation()` |

---

## Wildcard System

Wildcards are exploration picks from the candidate pool, designed to expose players to underutilized items.

**Selection Strategy:**
Wildcards use a **probability-based** system that scales with slot index:
- Slot 0 is excluded by default (`m_firstSlotExcluded = true`)
- Each subsequent slot has probability: `baseProbability * slotIndex`, capped at `maxProbability`
- Default base probability: 0.165 (16.5%)
- Default max probability: 0.5 (50%)

**Persistence and Cooldowns:**
- **Persistence:** Once selected, a wildcard holds its slot for `fCooldownSeconds` (30s default)
- **Refractory period:** After a wildcard expires, the slot waits `fRefractorySeconds` (5s default) before rolling again

**Visual Indicator:**
- Wildcard items show with blue highlight in UI
- Wheeler displays "Wildcard" subtext label (configurable)

---

## Wheeler Integration

**Multi-Wheel Management:**
- One managed wheel per page (up to 10 wheels)
- Each wheel shows current page's slot assignments
- Auto-focus on wheel open (fresh opens only, not scroll navigation)
- Urgent override updates propagate while wheel is open (priority-gated by `autoFocusMinPriority`)

**Feedback Loop:**

All Wheeler equip events flow through the EquipEventBus:

| Event | Action | Notes |
|-------|--------|-------|
| Activates item from wheel | Publish `EquipSource::Wheeler` with `rewardMultiplier=1.0` | FQLSubscriber applies `EQUIP_REWARD * 1.0 = +8.0` |
| Opens then closes without action | No penalty | Skip penalties removed in v0.12.x |
| Post-activation policy: Sticky | Lock slot 10s, skip cooldown | Activated item stays visible |
| Post-activation policy: Empty | Clear slot, start cooldown | Slot refills on next pipeline tick |
| Post-activation policy: Backfill | Break lock, allow normal refill | Default behavior |

---

## INI Configuration

### [Pages] Section

```ini
[Pages]
iPageCount = 1            ; Number of pages (1-10, code default: 1, shipped INI: 10)
```

### [PageN] Section (per page)

```ini
[Page0]
sName = Smart             ; Page display name
iSlotCount = 8            ; Slots on this page (1-10, code default: 7)
```

### [PageN.SlotM] Section (per slot)

```ini
[Page0.Slot0]
sClassification = DamageAny     ; SlotClassification enum name
bWildcardsEnabled = true        ; Allow wildcard exploration picks
bOverridesEnabled = Any         ; OverrideFilter: None, Any, HP, MP, SP (also accepts true/false)
iPriority = 6                   ; Allocation order (higher = filled first)
bSkipEquipped = false           ; Skip already-equipped candidates
```

### [SlotLocker] Section

```ini
[SlotLocker]
fLockDurationMs = 3000          ; Total lock hold time (ms), 0 = disable locking
fMinLockDurationMs = 500        ; Minimum time before lock can break (ms)
bLockOnFill = true              ; Lock when slot fills from empty
bOverridesBreakLock = true      ; Allow high-priority overrides to break locks
iImmediateBreakPriority = 50    ; Override priority threshold for immediate lock break
```

### [Overrides] Section

```ini
[Overrides]
; Enable flags
bEnableCriticalHealth = true    ; Enable HP < 10% override
bEnableCriticalMagicka = true   ; Enable MP < 10% override
bEnableCriticalStamina = true   ; Enable SP < 10% override
bEnableDrowning = true          ; Enable underwater override
bEnableLowAmmo = true           ; Enable ammo < 10 override
bEnableWeaponCharge = true      ; Enable charge = 0% override

; Potion selection
bAllowImpurePotions = true      ; Allow potions with side effects (Skooma, etc.)

; Thresholds (activation) and hysteresis (deactivation)
fCriticalHealthThreshold = 0.10   ; 10% HP
fCriticalHealthHysteresis = 0.15  ; Deactivate at 15% HP
fCriticalMagickaThreshold = 0.10
fCriticalMagickaHysteresis = 0.15
fCriticalStaminaThreshold = 0.10
fCriticalStaminaHysteresis = 0.15
fWeaponChargeThreshold = 0.0     ; 0% charge
fWeaponChargeHysteresis = 0.05   ; Deactivate at 5% charge
fLowAmmoThreshold = 10           ; 10 arrows/bolts (absolute count)
fLowAmmoHysteresis = 15          ; Deactivate at 15 arrows/bolts

; Anti-flicker
fMinOverrideDurationMs = 2000    ; Minimum time override stays active (ms)
```

### [Wildcards] Section

```ini
[Wildcards]
fBaseProbability = 0.165         ; Base wildcard probability for slot 1 (16.5%)
fMaxProbability = 0.5            ; Maximum probability cap (50%)
fCooldownSeconds = 30            ; How long a wildcard persists once selected
fRefractorySeconds = 5           ; Wait time after wildcard expires before rolling again
```

---

## Thread Safety

**SlotAllocator** is stateless -- `AllocateSlots()` takes inputs and returns a `SlotAssignments` vector with no shared state. It is inherently thread-safe.

**SlotLocker** uses `std::mutex` (`m_mutex`) to protect the per-slot lock state array. All public methods acquire a `lock_guard` before accessing `m_lockedSlots`. This is necessary because:
- **Writer:** Update thread calls `Update()` and `ApplyLocks()` on ~100ms tick
- **Readers/Writers:** Wheeler callbacks may call `OnItemUsed()` / `LockSlotForActivation()` from callback thread

**SlotAllocator page state** uses `std::atomic<size_t>` for `m_currentPage` and `std::atomic<bool>` for `m_pageChanged` -- safe for cross-thread read/write (input thread sets page, update thread reads it).

**Concurrency model:**
```
Input Thread (keys 4/5)
  -> SlotAllocator::NextPage() [atomic write]

Update Thread (100ms tick)
  -> SlotAllocator::AllocateSlots() [stateless, reads atomic page]
  -> SlotLocker::ApplyLocks() [mutex-guarded]
  -> IntuitionMenu / Wheeler [consumers]

Wheeler Callback Thread
  -> SlotLocker::OnItemUsed() [mutex-guarded]
  -> SlotLocker::LockSlotForActivation() [mutex-guarded]
```

---

## See Also

- [0-pipeline.md](0-pipeline.md) - Overall recommendation pipeline
- [1-states.md](1-states.md) - State models for override detection
- [4-contextual-bandits.md](4-contextual-bandits.md) - Learning system (EquipEventBus, reward signals)
- [6-ui-ux.md](6-ui-ux.md) - Intuition widget and Wheeler display

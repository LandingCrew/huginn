# Huginn Slot Architecture

This document describes the multi-page slot system — classification, allocation,
locking, overrides and wildcards — as implemented in **v0.19.x**.

> **Related documentation:**
> - [0-pipeline.md](0-pipeline.md) - Overall pipeline flow including slot allocation
> - [1-states.md](1-states.md) - State models used for override detection
> - [2-classifiers.md](2-classifiers.md) - Spell/item/weapon tags the classifier reads
> - [6-ui-ux.md](6-ui-ux.md) - How assignments are rendered (widget, Wheeler)

---

## Overview

- Multi-page slot system (up to 10 pages, up to 10 slots per page)
- 21 slot classifications: effect-based (`DamageAny`, `HealingAny`, …),
  item-type-based (`PotionsAny`, `WeaponsMelee`, per-school spells, …) and
  unrestricted (`Regular`)
- Override system with priority-ordered urgent replacements, routed to slots by
  category filter (`None` / `Any` / `HP` / `MP` / `SP` / `Other`)
- `SlotLocker` for temporal stability (prevents flicker), with a Sticky
  post-activation lock
- Per-page wildcard exploration (`WildcardManager`)
- INI-configurable page/slot layouts via `[Pages]` / `[PageN]` / `[PageN.SlotM]`

Owning code: `src/slot/` (allocation, locking, classification, settings),
`src/override/` (override evaluation), and `src/learning/WildcardManager.{h,cpp}`
(exploration picks — it lives under `learning/` because a wildcard is an
exploration action of the bandit, not a slot-layout concern).

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
|      +------cycle  - / = ----------+     |
+-------------------------------------------+
           |
           v
+-------------------------------------------+
|          Display Outputs                  |
|  IntuitionMenu  |  Wheeler  |  ImGui     |
|  (Scaleform HUD)|  (Radial) |  (Debug)   |
+-------------------------------------------+
```

**Key properties:**

- Each page has its own name and its own slot configuration (classification,
  priority, override filter, wildcards, skip-equipped).
- Slot count is per-page (`iSlotCount`, clamped to 1..10).
- Wheeler creates one managed wheel per page.
- Page cycling is bound to `-` and `=` by default (`iPreviousPageKey = 12`,
  `iNextPageKey = 13`, DirectInput scancodes — `src/input/KeybindingSettings.h:24`).
  `hg page <N>` switches directly.
- Switching pages calls `SlotLocker::UnlockAll()` — locks from the previous
  page's context would otherwise hold content the new layout never allocated
  (`src/slot/SlotAllocator.cpp:96`).

**Constants** (`src/slot/SlotSettings.h:17`):
```cpp
inline constexpr size_t MAX_PAGES = 10;
inline constexpr size_t MAX_SLOTS_PER_PAGE = 10;
```

**Code default vs shipped INI.** The compiled-in fallback is **1 page with 7
slots** (`Defaults::PAGE_COUNT`, `Defaults::SLOTS_PER_PAGE`,
`src/slot/SlotSettings.h:119`). The shipped `configs/Huginn.ini` overrides that
with **3 pages of 8 slots** (`Smart`, `Inventory`, `Regulars`), and ships three
further page templates commented out (`Fighter`, `Mage`, `Rogue`) for the player
to uncomment after raising `iPageCount`.

**Page state and dirty flags** (`src/slot/SlotAllocator.h`):

| Member / method | Purpose |
|---|---|
| `m_currentPage` (`std::atomic<size_t>`) | Active page; written from the input thread, read from the update thread |
| `m_pageChanged` (`std::atomic<bool>`) | Set on page switch; consumed by the update loop to bypass the pipeline hash-skip |
| `PeekPageChanged()` | Non-destructive check, so the flag is not lost if the pipeline guard rejects the tick |
| `MarkPageDirty()` | General "re-run the pipeline" signal — inventory changed, Wheeler closed, a consumer missed a page change |
| `GetConfigSnapshot()` | `shared_ptr` snapshot of all `PageConfig`s, rebuilt only when `SlotSettings::GetGeneration()` changes, so the hot path avoids a shared-lock plus vector copy every tick |

---

## Slot Classification System

`SlotClassification` (`src/slot/SlotConfig.h`) has 21 values. `SlotClassifier`
(`src/slot/SlotClassifier.cpp`) dispatches on the candidate variant — spell,
item, scroll, weapon or ammo — so the same classification means different things
per source type.

### Effect-Based Classifications

| Classification | Spells / scrolls match on | Items match on |
|---|---|---|
| **DamageAny** | `SpellType::Damage` | `ItemType::Poison` (weapon candidates also match) |
| **HealingAny** | `SpellType::Healing`, or the `RestoreHealth` tag | `HealthPotion`, or the `RestoreHealth` tag |
| **BuffsAny** | `SpellType::Buff`, or `Armor` / `Invisibility` / `Muffle` tags | `BuffPotion`, or any `Fortify*` / `Invisibility` tag |
| **DefensiveAny** | `SpellType::Defensive`, or `Ward` / `Armor` tags | `ResistPotion`, or any `Resist*` tag |
| **SummonsAny** | `SpellType::Summon`, or `Conjuration` / `SummonDaedra` / `SummonUndead` / `SummonCreature` / `BoundWeapon` tags | — (no item summons) |
| **Utility** | `SpellType::Utility`, or `Light` / `DetectLife` / `Telekinesis` tags, or the `Unlock` extended tag | `CurePotion`, or `Waterbreathing` / `CureDisease` / `CurePoison` tags |

### Item-Type-Based Classifications

| Classification | Matches |
|---|---|
| **PotionsAny** | Health, magicka, stamina, resist, buff and cure potions plus poisons — **not** food, alcohol or soul gems |
| **ScrollsAny** | Any scroll |
| **SpellsAny** | Any spell (scrolls do **not** match) |
| **SpellsDestruction / SpellsRestoration / SpellsConjuration / SpellsIllusion / SpellsAlteration** | Spells whose `MagicSchool` is that school |
| **WeaponsAny** | Any weapon candidate |
| **WeaponsMelee** | Weapons carrying the `Melee` tag |
| **WeaponsRanged** | Weapons carrying the `Ranged` tag |
| **FoodAny** | `ItemType::Food` |
| **AlcoholAny** | `ItemType::Alcohol` (ale, mead, wine, skooma) |
| **AmmoAny** | Ammo candidates (arrows, bolts) |

### Regular (Unrestricted)

**Regular** matches every candidate variant. It is also the only classification
that accepts soul gems, which have no dedicated classification — relevant
because the `WeaponCharge` override surfaces a soul gem and therefore needs a
`Regular` slot that also accepts its category (see
[Override placement](#override-placement)).

### Classify()

`SlotClassifier::Classify()` returns a single best-fit classification for
display and debugging. It walks a fixed most-specific-first order: the effect
classifications, then `FoodAny` before `AlcoholAny` before `PotionsAny`, then
`ScrollsAny`, then `WeaponsMelee` / `WeaponsRanged` before `WeaponsAny`, then
`AmmoAny`, then the per-school spell filters before `SpellsAny`, falling back to
`Regular`.

---

## Slot Configuration Structure

**SlotConfig fields** (`src/slot/SlotConfig.h`):

| Field | Type | Default | Purpose |
|-------|------|---------|---------|
| `classification` | `SlotClassification` | `Regular` | What type of candidate can appear |
| `wildcardsEnabled` | `bool` | `true` | Allow wildcard exploration picks |
| `overrideFilter` | `OverrideFilter` | `Any` | Which override categories accepted (`None`, `Any`, `HP`, `MP`, `SP`, `Other`) |
| `skipEquipped` | `bool` | `false` | Skip candidates already equipped (show alternatives only) |
| `priority` | `int8_t` | `0` | Allocation order (higher = filled first) |

---

## Slot Assignment Architecture

The allocator produces `SlotAssignment` objects, not raw content types. There
are three distinct type systems.

### AssignmentType (allocator output)

`src/slot/SlotAssignment.h`:

| Type | Meaning | Notes |
|------|---------|-------|
| **Empty** | Slot has no content | Slot hidden in UI |
| **Normal** | Standard utility-based assignment | Most common |
| **Override** | Forced by an override condition | Stamped with `kOverrideUtility = 1000.0f`, a cosmetic marker — consumers test `AssignmentType::Override`, never the utility value |
| **Wildcard** | Exploration pick | Distinct styling in the widget |

### SlotVisualState (animation hints)

Computed after locking by `Slot::ComputeVisualStates()` (`src/slot/SlotUtils.h`):

| State | Assigned when | Visual |
|-------|---------------|--------|
| **Override** | `AssignmentType::Override` (checked first) | Slide + flash |
| **Wildcard** | `AssignmentType::Wildcard` | Wildcard treatment |
| **Confirmed** | Slot had content last frame and the same FormID was re-assigned | Single flash |
| **Expiring** | Lock is in its last 40% *and* the raw (pre-lock) assignment for that slot is a different, non-zero FormID | Slow pulse |
| **Normal** | Everything else, including empty slots | Standard rendering |

The `Expiring` threshold is derived from the configured lock duration
(`lockDurationMs * 0.4`), so it scales with the INI setting.

### SlotContentType (display layer)

`SlotContentType` (`src/ui/SlotTypes.h:11`) is a **display-layer concern**, used
by `IntuitionMenu` and the Wheeler backend after converting a `SlotAssignment`
via `SlotUtils::ToSlotContent()`:

| Type | Displayed | Notes |
|------|-----------|-------|
| **Empty** | Nothing | Slot hidden |
| **NoMatch** | "(No damage)" etc. | Shows the slot's classification, dimmed |
| **Spell** | Spell name | Normal recommendation |
| **Wildcard** | Name | Exploration pick |
| **Potion** | Potion name | Generic fallback |
| **HealthPotion** / **MagickaPotion** / **StaminaPotion** | Potion name | Per-vital styling |
| **MeleeWeapon** / **RangedWeapon** | Weapon name | |
| **Ammo** | Arrow/bolt name | Equipped as ammo, not as a weapon |
| **SoulGem** | Soul gem name | Informational — the weapon needs recharging |

---

## Override System

Urgent conditions bypass normal scoring and are force-placed into slots.
`OverrideManager::EvaluateOverrides()` reports which conditions are urgent as an
`OverrideCollection`; it never touches the scored candidate list. The pipeline
hands that collection to `SlotAllocator` (placement), `SlotLocker` (lock
breaking) and the display path (auto-focus, subtexts).

### Override Rules

Priorities are fixed constants (`src/override/OverrideConditions.h:20`).
Thresholds are runtime settings; the table gives the **code default** and, where
it differs, the **shipped INI value**.

| Priority | Condition | Category | Activation threshold (code / shipped) | Hysteresis gap | Item surfaced | INI toggle |
|---|---|---|---|---|---|---|
| 100 | **CriticalHealth** | HP | HP < 10% / **35%** | +15% | Best health potion | `bEnableCriticalHealth` |
| 70 | **CriticalMagicka** | MP | MP < 10% / **35%** | +15% | Best magicka potion | `bEnableCriticalMagicka` |
| 60 | **CriticalStamina** | SP | SP < 10% / **35%** | +15% | Best stamina potion | `bEnableCriticalStamina` |
| 50 | **Drowning** | Other | Underwater with no waterbreathing buff (boolean, not threshold-based) | n/a | Waterbreathing **potion** | `bEnableDrowning` |
| 40 | **LowAmmo** | Other | Bow/crossbow equipped and ammo < 10 (absolute count) | +15 | Best arrow/bolt in inventory | `bEnableLowAmmo` |
| 35 | **WeaponCharge** | Other | Enchanted weapon equipped and charge < 25% | +5% | Best **filled** soul gem | `bEnableWeaponCharge` |

`WeaponCharge`'s threshold is deliberately non-zero: activation is
`charge < threshold`, and a drained enchanted weapon keeps a small unusable
charge remainder rather than exactly 0, so a 0.0 threshold would never fire.

Drowning surfaces only a potion. Waterbreathing *spells* have no dedicated tag
and reach the player through normal contextual scoring instead.

**Override category** (`src/override/OverrideConditions.h:38`):
```cpp
enum class OverrideCategory : uint8_t {
    HP,      // CriticalHealth
    MP,      // CriticalMagicka
    SP,      // CriticalStamina
    Other    // Drowning, LowAmmo, WeaponCharge
};
```

A parallel `OverrideCondition` enum stamps each result with the evaluator that
produced it. It exists as a dedup/latch key: unlike `reason`, it never embeds
live values (an ammo count inside a reason string would defeat any dedup keyed
on it). An unstamped result (`Unknown`) is treated as always-log, so a missing
stamp is loud rather than silently aliasing another condition's latch entry.

**Potion selection.** All three vitals finders share one policy
(`SelectPotion`): prefer the best *pure* potion from
`ItemRegistry::GetBestPotion`, and fall back to one with harmful side effects
(skooma and friends) only when `bAllowImpurePotions = true` (default true). If
neither exists the condition still reports as active but carries no candidate,
and the allocator skips it.

**Hysteresis is a release gap, not an absolute value.** Once active, a
threshold-based override deactivates only when the value reaches
`activationThreshold + hysteresisGap` **and** the minimum duration
(`fMinOverrideDurationMs`, default 2000 ms) has elapsed
(`CheckThresholdHysteresis`, `src/override/OverrideManager.cpp:552`). With the
shipped health values that reads: activate below 35%, release at or above 50%.

```
Time ->
Health %:  38% -> 30% -> 40% -> 33% -> 55%
            |      |      |      |      |
            |   ACTIVATE  |      |   DEACTIVATE
            |  (below 35%)|      |  (at/above 50%)
            |      |--hold-hold--hold---|
```

Boolean conditions (Drowning) use `CheckHysteresis` instead: they stay active
until the condition clears *and* the minimum duration has elapsed.

`OverrideManager::Update(deltaMs)` returns `true` when an active latch crossed
its minimum-duration window this tick. Deactivation is only decided inside a
pipeline run, so the caller must force one (`MarkPageDirty`) or a lapsed
override lingers on screen while the pipeline is idle.

### Override Filter

Slots restrict which override categories they accept
(`AcceptsOverride`, `src/slot/SlotAllocator.cpp:32`):

| Filter | Accepts | INI spellings accepted |
|--------|---------|---|
| **None** | Nothing | `false`, `none`, `off` |
| **Any** | HP, MP, SP and Other | `true`, `any`, `all` |
| **HP** | CriticalHealth only | `hp`, `health` |
| **MP** | CriticalMagicka only | `mp`, `magicka`, `mana` |
| **SP** | CriticalStamina only | `sp`, `stamina` |
| **Other** | Drowning, LowAmmo, WeaponCharge only | `other`, `misc` |

`Other` is why the shipped Page 0 reserves Slot 6: an `Other`-only slot rejects
HP/MP/SP, so the vitals overrides — which outrank all three `Other` conditions —
cannot starve the soul-gem / ammo / drowning prompts when several fire at once.
Parsing is case-insensitive; an unrecognised value falls back to `Any` with a
warning.

### Override placement

Placement happens in two passes inside allocation (below). Two config-level
checks back it up, both run from `SlotAllocator::Initialize()` at startup and on
every settings reload (`ValidateOverridePlaceability`,
`src/slot/SlotAllocator.cpp:643`):

1. **Unplaceable check.** For every *enabled* condition, verify that some slot
   on some page accepts its category. Failures are logged as a warning *and*
   surfaced in-game via `RE::DebugNotification` naming the conditions.
2. **Contention heuristic.** Warn (log only) when more enabled conditions map to
   a category than the best *single* page has accepting slots. The binding
   constraint is per-page, because the same higher-priority conditions win on
   every page. The heuristic is deliberately one-sided: a warning means real
   risk, but silence does not prove safety — `Any` slots are credited to every
   category here yet hold only one override at runtime.

At runtime the backstop is the `displaced` log: when a page accepts the category
but every accepting slot is already taken by a higher-priority override, that
transition logs once at `info`.

---

## Slot Allocation Pipeline

`SlotAllocator::AllocateSlotsInternal()` (`src/slot/SlotAllocator.cpp:268`) is a
two-pass fill over a priority-ordered slot list. It is `const` and stateless
with respect to allocation; the only mutable members are log-dedup caches and
the config snapshot cache.

```
   scored candidates            active overrides
  (sorted by utility)        (sorted by priority)
           |                          |
           v                          v
  +-------------------------------------------------+
  | priority order = slots sorted by config.priority |
  +-------------------------------------------------+
           |
           v
  PASS 1   override -> highest-priority slot that accepts its
           CATEGORY *and* matches its CLASSIFICATION
           |
           v
  PASS 1b  leftover override -> highest-priority empty slot that
           accepts its category (classification ignored)
           |
           v
  PASS 2   remaining slots <- best matching candidate, deduped
           by FormID and by name
           |
           v
     SlotLocker::ApplyLocks -> ComputeVisualStates -> widget / Wheeler
```

### Priority order

`ComputePriorityOrder()` (`src/slot/SlotAllocator.cpp:545`) writes slot indices
into a fixed `std::array<size_t, MAX_SLOTS_PER_PAGE>` — no heap allocation —
sorted by `config.priority` descending. A page configured with more slots than
`MAX_SLOTS_PER_PAGE` logs a warning rather than silently dropping the overflow.

### Pass 1 — type-matched override placement

For each active override (already priority-sorted), walk slots in priority order
and take the first that (a) accepts the override's category, (b) is still empty,
and (c) whose classification the override's candidate actually matches. Both the
FormID and the name are recorded as assigned.

### Pass 1b — override fallback

Any override not placed in Pass 1 gets a second walk that drops the
classification requirement: the first empty slot accepting the category wins. If
even that fails, one of three things happens:

- accepting slots existed but were occupied → `displaced`, logged once per
  transition at `info`;
- no slot on **any** page accepts the category → a genuine config gap, warned
  once per condition (latched on the condition enum, not the reason string);
- another page accepts it → silence, because the override is reachable by
  paging to that page or opening its wheel.

### Pass 2 — normal fill

For every still-empty slot in priority order, `FindBestCandidate()`
(`src/slot/SlotAllocator.cpp:575`) scans the utility-sorted candidate list and
returns the first candidate that:

1. is not already assigned **by FormID**, and
2. is not already assigned **by name** — this catches duplicate enchanted items
   that share a name across different FormIDs, and
3. matches the slot's classification, and
4. is not equipped, when `skipEquipped` is set. Equipped-ness is checked twice:
   the candidate's own `isEquipped` flag (from the weapon registry scan) *and*
   `PlayerActorState::IsItemEquipped` (the ~100 ms equipment poll), to cover the
   timing gap between the two.

If the winning candidate is flagged `isWildcard` but the slot sets
`wildcardsEnabled = false`, the search is re-run with `skipWildcards = true`.
Reusing `FindBestCandidate` for the retry (rather than an inline rescan) means
the fallback still honours `skipEquipped`, and returns cleanly empty when no
alternative exists — so a wildcard is never left sitting in a slot that forbids
wildcards.

Duplicates are simply filtered out. There is no learner penalty for being a
duplicate.

---

## Slot Priority System

Slot priority is a **user-defined `int8_t`** (range -128..127; the shipped pages
use 0..6). Higher-priority slots are filled first and therefore get first pick
of the candidate pool. Ties are resolved by `std::sort`, which is not stable, so
two slots sharing a priority (the shipped pages do this at priority 1) have no
guaranteed relative order — give slots distinct priorities if the order matters.

**Slot priority is a different system from override priority.** Override
priority is a fixed constant per condition (`CRITICAL_HEALTH = 100`, …) and
governs collection ordering, lock breaking and Wheeler auto-focus gating. Slot
priority governs fill order only. Neither reads the other.

### Code default layout (1 page, 7 slots)

`Defaults::PAGE0_SLOTS`, `src/slot/SlotSettings.h:132`:

| Slot | Classification | Priority | Override filter | Wildcards | SkipEquipped |
|------|----------------|----------|-----------------|-----------|--------------|
| 0 | DamageAny | 6 | Any | Yes | No |
| 1 | WeaponsAny | 5 | Any | Yes | No |
| 2 | BuffsAny | 4 | Any | Yes | No |
| 3 | Regular | 3 | None | Yes | No |
| 4 | Regular | 2 | None | Yes | No |
| 5 | Regular | 1 | None | Yes | No |
| 6 | Regular | 0 | **Other** | Yes | No |

Slot 6's `Other` filter reserves a home for the soul-gem / low-ammo / drowning
prompts. Slots past index 6 (only reachable when the INI asks for more) and all
slots on pages 1+ default to `Regular`, wildcards on, override filter `None`,
priority `slotCount - index - 1`.

### Shipped INI layout (3 pages, 8 slots each)

| Page | Name | Layout |
|------|------|--------|
| 0 | Smart | DamageAny(HP), WeaponsAny(MP), BuffsAny(SP), four `Regular`, one `Regular` pinned to `Other`; wildcards on throughout |
| 1 | Inventory | One category per slot — DamageAny, WeaponsMelee, WeaponsRanged, HealingAny, PotionsAny, FoodAny, BuffsAny, Utility; wildcards **off**, no overrides |
| 2 | Regulars | All `Regular`; HP / MP / SP pinned to slots 0/1/2, `Any` on the rest; wildcards on |

Commented-out `Fighter`, `Mage` and `Rogue` templates follow in the same file;
uncomment one and raise `iPageCount` to enable it.

---

## SlotLocker: Temporal Stability

Prevents UI flicker from brief state oscillations by holding slot content for a
configurable duration.

**Pipeline position:** `SlotAllocator (stateless)` → `[SlotLocker (stateful)]` → widget/Wheeler

```
  [Empty]  --new content-->                     [Locked]
  [Locked] --different content, lock active-->  [Locked] (hold old content)
  [Locked] --timer expired-->                   [Unlocked] -> [Locked] (new content)
  [Locked] --matching override >= break pri-->  [Unlocked]
  [Locked] --OnItemUsed(formID)-->              [Unlocked]

  Lock duration:      3000 ms code default, 1000 ms in the shipped INI
  Min before break:   500 ms (fMinLockDurationMs)
  Override bypass:    priority >= 50 (iImmediateBreakPriority)
  Sticky activation:  10000 ms (ACTIVATION_LOCK_MS, src/slot/SlotLocker.h:188)
```

**`ShouldLock`** (`src/slot/SlotLocker.cpp:314`) — never locks an empty
assignment; never locks at all when `lockDurationMs <= 0`; locks when the slot
fills from empty (if `bLockOnFill`), and locks on any FormID change. Re-assigning
the *same* FormID does not re-lock and does not extend the running timer.

**`ShouldBreakLock`** (`src/slot/SlotLocker.cpp:341`) — in order:

1. Timer expired → break.
2. `bOverridesBreakLock` is set, the new assignment is an `Override`, and its
   **FormID matches an active override** at or above `immediateBreakPriority` →
   break, bypassing `minLockDurationMs`. Matching by FormID is deliberate:
   without it a high-priority HP override could break an unrelated slot's lock.
3. Otherwise hold. Notably it does **not** break when the new assignment is
   empty — that is the whole point of the context window (surfacing while
   Waterbreathing is still on screen).

| Scenario | Lock status | Displayed content |
|----------|-------------|-------------------|
| New content, lock expired | Lock | New content |
| New content, lock active | Hold | Old content |
| Content changes rapidly | Hold | First stable pick |
| Matching override, priority ≥ 50 | Break | Override content |
| Item activated (Sticky policy) | Lock 10 s | Activated item |
| Item used or left inventory | Break (`OnItemUsed`) | Refills next tick |

**Post-lock dedup.** A locked slot can hold an item the allocator also placed in
another slot this frame, so `ApplyLocks` finishes with `DedupePreferLocked`
(`src/slot/SlotLocker.cpp:391`): unlocked slots whose name is held by a locked
slot are cleared, then remaining duplicates keep the first occurrence. Locked
content always wins, so a lower-index unlocked duplicate cannot evict it.

**Sticky and `OnItemUsed`.** `OnItemUsed(formID, respectActivationLock)` breaks
the lock on any slot holding that item. The inventory delta-scan path passes
`respectActivationLock = true`, so a Sticky-locked item that was just consumed
still honours its 10 s visibility window and expires on its own timer.

**Stored-view hazard.** When an assignment enters a `LockedSlot`, the embedded
candidate's `name` string_view is blanked (`TruncateCandidateViews`). It borrows
registry-owned storage that a registry reconcile can invalidate between pipeline
runs. Read the owned `SlotAssignment::name` instead — never the candidate name
out of a `LockedSlot`.

**Expiry drives a recompute.** `SlotLocker::Update(deltaMs)` is called every tick
from `UpdateSubsystems`, *not* from behind the pipeline-skip gate, so locks decay
in wall-clock time. It returns `true` if any lock expired, and the caller must
force a pipeline run so the freed slot's content actually swaps.

---

## Wildcard System

Wildcards are exploration picks: a lower-ranked candidate swapped up into a slot
so the bandit gets evidence about items the player would otherwise never see.
`WildcardManager` (`src/learning/WildcardManager.{h,cpp}`) runs *before*
allocation, mutating the ranked candidate list and setting `isWildcard` on the
promoted entry; `SlotAllocator` then treats it like any other candidate, apart
from the `wildcardsEnabled` check.

**Probability model** (`GetProbabilityForSlot`, `src/learning/WildcardManager.cpp:199`):

```
P(slot i) = min(fBaseProbability * i, fMaxProbability)
```

Slot 0 is excluded by default (`m_firstSlotExcluded = true`) — it is always the
top-scored pick. With the shipped base 0.165 and cap 0.5: slot 1 = 16.5%,
slot 2 = 33%, slot 3 = 49.5%, slot 4 and beyond = 50%.

**Per-page cache (v0.19.6).** Each page owns its own entry array, its own
cooldown clock and its own refractory clock (`PageWildcards`,
`src/learning/WildcardManager.h:162`). Each array also records the page **shape**
(`slotCount`, `wildcardSlots`) it was rolled against; any shape change — an INI
hot-reload resizing a page, or toggling `bWildcardsEnabled` — discards that
page's cache wholesale. This replaced a single global array indexed by position
and shared by every page, which could strand a cached wildcard that nothing
could display and thereby suppress re-rolls for a full cooldown.

**Capacity cap.** A page can display at most as many wildcards as it has slots
with `bWildcardsEnabled`. `SlotAllocator::GetWildcardSlotCount(pageIndex)`
supplies that count (via `PipelineContext::displayWildcardSlots`) and rolling
stops once it is reached. It is a page-level *capacity*, not a per-index map:
allocation decides which slot takes a given wildcard, by classification and
priority, so no caller can know in advance which one it will be.

**Draw window.** A wildcard is only ever surfaced by swapping a lower-ranked
candidate *up*. `SelectRandomCandidate` is therefore called with
`minIndex = i + 1`, so a roll for slot *i* can only draw from below it. Drawing
at or above *i* would cache an entry the swap step skips (`foundIdx <= slotIdx`)
while the cache still reads as active — a wasted roll that stalls re-rolling for
a whole cooldown.

Wildcards are also **source-type coherent**: the type is taken from the current
top-ranked candidate, so a spell-led ranking rolls spell wildcards.

**Persistence and cooldowns:**

- Once rolled, a page's wildcards persist for `fCooldownSeconds` (30 s default).
- After they expire, that page waits `fRefractorySeconds` before rolling again
  (code default 5 s; the **shipped INI sets 60 s**).
- `UpdateExpiry()` runs unconditionally every tick across **all** pages, because
  `ApplyWildcards` only runs on non-skipped pipeline ticks — otherwise an
  expired wildcard would stay on screen while the pipeline is idle. It returns
  `true` if any page's wildcards lapsed, and the caller forces a pipeline run.

---

## Wheeler Integration

See [6-ui-ux.md](6-ui-ux.md) for the display side. The slot-relevant parts:

- **One managed wheel per page.** `WheelSync` holds a `PageWheel` per page and
  maps a Wheeler wheel index back to a page (`FindPageForWheel`).
- **Urgent auto-focus** is priority-gated by `iAutoFocusMinPriority`
  (default 50), so drowning and the two higher critical-vitals overrides can
  pull focus while a wheel is open, and low-ammo / weapon-charge cannot.
- **Post-activation policy** (`[Wheeler]`) decides what happens to the slot
  after the player activates an entry (`src/wheeler/WheelerClient.cpp:133`):

| Policy | Behaviour |
|--------|-----------|
| **Backfill** (default) | `OnItemUsed` breaks the lock; the slot repopulates on the next pipeline tick, and a cooldown filters the consumed item out |
| **Sticky** | `LockSlotForActivation()` holds the slot for 10 s and the cooldown is skipped, so the activated item stays visible |
| **Empty** | `OnItemUsed` plus `MarkActivationEmptied`; the Wheeler entry is cleared and re-subtexted "Equipped", and a cooldown starts |

- **Feedback.** Every Wheeler activation publishes to the `EquipEventBus` as
  `EquipSource::Wheeler` with `rewardMultiplier = 1.0` and
  `wasRecommended = true` (`src/Main.cpp:496`); the learning subscriber applies
  `Config::EQUIP_REWARD * 1.0 = +8.0`.
- Opening and closing a wheel without acting carries **no penalty** — there is
  no skip-penalty path in the code.

---

## INI Configuration

Shipped template: `configs/Huginn.ini`, copied to
`Data/SKSE/Plugins/Huginn.ini`. Reloaded on save load, or in-game with
`hg reload`.

### [Pages] / [PageN] / [PageN.SlotM]

```ini
[Pages]
iPageCount = 3            ; 1-10 (clamped). Code default 1, shipped INI 3.

[Page0]
sName = Smart             ; Page display name
iSlotCount = 8            ; 1-10 (clamped). Code default 7 on page 0, 3 elsewhere.

[Page0.Slot0]
sClassification = DamageAny   ; SlotClassification enum name (unknown -> Regular + warning)
bWildcardsEnabled = true      ; Allow wildcard exploration picks in this slot
bOverridesEnabled = HP        ; OverrideFilter: None/Any/HP/MP/SP/Other (true/false also accepted)
iPriority = 6                 ; Allocation order (higher = filled first)
bSkipEquipped = false         ; Skip already-equipped candidates
```

Every key is optional; a missing `[PageN.SlotM]` section falls back to the
per-slot defaults described under
[Slot Priority System](#slot-priority-system).

### [SlotLocker]

```ini
[SlotLocker]
fLockDurationMs = 1000          ; Shipped value; code default 3000. 0 = disable locking
fMinLockDurationMs = 500        ; Minimum time before a lock can break
bLockOnFill = true              ; Lock when a slot fills from empty
bOverridesBreakLock = true      ; Allow high-priority overrides to break locks
iImmediateBreakPriority = 50    ; Override priority that bypasses fMinLockDurationMs
```

### [Overrides]

```ini
[Overrides]
bEnableCriticalHealth = true
fCriticalHealthThreshold = 0.35   ; Shipped; code default 0.10
fCriticalHealthHysteresis = 0.15  ; RELEASE GAP: deactivates at 0.35 + 0.15 = 50%

bEnableCriticalMagicka = true
fCriticalMagickaThreshold = 0.35  ; Code default 0.10
fCriticalMagickaHysteresis = 0.15

bEnableCriticalStamina = true
fCriticalStaminaThreshold = 0.35  ; Code default 0.10
fCriticalStaminaHysteresis = 0.15

bAllowImpurePotions = true        ; Allow potions with side effects (skooma, etc.)

bEnableWeaponCharge = true
fWeaponChargeThreshold = 0.25     ; Must be > 0 (see Override Rules)
fWeaponChargeHysteresis = 0.05

bEnableLowAmmo = true
fLowAmmoThreshold = 10            ; Absolute count, not a percentage
fLowAmmoHysteresis = 15           ; Release gap: deactivates at 25

fMinOverrideDurationMs = 2000     ; Minimum time an override stays active

bEnableDrowning = true            ; Boolean condition, no threshold
```

### [Wildcards]

```ini
[Wildcards]
fBaseProbability = 0.165         ; P(slot i) = base * i
fMaxProbability = 0.5            ; Probability cap
fCooldownSeconds = 30            ; How long a page's wildcards persist once rolled
fRefractorySeconds = 60          ; Shipped; code default 5. Wait after expiry before re-rolling
```

### [Keybindings] (page cycling)

```ini
[Keybindings]
iPreviousPageKey = 12            ; '-' key (DirectInput scancode)
iNextPageKey = 13                ; '=' key
```

---

## Thread Safety

**SlotSettings** guards `m_pages` with a `std::shared_mutex` and returns copies
from every accessor. A monotonic atomic `m_generation` is bumped on every config
change, so consumers can detect staleness without re-copying.

**SlotAllocator** allocation is stateless: `AllocateSlotsForPage()` takes its
inputs and returns a fresh `SlotAssignments`. Its mutable members are all
incidental and individually guarded:

- `m_currentPage` / `m_pageChanged` — `std::atomic` (input thread writes, update
  thread reads).
- `m_logMutex` — guards the two log-dedup `std::set`s, because allocation runs
  on both the update thread (display page) and a Wheeler callback thread
  (non-current pages).
- `m_cacheMutex` — guards the `shared_ptr` config snapshot. The snapshot is held
  for the duration of a call, so the `SlotConfig` references handed to
  `AllocateSlotsInternal` stay valid across a concurrent INI reload. A reload
  landing between the generation read and the page copy can leave the stored
  generation trailing by one, which only forces a redundant rebuild on the next
  call — it can never serve stale data, because the writer bumps the generation
  only after committing pages.

**SlotLocker** guards its per-slot lock array with `std::mutex`; every public
method takes a `lock_guard`. `GetLockSnapshot()` exists so a caller needing
several slots' state (visual-state computation) pays one acquisition instead of
up to three per slot.

**Concurrency model:**
```
Input Thread (- / = keys)
  -> SlotAllocator::SetCurrentPage()   [atomic write + SlotLocker::UnlockAll]

Update Thread (~100ms tick)
  -> OverrideManager::Update() / SlotLocker::Update()   [unconditional]
  -> WildcardManager::ApplyWildcards()                  [pipeline ticks only]
  -> SlotAllocator::AllocateSlotsForPage()              [stateless]
  -> SlotLocker::ApplyLocks()                           [mutex-guarded]
  -> Slot::ComputeVisualStates() -> IntuitionMenu / Wheeler

Wheeler Callback Thread
  -> SlotLocker::OnItemUsed() / LockSlotForActivation() [mutex-guarded]
  -> SlotAllocator::AllocateSlotsForPage()              [non-current pages]
```

---

## See Also

- [0-pipeline.md](0-pipeline.md) - Overall recommendation pipeline
- [1-states.md](1-states.md) - State models for override detection
- [2-classifiers.md](2-classifiers.md) - Spell/item/weapon tags the classifier reads
- [3-candidate-filtering.md](3-candidate-filtering.md) - What reaches the allocator
- [4-contextual-bandits.md](4-contextual-bandits.md) - Learning system (EquipEventBus, reward signals)
- [6-ui-ux.md](6-ui-ux.md) - Intuition widget and Wheeler display
- [7-dmenu-integration.md](7-dmenu-integration.md) - dMenu-owned settings
- [8-future-work.md](8-future-work.md) - Planned work
- [../reference/ConsoleCommands.md](../reference/ConsoleCommands.md) - `hg page`, `hg unlock`, `hg reload`

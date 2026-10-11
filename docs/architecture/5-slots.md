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
- 24 slot classifications: effect-based (`DamageAny`, `DamageMagic`, `HealingAny`, …),
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

**Code default vs shipped INI.** The compiled-in fallback is **1 page with 8
slots** (`Defaults::PAGE_COUNT`, `Defaults::SLOTS_PER_PAGE`,
`src/slot/SlotSettings.h`), the same layout as page 1 of the shipped
`configs/Huginn.ini`: **8 Regular keys** since 0.22.10. The shipped INI adds a
second page, `Jobs` (the old one-job-per-key default), as a measured fallback,
and keeps `Kit` as a third page, off unless `iPageCount = 3`. Seven layouts --
the old default and six archetypes -- and a set of five test pages live in
`configs/templates/` (see [Shipped INI layout](#shipped-ini-layout-2-pages)).

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

`SlotClassification` (`src/slot/SlotConfig.h`) has 24 values. `SlotClassifier`
(`src/slot/SlotClassifier.cpp`) dispatches on the candidate variant — spell,
item, scroll, weapon or ammo — so the same classification means different things
per source type.

### Effect-Based Classifications

| Classification | Spells / scrolls match on | Items match on |
|---|---|---|
| **DamageAny** | `SpellType::Damage` | `ItemType::Poison` (weapon candidates also match) |
| **DamageMagic** | `SpellType::Damage` | — (no items, no weapons): the "attack magic" key |
| **HealingAny** | `SpellType::Healing`, or the `RestoreHealth` tag | `HealthPotion`, or the `RestoreHealth` tag |
| **BuffsAny** | `SpellType::Buff`, or `Armor` / `Invisibility` / `Muffle` tags | `BuffPotion`, or any `Fortify*` / `Invisibility` tag |
| **DefensiveAny** | `SpellType::Defensive`, or `Ward` / `Armor` tags | `ResistPotion`, or any `Resist*` tag |
| **SummonsAny** | `SpellType::Summon`, or `Conjuration` / `SummonDaedra` / `SummonUndead` / `SummonCreature` / `BoundWeapon` tags | — (no item summons) |
| **Utility** | `SpellType::Utility`, or `Light` / `DetectLife` / `Telekinesis` tags, or the `Unlock` extended tag | `CurePotion`, or `Waterbreathing` / `CureDisease` / `CurePoison` tags |

### Item-Type-Based Classifications

| Classification | Matches |
|---|---|
| **PotionsAny** | Health, magicka, stamina, resist, buff and cure potions plus poisons — **not** food, alcohol or soul gems |
| **PoisonsAny** | `ItemType::Poison` only |
| **ScrollsAny** | Any scroll |
| **SpellsAny** | Any spell (scrolls do **not** match) |
| **SpellsDestruction / SpellsRestoration / SpellsConjuration / SpellsIllusion / SpellsAlteration** | Spells whose `MagicSchool` is that school |
| **WeaponsAny** | Any weapon candidate |
| **WeaponsMelee** | Weapons carrying the `Melee` tag |
| **WeaponsRanged** | Weapons carrying the `Ranged` tag |
| **FoodAny** | `ItemType::Food` |
| **AlcoholAny** | `ItemType::Alcohol` (ale, mead, wine, skooma) |
| **AmmoAny** | Ammo candidates (arrows, bolts) |
| **ApparelAny** | Fortify-crafting gear (scores only at a workstation) |

The classes overlap in ways a player cannot guess (DamageAny takes weapons,
HealingAny and PotionsAny both take health potions, …). A review of them as a
whole is on the roadmap.

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
| `skipEquipped` | `bool` | `true` | Skip candidates already equipped (show alternatives only). Default on since v0.21.51 |
| `remembrance` | `bool` | `true` | Pressing this slot holds what the equip took off here (see [Remembrance](#remembrance)) |
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
| **Remembered** | What pressing this slot took off ([Remembrance](#remembrance)) | Not ranked; labelled "Swap Back". Seating and dedup treat it like an override (`IsPinned()`) |

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

`Other` is why the shipped Page 0 reserves Slot 6 (key 7): an `Other`-only slot rejects
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

`SlotAllocator::AllocateSlotsInternal()` (`src/slot/SlotAllocator.cpp`) is a
five-pass fill over a priority-ordered slot list. Passes 1-2 decide WHICH items
are shown, by rank; passes 3-5 decide WHERE they sit.

1. **Overrides** take a slot whose filter and classification accept them.
   Then **Remembrance** holds (Pass 1a) take the slot the player pressed, and
   the slot hold (Pass 1c) keeps seated items against near-tied challengers.
2. **Rank fill** gives every remaining slot the best candidate it accepts.
3. **Seating** puts items that were already on screen back in the slots they
   held (see [Seating](#seating-anti-juggling) below).
4. **Refill** gives the next best candidate to any slot seating vacated.
5. **Record** stores the arrangement for the next allocation of this page.

Passes 3-5 run only while `bKeepSlotPositions` is on (the default). They are the
one part of allocation that is NOT stateless: the seating memory is a per-page
array of dedup keys on the allocator, guarded by `m_seatingMutex`. Everything
else about a call is still a pure function of its inputs.

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
  PASS 1a  remembered item -> the slot the player pressed
           (classification ignored; skipped if an override has it)
           |
           v
  PASS 1c  hold seated items unless a challenger wins by the margin
           |
           v
  PASS 2   remaining slots <- best matching candidate, deduped
           by FormID and by name (Regular keys: after the class cap)
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

**Pinned or marked in place.** Before that walk, an override whose item is
already on the page may simply *mark* the slot showing it (label and pulse, no
second copy), so nothing moves. The quieter prompts -- low ammo, soul gem,
drowning -- always do. The three vitals do not by default: health, magicka and
stamina are **pinned** to their configured slot, so the emergency key is muscle
memory (`[Overrides] bPin{Health,Magicka,Stamina}ToSlot`, all on). Set one to
0 and that vital marks in place too. Either way, a stale lock on the override's
item in another slot is released in the same pass (`SlotLocker::ApplyLocks`), so
the item never bounces between two slots.

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

### The slot class cap (Regular keys)

One strong slot class could take the whole page: a hungry character at an
alchemy lab got eight foods on eight keys and none of the craft gear
(2026-10-05). On a **Regular** key, the first `iClassFreeSlots` items of one
class on the page (3) compete at full utility; each one past that competes at
x`fClassRepeatDiscount` (0.5), then x0.25, and so on. Keys with a job are never
capped, and overrides and Remembrance holds are never moved for it, though they
count towards their class.

Called the "need cap" before 0.23.8 (keys `iNeedFreeSlots`,
`fNeedRepeatDiscount`, class `NeedCap`); renamed because "need" now means the
need vector of the engine rewrite. The selection log's JSONL keeps its `need`
column for `tools/replay`.

- **A class** is `SlotClassifier::Classify`, except that all food and drink is
  one class: foods classify by their effect (HealingAny, DefensiveAny, BuffsAny,
  FoodAny), so seven foods on a page never had more than three in one class.
- **Placement only.** `FindBestCandidate` weighs a Regular key's choice by the
  cap (`src/slot/SlotClassCap.h`); ranking, the learner and the selection log keep
  every item's true utility, so learning never sees an item as worse because a
  sibling took a key first.
- **The hold uses it too** (Pass 1c), or a crowd that formed before the cap
  applied would stay. Every held item is counted before any is judged, and a
  held item's factor is its rank by utility within its class -- the weakest of
  four held weapons holds at x0.5, the other three at full. The item being
  judged leaves the count while its challenger is found, so a challenger of
  the same class is weighed as the holder was. Counted slot by slot instead,
  a challenger for a high-priority slot could not see the holders further
  down: four weapons got through on an all-Regular page (vanilla, 2026-10-06
  17:13:24), and a weapon was swapped for a weapon (17:14:11).
- **Why from the 4th item, not the 2nd.** Replayed on 653 picks
  (`tools/replay/replay.py`): the cap at 3 free and x0.5 kept the chosen item
  in the top 8 at 76.3% (76.7% without) and menu picks at 30.8% (29.7%), and no
  page held five of one class (196 did without). Discounting from the 2nd item
  cost 3-10 points overall and up to 25 on potions.
- `[SlotClassCap]` (debug) logs, on change, which items the cap kept off a page.
  `fClassRepeatDiscount = 1.0` turns it off.

---

## Slot Priority System

Slot priority is a **user-defined `int8_t`** (range -128..127; the shipped pages
use 0..9). Higher-priority slots are filled first and therefore get first pick
of the candidate pool. Ties keep slot-index order (`ComputePriorityOrder` uses
`std::stable_sort`, #147); the shipped pages use distinct priorities anyway.

**Slot priority is a different system from override priority.** Override
priority is a fixed constant per condition (`CRITICAL_HEALTH = 100`, …) and
governs collection ordering, lock breaking and Wheeler auto-focus gating. Slot
priority governs fill order only. Neither reads the other.

### Code default layout (1 page, 8 slots)

`Defaults::PAGE0_SLOTS`, `src/slot/SlotSettings.h` -- eight Regular keys, the
same as the shipped INI since 0.22.10:

| Key | Classification | Priority | Overrides |
|-----|----------------|----------|-----------|
| 1 | Regular | 7 | HP |
| 2 | Regular | 6 | MP |
| 3 | Regular | 5 | SP |
| 4 | Regular | 4 | None |
| 5 | Regular | 3 | None |
| 6 | Regular | 2 | None |
| 7 | Regular | 1 | **Other** |
| 8 | Regular | 0 | None |

Every slot has wildcards on (the health key turns them off itself),
skip-equipped on and swap back on. Key 7's `Other` filter reserves a home for
the soul-gem / low-ammo / drowning prompts.

**Why Regular.** One page of plain slots is the end state Huginn is measured
against (roadmap, "What Huginn is measured by"): labels and classes exist
because the recommender is not there yet, and goal 2 counts presses on labeled
slots, so a labeled default would set that number rather than the recommender.
The previous default gave every key a job (1 weapon, 2 attack magic, 3 heal,
4 defend, 5 buff, 6 potion, 7 situational, 8 wildcard) and is kept as
`configs/templates/job-per-key.ini`. Slots on pages 1+ default to `Regular`, wildcards on, override filter
`None`, skip-equipped on, priority `slotCount - index - 1`.

### Shipped INI layout (2 pages)

Most players never edit the INI, so these pages are the product.

| In game | Name | Layout |
|---------|------|--------|
| 1 | Huginn | The code default above: eight Regular keys |
| 2 | Jobs | The previous default: Weapon, Attack magic, Heal, Defend, Buff, Potion, Regular, Regular; emergencies on the same keys as page 1 |
| (3) | Kit | Off by default (`iPageCount = 3` turns it on): Potions, Food, Scrolls, Utility, Summons, Ammo, craft gear, Regular; no overrides |

**Why Jobs is shipped.** It is the fallback, and it is measured. Every press on
it counts as `labeled=` (keys 1-6) and `offPage=`, and every trip to it as
`pageFlips=`, in the `[Soak]` heartbeat's goal-2 fields. If those stay rare the
slot filters can go; if not, they show where the plain page falls short.

**Test pages** (`configs/templates/test-pages.ini`), added after the pages above
for testing and log reading:

| In game | Name | Layout |
|---------|------|--------|
| 3 | T: Emergencies | Regular keys with HP / MP / SP / Other on 1-4; key 5 PotionsAny with no overrides as a control |
| 4 | T: Classes A | Ten keys, one class each: WeaponsMelee, WeaponsRanged, AmmoAny, DamageMagic, HealingAny, DefensiveAny, SummonsAny, BuffsAny, Utility, ScrollsAny |
| 5 | T: Classes B | PotionsAny, FoodAny, AlcoholAny, ApparelAny, DamageAny and the five spell schools |
| 6 | T: Ranking | Eight Regular keys with no overrides, wildcards or swap back: churn from ranking and the hold alone |
| 7 | T: Swap | Swap back and equip: one key with `bSkipEquipped = 0`, one with `bRemembrance = 0` |

The test pages run with wildcards and swap back off unless that is what they
test.

**Templates** (`configs/templates/`): job-per-key (the default before 0.22.10:
a job per key plus the Kit page), battlemage, paladin, pure-mage,
stealth-archer, summoner, survivalist. Each is a page layout to paste over
`[Pages]` and the page sections. All keep the flagship's emergency keys (health
1, magicka 2, stamina 3) and a key-7 home for the `Other` prompts, so switching
template never moves "heal me".

---

## Seating (anti-juggling)

Ranking decides which items are shown. Seating decides where they sit, and its
whole job is that the answer stops changing.

Without it, one item arriving or leaving shifted every item below it by a slot.
The recommendations stayed correct and every key under the player's fingers
changed meaning — dropping an Iron Mace moved Flames from key 3 to key 4, and
picking the mace back up moved it back (2026-09-19 log, 22:57:26 and 22:57:56).

**The memory.** One dedup key (`formID + uniqueID + sourceType`, the same key
the registries and `GetDeduplicationKey()` use) per slot, per page, on the
allocator. The allocator has to own it: `SlotLocker` is keyed by slot index and
knows only the display page, and the Wheeler pages never reach the locker at
all, so nothing else sees every page.

**Pass 3 resolves chains, not swaps.** A departure shifts a run of items up by
one, so putting them back starts at the bottom: the last item drops into the
hole the departure left, which frees the seat the item above it wants, and so on
up the run. The pass is therefore rounds of "move into an empty seat you own"
until nothing moves, followed by one pass for genuine two-item cycles — which a
chain cannot produce, and which no number of rounds can resolve because neither
seat is ever empty.

**What seating will not do:**

- **Move an override, or move into one.** The override pass chose that slot for
  that item and the subtext says so.
- **Seat an item where the layout forbids it.** A returner must pass the same
  classification, wildcard and skip-equipped tests `FindBestCandidate` applies
  when picking one in the first place.
- **Survive a layout reload.** A slot whose classification changed is not the
  slot the memory is about, so a generation bump clears every seat.
- **Let an override rehome its neighbour.** An override slot keeps the seat of
  whatever normally lives there, and the item it displaced keeps its claim
  rather than taking a seat where it was pushed. Otherwise a single low-health
  potion would permanently move the displaced item's key, which is the failure
  this feature exists to prevent.

**Home keys: a seat survives a short absence.** A seat is freed the moment
its owner leaves the screen, so an item that dropped off for a few seconds --
outranked through a burst of refreshes -- came back to whatever key was open.
Across 21 logs (2026-09-30 to 10-06), 1,073 of 1,365 returns within ten
minutes landed on a different key, nearly all of them within a minute. So:

- **The memory.** `m_departed`: per page and slot, the last 3 seat owners that
  left the screen entirely, and when. Written by `RecordSeating`, cleared with
  the seats (game load, layout reload). In memory only.
- **The rule** (phase 0 of `ApplySeating`). A *returner* is on screen, has no
  seat, and left a slot of this page within `fHomeKeyMemorySec` (60 s). It
  takes that slot's seat back from whatever filled the gap -- anything that
  arrived after it left only filled the gap -- and phases 1-2 move it: into
  the slot if empty, else by the swap, which puts the gap-filler where the
  returner landed. The transfer is written to `m_seating` at once; left to
  `RecordSeating`, the gap-filler, still on screen, would keep the seat.
- **Only where the move can happen.** The home slot must take the returner,
  and its occupant must fit the returner's slot. Otherwise the returner keeps
  the key it landed on, which becomes its seat: a recommendation is never
  hidden to wait for a key. Two returners for one slot: the one that left last
  wins.
- **The right of first refusal.** Blocked only by an override or a Remembrance
  hold, the returner claims the seat anyway and waits, shown where it landed --
  as an item an override displaced already does. It is held there (a guest in
  the slot hold) until the hold ends, not moved by the fill when another slot
  empties. When the slot frees, the slot
  hold seats it there unless a challenger beats it by `fChallengerMargin`; if
  it leaves the page meanwhile, the claim lapses with it. On LoreRim's first
  run this was 17 of the 22 returns that missed their key (2026-10-06).
- **One run, not three.** The gap-filler has usually been on the returner's
  key under three seconds, so it is still locked. The move that puts a home
  claimant on its key -- in seating or the hold -- and the gap-filler it swaps
  out are marked (`SlotAssignment::seatMoved`, claims in `m_homeClaims`), and
  `SlotLocker` lets go of a lock whose item moved that way, as it already did
  for an override or Remembrance item -- but only when the destination will
  show it (unlocked, or releasing in the same pass); otherwise the old slot
  keeps showing it. Ordinary seating moves still wait out the lock. Churn
  cause `seated`.
- **What counts as leaving.** A seat owner leaving the screen, and also an
  item with no seat leaving from where it stood: the slot hold clears an
  outranked item's seat on the spot, and that item is exactly the one that
  drops off for a few seconds. Every returner's departure is spent when it
  comes back, whatever the outcome, so it is counted and logged once.
- **Not an override's own item.** A returner never claims a key whose pinned
  item is that key's seat owner (an override marking its item in place): the
  override would lose the seat and jump to its configured slot.
- **Not Remembrance.** Remembrance puts the item a press took off under that
  key, whatever the ranking says. Home keys never add an item: the ranking
  still decides what is shown; this decides where a returning item sits.
- **Measured.** The heartbeat's `returns(home= wait= away=)` counts returns to the
  displayed page, with the switch off too, so `bReturnToHomeKey = 0` gives the
  baseline from the same build. `[HomeKey]` (debug) logs each return, and why
  an item could not go home.

**The visible cost.** Pass 4 refills the slot a departure freed, so normally one
slot changes and nothing else moves. With fewer candidates than slots there is
nothing to refill with, and the gap stays where the departed item was instead of
everything sliding up. `bKeepSlotPositions = 0` in `[SlotLocker]` turns seating
off for a player who prefers the shuffle.

---

## Remembrance

`src/slot/Remembrance.{h,cpp}`. Press a Huginn key, or pick an entry on one of
Huginn's own Wheeler wheels, that puts a weapon, spell or scroll in a hand
(or swaps one ammo for another), and whatever that equip took off appears
under the SAME key for `fRemembranceDurationMs` (15 s): a one-deep undo. An
equip into an empty hand or quiver displaces nothing and starts no hold.

- **Not a recommendation.** Pass 1a places it after overrides and before
  everything else, whatever the slot's classification or `skipEquipped`.
  Pressing it ends the hold and publishes no learner reward (EquipManager and
  the Wheeler `noteSlotActivated` wiring skip the equip callback).
- **When it does not fit the key.** `sRemembranceTarget = Pressed` (default)
  shows it on the pressed key anyway, but only for
  `fRemembranceMismatchDurationMs` (5 s): a dagger a spell took off shows
  briefly on the attack-magic key. `Job` sends it to the first swap-back key
  whose class fits (the dagger to the Weapon key), falling back to the pressed
  key. Undo and release follow the hold to wherever it is shown, and a hold
  breaks that key's lock like an override.
- **Pulses before it ends**, for the last 40% of its time -- the same
  `Expiring` pulse a slot lock uses (1 s cycle, 50-100%).
- **No chaining.** Equipping the remembered item does not remember what it
  took off, so two items cannot ping-pong in one slot.
- **Captured by observation.** The press registers a pending capture;
  `Remembrance::Update` (every tick, from `UpdateSubsystems`) reads both hands
  and the quiver directly, and when the pressed item arrives, what that hand
  held before is remembered. This works the same for hotkeys and for Wheeler,
  which equips before it calls back.
- **One hand.** A two-item displacement remembers the right hand, else the
  left. Instances are not tracked: a weapon owned twice shows its
  best-scoring stack.
- **Ends early** when the item goes back in a hand some other way.
- **Undo settle.** When the item goes back in hand (the undo, or any other
  way), the key that shows it settles for 1 s: it keeps the item, without the
  "Swap Back" label, then fills once and locks (see Churn guards).
- **Always a candidate.** If the ranking dropped the item, `ScoreCandidates`
  appends it unranked with `isRememberedOnly`, which only Pass 1a may place.
- **Wins dedup.** A lock elsewhere on the same item is released in
  `ApplyLocks`, as for an override.
- For one second after a hold starts, `Update` forces a pipeline run every
  tick. The player state and the ammo registry lag the direct read, and a
  spell-for-spell swap may move no hashed state.

Churn telemetry counts a hold arriving or leaving as `remembrance`.

## SlotLocker: Temporal Stability

Prevents UI flicker from brief state oscillations by holding slot content for a
configurable duration.

**Pipeline position:** `SlotAllocator` → `[SlotLocker (stateful)]` → widget/Wheeler

SlotLocker and seating stabilise different axes and do not compete: the locker
holds a SLOT'S CONTENT for a second against the content changing, while seating
keeps an ITEM'S SLOT the same across passes. In the steady state they agree, and
seating removes lock churn — slots whose content would have shifted no longer
change at all.

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

### Churn guards (0.23.37, 0.23.38)

Counts in this section are `[SlotChurn]` lines -- page-0 changes as the locker
made them -- in the 2026-10-11 LoreRim log, window 08:55:40-08:58:00 (119
changes in 140 s) unless another window is named, with at least three misclicks:
a key pressed while it showed one item acted on the item that replaced it a
fraction of a second later. Each key is locked for `fLockDurationMs` after every
change, but eight keys coming free at different times changed the page about
every second. The combat lock changes that policy in combat; the other guards
take the misclick out of it and close the gaps. Settings under `[SlotLocker]`;
the open-wheel rule has none.

**Combat lock** (`fCombatLockDurationMs`, default -1 = freeze, 0 = use
`fLockDurationMs`, else up to 120000 ms; `src/core/CombatLock.h`, wired in
`SlotLocker`). While the player is in combat a key locks for this long instead
of `fLockDurationMs`, and a freeze has no timer. "In combat" is the game's flag
(`StateManager::IsInCombat`) unless no enemy has been tracked for
`fCombatNoEnemyReleaseSec` (default 5, 0 = off; `Core::CombatPresence`, fed the
count the GameState `Enemies:` bucket uses): the flag can stay up for minutes
after the last kill (09:23:56 last enemy, 09:26:05 flag down). The result is
pushed every tick by `SlotLocker::SetInCombat`. At combat onset every key on a
normal lock becomes combat-locked on the item it holds
(`Core::ConvertsAtCombatOnset`, 0.23.38); before, it kept its 3 s timer, ran
out 1-3 s into the fight and was refilled (2026-10-11 12:40:04, 12:59:32,
13:01:02, 13:18:46). Keys holding an override or Remembrance item keep their
own rules, an undo settle ends on its own time, and a Sticky activation lock
keeps its 10 s (its item may be a potion drunk to the last, which the inventory
scan's Gone does not free under that lock; a combat lock would show it all
fight); the sweep locks what follows it. The challenger margin never
reaches a combat-locked key; only these change it (and the combat upsert,
below):

- its item is gone: `OnItemUsed(..., UseKind::Gone)` -- the inventory scan
  passes `Gone` only when none is left (`StillCarried`), weapon departures and
  stale stacks always; a use with more left (`UseKind::Used`, the Wheeler
  activation and a consumption from a stack) does not free it. Checked on
  every run as well (`RunContext::probeItem`): a spell or shout the player no
  longer knows (and that this run did not score) is gone;
- its item is now equipped / worn and the key holds no Remembrance or override
  item (pressed into an empty hand, equipped from the inventory) -- unless the
  allocator still places it there;
- an override placed on that key (urgent ones at once, lower after
  `fMinLockDurationMs`). An urgent override (>= `iImmediateBreakPriority`)
  takes the key even with `bOverridesBreakLock = false`, or a freeze would hide
  it all fight;
- a Remembrance hold on the key the player pressed (`RunContext::
  remembranceOnPressedKey`; a `sRemembranceTarget = Job` hold on another key
  waits);
- the end of the override or hold the key itself shows; and when the same item
  stays but stops (or starts) being an override, the key is re-locked as what
  it is now (`CombatLockBreak::Restyle`), so an ended override does not keep
  its urgent styling, Wheeler label and `[O]` selection flag.

A spell the affordability filter drops stays on its locked key (the lock
re-shows it whatever the candidate list says) instead of leaving and coming
back with the magicka. A seating move (home key, re-seat) does not release a
combat lock, so items do not move between keys. At the end of every run in
combat, every key showing an item that is not locked is locked; empty keys
still fill. When combat ends, combat locks become normal locks with
`fLockDurationMs` counted from then (`RemainingAfterCombat`). A frozen key
shows the Wheeler lock-timer prefix without a countdown (`LockTimerLabel`).
A held key -- combat or normal lock -- keeps its ITEM but takes this run's
numbers (`Core::PlanHeldRefresh`, same form and stack): from this run's scored
row (data, utility and score -- no score from a Remembrance-only row or onto a
pinned key), else from the item's current data as the generator gathered it
from the registries this run BEFORE any filter (`CandidateGenerator::
FindGathered`, through `RunContext::lookupItem`; data only: the potion just
drunk is on cooldown and, at full health, filtered, and the scorer never sees
it), else for an override key from the override's own candidate. This holds
for every kind of held key: a normal lock, the combat freeze, the Sticky
activation lock. An override key takes the override's own current candidate
first (only OverrideManager's candidates carry `overrideReason`, the "Low HP" /
"Poisoned" label), and whatever the source the stored reason is kept when the
new data has none. Under the combat lock a soul gem the gather no longer
offers (a reusable gem emptied: its count does not change) counts as gone. So the stack
count, staff charge and ammo count on the widget move as the player uses them.
An item nobody gathered this run (gone -- the Gone path frees the key) keeps
its stored data. The equipped check
matches the stack when the key names one (`EquippedInstanceMatches`): the twin
of an equipped dagger keeps its key. A combat lock forces one pipeline run when
it turns `fMinLockDurationMs` old, so a non-urgent override waiting on that
minimum is placed then. A seating or override move does not release the source
key toward a pinned destination whose own lock will not let go this run (the
item would be shown on no key). `fCombatNoEnemyReleaseSec` counts enemies within
detection range (about 2048 units): a distant archer or a circling dragon thaws
the keys, and they lock again when it returns. Changing
`fCombatLockDurationMs` mid-fight affects locks taken after the change.

**Combat upsert** (`fCombatUpsertMarginMult`, default 3, 0 = off;
`fCombatUpsertCooldownSec`, default 10, 0 = no limit; `src/core/CombatUpsert.h`,
wired in `SlotLocker::ApplyCombatUpserts`, after dedup and before the combat
sweep; 0.23.38). The freeze cut page-0 changes in combat from 34-60 a minute to
about 9, but made the key absolute. A challenger now takes a combat-locked key
when `s(challenger) - s(item on the key) > fCombatUpsertMarginMult x ln(1 +
fChallengerMargin)` (3 x ln 1.5 = 1.216, about 3.4x the utility under the
bridge; both scores finite). It is measured against the item ON the key -- this
run's score if it was scored, else the last score the key had (the conservative
side) -- never against the allocator's seating, which keeps reshuffling behind
the locks (48 `[Hold]` give-ways in 200 s at margins of 0.4-0.9). Challengers are
the allocator's ordinary picks for this page (not override, Remembrance or
wildcard) that no key shows (by item or name) and that are not equipped (the
locker cannot see `bSkipEquipped`), on a key the allocator placed them on or
whose class they fit (`SlotClassifier::Matches`). That set already passed the
ranking, the class cap and the slot filters; the class-cap term is not applied
again. Never a challenger: an item an upsert displaced this fight, or any item
that left a key less than max(`fCombatUpsertCooldownSec`, 3 s) ago
(`Core::CombatUpsertMemory`). Without that, a displaced item that the seating
wants on another key upserted that key a run later: the home-key move the
combat lock defers, as two changes in 0.1 s (verifier, #208). At most one
upsert per page per run, and 3 s apart (`kCombatUpsertPageSpacingMs`): a
context flip can put several keys past the barrier at once, and the spacing
shows them one at a time, each past the press grace and the double-tap window.
Pairing is greedy on the margin (`PlanCombatUpserts`). Never on a key showing
an override, a Remembrance item or a wildcard, that the allocator wants for an
override or Remembrance item this run (a non-urgent override waiting out
`fMinLockDurationMs`: an upsert first would be two changes and would restart
the wait), with an active hold of its own, changing this run, changed less than
`fPressGraceMs` ago, pressed less than max(`fPressGraceMs`, 400 ms) ago, or
upserted less than `fCombatUpsertCooldownSec` ago; never while the Wheeler wheel
is open. Presses are noted by `SlotLocker::NoteKeyPress` at hotkey down, at key
up (holding the key for the double-tap window + 100 ms, so a pending tap never
acts on an upsert even with `fPressGraceMs = 0`), when the press acts, and on a
Huginn-wheel activation. The new item is combat-locked like any other. One info
line each,
`[SlotLocker] Slot N combat upsert: 'Old' -> 'New' (s=+a vs -b, +d > 3.0 x ln m
0.405)`, and the change counts as `upsert` in `[SlotChurn]` and the heartbeat.
Replayed on the 2026-10-11 12:05-13:27 log (11 fights, 15.7 min in combat, 105
page-0 changes), before the page limit and the departure rule: about 32-36
upserts at 3 (2.0-2.3 a minute) and 69-71 at 2. An upper bound: the log has the
frozen items' scores only when some line printed them, often from a moment they
lost, and minutes old.

**Swap Back undo settle** (`src/core/UndoSettle.h`, `SlotLocker::
SettleAfterUndo`, called by `Remembrance` for the undo and for a held item back
in hand; 0.23.38). The first fill after an undo is made against state that has
not caught up (the 2026-09-28 Skooma case), so the key used to follow the
allocator unlocked for 1 s (`kSettleMs`), and in combat changed two or three
times in it (12:45:03.1-03.6: Circle of Strength -> Staff of the Flame Atronach
-> Raw Canine Meat in 0.45 s; also 12:54:53, 12:55:15, 12:56:20, 13:01:40). Now
the key showing the undone item holds it for the settle, re-typed as an ordinary
item so the "Swap Back" label goes on the next run as before; when the settle
runs out the next run fills the key once and locks that fill even if it is the
same item (`LockThisFill`; a combat lock in combat). An urgent override or a new
Remembrance hold still takes the key meanwhile (the normal lock's rules); a use
of the item (Wheeler reports the undo as one) does not end it, its being gone
does. The settle replaces whatever lock the key has, the Sticky activation lock
included (Wheeler takes it just before reporting the undo; the old per-tick
release let go of it, and kept it would show the undone item for 10 s); a key
whose lock holds the item without showing it is unlocked; a second report of
the same undo changes nothing. The same rule out of combat: the change it saves
is the same misclick hazard. The accepted cost, under every policy: the
just-equipped item stays up, unlabelled, for the full second instead of a stale
pick.

Override evaluation no longer depends on lock expiries: the pipeline also wakes
whenever a raw vital crosses an override's own activation or release threshold
(`Core::OverrideZone`, `PipelineContext::overrideSignature`). Before, the shipped
35% thresholds sit inside the Low bucket, and a fall from 39% to 30% woke the
pipeline only because something else (in combat, mostly a lock expiry) did.

**Post-lock dedup** (`src/core/DedupRules.h`). An override or Remembrance item
is never cleared for a non-pinned copy of its name (a frozen key on another
form with the same display name used to win "first seen" and hide the potion
all fight), and between two pinned copies an override beats a Remembrance item
wherever it sits; a non-pinned copy of a pinned item's name goes; a locked key that
already showed the item (the incumbent) beats a newcomer; an unlocked copy of a
locked key's name goes; otherwise the first stays. The refilled slot is locked.

**Changes under 3 s after the same key's previous change** (19 of 119; window
09:16:40-09:24:10: 29 of 257):

- *Seating moves* (8 and 12): `lock released: its item moved to seated slot`.
  Expected outside combat (a home-key return moves in one run); under the
  combat lock they wait.
- *"Override" breaks of another key* (1 and 3): Remembrance holds taking their
  own key; `ShouldBreakLock` labelled every non-timer break "override". It now
  returns the reason (`BreakReason`) and the log names it. An override breaks
  only its own key (in-game test `RunSlotLockerOverrideScopeTest`).
- *The player's own press* (`manually unlocked` + Remembrance): expected.
- *Dedup refills* (1 and 2): shown but never locked, changed again 0.1-0.7 s
  later. Now locked (rule above).
- On the widget, a key that changed while the Wheeler wheel was open (the
  widget is hidden then) was first seen at the close with part of its lock
  spent; on the push timeline that adds about 3 more per window. The wheel
  close now restarts every lock (`SlotLocker::RestartLocks`).

**Press grace** (`fPressGraceMs`, default 600, 0 = off; `src/core/PressGrace.h`,
wired in `EquipManager::EquipSlot`). A hotkey whose key changed less than that
long before the key went down, or while the tap waited out the 300 ms double-tap
window, acts on the item shown before the change, if it is still usable (an
eligible candidate on the last run and, for inventory items, still carried).
Never when the key now shows an override (`SlotContent::isOverride`): the urgent
item is what the press gets; grace back TO an override that just left is
allowed. `EquipManager::SetSlotContent` records each key's previous item and
change time; the InputHandler passes the gesture's first key-down time. It does
not apply when nothing was shown before, when the player's own press on that
key preceded the change by at most 2 s (a Remembrance swap-back: pressing again
swaps back), or across a page switch. Everything downstream -- the Remembrance
note, the equip and the selection credit -- concerns the item actually used.
One info line when it fires: `[Grace] Page P slot S: pressed X, shown Yms ago;
using previous Z`. Not covered: Huginn's own Wheeler wheels (Wheeler equips its
entry before it calls back), and shouts (not an inventory form; the usability
check refuses them, so a shout is never the previous item acted on).

**Bucket hysteresis** (`fVitalBucketHysteresis` default 0.03,
`fDistanceBucketHysteresis` default 0.1; `src/core/BucketHysteresis.h`, wired in
`StateEvaluator`). The HP/MP/SP level and the closest-enemy distance band feed
the GameState hash that wakes the pipeline, key UsageMemory's recency boost,
and feed PotionDiscriminator's "resource low" multiplier
(`PotionDiscriminator.cpp`, `state.health <= VeryLow` and the like), which now
stays on until the vital is 3 points past 25%. 85 of the 91 state transitions
in the window were MP or distance moves. A recovering vital moves up a level
only `band` past the boundary; one falling from above a boundary moves at the
boundary itself, but one that falls back after recovering less than the band
(8% -> 12% -> 9%) never left the lower level, so the second fall is no hash
change. No override depends on that: overrides read raw vitals and the pipeline
wakes on their own thresholds (above). The distance band is symmetric, a
fraction of the boundary distance. The pipeline commits the remembered buckets
(`EvaluateAndCommit`, once per run); the equip capture reads them without
moving them. The target digest keeps the raw band (it only opens the outer
skip gate). The context weights and the learner's features read raw values.

**Open-wheel writes** (`src/core/OpenWheelWrites.h`, wired in
`WheelerBackend::Push`). The Wheeler push skips while the wheel is open unless
an urgent override is up (priority >= `iAutoFocusMinPriority`, 50: critical
health 100, magicka 70, stamina 60). That bypass used to write every entry of
every page into the open wheel; 15 of 45 wheel opens in the 2026-10-11 session
had such an override. The gate and the auto-focus use the highest urgent
override that carries an item (`TopUrgent`), so critical health with no potion
over critical magicka with one still reaches the open wheel. While the wheel is
open, only the entries of urgent overrides (`UrgentEntries`: every override at
or above the threshold, not only the top one) are written, on every page; every other entry gets what the wheel
already holds (`WheelSync::GetCachedPage`), so its diff writes nothing. The
auto-focus is unchanged, and the close re-runs the pipeline, which writes the
full page. One debug line per entry written while open:
`[WheelerClient] open-wheel write page P slot S: X`.

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

- **Feedback.** Every Wheeler activation is a player selection
  (`SelectionTracker::Select`, `EquipSource::Wheeler`, wired in `src/Main.cpp`).
  Once it confirms -- a consumable's count drops, an equip is still worn 3 s
  later -- the learner gets one reward, `EQUIP_REWARD` (+8.0) or
  `CONSUME_REWARD` (+5.0). A pick off the player's own wheel is noted as player
  input instead, so the equip that follows counts as an outside selection.
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
sName = Huginn            ; Page display name
iSlotCount = 8            ; 1-10 (clamped). Code default 7 on page 0, 3 elsewhere.

[Page0.Slot0]
sClassification = DamageAny   ; SlotClassification enum name (unknown -> Regular + warning)
bWildcardsEnabled = true      ; Allow wildcard exploration picks in this slot
bOverridesEnabled = HP        ; OverrideFilter: None/Any/HP/MP/SP/Other (true/false also accepted)
iPriority = 6                 ; Allocation order (higher = filled first)
bSkipEquipped = true          ; Skip already-equipped candidates (default on)
bRemembrance = true           ; Hold what pressing this slot took off (Remembrance)
```

Every key is optional; a missing `[PageN.SlotM]` section falls back to the
per-slot defaults described under
[Slot Priority System](#slot-priority-system).

### [SlotLocker]

```ini
[SlotLocker]
bKeepSlotPositions = true       ; Keep an item in the slot it was already in (seating)
bHoldSeatedItems = true         ; Hold a seated item until a challenger beats it by the margin
fChallengerMargin = 0.5         ; How much better a challenger must score (0.5 = 50%; 0.25 before 0.23.6)
bReturnToHomeKey = true         ; A returning item takes back the key it left (home keys)
fHomeKeyMemorySec = 60          ; ...if it left within this long; 0-600, 0 = off
iClassFreeSlots = 3             ; Class cap: items of one slot class on Regular keys at full utility (1-10)
fClassRepeatDiscount = 0.5      ; ...and x this for each one past them; 1.0 = off
fRemembranceDurationMs = 15000  ; Remembrance hold length; 0 = off everywhere
fRemembranceMismatchDurationMs = 5000  ; ...on a key whose class it does not fit
sRemembranceTarget = Pressed    ; Pressed | Job (the key whose class fits)
bFillJobKeysFromRegular = false ; A blank job key takes a match off a Regular key
fLockDurationMs = 3000          ; 0 = disable locking
fMinLockDurationMs = 500        ; Minimum time before a lock can break
bLockOnFill = true              ; Lock when a slot fills from empty
bOverridesBreakLock = true      ; Allow high-priority overrides to break locks
iImmediateBreakPriority = 50    ; Override priority that bypasses fMinLockDurationMs
fCombatLockDurationMs = -1      ; In combat: -1 = freeze until combat ends, 0 = fLockDurationMs, else ms (<= 120000)
fCombatNoEnemyReleaseSec = 5    ; ...but combat ends for the lock after this long with no enemy; 0 = off, 0-60
fPressGraceMs = 600             ; Misclick guard: a key changed this recently acts on its previous item; 0-2000, 0 = off
fVitalBucketHysteresis = 0.03   ; HP/MP/SP level moves up only this far past a boundary (falls at it); 0-0.1
fDistanceBucketHysteresis = 0.1 ; Distance band moves only this fraction past a boundary, both ways; 0-0.5
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

bHealthSpellFallback = true       ; No potion: offer a self-cast restore spell
bMagickaSpellFallback = true
bStaminaSpellFallback = true

bPinHealthToSlot = true           ; Emergency on its own key (false: mark in place)
bPinMagickaToSlot = true
bPinStaminaToSlot = true

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

**SlotAllocator** allocation is a pure function of its inputs apart from
seating: `AllocateSlotsForPage()` takes its inputs and returns a fresh
`SlotAssignments`. Its mutable members are individually guarded:

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
- `m_seatingMutex` — guards the per-page seating memory (`m_seating`) and the
  generation it was recorded under. Allocation reads a page's row, works on a
  copy, and writes the result back; the lock is never held across the moves.
  The generation compared against is the one `GetConfigSnapshot()` reported for
  the configs this call is using, not a later re-read, so a reload on another
  thread cannot stamp old-layout seats with the new layout's number.

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
  -> SlotAllocator::AllocateSlotsForPage()              [seating: mutex-guarded]
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
- [Console commands](https://github.com/LandingCrew/huginn/wiki/Console-Commands) - `hg page`, `hg unlock`, `hg reload`

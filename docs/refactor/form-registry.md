# FormRegistry consolidation — architecture-critique finding #8

**Branch:** `tier2-registry-consolidation`
**Scope:** collapse the ~40–45% structural duplication across the four FormID-keyed
registries (Spell / Item / Scroll / Weapon) into a shared CRTP core, plus two generic
query helpers that absorb the ~1,100-line accessor farm.

## Problem (from the critique)

Spell (613+142), Weapon (1203+436), Scroll (635+325), Item (1292+565) = ~5,211 lines
implementing the same machine with the type name substituted, and the copies have already
**diverged into behavioral bugs** (`hg rebuild` reloads spell overrides but silently ignores
item overrides; scratch-map optimization applied to Item but not Scroll; reconcile count-sync
in Scroll but not Item; removal log levels drifted). The line count is not the cost — the
drift is. Every future edit has to be made four times, and the fourth is the one that gets
forgotten.

## The shared machine

Verified element-by-element against current source (post-Tier-1):

| Concern | Spell | Item | Scroll | Weapon |
|---|---|---|---|---|
| `vector<Entry>` + `unordered_map<FormID,size_t>` dual index | ✔ | ✔ | ✔ | ✔ (×2) |
| `shared_mutex` + copy-out reads / unique-lock writes | ✔ | ✔ | ✔ | ✔ |
| `atomic<bool> m_isLoading` + `IsLoading()` | ✔ | ✔ | ✔ | ✔ |
| `ForEach` visitor (bool early-exit vs void) | ✔ | ✔ | ✔ | ✔ |
| swap-pop `Remove(FormID)` | ✔ | ✔ | ✔ | ✔ |
| `GetCount` / `GetAll` copy-out / `GetByType` | ✔ | ✔ | ✔ | ✔ |
| accessor farm: `Get*ByMagnitude(topK)` + `GetBest*` | — | ✔ (~25) | ✔ (~13) | ✔ (~7) |
| change-event delta-diff + count-syncing reconcile | — | ✔ | ✔ | — |

**Entry types differ only in where the FormID lives:** `InventoryItem`/`InventoryScroll`
wrap a `data` member (`entry.data.formID`); `SpellData` *is* the entry (`entry.formID`).
That single difference is the CRTP customization point — nothing else about the core needs
to know the concrete type.

## Design

### Layer 1 — `Huginn::Registry::FormRegistry<Derived, Entry>` (`src/registry/FormRegistry.h`)

CRTP base, header-only. Owns storage + concurrency and provides the query primitives.

- **Derived contract:** `static RE::FormID FormIDOf(const Entry&)` — the only thing the base
  can't infer.
- **Storage (protected):** `m_entries`, `m_formIDIndex`, `m_mutex`, `m_isLoading` — protected
  so each registry's own scan/reconcile keeps operating under its own lock with minimal churn
  (a mechanical `m_scrolls → m_entries` rename).
- **Read API (locks internally):** `EntryCount`, `AllEntriesCopy`, `ForEachEntry`,
  `Collect(pred)`, `QueryTopK(pred, key, topK)`, `FindBest(pred, key)`, `CountMatching(pred)`.
- **Writer primitives (assert unique-lock held):** `RemoveEntryLocked(formID)` (swap-pop),
  `PushEntryLocked(entry, formID)`, `ClearStoreLocked()`.

`QueryTopK`/`FindBest` fold the per-accessor `count > 0` filter into the caller's predicate,
so the base stays count-agnostic and Spell (no counts) uses the exact same primitives. The
returned pointers carry the **same lifetime contract as today** — valid synchronously until
the next mutation; the lock is held only for the duration of the call, exactly as the current
hand-written accessors do.

### Layer 2 — counted-registry reconcile/delta mixin (phase 2b)

The `RefreshCountsFromScan` delta-diff and the reconcile skeleton (scan-outside-lock →
`currentFormIDs` set → add-under-lock → `toRemove` sweep → `previousCount` snapshot →
summary log) are identical between Item and Scroll modulo the scan source and the
classify/add hook. Extracted as a second template method taking the already-scanned
`(Form*, count)` list plus an `add`/`classify` hook. Spell does not use this layer.

Weapon's dual store stays as two `FormRegistry` instantiations — not folded further.

## Migration order (each step builds + is independently PR-able)

1. **Core + Scroll** (this doc's proof): land `FormRegistry.h`; migrate `ScrollRegistry` —
   storage/remove/count/getall/foreach to the base, accessor farm to `QueryTopK`/`FindBest`.
   Public method names kept as thin forwarders so **zero call sites change**.
2. **Counted mixin + Scroll+Item reconcile/refresh** — the delta-diff twin. This is where the
   Item-vs-Scroll drift (scratch-map, count-sync, override reload) gets fixed by construction.
3. **Item accessor farm** — the ~25 potion/soul-gem accessors to `QueryTopK`/`FindBest`.
4. **Spell** — storage + `GetByType`/`GetAll`/`ForEach`; keeps favorites + event-sink locally.
5. **Weapon** — two base instantiations; accessor farm last.

## Invariants preserved (must stay true through every step)

- Copy-out reads under `shared_lock`; build-outside-lock for scans; writers `unique_lock`.
- `AtomicBoolGuard` on `m_isLoading` across Rebuild/Reconcile.
- `GetBest*` remain `noexcept` (lock-or-terminate, matching today).
- No new call-site changes in migration steps 1–4 (public names forwarded).
- Removal/registration log levels **standardized** while consolidating (was drift: trace vs info).

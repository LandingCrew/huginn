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

1. ✅ **Core + Scroll** (`63b1259`): landed `FormRegistry.h`; migrated `ScrollRegistry` —
   storage/remove/count/getall/foreach to the base, 13-method accessor farm to
   `QueryTopK`/`FindBest`. Public names kept as forwarders → zero call-site churn. −314 lines.
2. ✅ **Item** (`25da293`): storage + the ~25-method potion/soul-gem accessor farm; soul-gem
   two-form scan/fill-state/two-tier refresh kept local. −440 lines.
3. ✅ **Spell** (`ace3c1e`): storage + `GetSpellCountByType`/`GetFavoritedSpells`; favorites,
   equip-event sink, `AddNewSpell`, classify-outside-lock reconcile kept local. −71 lines.
4. ✅ **Weapon** (`a3a5414`) — accessor farm only (Option C). Its two stores (`m_weapons`,
   `m_ammo`) share **one** `m_mutex` + one `m_isLoading`, and `RebuildRegistry` swaps both under
   a single lock (an atomic dual-store swap). Deriving would force either two independent locks
   (relaxing that atomicity) or an externally-owned shared mutex in the core. Neither was worth
   it: the perf is a wash (shared readers don't block; writers are cold) and the storage
   duplication is *in-file* weapon-vs-ammo, not the cross-registry drift #8 targets. So Weapon
   keeps its storage/reconcile/mutex verbatim and its ~19 accessors lock once, then call the
   lock-free `CollectLocked`/`QueryTopKLocked`/`FindBestLocked` helpers. ~−175 lines.

**Delivered:** all 4 registries. The Item↔Scroll counted-pair drift the critique called out is
now structurally impossible (both share one delta-diff/reconcile path via the core), the ~55-method
accessor farm across all four is single-sourced, and the query loop/sort logic lives in exactly one
place (the base members delegate to the same lock-free helpers Weapon calls directly). No
cosave-format change → landable during the soak (unlike addendum finding #15). Needs an in-game
pass before merge per the usual workflow.

**The counted-mixin (planned step 2) was folded into steps 1–3 directly:** each counted
registry keeps its own `RefreshCountsFromScan`/reconcile locally against the base primitives
rather than through a separate `CountedFormRegistry` layer. That layer is still worth extracting
if a 5th counted registry appears, but with only Item+Scroll it earned its keep as-is.

## Invariants preserved (must stay true through every step)

- Copy-out reads under `shared_lock`; build-outside-lock for scans; writers `unique_lock`.
- `AtomicBoolGuard` on `m_isLoading` across Rebuild/Reconcile.
- `GetBest*` remain `noexcept` (lock-or-terminate, matching today).
- No new call-site changes in migration steps 1–4 (public names forwarded).
- Removal/registration log levels **standardized** while consolidating (was drift: trace vs info).

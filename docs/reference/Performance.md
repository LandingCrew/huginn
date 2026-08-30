# Performance and Timing

**Applies to v0.19.x.** Code claims verified against `src/` on 2026-08-29; the
measurements are from the captures named below and nowhere else.

> **Related documentation**
> - [../profiling/tracy-traces.md](../profiling/tracy-traces.md) — the capture log. Every number here traces back to an entry there or to the roadmap items that cite one.
> - [../roadmap.md](../roadmap.md) — "Tier 3 — hot-path perf" carries the current ranking and the 2026-08-26 figures.
> - [../roadmap-archive.md](../roadmap-archive.md) — the archived O1 and #14 entries: what shipped, what was dropped, and why.
> - [../refactor/wheeler-push-spikes.md](../refactor/wheeler-push-spikes.md) — the Wheeler push analysis whose ranking premise the long capture overturned.
> - [../architecture/0-pipeline.md](../architecture/0-pipeline.md) — the pipeline these zones measure.
> - [../testing/performance-profiling-guide.md](../testing/performance-profiling-guide.md) — how to build with Tracy and take a capture.

---

## Read this before citing any number

**A measurement without its capture length is not trustworthy.** This is the
single most expensive lesson the project has learned about its own profiling,
and it is why every table below states how long the run was.

Four rules, in the order they bite:

1. **Capture length dominates MTPC for infrequent zones.** A zone that fires
   every few seconds accumulates a handful of samples in a 5-minute run, and
   cold calls — wheel creation, the first push after a load, a page the player
   has never opened — are *all* of them. `Display::Wheeler` measured
   **3.23–3.87 ms** MTPC across short captures of 31–66 pushes. The 44:40
   capture of 2026-08-26, with **874 pushes**, puts the same zone at **986 µs**.
   There was never a 3.6 ms frame spike to fix; there was a small sample. See
   the archived #14 entry in [../roadmap-archive.md](../roadmap-archive.md).
2. **Per-tick pollers need length for the opposite reason.** They fire ~10 Hz,
   so their *totals* are what matter, and a short run under-reports them
   relative to the spiky zones. The 2026-08-23/24 captures ranked the display
   push above `PollPlayerMagicEffects`; the long capture inverted that.
3. **These are DEBUG + Tracy-instrumented builds.** Absolute times are inflated
   by missing inlining and by capture overhead. Use them for **relative ranking
   and cross-build comparison**, never as a release frame budget.
4. **Zone counts are not a diagnostic.** `Huginn_ZONE_NAMED` sits at the top of
   `Push()` ahead of every early-out, so its count always equals the call count
   whether or not the early-out fires. MTPC is the only signal that a gate
   worked.

**MTPC** = mean time per call, i.e. what causes a visible *spike*. **Total** =
MTPC × count over the capture, i.e. cumulative CPU. A large total spread over
~19,000 ticks on a 100 ms budget is cheap per tick even when the total looks
alarming.

---

## Current measured cost — 2026-08-26, 44:40 capture

This is the only capture long enough to trust, and it supersedes everything
earlier in this document's history. Self-only timing, DEBUG + Tracy build.

| Zone | MTPC | Calls | Total | % of runtime | Tier-3 item |
|---|---|---|---|---|---|
| `PollPlayerMagicEffects` | 136.3 µs | 19,179 | **2.61 s** | 0.10% | O3 |
| `PollTargets` | 113.34 µs | 19,179 | 2.17 s | 0.08% | #12 |
| `Inventory::DeltaScan` | 267.78 µs | 3,848 | 1.03 s | 0.04% | #13 |
| `Display::Wheeler` | 986 µs | 874 | 862 ms | 0.03% | #14 (closed) |

**Nothing in the hot path exceeds 0.10% of runtime.** The two per-tick pollers
lead on total precisely because they run on every tick — `PollPlayerMagicEffects`
is not gated by the pipeline skip-check, it *feeds* it. `Inventory::DeltaScan`
overtook `Display::Wheeler` on total not by getting slower but by running ~4×
as often (a 2 Hz delta scan vs. a push every few seconds).

**Consequence for planning:** on CPU grounds Tier 3 is a budget list, not a work
list. The trigger to pick any of it up is a felt stutter, not a µs figure.

### Superseded captures — do not cite these as current

Kept only so a stale number can be recognised as stale. Full entries, with
session context and save size, are in
[../profiling/tracy-traces.md](../profiling/tracy-traces.md).

| Capture | Length | What it said | Why it is wrong now |
|---|---|---|---|
| 2026-08-23 / 2026-08-24 | 5–15 min | `Display::Wheeler` 3.23–3.87 ms MTPC, ranked #1 | 31–66 pushes; cold calls dominated. Long run: 986 µs |
| 2026-07-25 (`99cbb48`) | ~17:29 | `Display::Wheeler` 2.54 ms × 176; `Inventory::DeltaScan` 685 µs × 1,118 = 766 ms | Same small-sample problem on the push. The DeltaScan figure survives as a *large-inventory* data point — the hoarder worst case |
| 2026-07-24 (`4791318`) | ~11:17 | `Inventory::DeltaScan` 161.3 µs × 157 | Tiny test save (3 items / 0 spells / 2 weapons) — this zone scales with inventory size |
| 2026-06-13 (`slot-cleanup`) | ~13.8 min | `WeaponRegistry::Refresh` 1.25 ms × 736 = 918 ms, the #1 cost | This is the **before** side of O1, which shipped. The zone no longer walks the inventory at 2 Hz |
| 2026-02-28 (v0.13.x) | ~30 s | `ScoreCandidates` 16.49 ms, `Display::Wheeler` 5.25 ms | Thirty seconds. Recorded in [../refactor/performance-optimizations.md](../refactor/performance-optimizations.md); treat as archaeology |

---

## The update loop, as actually implemented

### One thread

`UpdateHandler` is a `BSInputDeviceManager` event sink
(`src/update/UpdateHandler.cpp`). Input events arrive every frame; the handler
throttles internally and calls `OnUpdate` once `Config::UPDATE_INTERVAL_MS`
(100 ms) has elapsed. There is no timer thread, no worker pool and no background
learning thread — a search of `src/` finds **no `std::thread`, `std::jthread` or
`std::async` anywhere**.

`deltaSeconds` is clamped to 1.0 s before dispatch, so an alt-tab or a long
pause cannot produce a burst of expired timers.

### Three stages per tick

`OnUpdate` (`src/UpdateLoop.cpp`) runs, in order:

| Stage | Tracy zone | What it does | Gated? |
|---|---|---|---|
| World-loaded guard | — | `IsWorldLoaded()`: LoadingMenu / MainMenu / `Get3D()`. Suspends the **whole** tick, not just the inventory scan | Every tick |
| `UpdateSubsystems` | `Update::Subsystems` | Cooldowns, scorer, override manager, `SlotLocker::Update` (wall-clock lock decay) | Every tick, unconditionally |
| `MaintainRegistries` | `Update::Registries` | Interval-gated registry work (see the interval table below) | Per-registry timers |
| `RunPipelineIfNeeded` | `Update::PipelineCheck` → `RunPipeline` | The recommendation pipeline | Two skip gates |
| Soak telemetry | — | `SoakMetrics::RecordTick(tickMs)` | Every tick |

Lock decay and wildcard expiry deliberately sit *outside* the skip gates: a lock
that only aged on non-skipped ticks would outlive its wall-clock duration in a
static scene.

### Two skip gates

**Outer gate — `RunPipelineIfNeeded`.** Returns before `RunPipeline` is ever
called unless one of these is true:

- the state manager reports a sensor change (`DidLastUpdateChangeState()`),
- a page was cycled (`PeekPageChanged()`),
- the elemental-damage window is live,
- `PipelineCoordinator::NeedsForcedRun()` — state the `GameState` hash cannot
  see: the elemental falling edge, falling (#60), underwater (#61), or a pending
  reason downgrade (#62).

**Inner gate — `CheckHashSkip`.** Compares a discretized `GameState` hash
against the last run's. It consults the same forced-run list, because the outer
gate returns first in a quiet scene and a latch wired into only one gate is
silently dead exactly where it is needed.

Recorded skip rates (both from `tracy-traces.md`, with their capture lengths):

| Capture | Length | Ticks | Passed outer gate | Reached `ScoreCandidates` |
|---|---|---|---|---|
| 2026-06-07 | ~11.5 min | 4,336 | 1,559 (64% skipped) | 52 (96.7% of pipeline runs skipped) |
| 2026-06-13 | ~13.8 min | 4,743 | 1,195 (~75% skipped) | 65 (~94.5% skipped) |

Full scoring therefore runs on the order of **tens of times per session**, not
per second. That is why nothing in the scoring path appears in the hot list.

### Fixed intervals (`src/Config.h`)

All compile-time `constexpr`. There is no adaptive or combat-scaled timing — see
"What this document used to claim" below.

| Constant | Value | Governs |
|---|---|---|
| `UPDATE_INTERVAL_MS` | 100 ms | The whole tick |
| `ITEM_COUNT_REFRESH_INTERVAL_MS` | 500 ms | `Inventory::DeltaScan` (the consumption detector) |
| `ITEM_RECONCILE_INTERVAL_MS` | 30,000 ms | Full item add/remove reconcile |
| `WEAPON_REFRESH_INTERVAL_MS` | 500 ms | Weapon charge delta scan |
| `WEAPON_RECONCILE_INTERVAL_MS` | 30,000 ms | Weapon favorites reconcile |
| `WEAPON_RECONCILE_RETRY_MS` | 1,000 ms | Retry for a load-time reconcile that could not read extraLists |
| `SPELL_RECONCILE_INTERVAL_MS` | 5,000 ms | Newly learned spells |
| `SPELL_FAVORITES_REFRESH_INTERVAL_MS` | 500 ms | Spell favorites delta |
| `EXTRALIST_STABILIZATION_MS` | 500 ms | Post-load window before extraLists may be touched |
| `CONSUMPTION_POST_LOAD_GRACE_MS` | 5,000 ms | Removals not rewarded as consumption after a load |
| `REASON_HOLD_MS` | 1,500 ms | Longest a downgraded context label may linger (#62) |
| `SOAK_HEARTBEAT_INTERVAL_MS` | 300,000 ms | `[Soak]` heartbeat line |

Registry limits: `MAX_TRACKED_TARGETS` 50, `MAX_TRACKED_ITEMS` 500,
`MAX_TRACKED_WEAPONS` 100, `MAX_TRACKED_AMMO` 50, `TARGET_DETECTION_RANGE` 2048
game units. `MAX_TRACKED_TARGETS` is a stress-test value; #12 proposes lowering
it to ~12.

---

## What this document used to claim, and what the code does instead

Earlier revisions of this file described a design that was **never built**. Every
row below was checked against `src/` on 2026-08-29.

| Claimed | Reality |
|---|---|
| Four update tiers (16 ms fast path / 100–500 ms polling / 200–1000 ms refresh / async learning) | **One** 100 ms tick, three sequential stages, two skip gates. No per-frame tier — the override check runs inside the pipeline, not every frame |
| `struct TimingConfig` with `contextPollIntervalMs`, `minContextPollMs`, `maxContextPollMs`, `recommendationRefreshMs` | No such type. Fixed `constexpr` intervals in `Config.h` |
| Adaptive timing: `combatSpeedMultiplier = 0.5`, `idleSpeedMultiplier = 2.0`, `GetEffectivePollInterval()` | Does not exist. No identifier matching `adaptiveTiming` or `combatSpeedMultiplier` appears in `src/` |
| `class LearningQueue`, batched events, `asyncLearning = true`, `learningBatchIntervalMs = 1000` | Does not exist. Learning updates run synchronously — `FeatureQLearner::Update` is called directly from the equip and consumption paths |
| A background **update thread** and a background **learning thread** | Neither exists. Everything is the game thread, plus Wheeler's callback thread and Scaleform's UI thread |
| "800 ms delay from action to learning effect" | No queue, so no such delay. The reward lands on the call; the *display* changes on the next non-skipped pipeline run |
| `class UpdateScheduler`, `UpdatePriority` enum, `maxUpdatesPerFrame = 2`, staggered slot updates | Does not exist. All slots on the current page are pushed together |
| `SlotState::cooldownMs = 500` per-slot cooldown | The mechanism is `SlotLocker`, with different semantics and different numbers — see below |
| Override activation 25% / deactivation 35% | Real defaults are 10% activation with a 15-point hysteresis *gap* (deactivating at 25%) — see below |
| Wildcards: 30 s global cooldown, 60 s per-slot cooldown, 10 per session, 30% probability | Real model is per-page, probability scales with slot index, and there is no session cap — see below |
| Effect-type cache and spell-cost cache keyed by FormID | Neither exists. Caching `SpellData.effectiveCost` is still **open** as Tier-3 item #11: `CandidateGenerator` calls `LookupByID` + `CalculateMagickaCost` per known spell per tick, inside the registry lock |
| 16-float feature vector | **18** floats (`StateFeatures::NUM_FEATURES = 18`), append-only because the order is the cosave wire order |
| Six fixed slots | `MAX_PAGES = 10`, `MAX_SLOTS_PER_PAGE = 10`; the shipped `Huginn.ini` configures 3 pages × 8 slots |
| JSON settings file | INI: `Data/SKSE/Plugins/Huginn.ini`, sections `[Overrides] [Candidates] [Scoring] [Favorites] [ContextWeights] [Wildcards] [Subtexts] [Wheeler] [Pages] [PageN] [PageN.SlotM] [Learning] [SlotLocker] [Keybindings]` |
| EMA velocity smoothing at α = 0.3 causing ~300 ms prediction lag | No EMA predictor exists. `HealthTrackingState` is a 10-entry ring buffer with exponentially decayed 2-second aggregates |

---

## Stability mechanisms that do exist

These are what actually prevent flicker. All three trade responsiveness for
steadiness, and all three are configurable.

### Slot locking (`src/slot/SlotLocker.h`, `[SlotLocker]`)

A slot that receives new content is held for a duration, so the player has time
to act on a recommendation whose triggering context has already passed.

| Setting | Code default | Shipped `Huginn.ini` |
|---|---|---|
| `fLockDurationMs` | 3000 | **1000** |
| `fMinLockDurationMs` | 500 | 500 |
| `bLockOnFill` | true | true |
| `bOverridesBreakLock` | true | true |
| `iImmediateBreakPriority` | 50 | 50 |

A post-activation lock (`LockSlotForActivation`) uses a longer 10 s hold so a
just-used item stays visible even after it is consumed; `OnItemUsed` will not
break it. `SlotLocker::Update` returns true when any lock expired, and the caller
forces a pipeline run so the freed slot refills without waiting for an unrelated
state change.

> **Measurement note.** With `fLockDurationMs = 1000`, `ceil(remaining / 1000)`
> only ever yields 1, so the lock-timer subtext never counts down. Any attempt to
> measure the 3 → 2 → 1 transitions needs the value raised to ~4000 first. This
> is why the #14 lock-label measurements are recorded as weak evidence — locks
> were live for only ~44 s of the 15-minute capture that produced them.

### Override hysteresis (`src/override/OverrideConfig.h`, `[Overrides]`)

Activation is `value < threshold`; deactivation is `value >= threshold + gap`,
and no override may clear before `minOverrideDurationMs` has elapsed.

| Condition | Activation threshold | Hysteresis gap | Effective deactivation |
|---|---|---|---|
| Critical health | 0.10 | 0.15 | 0.25 |
| Critical magicka | 0.10 | 0.15 | 0.25 |
| Critical stamina | 0.10 | 0.15 | 0.25 |
| Weapon charge | 0.25 | 0.05 | 0.30 |
| Low ammo | 10 (absolute count) | 15 | 25 |

`MIN_OVERRIDE_DURATION_MS = 2000`. Drowning is condition-based rather than
threshold-based and has no hysteresis band.

Weapon charge is deliberately nonzero: activation is `charge < threshold`, and a
drained enchanted weapon keeps a sub-cost remainder rather than reaching exactly
zero, so a zero threshold would never fire.

### Wildcards (`src/learning/WildcardManager.h`, `[Wildcards]`)

Exploration picks, so the learner can discover preferences it would otherwise
never see ranked.

| Setting | Value | Meaning |
|---|---|---|
| `fBaseProbability` | 0.165 | `P(slot i) = base × i`, capped |
| `fMaxProbability` | 0.5 | The cap |
| `fCooldownSeconds` | 30 | How long a rolled wildcard persists |
| `fRefractorySeconds` | 60 (INI) / 5 (code default) | Gap before a new roll |
| First slot excluded | true | Slot 0 is always the top-scored pick |

There is **no session cap**. Since v0.19.6 the cache is **per page**: each page
owns its entries, its cooldown and its refractory timer, and records the page
*shape* (`slotCount`, `wildcardSlots`) it was rolled against — a shape change
from an INI hot-reload discards that page's cache wholesale. This closed three
bugs of one kind (#70 and its two siblings), all "a cached wildcard nothing can
display", which suppressed re-rolls because the liveness check scanned the whole
cache while only a bounded prefix could ever be shown.

`UpdateExpiry()` runs unconditionally every tick across **all** pages, because
`ApplyWildcards` only runs on non-skipped pipeline ticks and a wildcard on a page
the player has switched away from must still age out on its own timer.

### Context reason hold

`REASON_HOLD_MS = 1500` damps only a *downgrade* of the displayed context label;
a more urgent reason is adopted instantly. It is clamped to the live lock
duration so a label can never outlive the slot contents it explains.

---

## Data structures

Verified against the headers named.

| Data | Structure | Where | Why |
|---|---|---|---|
| Per-item learning state | `std::unordered_map<FormID, ItemLearningData>` | `FeatureQLearner.h` | Sparse — most items are never trained. Weights, train count and last-update timestamp are **colocated in one struct**: one hash lookup per candidate, not three parallel maps |
| Feature vector | `std::array<float, 18>` | `StateFeatures.h` | Fixed size, stack allocated, append-only wire order |
| Wildcard state | `std::array<PageWildcards, MAX_PAGES>` | `WildcardManager.h` | Fixed, per-page; each holds `std::array<WildcardSlot, MAX_SLOTS_PER_PAGE>` |
| Lock state | `std::array<LockedSlot, MAX_SLOTS_PER_PAGE>` | `SlotLocker.h` | Small, contiguous, snapshot-copyable under one lock acquisition |
| Damage / healing history | `EventRingBuffer<T, 10>` | `StateTypes.h` | Fixed-size circular buffer, so the whole state copies out cheaply under a shared lock |
| Delta-scan scratch | three reused `std::unordered_map<FormID, int32_t>` | `ItemRegistry.h` | `m_scanCounts` / `m_scanFilledCounts` / `m_scanSoulLevels` are members, cleared not reallocated — the scan itself is allocation-free |
| Wheeler push arrays | four member `std::vector`s, `clear()` + `reserve()` | `WheelerBackend.h` | Was four fresh allocations per page per tick |
| Pipeline context | `PipelineContext m_ctx`, `Reset()` rather than reconstruct | `PipelineCoordinator.h` | Preserves container capacity across ticks. **Note:** roadmap #13 records this container reuse as "documented-but-broken" — verify before relying on it |
| Candidates | `std::vector<ScoredCandidate>`, rebuilt per scoring run | `ScoredCandidate.h` | Sorted in place; scoring runs tens of times a session, not per tick |

**Caches that do not exist:** there is no effect-type classification cache and no
spell magicka-cost cache. Adding the latter is open Tier-3 item #11.

---

## Thread model

Three threads touch Huginn state. None of them belong to Huginn.

| Thread | Origin | What runs on it |
|---|---|---|
| **Game thread** | Skyrim | `OnUpdate` and everything it calls: polling, registries, the whole pipeline, scoring, allocation, locking, the display push, learning updates, cosave serialization |
| **Wheeler callback thread** | Wheeler's DLL | `ItemActivatedCallback`, wheel-state and edit-mode callbacks. Treat as foreign |
| **Scaleform UI thread** | GFx | Everything `IntuitionMenu` defers via `SKSE::GetTaskInterface()->AddUITask()` |

The rule that follows: **never call `Invoke` or `CreateString` from the update
thread.** All `IntuitionMenu` public API methods defer their GFx work.

### Synchronization inventory

| Owner | Primitive | Protects |
|---|---|---|
| `State::StateManager` | 4 × `shared_mutex` (`m_worldMutex`, `m_playerMutex`, `m_targetsMutex`, `m_trackingMutex`) | Split by state type so a target poll does not block a vitals read. Copy-out accessors |
| `Learning::FeatureQLearner` | `shared_mutex` | `m_items`, `m_totalTrainCount`. A batch-query handle acquires the shared lock once and the caller loops N candidates under it |
| `Registry::FormRegistry` and the per-type registries | `shared_mutex` each | Registry contents. Accessors are deliberately **un-zoned** in Tracy — absent `QueryTopK` / `FindBest` zones are expected, not missing data |
| `Slot::SlotLocker` | `mutex` | Lock state — Wheeler callbacks reach `OnItemUsed` / `LockSlotForActivation` from the callback thread. `GetLockSnapshot()` exists so a push takes the lock once instead of up to 2 × slots × pages round-trips |
| `Slot::SlotAllocator` | `m_cacheMutex`, `m_logMutex` | Page cache and log dedup |
| `Wheeler::WheelSync` | `m_pageDataMutex` | Page-wheel mapping and add-fail cooldowns. Owns this state exclusively; callbacks get query/mutator methods, never a reference into it |
| `Wheeler::WheelerClient` | `m_callbackMutex` | Callback state |
| `Wheeler::WheelerConnection` | atomic | The API pointer — single writer (`TryConnect`), everyone else reads |
| `Learning::PipelineStateCache` | `shared_mutex` | The snapshot consumed by the consumption/attribution path |
| `Learning::EquipEventBus`, `EquipSourceTracker`, `ExternalEquipLearner`, `InventoryExitTracker` | `mutex` each | Equip attribution state, written from callbacks |
| `Input::InputHandler`, `Input::EquipManager` | `shared_mutex` each | Keycode map, slot contents |
| `Update::UpdateHandler` | `mutex` | `m_lastUpdate` and callback invocation |
| `State::DamageEventSink` | `m_queueMutex` | Damage event queue filled from the game's event dispatch |

### The StateManager pattern

- Accessors return **copies**, not references.
- `shared_lock` for reads, `unique_lock` for writes.
- Build the new state **outside** the lock, then copy in — short critical
  sections.

`PipelineCoordinator::ResetCrossSaveState()` is documented as **not uniformly
serialized**: the console path (`hg reset all`) runs under
`UpdateHandler::RunExclusive`, but the save-load path does not, so a reset can
land while the update thread is inside the pipeline. The worst case is one tick
reading a mix of both sides — a stale label or one redundant pipeline run, not a
torn structure.

### Wheeler

Wheeler is thread-safe internally. Huginn may call its API from any thread. The
contract lives in `src/wheeler/WheelerAPI.h` and the thread-safety notes in
`CLAUDE.md`; there is no separate Wheeler integration document.

---

## Optimization status

### Shipped

| Item | What changed | Evidence |
|---|---|---|
| **O1** — `WeaponRegistry::Refresh` | The full inventory walk at 2 Hz is gone; ~1.2 ms reclaimed per fire, and the ~1 s memory sawtooth from per-call `InventoryEntryData` map allocation went with it. Ammo counts stay per-tick on purpose: the low-ammo override gates on the cached count | Before: 2026-06-13, ~13.8 min, 1.25 ms × 736 = 918 ms, the #1 zone. After: 2026-07-24 capture |
| **#14 part 1** — `IntuitionBackend` change-detect | `Push()` compares the built frame against the last and returns on an identical one, then sends from `m_lastPush` rather than recomputing | 92.76 µs → 63.12 µs. Structurally understates the win: the zone wraps the scratch build but never the UI-thread GFx work it saves |
| **#14 part 2** — `GetLockSnapshot` hoist | One snapshot per push instead of `IsSlotLocked()` + `GetRemainingLockTime()` per slot per page | Gated on `showLockTimerLabel`, because the old per-slot calls were too — an unconditional hoist traded zero lock acquisitions for one in the default config |
| **#14 part 3** — lock-timer quantization | Whole seconds, not tenths. A tenths-place countdown changed on nearly every push, so `WheelSync::UpdatePage`'s content-unchanged early-out never fired while any slot was locked | — |
| **P3** — Wheeler push allocations | The four `Extract*` passes became one loop into four reused member vectors, with the soul-gem and Empty-policy fixups folded in | `WheelerBackend.h:24-27` |
| **#8** — registry consolidation | Perf-neutral, confirmed on a dense real save: every `*Registry::Reconcile` zone stays in the hundreds of µs | 2026-07-25, ~17:29 |
| **#9** — display abstraction | `Pipeline::PushDisplay` dropped to 1.85 µs/call; mid-tick re-allocation left the push path entirely | 2026-07-25, ~17:29 |

### Dropped

**#14 part 4 — lazy per-page allocation in `WheelerBackend`** (the same idea as
P1 in [../refactor/wheeler-push-spikes.md](../refactor/wheeler-push-spikes.md)).
Dropped on the 2026-08-26 measurement: the ranking that motivated it was a
small-sample artifact. `WheelerBackend::Push` still calls `AllocateSlotsForPage`
for every non-current page on every push, and at a 986 µs mean that is
affordable. Reopen only if a felt stutter turns up that a sub-1 ms mean cannot
explain.

Two lessons from that work are worth more than the code was:

1. **Both regressions in the first cut were caught only by Tracy, never by the
   log.** Change-detect never fired because `confidence`
   (= `assignment.utility`) varies every run; the lock snapshot was hoisted
   unconditionally past a setting gate, turning zero lock acquisitions into one.
2. **Zone-count equality proves nothing** — see rule 4 at the top of this
   document.

### Open (a budget list, not a work list)

| Item | Zone | 2026-08-26 figure | Note |
|---|---|---|---|
| **O3** | `PollPlayerMagicEffects` | 136.3 µs × 19,179 = 2.61 s (0.10%) | The honest top of Tier 3. Not gated by the skip-check — it *feeds* it. Options: early-out on an unchanged active-effect list, or cache by effect-list revision |
| **#12** | `PollTargets` | 113.34 µs × 19,179 = 2.17 s (0.08%) | Build outside the write lock; one classification pass instead of up to 3×/tick; `MAX_TRACKED_TARGETS` 50 → ~12; squared-distance compares. A steady per-tick cost, never a hitch |
| **#13** | `Inventory::DeltaScan` | 267.78 µs × 3,848 = 1.03 s (0.04%) | Count-only two-phase `GetInventorySafe`. Constrained, not eliminable — this is the consumption detector and it needs a count snapshot (verified to fire exactly once per consumption). The scratch maps are already allocation-free, so what is left is the SKSE query itself. Scales with inventory size: 685 µs/call on the 2026-07-25 hoarder save vs 161 µs on the small test save |
| **#11** | (un-zoned) | — | Cache `SpellData.effectiveCost`. `CandidateGenerator` calls `LookupByID` + `CalculateMagickaCost` per known spell per tick, inside the registry lock |

Squared-distance comparison (`distanceSq < threshold * threshold`), early-exit
loops and pre-computed `constexpr` thresholds are the standing conventions in
the polling code.

---

## Performance targets

The project has **no formally agreed frame-budget targets.** Earlier revisions of
this file listed a table of them; none of those figures can be traced to a
decision or a measurement, and one of them refers to a cache that does not exist.

<!-- UNVERIFIED: the numeric performance targets in the pre-2026-08-29 revision
     of this document (frame impact < 1 ms, context poll < 5 ms, full pipeline
     < 15 ms, memory overhead < 5 MB, learning queue depth < 100, hash-map
     lookup < 0.1 ms, effect-cache hit rate > 95%) have no traceable source and
     are not reproduced here. If targets are wanted, set them against a long
     capture. -->

What *is* measured, and can stand in until real targets are set:

- **The 0.10% ceiling.** No zone on the 2026-08-26 capture exceeded 0.10% of
  runtime. A future capture where one does is the signal to act.
- **`[Soak]` heartbeat tick cost.** `SoakMetrics` emits
  `tick avg=… peak=… ms` every 5 minutes, alongside recompute/tick counts,
  override runs, page bails and learned-item growth. This is the cheapest
  continuous perf signal the project has, it runs in release builds, and unlike
  a Tracy capture it covers 20–50 hour sessions. See
  [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md).
- **Memory** was stable with no leaks on every capture that recorded it, and the
  ~1 s allocation sawtooth visible in the pre-O1 Memory plot is gone.

---

## Logging and error handling

**Silent failures are not an option**, but neither is a log line per tick. The
debug log runs at roughly 10 lines/sec; the governing rule is **log transitions,
not ticks** (see `CLAUDE.md`).

1. **No "nothing happened" logs.** The absence of a line is itself the signal.
2. **Dedup periodic logs** — only log when the result changes, typically with a
   `static` last-value. `PipelineCoordinator` keeps both the raw and the
   displayed context reason for exactly this purpose: deduping on the displayed
   label alone would swallow every raw change a hold was masking.
3. **Every failure path logs why**, with FormIDs, indices and counts.
4. **Rate-limit anything high-frequency.** The Wheeler edit-mode push gate logs
   two lines per editor session, not one per tick.

| Level | Use | Example |
|---|---|---|
| `error` | Operation failed, feature degraded | `"[SpellRegistry] Spell {:08X} not found"` |
| `warn` | Unexpected but recoverable | `"[WheelerClient] API not connected, skipping wheel update"` |
| `info` | Transitions, summaries, initialization | `"[Huginn] Pipeline suspended (world unloaded)"` |
| `debug` | Diagnostics (debug builds) | `"[Wheeler] Push suppressed — edit mode entered"` |
| `trace` | Per-item / per-tick | Registration lines. Effectively off; a summary count at `info` is sufficient |

### Graceful degradation

| Failure | Behavior |
|---|---|
| Wheeler API unavailable | Recommendations still computed and shown on the Intuition widget |
| Cosave record absent or fails to load | Start from priors, learn fresh |
| Spell registry empty or still loading | The pipeline gate declines to run |
| Invalid FormID from a callback | Logged and ignored |
| Update handler registration failed | Degraded mode: `OnUpdate` returns early and warns at most once a minute |
| World unloaded mid-session | The whole tick is suspended; both edges logged as transitions |
| Every Wheeler page invalidated | `RecoverInvalidatedWheels()` runs *ahead* of the has-wheel guard, so recovery is reachable from the state that looks identical to "wheels never created" (#76) |

---

## Known limitations and trade-offs

### Design trade-offs

**Recommendation lag is deliberate.** The pipeline is gated twice and slots are
locked for at least `fLockDurationMs` after they fill, so a recommendation can be
up to a lock duration stale. That is the entire point: without it, surfacing
Waterbreathing while the player is underwater removes it the instant they surface
to cast.

**Wildcards can surface a low-ranked pick during a hard fight.** They exist so
the learner sees items the ranking would otherwise bury, and the probability
scales with slot index so slot 0 is never affected. The cooldown/refractory pair
bounds how often it happens.

<!-- UNVERIFIED: an earlier revision claimed wildcards are disabled below 30%
     health. No such gate exists in WildcardManager. -->

**Threshold discontinuities.** Bucketed thresholds mean 25% and 26% health can be
treated differently. Hysteresis bands reduce oscillation at the boundaries; the
18-float continuous feature vector is what removed the need for bucketing inside
the learner itself — the discrete 36,288-state hash it replaced is gone.

**Mod spell packs cold-start.** A 200-spell pack arrives with no learned weights,
so ranking falls back entirely on `PriorCalculator` until the player uses things.
Effect-keyword classification and the `[Overrides]` INI mechanism are the current
mitigations.

### Edge cases

**Cell transitions and load screens.** No `kPostLoadGame` fires for a cell
change, so the tick guard is the only signal. On resume, `PipelineStateCache`'s
timestamp is refreshed (otherwise the next delta scan would drop every legitimate
consumption as "stale cache") and slot locks are reset — they are wall-clock
timers that stopped decaying and pin a world that no longer exists.

**Quit to main menu.** `PlayerCharacter::GetSingleton()` stays non-null through a
quit-to-menu, so a null check is *not* a liveness test. `IsWorldLoaded()` gates
on LoadingMenu, MainMenu and `Get3D()`. Before that guard existed, the registry
walk faulted on a freed `ContainerObject`, and the tick before the crash reported
the entire inventory as consumed.

**Bulk inventory strips.** `TEARDOWN_MIN_DROPS = 3` plus
`TEARDOWN_DROP_RATIO = 0.5` — measured against **live** entries, not registry
size — catch the teardown-shaped scan. `CONSUMPTION_POST_LOAD_GRACE_MS = 5000`
covers alternate-start mods stripping starter items shortly after a load.

**Player death** is not penalized. Death attribution is genuinely ambiguous, and
a wrong penalty is worse than no signal.

**Mod load-order changes.** The cosave stores plugin name plus local FormID, so
weights survive a reorder. If a mod is removed, its weights are orphaned and
ignored.

**Learning-table growth.** `m_items` is keyed by FormID, not by state — it grows
with the number of *items the player has used*, not with the state space, so it
is bounded by the load order in practice. Growth is reported in the `[Soak]`
heartbeat (`learn items=… trains=…`). No eviction policy is implemented, and
none has been needed.

---

## Related documentation

| Document | Contents |
|---|---|
| [../architecture/0-pipeline.md](../architecture/0-pipeline.md) | Data flow: context → candidates → scoring → slots |
| [../architecture/4-contextual-bandits.md](../architecture/4-contextual-bandits.md) | The learner's update rule and feature vector |
| [../architecture/5-slots.md](../architecture/5-slots.md) | Slot classification, overrides, wildcards, locking |
| [../profiling/tracy-traces.md](../profiling/tracy-traces.md) | Capture log — the source for every number above |
| [../testing/performance-profiling-guide.md](../testing/performance-profiling-guide.md) | How to build with Tracy and take a capture |
| [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) | Long-play soak protocol and the `[Soak]` heartbeat |
| [../refactor/wheeler-push-spikes.md](../refactor/wheeler-push-spikes.md) | Wheeler push analysis (its P1 was dropped; P2–P3 landed) |
| [../refactor/performance-optimizations.md](../refactor/performance-optimizations.md) | The 2026-02 optimization pass — archaeology, from a 30-second capture |
| [ConsoleCommands.md](ConsoleCommands.md) | `hg status`, `hg recs`, `hg reload` and the rest |

> **Terminology.** The learner is a **contextual bandit**. The identifiers
> `FeatureQLearner`, `QLearnerSerializer`, the `FQLW` cosave record and
> `hg reset qvalues` keep their historical names, because renaming them would
> break the cosave format and a documented console command.

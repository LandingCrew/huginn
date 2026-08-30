# Performance Optimizations

**Created:** 2026-02-28
**Based on:** Tracy profiler capture (v0.13.x, ~30s, ~69,674 frames)

---

## Profiler Baseline

| Zone | MTPC | Calls | Total | Notes |
|------|------|-------|-------|-------|
| **ScoreCandidates** | 16.49ms | 382 | 6.3s (0.36%) | Biggest per-call cost |
| **Display::Wheeler** | 5.25ms | 382 | 2s (0.11%) | 45x more expensive than Intuition |
| **PollTargets** | 423us | 12,805 | 5.42s (0.31%) | Most expensive sensor |
| **PollPlayerMagicEffects** | 146us | 12,805 | 1.88s (0.11%) | 2nd most expensive sensor |
| **OnUpdate** | 733us | 12,804 | 9.39s (0.53%) | Sum of all per-tick work |
| Display::Intuition | 116us | 382 | 44ms (0.00%) | Scaleform widget (lean) |
| Pipeline::AllocateAndLock | 357us | 382 | 136ms (0.01%) | Slot allocation |
| Pipeline::GatherState | 52us | 11,849 | 611ms (0.03%) | State copy-out |
| Pipeline::PushDisplay | 3us | 382 | 1ms (0.00%) | Display push |
| PollPlayerEquipment | 41us | 12,805 | 523ms (0.03%) | Inventory scan |
| PollPlayerVitals | 26us | 12,805 | 330ms (0.02%) | |
| PollPlayerPosition | 17us | 12,805 | 221ms (0.01%) | |
| PollWorldObjects | 17us | 12,805 | 212ms (0.01%) | |
| PollHealthTracking | 14us | 12,805 | 175ms (0.01%) | |
| StateManager::Update | 8us | 12,804 | 109ms (0.01%) | |
| PollMagickaTracking | 7us | 12,805 | 87ms (0.00%) | |
| PollStaminaTracking | 6us | 12,805 | 82ms (0.00%) | |
| PollPlayerSurvival | 29us | 1,352 | 39ms (0.00%) | Interval-gated |
| PollPlayerResistances | 5us | 2,584 | 13ms (0.00%) | Interval-gated |

**Memory:** Stable at 271.95 KB, no leaks.
**Pipeline skip rate:** 382 scoring runs / 12,805 updates = ~3% (dirty flag + hash check working well).

---

## ScoreCandidates (16.49ms)

### Root Cause

No single expensive operation. Cost accumulates across multiple passes and per-candidate work:

1. **`MaybeDecay()` loop** (pre-scoring) -- acquires `shared_lock` per candidate (~200 lock ops)
2. **Main scoring loop** -- per candidate: `GetContextWeight` + `GetMetrics` + `CalculatePrior` + `CalculateBonus` + `GetMultiplier` + `GetFavoritesMultiplier` (6x `std::visit` dispatches)
3. **Cold-start fallback** (conditional) -- 2nd candidate loop + `unordered_set` heap allocation
4. **Partial sort** -- O(N log K) for top 10

### P1: Batch MaybeDecay into Locked Reader

**Problem:** `MaybeDecay()` is called per-candidate (line 50-52 of UtilityScorer.cpp) BEFORE the amortized `AcquireReader()`. Each call acquires its own `shared_lock` on `m_mutex`, does 2 map lookups (`m_lastUpdateTime`, `m_weights`), and usually exits early (item was recently updated). With ~200 candidates, that's ~200 redundant lock acquisitions.

**Fix:** Move decay into the locked reader pattern. Either:
- (a) Add a `MaybeDecayBatch(vector<FormID>)` that acquires lock once, or
- (b) Integrate decay check into `LockedReader::GetMetrics()` so it piggybacks on the existing lock

**Expected savings:** Eliminate ~200 `shared_lock` acquisitions per scoring pass.
**Risk:** Low. Decay is lazy and idempotent.
**Files:** `FeatureQLearner.cpp`, `FeatureQLearner.h`, `UtilityScorer.cpp`

### P2: Pre-compute hasUndeadTarget

**Problem:** `CorrelationBooster::HasUndeadTarget()` iterates `targets.targets` for every weapon, ammo, and scroll candidate. With 50 tracked targets and 30 weapon/ammo candidates, that's ~1,500 iterations per scoring pass.

**Fix:** Pre-compute `bool hasUndead` once at the start of `ScoreCandidates()` (or in `EvaluateRules`) and pass it through. Single O(N) scan replaces N*M.

**Expected savings:** Eliminate redundant target iteration in CorrelationBooster.
**Risk:** Low. Pure read-only optimization.
**Files:** `CorrelationBooster.cpp`, `CorrelationBooster.h`, `UtilityScorer.cpp`

### P3: Reuse Member-Level Set for Cold-Start Dedup

**Problem:** Cold-start fallback (line 107) creates `unordered_set<RE::FormID>` with heap allocation on every conditional trigger.

**Fix:** Promote to member variable (same pattern as `m_processedAllies` in StateManager). Clear per-call instead of allocate.

**Expected savings:** Eliminate conditional heap allocation.
**Risk:** Low.
**Files:** `UtilityScorer.cpp`, `UtilityScorer.h`

---

## PollTargets (423us, 12,805 calls)

### Root Cause

Three stages run every ~100ms tick:
1. **Crosshair/sticky detection** -- cheap (FormID lookup + one GetPosition)
2. **Combat enemy loop** -- `highActorHandles` iteration, only in combat
3. **Ally scan** -- iterates ALL 3 process levels (high, middleHigh, middleLow) with `IsHostileToActor()` per actor, **runs every tick regardless of combat state**

The ally scan is the dominant cost outside of combat. `IsHostileToActor()` is the most expensive per-actor call (faction + crime checks).

### P1: Interval-Gate Ally Scan

**Problem:** Allies are scanned every ~100ms across 3 process levels. Ally state changes slowly (followers don't appear/disappear/move dramatically between ticks).

**Fix:** Add interval gating for the ally scan portion:
```cpp
static constexpr float ALLY_SCAN_INTERVAL_MS = 500.0f;  // 5x reduction
```
Skip the ally scan if less than 500ms since last scan. Keep combat enemy scan at full rate.

**Expected savings:** ~4x reduction in out-of-combat PollTargets cost (~100us instead of ~423us for non-combat ticks).
**Risk:** Low. Ally state is used for heal-other suggestions and ally-aware heuristics, neither of which is latency-sensitive.
**Files:** `StateManager_Targets.cpp`, `StateManagerConstants.h` (or `StateConstants.h`)

### P2: Smart Dirty Return from PollTargets

**Problem:** `PollTargets()` unconditionally returns `true` (line 679), meaning downstream dirty-flag optimization never benefits from stable target state. Even when nothing changed (same targets, same distances, no new/removed actors), the scoring pipeline re-runs.

**Fix:** Track actual changes:
- New target added
- Target removed (pruned)
- Primary target changed
- Return `false` when none of these occurred

**Expected savings:** Reduce scoring pipeline triggers. In peaceful exploration with stable followers, PollTargets could return false for most ticks.
**Risk:** Medium. Must be thorough about what constitutes a "change" to avoid stale recommendations.
**Files:** `StateManager_Targets.cpp`

### P3: Merge Combat + Ally into Single ProcessList Pass

**Problem:** Combat enemy scan and ally scan both iterate `processLists->highActorHandles` independently. Each actor gets `Get3D()`, dead/disabled/deleted checks, and distance calculation twice.

**Fix:** Single pass over `highActorHandles` that tags each actor as hostile/ally/irrelevant, then processes accordingly. Share the common validation and distance work.

**Expected savings:** Eliminate duplicate iteration of highActorHandles during combat. Saves ~200 `Get3D()` + distance calculations per tick in combat.
**Risk:** Medium. Increases code complexity. The ally scan also covers middleHigh/middleLow levels which the combat loop doesn't, so the merge is partial.
**Files:** `StateManager_Targets.cpp`

---

## Not Optimized (Acceptable)

| Zone | Why It's Fine |
|------|--------------|
| **Display::Wheeler** (5.25ms) | External mod API overhead, out of our control |
| **PollPlayerMagicEffects** (146us) | Already optimized; cost is inherent to SKSE MagicTarget traversal |
| **PollPlayerEquipment** (41us) | Already has early-exit; could skip loop entirely when nothing needed (backlog item) |
| **Pipeline::GatherState** (52us) | State copy-out under lock, already minimal |

---

## Implementation Order

1. **P1: Batch MaybeDecay** -- lowest risk, clear lock contention reduction
2. **P1: Interval-gate ally scan** -- biggest per-tick savings, simple timer check
3. **P2: Pre-compute hasUndeadTarget** -- quick win, eliminates N*M iteration
4. **P2: Smart dirty return** -- medium effort, high impact for peaceful gameplay
5. **P3: Member-level cold-start set** -- trivial cleanup
6. **P3: Merge ProcessList pass** -- most complex, save for last

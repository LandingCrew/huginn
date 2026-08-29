
---

## Performance & Timing

### Update Loop Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           UPDATE LOOP TIERS                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  TIER 1: FAST PATH (Every Frame / 16ms)                                     │
│  ───────────────────────────────────────                                    │
│  Critical checks only - must not block game thread                          │
│                                                                              │
│  • Override condition check (health critical, drowning, falling)            │
│  • UI render (already-computed recommendations)                             │
│  • Input handling (slot activation)                                         │
│                                                                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  TIER 2: CONTEXT POLLING (Configurable: 100-500ms)                          │
│  ─────────────────────────────────────────────────                          │
│  Full context state refresh - triggers re-scoring if state changed          │
│                                                                              │
│  • Read player stats (health%, magicka%, stamina%)                          │
│  • Check active effects                                                     │
│  • Evaluate crosshair target                                                │
│  • Combat state check                                                       │
│                                                                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  TIER 3: RECOMMENDATION REFRESH (Configurable: 200-1000ms)                  │
│  ──────────────────────────────────────────────────────────                 │
│  Full pipeline run - only when context has meaningfully changed             │
│                                                                              │
│  • Gather candidates                                                        │
│  • Score candidates                                                         │
│  • Allocate to slots                                                        │
│  • Update Wheeler wheel                                                     │
│                                                                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  TIER 4: LEARNING UPDATES (Async / Queued)                                  │
│  ─────────────────────────────────────────                                  │
│  Non-blocking - can be delayed without affecting recommendations            │
│                                                                              │
│  • reward estimate updates from feedback events                                     │
│  • Training count increments                                                │
│  • Dynamic threshold adjustments                                            │
│  • Persistence (auto-save)                                                  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Configurable Refresh Rates

```cpp
struct TimingConfig {
    // ─────────────────────────────────────────────────────
    // Context Polling
    // ─────────────────────────────────────────────────────
    float contextPollIntervalMs = 200.0f;   // How often to read game state
    float minContextPollMs = 50.0f;         // Minimum (for combat)
    float maxContextPollMs = 500.0f;        // Maximum (out of combat)

    // ─────────────────────────────────────────────────────
    // Recommendation Refresh
    // ─────────────────────────────────────────────────────
    float recommendationRefreshMs = 300.0f; // Full pipeline run interval
    bool refreshOnContextChange = true;     // Immediate refresh when state changes
    bool refreshOnCombatStart = true;       // Immediate refresh on combat enter

    // ─────────────────────────────────────────────────────
    // Learning Queue
    // ─────────────────────────────────────────────────────
    float learningBatchIntervalMs = 1000.0f; // Process learning queue every 1s
    int maxLearningBatchSize = 10;           // Max events per batch
    bool asyncLearning = true;               // Process on background thread

    // ─────────────────────────────────────────────────────
    // Adaptive Timing
    // ─────────────────────────────────────────────────────
    bool adaptiveTiming = true;             // Adjust rates based on context
    float combatSpeedMultiplier = 0.5f;     // 2x faster in combat
    float idleSpeedMultiplier = 2.0f;       // 2x slower when idle
};
```

### Adaptive Timing

Context-aware polling rates:

```cpp
float GetEffectivePollInterval(const TimingConfig& config, const ContextState& ctx) {
    float base = config.contextPollIntervalMs;

    if (!config.adaptiveTiming) return base;

    if (ctx.inCombat) {
        // Combat: faster polling, player needs responsive recommendations
        return std::max(config.minContextPollMs, base * config.combatSpeedMultiplier);
    }

    if (!ctx.inCombat && ctx.timeSinceCombatEnd > 30.0f) {
        // Long idle: slower polling, conserve resources
        return std::min(config.maxContextPollMs, base * config.idleSpeedMultiplier);
    }

    return base;
}
```

### Event Queue System

Events are queued for batch processing to avoid frame drops:

```cpp
struct LearningEvent {
    enum class Type { Equip, Cast, Skip, OverrideUsed };

    Type type;
    RE::FormID formID;
    StateFeatures features;
    float reward;
    float timestamp;
};

class LearningQueue {
    std::queue<LearningEvent> m_queue;
    std::mutex m_mutex;
    std::atomic<bool> m_processing{false};

public:
    // Called from game thread - non-blocking
    void Push(LearningEvent event) {
        std::lock_guard lock(m_mutex);
        m_queue.push(std::move(event));
    }

    // Called from background thread or during idle
    void ProcessBatch(FeatureQLearner& learner, int maxEvents) {
        if (m_processing.exchange(true)) return; // Already processing

        std::vector<LearningEvent> batch;
        {
            std::lock_guard lock(m_mutex);
            while (!m_queue.empty() && batch.size() < maxEvents) {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop();
            }
        }

        // Process outside lock
        for (const auto& event : batch) {
            learner.Update(event.formID, event.features, event.reward);
        }

        m_processing = false;
    }

    size_t Size() const {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }
};
```

### Sync vs Async Decisions

| Operation | Sync/Async | Rationale |
|-----------|------------|-----------|
| Override check | **Sync** | Must be immediate (life/death) |
| Context read | **Sync** | Cheap, needed for decisions |
| Candidate gather | **Sync** | Needed for scoring |
| Utility scoring | **Sync** | Cheap (dot products) |
| UI render | **Sync** | Must be every frame |
| Learning update | **Async** | Can be delayed, no user impact |
| Persistence save | **Async** | Slow I/O, don't block game |
| Inventory scan | **Sync*** | *Can be cached, refresh on change |
| Wheeler update | **Sync** | API call, but fast |

### Dirty Flag Optimization

Only re-run expensive operations when needed:

```cpp
class ContextSensor {
    ContextState m_context;
    uint32_t m_contextHash = 0;
    bool m_dirty = true;

    // Cached results
    std::vector<Candidate> m_cachedCandidates;
    std::vector<SlotRecommendation> m_cachedSlots;

public:
    void Update() {
        auto newContext = ReadGameState();
        uint32_t newHash = HashContext(newContext);

        if (newHash != m_contextHash) {
            m_context = newContext;
            m_contextHash = newHash;
            m_dirty = true;
        }
    }

    bool IsDirty() const { return m_dirty; }

    void RefreshRecommendations() {
        if (!m_dirty) return;

        m_cachedCandidates = GatherCandidates(m_context);
        ScoreCandidates(m_cachedCandidates);
        m_cachedSlots = AllocateSlots(m_cachedCandidates);

        m_dirty = false;
    }

    const std::vector<SlotRecommendation>& GetSlots() const {
        return m_cachedSlots;
    }
};
```

### Per-Slot Update Cooldowns

Each slot manages its own update state independently:

```cpp
struct SlotState {
    RE::FormID currentFormID = 0;
    float lastUpdateTime = 0.0f;
    float cooldownMs = 500.0f;          // Minimum time before slot can change
    bool isOverridden = false;
    bool isWildcard = false;

    bool CanUpdate(float currentTime) const {
        return (currentTime - lastUpdateTime) >= cooldownMs;
    }

    void Update(RE::FormID newFormID, float currentTime) {
        currentFormID = newFormID;
        lastUpdateTime = currentTime;
    }
};

class SlotManager {
    std::vector<SlotState> m_slots;

public:
    void UpdateSlot(int slotIndex, RE::FormID newFormID, float currentTime) {
        auto& slot = m_slots[slotIndex];

        // Respect cooldown - don't flicker
        if (!slot.CanUpdate(currentTime)) return;

        // Only update if actually changed
        if (slot.currentFormID == newFormID) return;

        slot.Update(newFormID, currentTime);
    }
};
```

### Override Hysteresis

Prevent flickering when values hover near thresholds:

```cpp
struct OverrideState {
    bool isActive = false;
    float activationThreshold = 0.25f;   // Trigger when health < 25%
    float deactivationThreshold = 0.35f; // Clear when health > 35%
    float lastTriggerTime = 0.0f;
    float minActiveTimeMs = 2000.0f;     // Stay active for at least 2s

    bool ShouldActivate(float healthPercent) const {
        if (isActive) return true;  // Already active
        return healthPercent < activationThreshold;
    }

    bool ShouldDeactivate(float healthPercent, float currentTime) const {
        if (!isActive) return false;

        // Must stay active for minimum time
        if ((currentTime - lastTriggerTime) < minActiveTimeMs) return false;

        // Hysteresis: need to be ABOVE deactivation threshold to clear
        return healthPercent > deactivationThreshold;
    }
};
```

**Example: Health override at 25%**

```
Health:  30% → 24% → 26% → 24% → 40%
         ↓     ↓     ↓     ↓     ↓
Active:  No   YES   YES   YES   No
                     ↑     ↑
            Hysteresis prevents flicker
```

| Threshold Type | Value | Purpose |
|----------------|-------|---------|
| Activation | 25% | Trigger override |
| Deactivation | 35% | Clear override (10% hysteresis band) |
| Min active time | 2000ms | Don't clear immediately even if healed |

### Wildcard Rate Limiting

Prevent multiple wildcards from triggering simultaneously:

```cpp
struct WildcardState {
    float lastTriggerTime = 0.0f;
    float globalCooldownMs = 30000.0f;  // 30s between ANY wildcard
    int triggersThisSession = 0;
    int maxTriggersPerSession = 10;     // Cap total wildcards

    // Per-slot wildcard tracking
    std::unordered_map<int, float> slotLastWildcard;
    float perSlotCooldownMs = 60000.0f; // 60s per slot

    bool CanTriggerWildcard(int slotIndex, float currentTime) {
        // Global rate limit
        if ((currentTime - lastTriggerTime) < globalCooldownMs) return false;

        // Session limit
        if (triggersThisSession >= maxTriggersPerSession) return false;

        // Per-slot limit
        auto it = slotLastWildcard.find(slotIndex);
        if (it != slotLastWildcard.end()) {
            if ((currentTime - it->second) < perSlotCooldownMs) return false;
        }

        return true;
    }

    void OnWildcardTriggered(int slotIndex, float currentTime) {
        lastTriggerTime = currentTime;
        slotLastWildcard[slotIndex] = currentTime;
        triggersThisSession++;
    }
};
```

**Wildcard constraints:**
- Global cooldown: Only one wildcard every 30s across all slots
- Per-slot cooldown: Same slot can't wildcard again for 60s
- Session limit: Max 10 wildcards per play session
- Probability still applies (30% chance when eligible)

### Slot Update Priority

When multiple slots want to update, process in priority order:

```cpp
enum class UpdatePriority {
    Override = 0,    // Highest - life/death situations
    ContextChange,   // Context meaningfully changed
    Scheduled,       // Regular refresh interval
    Wildcard         // Lowest - exploration picks
};

struct PendingUpdate {
    int slotIndex;
    RE::FormID newFormID;
    UpdatePriority priority;
    float requestTime;
};

class UpdateScheduler {
    std::priority_queue<PendingUpdate> m_pending;
    int m_maxUpdatesPerFrame = 2;  // Don't update all slots at once

public:
    void ProcessUpdates(float currentTime) {
        int processed = 0;

        while (!m_pending.empty() && processed < m_maxUpdatesPerFrame) {
            auto update = m_pending.top();
            m_pending.pop();

            if (m_slots[update.slotIndex].CanUpdate(currentTime)) {
                ApplyUpdate(update);
                processed++;
            }
        }
    }
};
```

**Why stagger updates:**
1. Prevents UI "popping" when all slots change at once
2. Smoother visual transitions
3. Spreads CPU work across frames

### Learning Latency Tolerance

contextual bandit learning updates don't need to be immediate:

```
Timeline:
  T+0ms:    Player equips Fireball (event queued)
  T+0ms:    Recommendation still shows Fireball (current slot unchanged)
  T+500ms:  Learning batch processed
  T+500ms:  Fireball reward estimate updated
  T+800ms:  Next recommendation refresh
  T+800ms:  Updated reward estimate now affects scoring

Result: 800ms delay from action to learning effect
Impact: Negligible - player doesn't notice, recommendations still work
```

**Why this is acceptable:**
1. Player sees immediate feedback (item equipped)
2. Next recommendation will reflect learned preference
3. Learning is for long-term personalization, not instant response
4. Avoids blocking game thread during combat

### Data Structure Choices

**Use Hash Maps (`std::unordered_map`) for:**

| Data | Key | Value | Reason |
|------|-----|-------|--------|
| reward estimate weights | `FormID` | `float[16]` | O(1) lookup, sparse (not all items trained) |
| Training counts | `FormID` | `int` | Same as weights, 1:1 correspondence |
| Effect type cache | `FormID` | `EffectType` | Avoid re-classifying every frame |
| Spell cost cache | `FormID` | `float` | Avoid recalculating magicka cost |
| Active effect lookup | `EffectID` | `bool` | Fast "is player on fire?" check |
| Override state | `OverrideCondition` | `OverrideState` | Few conditions, but need fast lookup |

**Use Arrays/Vectors for:**

| Data | Size | Reason |
|------|------|--------|
| Slot states | 6 (fixed) | Small, contiguous, cache-friendly |
| Candidates per refresh | ~50-200 | Rebuilt each cycle, sort in place |
| Feature vector | 16 floats | Fixed size, stack allocated |
| Override rules | ~10 | Small, rarely changes |

**Use Flat Maps (`std::flat_map`) for:**

| Data | Size | Reason |
|------|------|--------|
| Allowed effects per slot | ~5-10 | Small, sorted, frequent iteration |
| Allowed schools per slot | ~3-5 | Same |

```cpp
// learner: hash map for sparse FormID → weights
class FeatureQLearner {
    std::unordered_map<RE::FormID, std::array<float, 16>> m_weights;
    std::unordered_map<RE::FormID, int> m_trainCount;
};

// Effect cache: avoid re-classifying every frame
class EffectClassifier {
    mutable std::unordered_map<RE::FormID, EffectType> m_cache;

    EffectType GetEffectType(RE::FormID formID) const {
        auto it = m_cache.find(formID);
        if (it != m_cache.end()) return it->second;

        // Expensive classification
        EffectType type = ClassifyByEffect(formID);
        m_cache[formID] = type;
        return type;
    }

    void InvalidateCache() { m_cache.clear(); }  // On game load
};

// Slots: fixed array, not hash map
class SlotManager {
    std::array<SlotState, 6> m_slots;  // Fixed, cache-friendly

    SlotState& GetSlot(int index) {
        return m_slots[index];  // O(1), no hash
    }
};

// Candidates: vector, rebuilt each cycle
class CandidateGenerator {
    std::vector<Candidate> m_candidates;  // Reserve capacity

    void GatherCandidates() {
        m_candidates.clear();
        m_candidates.reserve(200);  // Avoid reallocation
        // ... gather ...
    }
};
```

**Cache Invalidation:**

| Cache | Invalidate When |
|-------|-----------------|
| Effect type cache | Game load (mods may change) |
| Spell cost cache | Game load, perk change |
| Active effects | Never (re-query each poll) |
| Candidate list | Every refresh cycle |

### Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| Frame impact | < 1ms | Total per-frame processing |
| Context poll | < 5ms | Game state reads |
| Full pipeline | < 15ms | Gather + score + allocate |
| Memory overhead | < 5MB | Candidates + reward estimates + cache |
| Queue depth | < 100 | Learning events before flush |
| Hash map lookups | < 0.1ms | Per FormID lookup |
| Effect cache hit rate | > 95% | Most items seen repeatedly |

### Configuration in Settings

```json
{
  "timing": {
    "context_poll_ms": 200,
    "recommendation_refresh_ms": 300,
    "learning_batch_ms": 1000,
    "adaptive_timing": true,
    "combat_speed_multiplier": 0.5,
    "async_learning": true
  },
  "slots": {
    "cooldown_ms": 500,
    "max_updates_per_frame": 2
  },
  "overrides": {
    "hysteresis_band": 0.10,
    "min_active_time_ms": 2000
  },
  "wildcards": {
    "global_cooldown_ms": 30000,
    "per_slot_cooldown_ms": 60000,
    "max_per_session": 10,
    "probability": 0.3
  }
}
```

---

## Logging and Error Handling

**Silent failures are not an option.** All failures must be logged and handled gracefully.

### Logging Principles

1. **Every failure path must log** - If a function can fail, log why it failed
2. **Use appropriate log levels** - `error` for failures, `warn` for recoverable issues, `info` for state changes, `debug` for diagnostics
3. **Include context** - Log FormIDs, indices, counts, and other relevant data
4. **Don't spam** - Use rate limiting for high-frequency operations

### Log Levels

| Level | When to Use | Example |
|-------|-------------|---------|
| `error` | Operation failed, feature degraded | `"Failed to create wheel: {}"` |
| `warn` | Unexpected but recoverable | `"Wheeler API not available, recommendations disabled"` |
| `info` | State changes, initialization | `"Created recommendation wheel at index {}"` |
| `debug` | Diagnostic info (disabled in release) | `"Scored {} spells, top: {} (score: {})"` |
| `trace` | High-frequency diagnostics | `"Context poll: health={}%, magicka={}%"` |

### Error Handling Patterns

```cpp
// BAD: Silent failure
void UpdateWheel() {
    if (!m_api) return;  // Silent - user has no idea why it's not working
}

// GOOD: Log and gracefully degrade
void UpdateWheel() {
    if (!m_api) {
        spdlog::warn("[WheelerClient] API not connected, skipping wheel update");
        return;
    }
}

// BAD: Crash on unexpected state
auto* spell = GetSpell(formID);
spell->GetName();  // Crash if null

// GOOD: Defensive with logging
auto* spell = GetSpell(formID);
if (!spell) {
    spdlog::error("[SpellRegistry] Spell {:08X} not found", formID);
    return;
}
```

### Critical Logging Points

| Operation | Log Level | What to Log |
|-----------|-----------|-------------|
| Plugin initialization | `info` | Version, API connection status |
| API connection failure | `warn` | Which API, why it failed |
| Wheel creation | `info` | Index, entry count, client name |
| Wheel creation failure | `error` | Error code, config values |
| Spell not found | `warn` | FormID (may be mod conflict) |
| Q-table load/save | `info` | Path, entry count |
| Q-table load failure | `error` | Path, error details |
| Invalid state detected | `error` | What was expected vs actual |

### Rate-Limited Logging

For high-frequency operations, use rate limiting to avoid log spam:

```cpp
// Log at most once per second
static auto lastLogTime = std::chrono::steady_clock::now();
auto now = std::chrono::steady_clock::now();
if (std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count() >= 1) {
    spdlog::debug("[Update] Processing {} candidates", candidates.size());
    lastLogTime = now;
}
```

### Graceful Degradation

When a subsystem fails, the rest of Huginn should continue working:

| Failure | Graceful Behavior |
|---------|-------------------|
| Wheeler API unavailable | Recommendations computed but not displayed |
| Q-table load fails | Use default priors, start fresh learning |
| Spell registry empty | Log warning, no recommendations shown |
| Invalid FormID from callback | Log and ignore, don't crash |

---

## Thread Safety

Huginn interacts with multiple threads and must protect shared state appropriately.

### Thread Model

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              THREAD MODEL                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  GAME THREAD (Main)                                                         │
│  ─────────────────                                                          │
│  • SKSE event handlers (OnEquip, OnSpellCast, etc.)                        │
│  • Context polling (read game state)                                        │
│  • Override detection                                                       │
│  • UI rendering                                                             │
│                                                                              │
│  UPDATE THREAD (Background)                                                 │
│  ──────────────────────────                                                 │
│  • Recommendation refresh (full pipeline)                                   │
│  • Wheeler API calls                                                        │
│  • Candidate scoring                                                        │
│                                                                              │
│  LEARNING THREAD (Background)                                               │
│  ────────────────────────────                                               │
│  • reward estimate updates from event queue                                         │
│  • Batch processing                                                         │
│  • Persistence (auto-save)                                                  │
│                                                                              │
│  WHEELER CALLBACK THREAD                                                    │
│  ────────────────────────                                                   │
│  • ItemActivatedCallback                                                    │
│  • WheelStateCallback                                                       │
│  • EditModeCallback                                                         │
│  • May be any thread - treat as foreign                                     │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Synchronization Primitives

```cpp
class HuginnCore {
    // ─────────────────────────────────────────────────────
    // State Protection
    // ─────────────────────────────────────────────────────

    // Context state - read often, write during poll
    mutable std::shared_mutex m_contextMutex;
    ContextState m_context;

    // Slot state - read by UI, write by update thread
    mutable std::shared_mutex m_slotMutex;
    std::array<SlotState, NUM_SLOTS> m_slots;

    // learner - accessed by learning thread and scoring
    mutable std::shared_mutex m_learnerMutex;
    FeatureQLearner m_qLearner;

    // ─────────────────────────────────────────────────────
    // Event Queue Protection
    // ─────────────────────────────────────────────────────

    // Learning queue - producer/consumer pattern
    std::mutex m_queueMutex;
    std::queue<LearningEvent> m_learningQueue;

    // ─────────────────────────────────────────────────────
    // Callback Serialization
    // ─────────────────────────────────────────────────────

    // Wheeler callbacks may fire from any thread
    std::mutex m_callbackMutex;

public:
    // ─────────────────────────────────────────────────────
    // Game Thread Operations
    // ─────────────────────────────────────────────────────

    void PollGameState() {
        ContextState newContext = ReadFromGame();

        std::unique_lock lock(m_contextMutex);
        m_context = newContext;
    }

    bool CheckOverride() {
        std::shared_lock lock(m_contextMutex);
        return m_context.healthPercent < 0.25f;  // etc.
    }

    // ─────────────────────────────────────────────────────
    // Update Thread Operations
    // ─────────────────────────────────────────────────────

    void RefreshRecommendations() {
        ContextState ctx;
        {
            std::shared_lock lock(m_contextMutex);
            ctx = m_context;  // Copy for use outside lock
        }

        auto candidates = GatherCandidates(ctx);

        {
            std::shared_lock lock(m_learnerMutex);
            ScoreCandidates(candidates, ctx, m_qLearner);
        }

        auto newSlots = AllocateSlots(candidates);

        {
            std::unique_lock lock(m_slotMutex);
            UpdateSlotsIfAllowed(newSlots);
        }
    }

    // ─────────────────────────────────────────────────────
    // Wheeler Callback Handlers
    // ─────────────────────────────────────────────────────

    void OnItemActivated(int32_t wheelId, int32_t entryId,
                         int32_t itemId, uint32_t formId, bool wasEquipped) {
        std::lock_guard lock(m_callbackMutex);

        // Push to queue - don't block callback thread
        LearningEvent event{
            .type = LearningEvent::Type::Equip,
            .formID = formId,
            .features = GetCurrentFeatures(),  // Uses shared_lock internally
            .reward = EQUIP_REWARD,
        };

        {
            std::lock_guard qLock(m_queueMutex);
            m_learningQueue.push(std::move(event));
        }
    }

    // ─────────────────────────────────────────────────────
    // Learning Thread Operations
    // ─────────────────────────────────────────────────────

    void ProcessLearningBatch(int maxEvents) {
        std::vector<LearningEvent> batch;

        {
            std::lock_guard lock(m_queueMutex);
            while (!m_learningQueue.empty() && batch.size() < maxEvents) {
                batch.push_back(std::move(m_learningQueue.front()));
                m_learningQueue.pop();
            }
        }

        if (!batch.empty()) {
            std::unique_lock lock(m_learnerMutex);
            for (const auto& event : batch) {
                m_qLearner.Update(event.formID, event.features, event.reward);
            }
        }
    }
};
```

### Lock Ordering

To prevent deadlocks, always acquire locks in this order:

1. `m_callbackMutex` (outermost)
2. `m_contextMutex`
3. `m_slotMutex`
4. `m_learnerMutex`
5. `m_queueMutex` (innermost)

### Thread Safety Summary

| Resource | Mutex Type | Readers | Writers |
|----------|------------|---------|---------|
| ContextState | `shared_mutex` | Override check, UI, scoring | Context poll |
| SlotState | `shared_mutex` | UI render, Wheeler sync | Recommendation refresh |
| FeatureQLearner | `shared_mutex` | Scoring | Learning batch |
| LearningQueue | `mutex` | - | Push (events), Pop (batch) |
| Wheeler callbacks | `mutex` | - | Callback handlers |

### Wheeler Thread Safety

Wheeler is thread-safe internally (uses `shared_mutex`). Huginn can call Wheeler API from any thread. See `src/wheeler/WheelerAPI.h` and the thread-safety notes in CLAUDE.md; there is no Wheeler integration doc yet.

---

## Known Limitations & Edge Cases

### Design Trade-offs

#### Temporal Prediction Lag

**Issue:** EMA velocity smoothing (α=0.3) introduces ~300ms lag.

**Trade-off:**
- **Smoothing benefits:** Filters combat jitter, prevents overreaction to noise
- **Lag downside:** Slow to react to dragon breath, combat start

**Resolution:** Acceptable for gradual health drain. Regime change detection (combat start → reset predictor) documented in Future Work section.

#### Wildcard Recommendations

**Issue:** Exploration picks may show random spells during intense combat.

**Trade-off:**
- **Benefits:** Helps discover underused spells, prevents reward estimate stagnation
- **Annoyance:** Random spells during boss fights

**Mitigation:** Context-aware wildcards (disable when health < 30%) and user-configurable wildcard rate.

#### State Representation Granularity

**Issue:** Bucketing thresholds (e.g., 25% vs 26% health) create discontinuities.

**Trade-off:**
- **Buckets benefit:** Reduces state space, enabled the tabular QLearner (removed in v0.13.x)
- **Discontinuity problem:** Similar states treated as completely different

**Resolution:** Hysteresis bands (25% on, 35% off) reduce oscillation at boundaries. Feature-based learning (see Future Work) would eliminate need for bucketing.

### User Experience Considerations

#### Recommendation Flickering

**Issue:** Rapid context changes (health 34% → 36% → 34%) cause UI flicker.

**Mitigation (current):**
- Hysteresis bands (25% on, 35% off)
- Slot cooldowns (500ms minimum)

**Additional improvements:**
- Wider hysteresis (25% on, 45% off)
- Visual smoothing (fade transitions)
- Sticky recommendations (persist until explicitly replaced)

#### Mod Spell Compatibility

**Issue:** Large spell packs (200+ spells) with no prior reward estimates cause confusion.

**Considerations:**
- Prior system may misclassify mod spell effects
- contextual bandit learning cold start for all new spells

**Improvements:**
1. Better effect keyword detection for auto-classification
2. Metadata learning (track usage patterns)
3. User curation (mark "never recommend")

### Edge Cases

#### Fast Travel / Loading Screens

**Handling:** Reset context sensors, clear learning queue on cell change.

**Implementation:** Requires SKSE event hooks for `kPostLoadGame` and cell transition events.

#### Player Death

**Handling:** Should recent recommendations be penalized?

**Consideration:** Death attribution is complex (many factors contribute). Currently does not penalize spells on death. Future versions may implement optional death penalty with configurable lookback window.

#### Mod Load Order Changes

**Issue:** FormIDs may change between sessions if mod load order changes.

**Mitigation:** Per-save Q-table includes plugin name + local FormID for resilience.

**Edge case:** If mod is removed or replaced, reward estimates become orphaned and are ignored.

#### Memory Growth (Tabular Contextual Bandit Learning)

**Issue:** Q-table grows as O(states × items) with tabular learning.

**Monitoring:**
- Track Q-table size over long play sessions
- Log memory usage in debug builds

**Mitigation:**
1. Sparse storage (only non-zero values) - Implemented
2. LRU eviction policy - Planned
3. Visit-count thresholding (remove rarely-visited state-action pairs) - Planned

---

## Related Documentation

| Document | Description |
|----------|-------------|
| [learning/qlearning.md](../architecture/4-contextual-bandits.md) | Feature-based learning design (target for v2.0) |
| [architecture/pipeline.md](../architecture/0-pipeline.md) | Detailed data flow: context → candidates → scoring → slots |
| [architecture/slots.md](../architecture/5-slots.md) | Slot classification, overrides, wildcards, Wheeler integration |
| _(not written)_ | Wheeler API contract and thread safety — see `src/wheeler/WheelerAPI.h` |

---

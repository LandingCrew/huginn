# Huginn Learning System

> **Implementation Status (v0.13.x):** **FeatureQLearner** is the sole learning system (linear function approximation, 18-float state vectors). The tabular QLearner has been fully removed. See `src/learning/FeatureQLearner.h` for the implementation.

This document details the feature-based reward learning architecture for learning player preferences.

> **Terminology:** Huginn runs a **contextual bandit**, not Q-learning. The
> distinction is the update target:
>
> | | Target | Models the future? |
> |---|---|---|
> | Q-learning | `Q(s,a) <- r + gamma * max Q(s', a')` | Yes — bootstraps off the next state |
> | Contextual bandit | `r_hat(context, arm) <- r` | No — immediate reward only |
>
> `FeatureQLearner::Update` computes `error = reward - prediction` and takes a
> semi-gradient step on it. There is no `gamma`, no `s'`, no trajectory: each
> item is an arm, the 18-float feature vector is the context, and the target is
> the reward observed for that one decision. Nothing in Huginn models what state
> the player transitions into after using an item, which is the whole content of
> the Q-learning update.
>
> **The class names do not match the algorithm.** `FeatureQLearner`,
> `QLearnerSerializer`, the `FQLW` cosave record and `hg reset qvalues` all keep
> the historical name — renaming them would break the cosave format and a
> documented console command. Read "QLearner" in an identifier as "the learner";
> the docs use bandit vocabulary because that is what the code does.

---

## Overview

Huginn uses **feature-based reward learning** (linear function approximation) instead of the tabular QLearner it replaced. This provides:

1. **Generalization** - Similar states behave similarly
2. **No cold start** - New states work immediately
3. **Smaller memory** - Weights per item, not per state x item
4. **Interpretable** - Can inspect learned weights

---

## Why Feature-Based? (Tabular Deprecation Rationale)

### Problem with the Tabular QLearner

The tabular QLearner (v0.6-v0.13) mapped `(discrete_state_hash, FormID) -> reward estimate`. This approach had five fundamental problems that couldn't be fixed without changing the representation:

**1. State sparsity -- most states are never visited.**
Even after aggressive dimensionalization reduction (v0.13.0), the Q-table has 36,288 states -- but a typical play session visits <0.07% of them. Learning "heal at 30% HP in combat with undead" taught the system nothing about "heal at 31% HP in combat with undead" because those are different hash buckets.

```
Original:  6x6x6x3x6x4x3x2x2x2 = 435,456 states (too many, <0.07% visited)
Reduced:   6x6x3x7x4x3x2x2     =  36,288 states (12x smaller, stamina excluded, allies collapsed)
With more context: would still explode multiplicatively
```

**2. No generalization -- learning doesn't transfer between similar states.**
A player who always equips Resist Fire when taking fire damage would need to re-learn this preference independently for every combination of health level x distance x target type x combat status. In practice, players don't have enough play sessions to fill out these combinations.

**3. Bucket boundary artifacts.**
Health discretized into 6 buckets means 49% HP and 51% HP can fall in different states, creating discontinuous reward estimates at bucket boundaries. Players experience this as inconsistent recommendations that "flicker" near thresholds.

**4. Can't add features without exponential blowup.**
Adding any new context dimension (e.g., "has ward up", "poison resistance") multiplies the state space. The tabular approach structurally prevents enriching the context model.

**5. Storage scales with states x items, not just items.**
Each (state, item) pair requires its own Q-entry. With ~400 tracked items across 36K states, the theoretical table is 14.5M entries. In practice, most are never visited and thus zero, but the sparse data means the entries that do exist are low-confidence.

### Solution: Feature-Based (Phase 3.5)

Instead of `Q[state_hash][item]`, we use:

```
r_hat(context, item) = weights[item] . context
```

Each item has a weight vector (18 floats). The reward estimate is the dot product of weights and the current state feature vector. This solves all five problems:

| Problem | Tabular | Feature-Based |
|---------|---------|---------------|
| State sparsity | 36,288 discrete states, <0.07% visited | Continuous -- every state is reachable |
| Generalization | None -- each bucket is independent | Shared weights -- similar states produce similar reward estimates |
| Bucket artifacts | Discontinuous at bucket boundaries | Smooth -- the estimate changes continuously with health% |
| Adding features | Exponential blowup (x new dimension) | Linear -- just add a column to the feature vector |
| Storage | O(states x items) | O(items x 18) -- 72 bytes per item regardless of state count |

---

## StateFeatures

Normalized feature vector extracted from state models (WorldState, PlayerActorState, TargetCollection).

```
+------------------------------------------------------------------------------+
|                         STATE FEATURES VECTOR (18 floats)                    |
+------------------------------------------------------------------------------+
|                                                                              |
|  RESOURCES (continuous 0.0-1.0)                                              |
|  +--------+--------+--------+                                                |
|  | [0]    | [1]    | [2]    |                                                |
|  |health% |magicka%|stamina%|                                                |
|  | 0.45   | 0.80   | 0.60   |  <- Example: 45% HP, 80% MP, 60% SP           |
|  +--------+--------+--------+                                                |
|                                                                              |
|  COMBAT STATE (binary 0/1 + normalized distance)                             |
|  +--------+--------+--------+                                                |
|  | [3]    | [4]    | [5]    |                                                |
|  |inCombat|sneaking|distNorm|  <- distNorm: 0=melee, 0.5=mid, 1=far         |
|  | 1.0    | 0.0    | 0.75   |  <- Example: fighting at range                |
|  +--------+--------+--------+                                                |
|                                                                              |
|  TARGET TYPE (one-hot: exactly one is 1.0, others 0.0)                       |
|  +--------+--------+--------+--------+--------+--------+--------+            |
|  | [6]    | [7]    | [8]    | [9]    | [10]   | [11]   | [12]   |            |
|  | None   |Humanoid| Undead | Beast  |Construc| Dragon | Daedra |            |
|  | 0.0    | 0.0    | 1.0    | 0.0    | 0.0    | 0.0    | 0.0    |  <- Undead |
|  +--------+--------+--------+--------+--------+--------+--------+            |
|                                                                              |
|  EQUIPMENT (binary 0/1, can have multiple)                                   |
|  +--------+--------+--------+--------+                                       |
|  | [13]   | [14]   | [15]   | [16]   |                                       |
|  | melee  |  bow   | spell  | shield |                                       |
|  | 0.0    | 0.0    | 1.0    | 0.0    |  <- Spell equipped                    |
|  +--------+--------+--------+--------+                                       |
|                                                                              |
|  BIAS (always 1.0)                                                           |
|  +--------+                                                                  |
|  | [17]   |                                                                  |
|  | bias   |  <- Captures item's baseline preference                          |
|  | 1.0    |                                                                  |
|  +--------+                                                                  |
|                                                                              |
+------------------------------------------------------------------------------+
```

```cpp
struct StateFeatures {
    // Resources (0-1)
    float healthPct;
    float magickaPct;
    float staminaPct;

    // Combat (binary 0/1)
    float inCombat;
    float isSneaking;
    float distanceNorm;     // 0=melee, 0.5=mid, 1=ranged

    // Target type (one-hot encoding)
    float targetNone;       // 1 if no target
    float targetHumanoid;
    float targetUndead;
    float targetBeast;
    float targetConstruct;
    float targetDragon;     // Dragons are special - breath attacks, flight
    float targetDaedra;     // Atronachs, Dremora - affected by anti-daedra magic

    // Equipment (binary 0/1)
    float hasMeleeEquipped;
    float hasBowEquipped;
    float hasSpellEquipped;
    float hasShieldEquipped;

    float bias;             // Always 1.0

    // Total: 18 features
};
```

### Feature Extraction

```cpp
StateFeatures StateFeatures::FromState(const PlayerActorState& player,
                                       const TargetCollection& targets) {
    StateFeatures f;

    // Resources (from PlayerActorState.vitals)
    f.healthPct = std::clamp(player.vitals.health, 0.0f, 1.0f);
    f.magickaPct = std::clamp(player.vitals.magicka, 0.0f, 1.0f);
    f.staminaPct = std::clamp(player.vitals.stamina, 0.0f, 1.0f);

    // Combat (from PlayerActorState top-level booleans)
    f.inCombat = player.isInCombat ? 1.0f : 0.0f;
    f.isSneaking = player.isSneaking ? 1.0f : 0.0f;

    // Distance (from TargetCollection closest enemy)
    // GetClosestEnemy() returns std::optional<TargetActorState>
    // 4096 units ~ 56 meters - covers archery and destruction spell range
    auto closestEnemy = targets.GetClosestEnemy();
    if (closestEnemy.has_value()) {
        float dist = std::sqrt(closestEnemy->distanceToPlayerSq);
        f.distanceNorm = std::clamp(dist / 4096.0f, 0.0f, 1.0f);
    } else {
        f.distanceNorm = 1.0f;  // No enemy -> max range
    }

    // Target type (one-hot, from TargetCollection.primary — std::optional)
    TargetType type = TargetType::None;
    if (targets.primary.has_value()) {
        type = targets.primary->targetType;
    }
    // ... switch statement sets one-hot encoding ...

    // Equipment (from PlayerActorState top-level booleans)
    f.hasMeleeEquipped  = player.hasMeleeEquipped  ? 1.0f : 0.0f;
    f.hasBowEquipped    = player.hasBowEquipped    ? 1.0f : 0.0f;
    f.hasSpellEquipped  = player.hasSpellEquipped  ? 1.0f : 0.0f;
    f.hasShieldEquipped = player.hasShieldEquipped ? 1.0f : 0.0f;

    f.bias = 1.0f;

    return f;
}
```

---

## FeatureQLearner

Contextual bandit with a linear reward model — one weight vector per item (arm), scored against the 18-float context.

```cpp
class FeatureQLearner {
    static constexpr size_t NUM_FEATURES = 18;

    // Per-item weight vectors
    std::unordered_map<RE::FormID, std::array<float, NUM_FEATURES>> m_weights;

    // Per-item training count (for confidence + UCB)
    std::unordered_map<RE::FormID, uint32_t> m_trainCount;

    // Per-item last-update timestamp (for time-based decay)
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> m_lastUpdateTime;

    // Global train count (for UCB calculation)
    uint32_t m_totalTrainCount = 0;

    // Hyperparameters
    static constexpr float LEARNING_RATE = 0.1f;
    static constexpr float L2_LAMBDA = 0.01f;
    static constexpr float WEIGHT_CLAMP = 10.0f;
    static constexpr float CONFIDENCE_MIDPOINT = 5.0f;
    static constexpr float CONFIDENCE_STEEPNESS = 0.3f;
    static constexpr float UCB_NORMALIZATION_FACTOR = 0.2f;

public:
    // Get reward estimate for item given features
    float GetQValue(RE::FormID item, const StateFeatures& features) const {
        auto it = m_weights.find(item);
        if (it == m_weights.end()) {
            return 0.0f;  // No training data
        }

        auto phi = features.ToArray();
        float q = 0.0f;
        for (size_t i = 0; i < NUM_FEATURES; ++i) {
            q += it->second[i] * phi[i];
        }
        return q;
    }

    // Update weights based on reward
    void Update(RE::FormID item, const StateFeatures& features, float reward) {
        auto& w = m_weights[item];
        auto phi = features.ToArray();

        float prediction = GetQValue(item, features);
        float error = reward - prediction;

        // Gradient descent with L2 regularization
        // w <- w + alpha(r - w.phi)phi - alpha*lambda*w
        for (size_t i = 0; i < NUM_FEATURES; ++i) {
            w[i] += LEARNING_RATE * (error * phi[i] - L2_LAMBDA * w[i]);
            w[i] = std::clamp(w[i], -WEIGHT_CLAMP, WEIGHT_CLAMP);
        }

        m_trainCount[item]++;
        m_totalTrainCount++;
        m_lastUpdateTime[item] = std::chrono::steady_clock::now();
    }

    // Lazy time-based decay: apply exponential weight decay to items idle > threshold.
    // Called per-candidate before scoring (not a global sweep).
    bool MaybeDecay(RE::FormID formID);

    // Get confidence (0-1) based on training count
    float GetConfidence(RE::FormID item) const {
        auto it = m_trainCount.find(item);
        if (it == m_trainCount.end()) return 0.0f;

        // Sigmoid centered at CONFIDENCE_MIDPOINT:
        //   0 visits -> ~18% confidence
        //   5 visits -> 50% confidence
        //  10 visits -> ~82% confidence
        //  15 visits -> ~95% confidence
        float x = static_cast<float>(it->second);
        return 1.0f / (1.0f + std::exp(-CONFIDENCE_STEEPNESS * (x - CONFIDENCE_MIDPOINT)));
    }

    // Serialization
    struct SerializedEntry {
        RE::FormID formID;
        std::array<float, NUM_FEATURES> weights;
        uint32_t trainCount;
        uint32_t minutesSinceLastUpdate = 0;  // v2: relative to save time
    };
    void ExportData(std::function<void(SerializedEntry)> entryCallback,
                    uint32_t& outTotalTrainCount) const;
    void ImportData(const std::vector<SerializedEntry>& entries,
                    uint32_t totalTrainCount);
};
```

---

## EquipEventBus Architecture

All equip-related learning signals flow through the **EquipEventBus** (Observer pattern). Publishers call `Publish()` with raw parameters; the bus evaluates state once and dispatches an `EquipEvent` to all subscribers.

### Event Flow

```
Equip Sources (publishers):              Subscribers:
+---------------------------+            +----------------------------+
| WheelerClient             |---+        | FQLSubscriber              |
| EquipManager (hotkeys)    |   |        |   -> FeatureQLearner       |
| ExternalEquipLearner      |   |  Pub   +----------------------------+
| ConsumptionDetector       |---+------->| UsageMemorySubscriber      |
+---------------------------+   |        |   -> UsageMemory + misclick|
                                |        +----------------------------+
                            EquipEventBus| CooldownSubscriber         |
                            (evaluates   |   -> CandidateGenerator    |
                             state once) +----------------------------+
```

### EquipEvent

```cpp
struct EquipEvent {
    RE::FormID      formID;
    EquipSource     source;           // Hotkey, Wheeler, External, Consumption
    float           rewardMultiplier; // Scaling factor (External uses attribution)
    bool            wasRecommended;   // Hotkey: was on widget, Wheeler: always true,
                                      // External/Consumption: always false
    StateFeatures   features;         // Pre-computed (evaluated once per event)
    GameState       gameState;        // Pre-computed (for UsageMemory context)
};
```

### Subscribers

| Subscriber | Fires For | Action |
|------------|-----------|--------|
| **FQLSubscriber** | Hotkey/Wheeler (if recommended), External, Consumption | `FQL.Update(formID, features, reward)` |
| **UsageMemorySubscriber** | All sources | Records usage for recency boost, detects misclicks |
| **CooldownSubscriber** | Consumption only | Starts candidate cooldown timer |

### Reward Calculation (FQLSubscriber)

```cpp
// Source-dependent reward:
switch (event.source) {
    case Hotkey/Wheeler:
        if (!event.wasRecommended) return;  // No reward for non-recommended
        reward = EQUIP_REWARD * event.rewardMultiplier;  // 8.0 * 1.0 = 8.0
        break;
    case External:
        reward = EQUIP_REWARD * event.rewardMultiplier;  // 8.0 * attribution
        break;
    case Consumption:
        reward = CONSUME_REWARD * event.rewardMultiplier; // 5.0 * 1.0 = 5.0
        break;
}
```

---

## Learning Update

### Learning Signal Sources

> **Design Principle (v0.13.0+):** Learning is decoupled from the presentation layer (Wheeler/Widget). The system learns exclusively from equip events via the EquipEventBus. Negative signals come from time-based weight decay and misclick detection, not from Wheeler open/close events.

**Equip reward** is the primary explicit learning signal. It fires when the player equips an item via:
1. Wheeler radial menu selection (`OnItemActivated` callback) -> +8.0 reward
2. Huginn hotkeys 1-0 (`EquipManager` callback) -> +8.0 reward
3. **External equips** (vanilla menu, favorites, hotkeys 1-8) -> tiered reward via `ExternalEquipLearner` (Phase 3b)
4. **Consumption** (potion/scroll count delta detected) -> +5.0 reward

### External Equip Attribution

`ExternalEquipLearner` uses `PipelineStateCache` to determine what the pipeline "thought" about the item at equip time, and applies a scaled multiplier on `EQUIP_REWARD` (8.0):

```
Case A: Not a candidate        -> 1.0x = +8.0  (STRONGEST: player went out of their way)
Case B-low: Low rank (far miss) -> 0.2x = +1.6  (low rank, scoring disagrees)
Case B-med: Mid rank            -> 0.4x = +3.2  (moderate preference signal)
Case C: Near-miss (not shown)   -> 0.8x = +6.4  (near the display cutoff)
Case D: Shown on other page     -> 0.5x = +4.0  (multi-page UX issue)
Case E: Shown on current page   -> 0.0x = +0.0  (Huginn already surfaced it — skip)
```

**Rank thresholds** (slot-relative):
- "Near-miss" (Case C): rank within `NEAR_MISS_SLOTS` (2) of the display cutoff
- "Mid rank" (Case B-med): rank within `FAR_MISS_SLOTS` (5) of the display cutoff
- "Low rank" (Case B-low): rank beyond `FAR_MISS_SLOTS` (5) past the cutoff

**Anti-spam filters** for external equips:
1. **Master toggle** (`bLearnFromExternalEquips` in `[Learning]`)
2. **Cache staleness** — pipeline data older than `fExternalEquipTimeWindow` (2000ms default)
3. **Wheeler-open filter** — skip when wheel is open (player may be mid-selection via Huginn)
4. **Anti-spam timer** — same FormID within `fExternalEquipMinInterval` (30s testing, 3s production)

Double-reward prevention via `EquipSourceTracker` (prevents learning from an Huginn equip that also fires an external equip event).

### Weight Decay

FeatureQLearner uses **two complementary decay mechanisms**:

**1. L2 Regularization (on update):**
During each weight update, the term `w[i] -= alpha * lambda * w[i]` pulls weights toward zero. Items that are frequently equipped outpace this pull; items that stop receiving updates retain their last-trained weights.

**2. Lazy Time-Based Decay (per scoring pass):**
`MaybeDecay(formID)` is called for all candidates before scoring. Items idle longer than `DECAY_THRESHOLD_MINUTES` (5 min) have their weights exponentially decayed at `DECAY_RATE_PER_HOUR` (2%/hr). This is lazy — it only fires for items about to be scored, not a global sweep.

```cpp
// Config.h
inline constexpr float DECAY_RATE_PER_HOUR = 0.02f;        // 2%/hr exponential decay
inline constexpr float DECAY_THRESHOLD_MINUTES = 5.0f;      // Don't decay within 5 min
```

**Why skip penalties were removed (v0.12.x):**
- Skip penalties punished correct recommendations during state transitions (e.g. combat ending while wheel was open)
- All 10 visible items received identical -1.0 regardless of rank
- L2 regularization + time-based decay provide the same "drift toward zero" signal without these failure modes

### Misclick Detection

`UsageMemorySubscriber` detects rapid equip-then-switch patterns via `UsageMemory::RecordUsage()`. If a different item is equipped within `MISCLICK_WINDOW_SECONDS` (3s) of the previous equip, the **previous** item receives `MISCLICK_PENALTY` (-3.0) — approximately 37.5% of `EQUIP_REWARD`.

### Wheeler Callback Integration

Huginn receives feedback through Wheeler's callback system:

```cpp
// Registered with Wheeler API
m_api->RegisterItemActivatedCallback(&OnItemActivated);
m_api->RegisterWheelStateCallback(&OnWheelStateChanged);
```

**ItemActivatedCallback:** Fired when player selects an item from the wheel
- Publishes to EquipEventBus with `EquipSource::Wheeler` and `wasRecommended=true`

**WheelStateCallback:** Fired when wheel opens/closes
- Used for Wheeler UX behavior (auto-focus, scroll tracking)
- No longer applies contextual bandit learning penalties on close

**Note:** Wheeler v2 is required for reliable callback support. See [MOD_COMPATIBILITY.md](MOD_COMPATIBILITY.md#wheeler-integration) for version differences.

---

## Learning Signals

| Signal | Value | Source | Purpose |
|--------|-------|--------|---------|
| Equip reward | +8.0 | Wheeler selection / hotkey 1-0 | Positive reinforcement for Huginn-mediated equips |
| Consume reward | +5.0 | Potion/scroll consumption | Signal for finite resources (weaker than equip) |
| External equip reward | 0.0 to +8.0 | Vanilla menu / favorites / hotkey 1-8 | Tiered reward via pipeline attribution (multiplier x EQUIP_REWARD) |
| Misclick penalty | -3.0 | Rapid equip-then-switch (<3s) | Penalizes discarded item |
| L2 regularization | Continuous | Applied during each weight update | Pulls weights toward zero |
| Time-based decay | Lazy | MaybeDecay before scoring | 2%/hr exponential decay on idle items |

**Removed signals:** Skip penalty (-1.0) removed in v0.12.x (replaced by implicit decay via L2 regularization + time-based decay).

### Feedback Loop

```
+-----------------------------------------------------------------------------+
|                          Q-LEARNING FEEDBACK LOOP                            |
+-----------------------------------------------------------------------------+
|                                                                              |
|                    +-------------------------+                               |
|                    |     GAME STATE          |                               |
|                    |  (health, combat, etc)  |                               |
|                    +-----------+-------------+                               |
|                                |                                             |
|                                v                                             |
|                    +-------------------------+                               |
|                    |    StateFeatures        |                               |
|                    |    (18-float vector)    |                               |
|                    +-----------+-------------+                               |
|                                |                                             |
|            +-------------------+-------------------+                         |
|            |                   |                   |                         |
|            v                   v                   v                         |
|   +----------------+  +----------------+  +----------------+                |
|   | Context Weight |  |   Q-Values     |  |  Exploration   |                |
|   |  (heuristic)   |  |  (per-item)    |  |    (UCB)       |                |
|   +-------+--------+  +-------+--------+  +-------+--------+                |
|           |                   |                   |                          |
|           |              +----+----+              |                          |
|           |              | Decay:  |              |                          |
|           |              | L2 reg  |              |                          |
|           |              | + time  |              |                          |
|           |              +----+----+              |                          |
|           |                   |                   |                          |
|           +-------------------+-------------------+                          |
|                               v                                              |
|                    +-------------------------+                               |
|                    |   UTILITY SCORING       |                               |
|                    | ctx*(1+lambda*learn)    |                               |
|                    |  *corr*pot*fav          |                               |
|                    +-----------+-------------+                               |
|                                |                                             |
|                                v                                             |
|                    +-------------------------+                               |
|                    |   RECOMMENDATIONS       |                               |
|                    |   (shown in widget/     |                               |
|                    |    wheel)               |                               |
|                    +-----------+-------------+                               |
|                                |                                             |
|                                v                                             |
|                    +-------------------------+                               |
|                    |   PLAYER ACTION         |                               |
|                    |   (equip or ignore)     |                               |
|                    +-----------+-------------+                               |
|                                |                                             |
|                    +-----------+-----------+                                  |
|                    |                       |                                  |
|                    v                       v                                  |
|             +-------------+        +-------------+                           |
|             |   EQUIP     |        |   IGNORE    |                           |
|             |   +8.0      |        |  (time decay|                           |
|             |  (via Bus)  |        |   handles)  |                           |
|             +------+------+        +-------------+                           |
|                    |                                                          |
|                    v                                                          |
|          +-------------------------+                                         |
|          |   EQUIP EVENT BUS       |                                         |
|          |  -> FQL reward          |                                         |
|          |  -> Usage memory        |                                         |
|          |  -> Misclick detect     |                                         |
|          +-----------+-------------+                                         |
|                      |                                                       |
|                      v                                                       |
|          +-------------------------+                                         |
|          |   WEIGHT UPDATE         |                                         |
|          | w += alpha(r - w.phi)phi |                                         |
|          |     - alpha*lambda*w    |                                         |
|          +-------------------------+                                         |
|                                                                              |
+-----------------------------------------------------------------------------+
```

---

## Interpreting Weights

The learned weights are interpretable:

```cpp
void LogWeights(RE::FormID item) {
    auto& w = m_weights[item];

    spdlog::info("Weights for {:08X}:", item);
    spdlog::info("  healthPct:      {:.2f}", w[0]);
    spdlog::info("  magickaPct:     {:.2f}", w[1]);
    spdlog::info("  staminaPct:     {:.2f}", w[2]);
    spdlog::info("  inCombat:       {:.2f}", w[3]);
    spdlog::info("  isSneaking:     {:.2f}", w[4]);
    spdlog::info("  distanceNorm:   {:.2f}", w[5]);
    spdlog::info("  targetNone:     {:.2f}", w[6]);
    spdlog::info("  targetHumanoid: {:.2f}", w[7]);
    spdlog::info("  targetUndead:   {:.2f}", w[8]);
    spdlog::info("  targetBeast:    {:.2f}", w[9]);
    spdlog::info("  targetConstruct:{:.2f}", w[10]);
    spdlog::info("  targetDragon:   {:.2f}", w[11]);
    spdlog::info("  targetDaedra:   {:.2f}", w[12]);
    spdlog::info("  hasMelee:       {:.2f}", w[13]);
    spdlog::info("  hasBow:         {:.2f}", w[14]);
    spdlog::info("  hasSpell:       {:.2f}", w[15]);
    spdlog::info("  hasShield:      {:.2f}", w[16]);
    spdlog::info("  bias:           {:.2f}", w[17]);
}
```

Example interpretation:
```
Fireball weights:
  inCombat:       2.1    -> "Used in combat"
  distanceNorm:   1.5    -> "Used at range"
  targetUndead:  -0.5    -> "Not used against undead"
  bias:           0.3    -> "Generally useful"
```

---

## Utility Calculation

Final utility combines context weights (heuristic), reward estimates (learned), recency boost, and favorites (manual preference):

```cpp
float GetUtility(const Candidate& item,
                 const GameState& gameState,
                 const PlayerActorState& playerState,
                 const StateFeatures& features) {

    // Step 1: Heuristic relevance (rules)
    float contextWeight = GetContextWeight(item, contextWeightMap);

    // Step 2: Get learning components from FeatureQLearner
    auto metrics = m_featureLearner.GetMetrics(item.formID, features);
    // metrics.qValue, metrics.ucb, metrics.confidence

    // Step 3: Calculate prior from PriorCalculator
    float prior = m_priorCalc.CalculatePrior(state, player, candidate);

    // Step 4: Compute learning score: alpha*Q + (1-alpha)*prior + beta*UCB
    float alpha = metrics.confidence;
    float beta = m_config.explorationWeight;
    float learningScore = alpha * metrics.qValue +
                          (1.0f - alpha) * prior +
                          beta * metrics.ucb;

    // Step 4b: Recency boost from UsageMemory (event-driven short-term recall)
    // Additive to learningScore -- context weight still gates final utility.
    learningScore += m_usageMemory.GetRecencyBoost(formID, state);

    // Step 5-6: Correlation and potion multipliers
    float correlationBonus = GetCorrelationBonus(item, playerState);
    float potionMultiplier = GetPotionMultiplier(item, gameState);

    // Step 7: Favorites multiplier (separate from contextual bandit learning)
    // NOTE: Currently called with rank=0, totalItems=1 (no post-sort recalculation)
    float favoritesMultiplier = GetFavoritesMultiplier(item, 0, 1);

    // Step 8: Compute adaptive lambda and final utility
    float lambda = ComputeAdaptiveLambda(metrics.confidence);
    // lambda(confidence) = lambdaMin + confidence * (lambdaMax - lambdaMin)
    // At confidence=0: lambda = 0.5 (context dominates)
    // At confidence=1: lambda = 3.0 (learning amplified 6x)

    return contextWeight * (1.0f + lambda * learningScore)
           * correlationBonus * potionMultiplier * favoritesMultiplier;
}
```

---

## Cold-Start UCB Fallback (Phase 1.5)

With the multiplicative formula, items with no context signal only receive `baseRelevanceWeight = 0.05`. Even with maximum UCB exploration bonus and prior, the final utility caps below `fMinimumUtility = 0.1` -- resulting in empty slots on all-Regular pages despite hundreds of items in registries.

**Solution:** When fewer candidates pass the normal scoring path than `topNCandidates`, a fallback pass runs with UCB-boosted context weights:

```cpp
effectiveContext = max(contextWeight, coldStartUCBBoost * UCB)
```

- Untried items get UCB ~ 1.0 -> `effectiveContext ~ 0.2` (enough to pass minimum utility)
- As items accumulate visits, UCB drops -> fallback self-heals
- Empty Regular slots show "(Learning...)" in both IntuitionMenu and Wheeler
- Configured via `fColdStartUCBBoost = 0.2` in `[Scoring]` section (0.0 disables)
- `isColdStartBoosted` diagnostic flag on `ScoredCandidate` for debug display

---

## Favorites Multiplier

The favorites system is **separate from contextual bandit learning** -- it applies a post-processing multiplier to utility scores based on whether the player has marked an item as "favorited" in Skyrim's favorites menu.

### Architectural Separation

```
+-------------------------------------------------------------+
|  Q-LEARNING (learns from actions)                            |
|  v                                                           |
|  learningScore = alpha*Q + (1-alpha)*prior + beta*UCB        |
|                  + recencyBoost                              |
|                                                              |
|  FAVORITES (manual preference override)                      |
|  v                                                           |
|  favoritesMultiplier = 1.0 (non-favorited)                   |
|                     or 1.3-2.5 (favorited, rank-based)       |
|                                                              |
|  COMBINED                                                    |
|  v                                                           |
|  utility = contextWeight * (1 + lambda*learningScore)        |
|            * correlationBonus * potionMultiplier              |
|            * favoritesMultiplier  <-- Applied at the end     |
+-------------------------------------------------------------+
```

**Key point:** Favoriting an item does NOT affect learning weights. It only boosts the final utility score, making favorited items more likely to appear in recommendations regardless of learned preferences.

**Implementation note:** The favorites multiplier is currently always computed with `rank=0, totalItems=1` (i.e., all favorited items get the maximum 2.5x boost). Post-sort rank-based recalculation is not yet implemented.

### Three Modes

Configured via `[Favorites]` section in `Huginn.ini`:

| Mode | Behavior | Use Case |
|------|----------|----------|
| **Boost** (default) | Favorited items get 1.3x to 2.5x multiplier | Surface preferred items alongside learned recommendations |
| **Off** | Favorites flag is ignored | Pure learning-driven recommendations |
| **Suppress** | Favorited items are excluded | "I already have these in my favorites menu, show me other options" |

### Boost Multiplier Formula

When `sFavoritesMode = Boost`:

```cpp
float GetFavoritesMultiplier(candidate, rank, totalItems) {
    if (!IsCandidateFavorited(candidate)) return 1.0f;

    // Linear interpolation: top-ranked items get max boost
    float t = rank / (totalItems - 1);
    return favoritesBoostMax - t * (favoritesBoostMax - favoritesBoostMin);
    // Default: 2.5 - t * (2.5 - 1.3)
}
```

**Current behavior:** Since `rank=0` is always passed, all favorited items receive the maximum **2.5x** boost.

### Limited Scope

Only **spells** and **weapons** can be favorited:

```cpp
bool IsCandidateFavorited(candidate) {
    if (SpellCandidate)  return c.isFavorited;   // Supported
    if (WeaponCandidate) return c.isFavorited;   // Supported
    // Items, scrolls, ammo -> always false       // Not supported
}
```

Potions, items, scrolls, and ammo don't use Skyrim's favorites system in the same way, so they always receive `favoritesMultiplier = 1.0` (no boost).

### INI Configuration

```ini
[Favorites]
; sFavoritesMode: How to handle favorited items
;   Boost    - Favorited items get a utility multiplier (default)
;   Off      - Ignore favorites flag entirely (neutral scoring)
;   Suppress - Skip favorited items (they're already accessible)
sFavoritesMode = Boost

; fFavoritesBoostMin: Minimum multiplier for lowest-ranked favorited item
fFavoritesBoostMin = 1.3

; fFavoritesBoostMax: Maximum multiplier for top-ranked favorited item
fFavoritesBoostMax = 2.5
```

### Example: Close Wounds (Favorited)

Scenario: Player has favorited "Close Wounds" spell, health at 40%.

**Without favorites (mode = Off):**
```
contextWeight = healingWeight (e.g., 2.43 from health formula)
learningScore = 0.7*2.5 + 0.3*1.2 + 0.2*0.3 = 2.17
lambda = ComputeAdaptiveLambda(0.7) = 0.5 + 0.7*(3.0-0.5) = 2.25
utility = 2.43 * (1 + 2.25*2.17) * 1.0 * 1.0 * 1.0 = 14.3
```

**With favorites boost (mode = Boost, always rank 0):**
```
Same calculation, but final step:
utility = 14.3 * 2.5 = 35.75  <-- 2.5x boost pushes to top
```

### Why Separate from Contextual Bandit Learning?

**contextual bandit learning learns:** "This item works well in this situation" (context-dependent, data-driven)

**Favorites declare:** "I like this item generally" (context-independent, player-driven)

Keeping them separate allows:
1. **Cold-start assistance** -- Favorited items surface even with no Q-data
2. **Manual override** -- Player can boost items the learner hasn't discovered yet
3. **Clean learning** -- reward estimates reflect actual performance, not favorited status
4. **User control** -- Can disable favorites (mode = Off) for pure learning-driven recs

---

## Contextual Bandit Learning is Slot-Agnostic

Important: contextual bandit learning learns item preference in **game context**, not slot context.

```cpp
// contextual bandit learning doesn't know about slots
// It learns: "Player prefers Fireball in combat at range"
// NOT: "Player prefers Fireball in slot 2"

void OnItemSelected(RE::FormID formID, int slotIndex, bool wasRecommended) {
    StateFeatures features = StateFeatures::FromState(playerState, targets);

    // Slot index is NOT used in learning
    // The slot just determines visibility, not preference

    if (wasRecommended) {
        // Published via EquipEventBus -> FQLSubscriber applies reward
        EquipEventBus::GetSingleton().Publish(
            formID, EquipSource::Wheeler, 1.0f, true);
    }
}
```

---

## Persistence

### SKSE Cosave (v0.13.0+)

All learning data is persisted via SKSE's cosave system, which automatically saves/loads alongside the player's save files. `QLearnerSerializer` uses the static buffer pattern (handles the case where `Load` fires before the global `g_featureQLearner` exists).

**Record Types:**
- `FQLW` -- FeatureQLearner weight vectors: `(formID, weights[18], trainCount, minutesSinceLastUpdate)` + global train count

**Implementation:** `QLearnerSerializer` (`src/persist/QLearnerSerializer.h/.cpp`)

**Key Features:**
- `ResolveFormID()` for mod reordering on every FormID
- Safety cap: 50K max FQL items
- Feature count validation: FQLW records store `numFeatures` and reject mismatched saves (forward-compatible if features are added)
- `ExportData`/`ImportData` on FeatureQLearner with proper locking (`shared_lock` for export, `unique_lock` for import)
- Revert handler clears FeatureQLearner on save revert

**Serialization Callbacks** (registered in `SKSEPlugin_Load`):
- `Save` -> Export FQL weights to cosave
- `Load` -> Import from cosave into static buffer, applied after learner construction
- `Revert` -> Clear all learned data

**FQLW Record Format:**
```
[version: uint32]                = 2
[numFeatures: uint32]            = 18 (validated on load)
[totalTrainCount: uint32]        = global train counter
[numItems: uint32]
For each item:
  [formID: uint32]               -> resolved via ResolveFormID
  [weights: float[18]]           -> per-item weight vector
  [trainCount: uint32]           -> per-item training count
  [minutesSinceLastUpdate: u32]  -> v2: minutes since last FQL update (relative to save time)
```

The `minutesSinceLastUpdate` field (added in v2) allows `MaybeDecay` to apply time-based weight decay even across save/load boundaries — items idle before saving continue to decay proportionally after loading.

See [5-slots.md](5-slots.md) for full profile management details including:
- Auto-save on game save events
- Export/import for sharing
- Reset options (config only, learning only, or both)

---

## Hyperparameters

### FeatureQLearner (active scorer)

| Parameter | Default | Description |
|-----------|---------|-------------|
| LEARNING_RATE | 0.1 | Semi-gradient update step size |
| L2_LAMBDA | 0.01 | L2 regularization (implicit weight decay on update) |
| WEIGHT_CLAMP | +/-10.0 | Hard bounds on individual weights |
| CONFIDENCE_STEEPNESS | 0.3 | Sigmoid steepness for confidence curve |
| CONFIDENCE_MIDPOINT | 5.0 | 50% confidence at 5 trains per item |
| UCB_NORMALIZATION_FACTOR | 0.2 | Scales UCB1 bonus to [0, 1] range |
| EQUIP_REWARD | 8.0 | Reward for using recommendation |
| CONSUME_REWARD | 5.0 | Reward for consuming potion/scroll |
| DECAY_RATE_PER_HOUR | 0.02 | Exponential weight decay for idle items |
| DECAY_THRESHOLD_MINUTES | 5.0 | Don't decay if updated within this window |
| MISCLICK_PENALTY | -3.0 | Penalty for rapidly discarded item |
| MISCLICK_WINDOW_SECONDS | 3.0 | Max gap to count as misclick |

**Removed parameters:**

| Parameter | Old Default | Reason Removed |
|-----------|-------------|----------------|
| SKIP_PENALTY | -1.0 | Replaced by L2 regularization + time-based decay. Skip penalties punished correct recommendations during state transitions. |
| Tabular hyperparameters | various | Tabular QLearner fully removed in v0.13.x (alpha, activeDecayRate, passiveDecayRate, etc.) |

---

## INI Configuration

### [Learning] Section

External equip learning parameters, loaded by `LearningSettings` singleton.

```ini
[Learning]
; bLearnFromExternalEquips: Master toggle for learning from vanilla UI equips
bLearnFromExternalEquips = true

; fExternalEquipTimeWindow: Max cache age for pipeline state to be valid (ms)
; The update loop refreshes the cache every ~100ms, so 2000ms covers menu pauses.
fExternalEquipTimeWindow = 2000

; fExternalEquipMinInterval: Anti-spam minimum interval per item (seconds)
; NOTE: Set to 30 temporarily for testing. Production value: 3.0
fExternalEquipMinInterval = 30

; fHighUtilityRewardMult: Reward multiplier for Case C (near-miss)
fHighUtilityRewardMult = 0.8

; fMediumUtilityRewardMult: Reward multiplier for Case B-med (mid rank)
fMediumUtilityRewardMult = 0.4

; fLowUtilityRewardMult: Reward multiplier for Case B-low (low rank)
fLowUtilityRewardMult = 0.2

; fDifferentPageRewardMult: Reward multiplier for Case D (other page)
fDifferentPageRewardMult = 0.5

; fNotCandidateRewardMult: Reward multiplier for Case A (not a candidate)
; 1.0 = strongest signal (player went out of their way)
fNotCandidateRewardMult = 1.0
```

---

## Resetting and Randomizing Weights

### Initial Heuristic Defaults

The Candidate Generator and Utility Scorer use heuristic defaults (priors) to bootstrap the learning system before sufficient training data is collected. These defaults may not match your playstyle initially.

### When Recommendations Don't Match Your Playstyle

The learning system requires **50-100 actions** (equipping spells/weapons/items) plus a few hours of gameplay to adapt to your preferences. If the initial recommendations feel off:

1. **Train the system** - Use the recommended items when appropriate to teach the system your preferences
2. **Favorite preferred equipment** - Favorited spells/weapons get a 2.5x utility boost (see [Favorites Multiplier](#favorites-multiplier) section). This helps surface items you like before the learner has enough training data.
3. **Reset weights** - You can "nuke" or "jumble" the weights to get randomized values for recommendations

### Reset Options

> **WARNING:** Resetting weights is a **destructive operation** that will erase your current learned preferences. Making an external backup of your save file is strongly recommended before resetting.

Available reset options:
- **Config only** - Resets configuration settings to defaults, preserves learned weights
- **Learning only** - Resets reward estimates and priors to random/default values, preserves config
- **Both** - Full reset of both configuration and learned weights

See [5-slots.md](5-slots.md) for full profile management details including auto-save, export/import, and reset procedures.

---

## See Also

- [1-states.md](1-states.md) - State models (WorldState, PlayerActorState, TargetCollection)
- [0-pipeline.md](0-pipeline.md) - Overall pipeline flow
- [5-slots.md](5-slots.md) - Slot classification

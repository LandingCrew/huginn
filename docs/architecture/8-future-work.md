# Future Work - Learning System

This document consolidates future work items related to the learning and prediction systems or just on off ideas to be evaluted. These are deferred features that may be implemented in v1.0+ releases.

---

## Temporal Prediction & Discounting

> **Implementation order (see [../refactor/roadmap.md](../roadmap.md) Phase 5):** Stage 1 (Urgency Multiplier) -> Stage 2 (Uncertainty-Aware Prediction) -> Stage 3 (Branching Futures) -> Stage 4 (HMM Combat States).

The current system is purely reactive - it evaluates the current state snapshot and scores items accordingly. This creates a **timeliness problem**: by the time health is critical (20%), the player may already be dead. Recommendations need to arrive *before* the crisis, not during it.

**Core Insight:** The system should predict a few states ahead based on observable trends (velocity), but discount future predictions so current state remains dominant.

### Why This Isn't Cheating

Prediction based on observable trends is information the player already has:
- "My health bar is dropping fast" -> player can see this
- "I've taken 3 hits in 2 seconds" -> player experienced this
- "Enemy is winding up a power attack" -> player can see animation

This is different from forbidden prediction like "enemy will cast fireball next" which requires reading hidden AI state.

### Implementation Approaches

**1. Urgency Multiplier (Stage 1 — Pre-v1.0, ~80% of Benefit)**

Use velocity to adjust the *urgency* of recommendations without full temporal rollout. This is the simplest approach and captures most of the value.

```cpp
float GetUrgencyMultiplier(float healthVelocity, float currentHealth) {
    if (healthVelocity >= 0) return 1.0f;

    float lossRate = -healthVelocity;
    float timeToCritical = (currentHealth - 0.2f) / lossRate;

    if (timeToCritical < 1.0f) return 3.0f;      // Critical in <1 second
    else if (timeToCritical < 3.0f) return 2.0f; // Critical in 1-3 seconds
    else if (timeToCritical < 5.0f) return 1.5f; // Critical in 3-5 seconds
    return 1.0f;
}
```

Velocity from a 30-sample ring buffer, EMA-smoothed (`0.7 * velocity + 0.3 * instantVelocity`), ~300ms lag to respond to step changes (acceptable for gradual drain).

**2. Uncertainty-Aware Prediction (Stage 2 — v1.0)**

```cpp
struct ResourcePredictor {
    float value;              // Current authoritative value
    float velocity;           // Smoothed velocity (EMA)
    float uncertaintyBand;    // +/- this much at prediction horizon

    void Update(float observed, float dt) {
        float instantVelocity = (observed - value) / dt;
        velocity = 0.7f * velocity + 0.3f * instantVelocity;
        value = observed;

        float velocityError = std::abs(instantVelocity - velocity);
        uncertaintyBand = 0.8f * uncertaintyBand + 0.2f * velocityError;
    }

    float PredictWithConfidence(float horizon, float& outConfidence) {
        float predicted = value + velocity * horizon;
        float uncertainty = uncertaintyBand * horizon * 2.0f;
        outConfidence = 1.0f / (1.0f + 2.0f * uncertainty * uncertainty);
        return std::clamp(predicted, 0.0f, 1.0f);
    }
};
```

**3. Branching Futures (Stage 3 — Post v1.0)**

Combat has **branching futures** - linear prediction only considers one trajectory.

```cpp
struct FutureState {
    float health, magicka, stamina;
    float probability;  // P(reaching this state)
    int depth;          // Steps from current
};

// Explore possible futures with probability-weighted pruning
std::vector<FutureState> ExploreNearFutures(const GameState& current, int maxDepth = 3);
```

**4. HMM Combat State Modeling (Stage 4 — v1.1+)**

Combat modeled as an HMM with hidden states {Peaceful, LightCombat, HeavyCombat, BossFight}. Requires enough combat encounters for reliable transition matrices.

---

## SARSA / Temporal Learning

The current contextual bandit learning approach learns "best item for this situation" with immediate rewards. SARSA could learn **spell sequences**:

```
Q(s,a) <- Q(s,a) + alpha[r + gamma*Q(s',a') - Q(s,a)]
```

> **Note (v0.13.0):** `FeatureQLearner` uses continuous 18-float feature vectors, not discrete state hashes. A feature-based SARSA variant would store `Q(s, a) = w_a . phi(s)` and update with the next feature vector `phi(s')` and next action `a'`. The deferral rationale still holds.

### When SARSA Would Help
> Honestly, i think in a differnt game this would be IDEAL. Skyrim doesnt really have status effect combos that i know of without mods

**Scenario:** Player is in combat, low health, casts Healing.

- With **contextual bandit learning**: Healing gets +8 reward. Done.
- With **SARSA**: Healing gets +8, next state observed, Fireball cast -> SARSA learns "Healing -> Fireball" is a good sequence

**Benefits:**
1. Learn spell combos (buff -> attack, cloak -> melee)
2. Credit "setup" spells that enable good follow-up
3. Penalize spells that lead to bad outcomes

### When SARSA Might NOT Help

1. **Delayed feedback is noisy** - Many things happen between actions
2. **Combat is chaotic** - Hard to attribute outcomes to specific spells
3. **Implementation complexity** - Need to track state transitions
4. **More hyperparameters** - Discount factor gamma matters a lot

### Decision: Contextual Bandit Learning (SARSA Deferred)

**Rationale:** contextual bandit learning is sufficient for "best item for this situation" at this stage.

**Trigger to revisit:** If we observe patterns like "players who cast X often follow with Y, and that combo works well consistently." Would require:
- Transition logging (state, action, reward, nextState, nextAction)
- Combat outcome detection (win/lose/flee)
- Configurable discount factor gamma for temporal credit

---

## Experience Replay / Batch Learning

Current learning is **online** - rewards applied immediately. This can cause recency bias.

> **Note (v0.13.0):** Current learning uses `FeatureQLearner` (linear function approximation, `src/learning/FeatureQLearner.h`), not a tabular Q-table. L2 regularization during gradient descent (`L2_LAMBDA = 0.01`) provides intrinsic weight decay without a separate replay buffer. The `StateFeatures` struct in the `Experience` example below matches the 18-float vector used by `FeatureQLearner`. Experience replay remains deferred.

```cpp
struct Experience {
    StateFeatures features;
    RE::FormID item;
    float reward;
    float timestamp;
};

class ExperienceBuffer {
    std::deque<Experience> m_buffer;
    static constexpr size_t MAX_SIZE = 10000;

public:
    void Add(const Experience& exp);
    std::vector<Experience> SampleBatch(size_t batchSize);
};
```

**Benefits:**
- Reduces recency bias
- More stable learning
- Can replay rare but important experiences

**Why deferred:**
1. Online learning is working adequately
2. Adds memory overhead (~800KB for 10K experiences)
3. Current session lengths may not generate enough diverse experiences
4. L2 regularization already provides continuous weight dampening

**Trigger to revisit:** If users report the system "forgets" preferences or oscillates between recommendations.

---

## Reward Structure Refinement

> **Status (v0.13.0):** Most issues in this section have been resolved by the learning decoupling refactor (Phase 1) and the feature-based contextual bandit learning migration (Phase 3.5). See [../refactor/roadmap.md](../roadmap.md) for details.

### Resolved Issues

| Issue | Resolution |
|-------|------------|
| Skip penalty punishes exploration | Resolved. No skip penalties exist. `FeatureQLearner` uses L2 regularization (`L2_LAMBDA = 0.01`) during gradient descent — weights for unused items shrink toward zero naturally. A secondary time-based `MaybeDecay()` (2%/hr, items idle > 5 min) provides explicit staleness decay. |
| Double-dipping (equip + cast) | Resolved. Cast bonus was never implemented. `EQUIP_REWARD = 8.0f` (hotkey/Wheeler) and `CONSUME_REWARD = 5.0f` (potion/scroll consumption) are the sole explicit signals. |
| Skip penalty ambiguity | Resolved. No skip penalties applied. Reasonable alternatives are not penalized. |
| reward estimate drift / build changes | Resolved. L2 regularization pulls weights toward zero without reinforcement. `MaybeDecay()` (2%/hr, threshold 5 min) provides time-based staleness decay per candidate during scoring. |
| Positive feedback loop | Mitigated. Per-feature weight clamping (`WEIGHT_CLAMP = 10.0f`) prevents unbounded drift. L2 regularization provides continuous dampening. |
| Misclick detection | Implemented. Rapid equip-then-switch (< 3s, same context) applies `MISCLICK_PENALTY = -3.0f` to the discarded item via `UsageMemorySubscriber`. Consolidated into `EquipEventBus` subscriber pattern. |

### Remaining Future Work

**Combat outcome rewards** — Not yet implemented. Could provide additional signal:
```cpp
COMBAT_WIN_BONUS = +1.0   // Applied to recently used spells after winning
DEATH_PENALTY = -2.0      // Applied after death
```

**Category-level negative signal** — When player picks a DIFFERENT category than shown (e.g., showed healing, equipped damage), could apply mild penalty to shown category. Deferred pending field testing of current decay model.

---

## Hidden Markov Model for Combat States

Combat can be modeled as an HMM with hidden states {Peaceful, LightCombat, HeavyCombat, BossFight}:

**Why HMM is powerful:**
1. Captures combat intensity
2. Learns from history
3. Handles uncertainty
4. Efficient (O(states^2 x observations))

See [../refactor/roadmap.md](../roadmap.md) Phase 5, Stage 4 for implementation plan.

---

## Configuration Constants (Proposed)

```cpp
namespace TemporalConfig {
    // Velocity smoothing
    inline constexpr float VELOCITY_EMA_ALPHA = 0.3f;

    // Uncertainty tracking
    inline constexpr float UNCERTAINTY_EMA_ALPHA = 0.2f;
    inline constexpr float UNCERTAINTY_COMBAT_BONUS = 0.1f;
    inline constexpr float MAX_UNCERTAINTY = 0.5f;

    // Discounting
    inline constexpr float TEMPORAL_GAMMA = 0.7f;
    inline constexpr int PREDICTION_HORIZON = 3;
    inline constexpr float MIN_HOLD_TIME_MS = 1500.0f;
}
```

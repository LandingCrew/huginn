# Future Work — Learning System

Deferred and speculative work on the learning and prediction systems, plus
one-off ideas kept here so they are not lost.

> **Verified against v0.19.10.** This is the only *forward-looking* document in
> `architecture/`, so it is reconciled against [../roadmap.md](../roadmap.md)
> rather than treated as a design spec. **The roadmap is authoritative for what is
> actually planned.** Nothing in this file is scheduled unless the roadmap says
> so — treat everything below as either shipped (marked) or an unscheduled idea
> with the code facts attached.

> **Related documentation:**
> - [../roadmap.md](../roadmap.md) — open work, and the only planning source of truth
> - [../roadmap-archive.md](../roadmap-archive.md) — completed items, several recording *why* an approach was rejected
> - [4-contextual-bandits.md](4-contextual-bandits.md) — the learner as it exists, and why it is a bandit and not Q-learning
> - [1-states.md](1-states.md) — the state models the prediction ideas below would draw on

> **Terminology:** Huginn's learner is a **contextual bandit**. The code
> identifiers (`FeatureQLearner`, `QLearnerSerializer`, the `FQLW` cosave record,
> `hg reset qvalues`) keep the historical "Q" name and will not be renamed — the
> cosave format and a documented console command depend on them.

---

## Status at a glance

| Item | Status at 0.19.10 | Tracked on the roadmap? |
|---|---|---|
| Reward structure refinement | **Shipped** (v0.13.0) | No — done |
| Misclick detection | **Shipped** (v0.13.0, `EquipEventBus`) | No — done |
| Decay / anti-drift (L2 + lazy time decay) | **Shipped** (v0.13.0) | No — done |
| Stage 1 — urgency multiplier | Not implemented | **No** |
| Stage 2 — uncertainty-aware prediction | Not implemented | **No** |
| Stage 3 — branching futures | Not implemented | **No** |
| Stage 4 / HMM combat states | Not implemented | **No** |
| SARSA / temporal credit | Deferred by design | No |
| Experience replay / batch learning | Deferred | No |
| Combat outcome rewards | Not implemented | **No** |
| Category-level negative signal | Not implemented | **No** |
| Kalman FQL / learnable context weights (Addendum #15/#16) | Not implemented | **Yes — parked** (needs a v3 cosave bump; not landable during an active soak run) |

> **Dead reference, removed:** earlier revisions of this document pointed at
> the v0.13.x roadmap's "Phase 5" for the implementation order of the four
> temporal stages. There is no Phase 5. That numbering came from a v0.13.x
> `ROADMAP.md` which was superseded by [../roadmap.md](../roadmap.md) and
> deliberately not imported (see [../README.md](../README.md)). The current
> roadmap has no phases and carries none of the temporal-prediction work, so the
> staging below is a *proposal*, not a schedule.

---

## Shipped: reward structure refinement

Landed with the feature-based learner in **v0.13.0** and still true at 0.19.10.
Kept here because the "why" is useful and the constants move.

| Original issue | Resolution (verified at 0.19.10) |
|---|---|
| Skip penalty punishes exploration | No skip penalties exist. `FeatureQLearner::Update` applies L2 regularization (`L2_LAMBDA = 0.01f`) on every gradient step, so weights for unused items shrink toward zero naturally (`FeatureQLearner.cpp:41`) |
| Double-dipping (equip + cast) | A cast bonus was never implemented. `EQUIP_REWARD = 8.0f` and `CONSUME_REWARD = 5.0f` are the only positive signals (`Config.h:46`, `Config.h:51`), both scaled by `event.rewardMultiplier` in `FQLSubscriber` |
| Skip penalty ambiguity | Not applicable — reasonable alternatives are never penalized |
| Weight drift across build changes | L2 pulls weights toward zero without reinforcement; a lazy time-based decay adds explicit staleness handling (`DECAY_RATE_PER_HOUR = 0.02f`, `DECAY_THRESHOLD_MINUTES = 5.0f`, `Config.h:179`) |
| Positive feedback loop | Mitigated by per-feature clamping (`WEIGHT_CLAMP = 10.0f`, `FeatureQLearner.h:130`) applied immediately after each update |
| Misclick detection | Implemented: a rapid equip-then-switch inside `MISCLICK_WINDOW_SECONDS = 3.0f` in the same context applies `MISCLICK_PENALTY = -3.0f` to the discarded item (`UsageMemory::RecordUsage` → `UsageMemorySubscriber`, `EquipSubscribers.h:66`) |

Two identifier corrections against older revisions of this document:

- **`MaybeDecay()` is now `MaybeDecayBatch()`.** Per-candidate decay cost ~N lock
  acquisitions per scoring tick; it is now one shared-lock collection pass plus one
  unique-lock apply pass, skipped entirely when nothing qualifies
  (`FeatureQLearner.h:49`, called from `UtilityScorer.cpp:63`).
- Rewards are dispatched through the `EquipEventBus` subscriber pattern, not from
  the update loop: `FQLSubscriber` (rewards), `UsageMemorySubscriber` (recency +
  misclick penalty), `CooldownSubscriber` (consumption cooldown). Hotkey and
  Wheeler equips are rewarded **only when the item was recommended**
  (`event.wasRecommended`); external equips always apply, with attribution
  scaling already folded into `rewardMultiplier`.

---

## Open: temporal prediction and discounting

The system is purely reactive — it evaluates the current state snapshot and
scores accordingly. That creates a **timeliness problem**: by the time health is
critical, the player may already be dead. Recommendations want to arrive *before*
the crisis.

**Core idea:** predict a few states ahead from observable trends (velocity), but
discount the prediction so current state stays dominant.

### Why this would not be cheating

Prediction from observable trends uses information the player already has:

- "My health bar is dropping fast" — the player can see this
- "I've taken 3 hits in 2 seconds" — the player experienced this
- "The enemy is winding up a power attack" — the player can see the animation

This is categorically different from "the enemy will cast fireball next", which
requires reading hidden AI state. See the Forbidden Information list in
[../../CLAUDE.md](../../CLAUDE.md).

### What already exists in the code

This matters for scoping, and it is the biggest correction to earlier revisions
of this document: **the trend substrate is already computed every tick, and
nothing in scoring reads it.**

`HealthTrackingState` (`src/state/StateTypes.h:329`) maintains a 10-event damage
ring buffer and a 10-event healing ring buffer, and derives:

| Field | Meaning |
|---|---|
| `damageRate` | HP/sec damage rate, weighted average |
| `healingRate` | HP/sec healing rate, weighted average |
| `damageIncreasing` / `damageDecreasing` | trend flags, commented in the header as "for future panic/retreat mechanics" |
| `IsUnderSustainedAttack()` | `damageRate > 5.0f` |
| `recentDamageTaken`, `timeSinceLastHit` | decayed 2-second window |

The proposed `healthVelocity` is essentially `healingRate - damageRate`, which
already exists. What is missing is the wiring: of the whole struct, the pipeline
consumes only the elemental `timeSinceLastFire/Frost/Shock` fields (for elemental
resist context, `PipelineCoordinator.cpp:155`). `damageRate`, the trend flags and
`IsUnderSustainedAttack()` reach **only the ImGui debug widget**
(`StateManagerDebugWidget.cpp:825`). Note also that the proposal's "30-sample ring
buffer" does not match what is there: the buffers hold 10 events or 5 seconds.

The other constraint is the feature vector. `StateFeatures` is 18 floats and
carries **no trend or velocity feature** — vitals, combat/sneak flags, normalized
enemy distance, a 7-way target-type one-hot, four equipment flags, and a bias
term. It is **APPEND-ONLY**: `ToArray()` order is the cosave wire order and
`QLearnerSerializer` migrates saved weights positionally
(`StateFeatures.h:61`). Adding a velocity feature is therefore a cosave-format
change, which puts it in the same bucket as the parked Addendum #15/#16 — not
landable during an active soak run.

### Proposed staging

**1. Urgency multiplier (~80% of the benefit, no temporal rollout)**

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

This is the cheapest stage precisely because it needs no new feature: it would
multiply the *context weight*, not the learned score, so no cosave change is
implied. It would belong next to the existing health/magicka urgency curves in
`ContextRuleEngine` (`ContextRuleEngine.cpp:79`), which already shape weight as a
continuous function of deficit.

**2. Uncertainty-aware prediction**

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

**3. Branching futures** — combat has branching futures; linear prediction
considers only one trajectory.

```cpp
struct FutureState {
    float health, magicka, stamina;
    float probability;  // P(reaching this state)
    int depth;          // Steps from current
};

std::vector<FutureState> ExploreNearFutures(const GameState& current, int maxDepth = 3);
```

**4. HMM combat state modelling** — combat as a hidden Markov model over
{Peaceful, LightCombat, HeavyCombat, BossFight}. Attractive because it captures
intensity, learns from history, handles uncertainty and is cheap
(O(states² × observations)); blocked on having enough combat encounters to
estimate transition matrices, and on the same cosave question as any new feature.
Stages 3 and 4 have no implementation sketch beyond the above and no consumer
designed for them.

### Proposed constants (never landed)

No `TemporalConfig` namespace exists — `src/Config.h` has only
`namespace Huginn::Config`. Kept as a starting point, not as documentation of
anything real:

```cpp
namespace TemporalConfig {
    inline constexpr float VELOCITY_EMA_ALPHA = 0.3f;
    inline constexpr float UNCERTAINTY_EMA_ALPHA = 0.2f;
    inline constexpr float UNCERTAINTY_COMBAT_BONUS = 0.1f;
    inline constexpr float MAX_UNCERTAINTY = 0.5f;
    inline constexpr float TEMPORAL_GAMMA = 0.7f;
    inline constexpr int PREDICTION_HORIZON = 3;
    inline constexpr float MIN_HOLD_TIME_MS = 1500.0f;
}
```

---

## Deferred by design: SARSA / temporal learning

The learner rewards "best item for this situation" from immediate feedback.
SARSA would instead learn **sequences**:

```
Q(s,a) <- Q(s,a) + alpha[r + gamma*Q(s',a') - Q(s,a)]
```

This is exactly the update Huginn does *not* perform, and it is what makes the
current system a contextual bandit rather than Q-learning: `FeatureQLearner::Update`
computes `error = reward - prediction` and takes a semi-gradient step on it —
no `gamma`, no `s'`, no trajectory (see [4-contextual-bandits.md](4-contextual-bandits.md)).
A feature-based SARSA variant would keep `Q(s,a) = w_a · phi(s)` and update using
the next feature vector `phi(s')` and next action `a'`.

> Author's note, kept: in a different game this would be ideal. Skyrim doesn't
> really have status-effect combos without mods.

**Where it would help:** learning combos (buff → attack, cloak → melee),
crediting setup spells that enable a good follow-up, penalizing spells that lead
to bad outcomes.

**Where it would not:** delayed feedback is noisy, combat is chaotic and hard to
attribute, state-transition tracking is real complexity, and `gamma` is another
hyperparameter that matters a lot.

**Trigger to revisit:** an observed pattern like "players who cast X often follow
with Y, and that combo works well consistently". Would require transition logging
(state, action, reward, nextState, nextAction), combat outcome detection
(win/lose/flee), and a configurable discount factor.

---

## Deferred: experience replay / batch learning

Learning is **online** — rewards are applied immediately, which can cause recency
bias.

```cpp
struct Experience {
    StateFeatures features;   // the same 18-float vector the learner uses
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

**Benefits:** less recency bias, more stable learning, rare-but-important
experiences can be replayed.

**Why deferred:** online learning is working adequately; ~800 KB of memory for
10K experiences; session lengths may not generate enough diverse experiences; and
L2 regularization already provides continuous dampening.

**Trigger to revisit:** users reporting that the system "forgets" preferences or
oscillates between recommendations.

---

## Open: additional reward signals

Neither of these exists in the code — no combat-outcome hook, no death penalty,
no category-level penalty (searched across `src/learning/`).

**Combat outcome rewards** — would need win/lose/flee detection, which does not
exist today, plus a credit window over recently used items:

```cpp
COMBAT_WIN_BONUS = +1.0   // Applied to recently used spells after winning
DEATH_PENALTY = -2.0      // Applied after death
```

**Category-level negative signal** — when the player picks a different category
than the one shown (healing shown, damage equipped), apply a mild penalty to the
shown category. Deferred pending field testing of the current decay model. Note
this overlaps the soak-testing question already on the roadmap about whether
accept% is the right quality metric for a wheel-driven player at all.

---

## What the roadmap actually carries (cross-reference, not a copy)

Read [../roadmap.md](../roadmap.md) for the live list. The learning-adjacent
entries as of 2026-08-29:

- **Addendum #15/#16 — Kalman FQL / learnable context weights: parked.** Needs a
  v3 cosave bump, which is not landable during an active soak run. This is the
  only learning-system item with a real plan behind it.
- **Scroll cold-start** — every scroll sits in the candidate pool each tick but
  scores `learn ≈ 0` against trained items at `learn = 7–8`, so a scroll can
  never surface until it is used and cannot be used until it surfaces. A learning
  problem in practice, filed under recommendation issues.
- **`Context::WeightForCandidate` unit tests** — the tests currently
  hand-reimplement the weight mapping instead of calling the real one.

Anything in this document that is *not* in that list is unscheduled by
definition.

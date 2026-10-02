# Huginn Learning System

> **Implementation Status (v0.19.x):** **FeatureBanditLearner** is the sole learning
> system (linear reward model, 18-float context vectors). The tabular learner
> was removed in v0.13.x and no trace of it remains in `src/`. See
> `src/learning/FeatureBanditLearner.h` / `.cpp` for the implementation and
> `src/learning/UtilityScorer.cpp` for how its output enters the final score.

This document details the feature-based reward learning architecture for learning player preferences.

> **Upgrading to 0.20.0 resets learning.** The cosave record tag changed to
> `BNDW` when the learner's identifiers were renamed, and nothing reads the old
> tag. Saves written before 0.20.0 lose their learned weights and start over;
> nothing else in the save is affected. The console command is now
> `hg reset weights`.

---

## Overview

Huginn uses **feature-based reward learning** (linear function approximation) instead of the tabular learner it replaced. This provides:

1. **Generalization** - Similar states behave similarly
2. **No cold start** - New states work immediately
3. **Smaller memory** - Weights per item, not per state x item
4. **Interpretable** - Can inspect learned weights (`hg weights <FormID>`)

---

## Why Feature-Based? (Tabular Deprecation Rationale)

### Problem with the Tabular Learner

The tabular learner (v0.6-v0.13) mapped `(discrete_state_hash, FormID) -> reward estimate`. This approach had five fundamental problems that couldn't be fixed without changing the representation:

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
| Adding features | Exponential blowup (x new dimension) | Linear -- append a column to the feature vector |
| Storage | O(states x items) | O(items x 18) -- 72 bytes of weights per item regardless of state count |

---

## StateFeatures

Normalized feature vector extracted from the state models
(`PlayerActorState`, `TargetCollection`) — see
[1-states.md](1-states.md). Defined in `src/learning/StateFeatures.h`.

```
+------------------------------------------------------------------------------+
|                         STATE FEATURES VECTOR (18 floats)                    |
+------------------------------------------------------------------------------+
|                                                                              |
|  RESOURCES (continuous 0.0-1.0)                                              |
|  +--------+--------+--------+                                                |
|  | [0]    | [1]    | [2]    |                                                |
|  |health% |magicka%|stamina%|                                                |
|  | 0.45   | 0.80   | 0.60   |  <- Example: 45% HP, 80% MP, 60% SP            |
|  +--------+--------+--------+                                                |
|                                                                              |
|  COMBAT STATE (binary 0/1 + normalized distance)                             |
|  +--------+--------+--------+                                                |
|  | [3]    | [4]    | [5]    |                                                |
|  |inCombat|sneaking|distNorm|  <- distNorm: 0=melee, 0.5=mid, 1=far          |
|  | 1.0    | 0.0    | 0.75   |  <- Example: fighting at range                 |
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
    // [0-2] Vitals — already normalized 0-1 in ActorVitals
    float healthPct   = 1.0f;
    float magickaPct  = 1.0f;
    float staminaPct  = 1.0f;

    // [3-5] Player state — binary + normalized distance
    float inCombat     = 0.0f;
    float isSneaking   = 0.0f;
    float distanceNorm = 1.0f;  // 0=melee, 0.5=mid, 1=ranged (or no enemy)

    // [6-12] Target type — one-hot (defaults to targetNone)
    float targetNone      = 1.0f;
    float targetHumanoid  = 0.0f;
    float targetUndead    = 0.0f;
    float targetBeast     = 0.0f;
    float targetConstruct = 0.0f;
    float targetDragon    = 0.0f;   // Dragons are special — breath attacks, flight
    float targetDaedra    = 0.0f;   // Atronachs, Dremora — anti-daedra magic applies

    // [13-16] Equipment — binary
    float hasMeleeEquipped  = 0.0f;
    float hasBowEquipped    = 0.0f;
    float hasSpellEquipped  = 0.0f;
    float hasShieldEquipped = 0.0f;

    // [17] Bias — always 1.0 (intercept term)
    float bias = 1.0f;

    static constexpr size_t NUM_FEATURES = 18;
    static constexpr float  MAX_DISTANCE = 4096.0f;  // ~56m: archery + destruction range
};
```

### The APPEND-ONLY contract

`ToArray()`'s order **is** the cosave wire order, and `BanditSerializer`
migrates saved weights *positionally* when `NUM_FEATURES` changes. New features
must be appended at the **end** of `ToArray()`; positions must never be
reordered or removed, or saved weights would silently apply to the wrong
features. Retiring a feature means keeping its slot and feeding it a constant
`0`. A `static_assert` inside `ToArray()` guards the count
(`src/learning/StateFeatures.h:61-68`, `:137-150`).

### Feature Extraction

`StateFeatures::FromState` (`src/learning/StateFeatures.h:76`):

```cpp
StateFeatures StateFeatures::FromState(const State::PlayerActorState& player,
                                       const State::TargetCollection& targets)
{
    StateFeatures f;

    // Vitals (already 0-1 in ActorVitals; clamp guards SKSE edge cases)
    f.healthPct  = std::clamp(player.vitals.health,  0.0f, 1.0f);
    f.magickaPct = std::clamp(player.vitals.magicka, 0.0f, 1.0f);
    f.staminaPct = std::clamp(player.vitals.stamina, 0.0f, 1.0f);

    f.inCombat   = player.isInCombat ? 1.0f : 0.0f;
    f.isSneaking = player.isSneaking ? 1.0f : 0.0f;

    // Distance from the CLOSEST enemy (GetClosestEnemy filters NO_TARGET sentinels)
    auto closestEnemy = targets.GetClosestEnemy();
    if (closestEnemy.has_value()) {
        float dist = std::sqrt(closestEnemy->distanceToPlayerSq);
        f.distanceNorm = std::clamp(dist / MAX_DISTANCE, 0.0f, 1.0f);
    } else {
        f.distanceNorm = 1.0f;  // No enemy → max range
    }

    // Target type one-hot from targets.primary (crosshair / combat focus).
    // All seven fields are cleared first, then the switch sets exactly one —
    // the fresh struct defaults targetNone to 1.0.
    // ... switch on targets.primary->targetType ...

    f.hasMeleeEquipped  = player.hasMeleeEquipped  ? 1.0f : 0.0f;
    f.hasBowEquipped    = player.hasBowEquipped    ? 1.0f : 0.0f;
    f.hasSpellEquipped  = player.hasSpellEquipped  ? 1.0f : 0.0f;
    f.hasShieldEquipped = player.hasShieldEquipped ? 1.0f : 0.0f;

    return f;  // bias stays at its 1.0 default
}
```

**Design note (from the header):** `distanceNorm` comes from
`GetClosestEnemy()` while the one-hot comes from `targets.primary`. These can
refer to *different actors* — intentionally. Distance encodes tactical
proximity; target type encodes what the player is focused on. `inCombat` and
`isSneaking` can both be 1.0 (sneak-combat).

---

## FeatureBanditLearner

Contextual bandit with a linear reward model — one weight vector per item (arm),
scored against the 18-float context. `src/learning/FeatureBanditLearner.h`.

```cpp
class FeatureBanditLearner {
public:
    // Reward estimate: w_item . phi(context). Unknown item → 0.0
    [[nodiscard]] float GetRewardEstimate(RE::FormID formID, const StateFeatures&) const;

    // Semi-gradient step on (reward - prediction). No gamma, no next state.
    void Update(RE::FormID formID, const StateFeatures& features, float reward);

    // Batched lazy time decay over a candidate pool. Returns items decayed.
    // `now` is injectable for tests.
    size_t MaybeDecayBatch(const std::vector<RE::FormID>& formIDs,
                           std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now());

    // Metrics
    [[nodiscard]] float GetConfidence(RE::FormID) const;
    [[nodiscard]] float GetUCB(RE::FormID) const;
    [[nodiscard]] FeatureItemMetrics GetMetrics(RE::FormID, const StateFeatures&) const;

    // Amortized reader: one shared_lock for a whole scoring loop
    class LockedReader {
        FeatureItemMetrics GetMetrics(
            RE::FormID, const std::array<float, StateFeatures::NUM_FEATURES>& phi) const;
    };
    [[nodiscard]] LockedReader AcquireReader() const;

    // Serialization (cosave)
    struct SerializedEntry {
        RE::FormID formID;
        std::array<float, StateFeatures::NUM_FEATURES> weights;
        uint32_t trainCount;
        uint32_t minutesSinceLastUpdate = 0;  // v2: relative to save time
    };
    void ExportData(const std::function<void(SerializedEntry)>&, uint32_t& outTotalTrainCount) const;
    void ImportData(const std::vector<SerializedEntry>&, uint32_t totalTrainCount);

    // Diagnostics (backing `hg status` / `hg weights`)
    [[nodiscard]] size_t   GetItemCount() const;
    [[nodiscard]] uint32_t GetTotalTrainCount() const;
    [[nodiscard]] uint32_t GetTrainCount(RE::FormID) const;
    [[nodiscard]] std::array<float, StateFeatures::NUM_FEATURES> GetWeights(RE::FormID) const;
    void Clear();

private:
    // Weights, train count and last-update timestamp are COLOCATED in one map:
    // one hash lookup per candidate instead of three parallel-map lookups.
    // The cosave format is unaffected — serialization goes only through
    // SerializedEntry.
    struct ItemLearningData {
        std::array<float, StateFeatures::NUM_FEATURES> weights{};
        uint32_t trainCount = 0;
        std::chrono::steady_clock::time_point lastUpdate{};
    };
    std::unordered_map<RE::FormID, ItemLearningData> m_items;
    uint32_t m_totalTrainCount = 0;   // for UCB

    static constexpr float LEARNING_RATE           = 0.1f;
    static constexpr float L2_LAMBDA               = 0.01f;
    static constexpr float WEIGHT_CLAMP            = 10.0f;
    static constexpr float CONFIDENCE_MIDPOINT     = 5.0f;
    static constexpr float CONFIDENCE_STEEPNESS    = 0.3f;
    static constexpr float UCB_NORMALIZATION_FACTOR = 0.2f;

    mutable std::shared_mutex m_mutex;
};
```

### The update rule — why this is a bandit

`FeatureBanditLearner::Update` (`src/learning/FeatureBanditLearner.cpp:23`):

```cpp
auto phi = features.ToArray();          // computed OUTSIDE the lock
std::unique_lock lock(m_mutex);
auto& data = m_items[formID];           // zero-init on first access
auto& w    = data.weights;

float prediction = DotProduct(w, phi);
float error      = reward - prediction;          // ← line 36: no gamma, no s'

for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
    w[i] += LEARNING_RATE * error * phi[i] - LEARNING_RATE * L2_LAMBDA * w[i];
    w[i] = std::clamp(w[i], -WEIGHT_CLAMP, WEIGHT_CLAMP);
}

data.trainCount++;
m_totalTrainCount++;
data.lastUpdate = std::chrono::steady_clock::now();
```

`Update` receives one context, one arm and one scalar reward. Nothing in
`src/learning/` records a state transition or a discount factor, so `error` is
a one-step regression residual: least-mean-squares regression on a per-arm
linear reward model.

### Confidence and UCB

`ComputeConfidence` (`FeatureBanditLearner.cpp:127`) is a logistic on the *per-item*
train count:

```
confidence(n) = 1 / (1 + exp(-0.3 * (n - 5)))
   0 trains → ~18%     5 → 50%     10 → ~82%     15 → ~95%
```

`ComputeUCB` (`FeatureBanditLearner.cpp:135`) is UCB1, normalized and clamped:

```
UCB(n) = clamp(0.2 * sqrt(2 * ln(totalTrains) / n), 0, 1)
UCB    = 1.0 exactly when n == 0 or totalTrains == 0   (maximum exploration)
```

Both helpers require `m_mutex` to be held by the caller (`ComputeUCB` reads
`m_totalTrainCount`).

### Locking

Reads take `std::shared_lock`, writes `std::unique_lock`; `phi` is always
computed outside the lock. For scoring loops the class exposes
`AcquireReader()`, which holds **one** shared lock while the caller queries N
candidates — replacing roughly 200 lock acquire/release pairs per tick with 1.
`UtilityScorer::ScoreCandidates` uses it for both the main pass and the
cold-start fallback.

---

## One Selection Path: SelectionTracker and EquipEventBus

Every learning signal is a **confirmed player selection** (one selection
path, v0.22.9, `src/learning/SelectionTracker.h/.cpp`). Whatever device the
player used, the selection is held, confirmed or dropped, and only then
dispatched -- once -- through the **EquipEventBus** to the subscribers.

### Event Flow

```
Selection sources:                SelectionTracker            Subscribers:
+-------------------------+       +------------------+        +-----------------------+
| EquipManager (key)      |--+    | Select():        |        | BanditSubscriber      |
| WheelerClient (Huginn   |  |    |  capture state,  | confirm|  -> learner, 1 reward |
|   wheel)                |  +--->|  bar snapshot    |------->+-----------------------+
| ExternalEquipLearner    |  |    | pending (1/item) |  bus   | UsageMemorySubscriber |
|   (menu / vanilla hotkey|--+    |                  |Dispatch|  -> recency memory    |
|    / own wheel, gated)  |       | OnConsumed():    |        +-----------------------+
+-------------------------+       |  count drop      |
UpdateLoop count drop ----------->| Update():        |---> SelectionLog (debug log +
  (also starts the cooldown)      |  still equipped? |      Huginn_Selections.jsonl)
                                  +------------------+
```

Selection sites:

| Source | Call site | `via` |
|---|---|---|
| Hotkey | `src/Main.cpp` (`EquipManager` equip callback) | `key N (sN-1)` |
| Wheeler | `src/Main.cpp` (`publishWheelerEquip`) | `Huginn wheel` |
| External | `src/learning/ExternalEquipLearner.cpp` (`OnExternalEquip`) | `inventory menu` / `favorites menu` / `magic menu` / `vanilla hotkey` / `own wheel` |

The source is a **label, never a weight**: it feeds the logs and the goal
metrics (goal 1 is defined by it), and a selection teaches the same whatever
device made it.

**Confirmation.** `Select` records a pending selection with the game state at
that moment (`EquipEventBus::Capture`), the pipeline's whole last run
(`PipelineStateCache::TakeSnapshot`) and the load generation. It confirms:

| Kind | Items | Confirms when | Window |
|---|---|---|---|
| Consumable | potion, food, poison | its count drops for real (`OnConsumed`, from the inventory delta scan; drops/sells/stores excluded by `InventoryExitTracker`) | `CONSUMPTION_HUGINN_WINDOW_MS` (2.5 s) |
| Equip | weapon, spell, scroll, ammo, torch, apparel | still equipped at the deadline (`Update`), or its count drops first (a scroll cast at once) | `SELECTION_CONFIRM_MS` (3 s) |
| Soul gem | soul gem (a Huginn key only) | at once -- the key calls back only when the recharge succeeded, and a reusable gem (Azura's Star) is emptied, not removed, so it has no count drop to wait for | none |

Anything not confirmed expires and teaches nothing (logged as
`[Selection] Not confirmed`). One pending record per item: both hands, a
doubled `TESEquipEvent`, a key press plus the equip event it causes -- one
selection. A Remembrance swap-back is not a selection (any record its own equip
event created is withdrawn), and it unequips the item it undid, so that item
does not confirm either. Selecting Unarmed confirms on an empty right hand. Pending selections are cleared
on game load.

A **count drop with no pending selection** teaches nothing. It still frees the
slot and starts the candidate cooldown (`HandleConsumption`,
`src/UpdateLoop.cpp`) -- bookkeeping, not learning. So a script drinking a
potion, or a held scroll being cast long after it was equipped, earns nothing.

**Why (2026-10-02, roadmap "The reward path").** Before this, a drink trained
twice (+8 for the key, then +5 when the delta scan saw the count drop), the late
consumption event fired false misclick penalties on whatever was equipped next,
the learner trained on the state at *detection* time (1.3 s after the potion
started working), and scripted equips counted as the player.

Subscribers are registered once, in `src/Main.cpp` step 5b, after
`g_featureBanditLearner` and `g_usageMemory` exist.

**Lock ordering** (documented in `EquipEventBus.h`): StateManager shared locks
(inside `Capture`) → bus `m_mutex` → subscriber internal locks. `Capture` takes
no bus lock, and `Dispatch` snapshots the subscriber list under `m_mutex` and
calls subscribers outside it. `SelectionTracker` captures and dispatches
outside its own mutex too.

### EquipEvent

```cpp
struct EquipEvent {
    RE::FormID       formID = 0;
    EquipSource      source = EquipSource::Hotkey;   // Hotkey, Wheeler, External -- a label
    SelectionKind    kind = SelectionKind::Equip;    // Consumable or Equip
    std::string      via;                            // "key 3 (s2)", "inventory menu", ...
    std::string      attribution;                    // External only: the A-E case label
    StateFeatures    features{};                     // Press-time (Capture)
    State::GameState gameState{};                    // Press-time (UsageMemory context hash)
    PipelineStateCache::Snapshot shown{};            // Press-time scored list + page
    uint32_t         loadGeneration = 0;
    float            confirmMs = 0.0f;
};
```

`Capture` derives `features` and `gameState` from the **same** player/targets
copies, so the continuous and discretized views of the same moment can't
disagree. `WorldState` is fetched separately and may skew slightly — accepted,
and stated in the code.

### Subscribers

| Subscriber | Action |
|------------|--------|
| **BanditSubscriber** | `FeatureBanditLearner::Update(formID, features, RewardFor(kind))` |
| **UsageMemorySubscriber** | `UsageMemory::RecordUsage` (recency boost) |

The cooldown is no longer a subscriber: it follows the count drop itself, so a
potion goes on cooldown whoever drank it.

### Reward Calculation

One confirmed selection, one reward (`RewardFor`, `src/learning/EquipEvent.h`):

```cpp
kind == SelectionKind::Consumable ? Config::CONSUME_REWARD   // 5.0
                                  : Config::EQUIP_REWARD;    // 8.0
```

No source filtering, no multipliers. (The choice target, roadmap Phase 3 #1,
replaces both values with 1.)

### Selection Log

`SelectionLog::Write` runs at each confirmation, before the dispatch, so it
records the learner's prediction for the press-time state before this
selection's update. Two outputs:

- **The debug log:** a `[Selection] Confirmed ...` header (source, via, kind,
  reward, how it confirmed, rank / utility / ctx / prediction / need of the
  chosen item, `over=` the best-ranked same-need item it passed over, load
  generation) at info, then one line per item on the page at debug, with the chosen item marked
  `*`, same-need items `=`, and override / wildcard / Remembrance slots
  `[O]` / `[W]` / `[R]`. "Need" is the primary slot class
  (`SlotClassifier::Classify`).
- **`Huginn_Selections.jsonl`** in the SKSE log folder, one JSON object per
  selection: press-time `phi`, the wildcard odds, the page (slot index and
  assignment type) and **every scored candidate** with its full score
  breakdown and the learner's press-time prediction. Each record names its
  candidate columns (`cols`). Appended across sessions, instances and
  characters, so each record says where it came from (v2, 0.22.10): `list`
  (the modlist folder the game runs from), `launch` (UTC start of the game
  launch) and `gen` (the load within it -- what a death-and-reload
  abandoned). This is the input for re-ranking logged selections
  offline under a new formula. The game thread only takes the predictions
  (one learner lock) and queues the record; a background writer formats it
  and appends it to a stream it keeps open, flushed per record (0.22.10).

---

## Learning Update

### Learning Signal Sources

> **Design Principle (v0.13.0+):** Learning is decoupled from the presentation layer (Wheeler/Widget). The system learns exclusively from confirmed player selections. Negative signals come from time-based weight decay and L2, not from Wheeler open/close events -- and, since v0.22.9, not from misclicks either: an item swapped away inside the confirm window is simply never confirmed.

A confirmed selection is the only explicit learning signal. Every source earns
the same: `EQUIP_REWARD` (+8.0) for an equip, `CONSUME_REWARD` (+5.0) for a
consumable.

1. Huginn slot keys (`EquipManager` callback)
2. Huginn's Wheeler wheel (`OnItemActivated` → `publishWheelerEquip`)
3. **Outside selections** -- inventory / favorites / magic menu, vanilla
   hotkeys, the player's own Wheeler wheels -- via `ExternalEquipLearner`,
   gated on player input

The inventory delta scan keeps a teardown heuristic (`TEARDOWN_MIN_DROPS = 3`,
`TEARDOWN_DROP_RATIO = 0.5`) that suppresses the whole-inventory-to-zero scan a
quit-to-main-menu produces. The post-load grace window
(`CONSUMPTION_POST_LOAD_GRACE_MS`) was removed in v0.22.9: alt-start strips
used to train the learner because every removal did, but now a removal only
confirms a selection, and pending selections are cleared on load -- while a
real key drink in the first seconds after a load still has to confirm.

### Outside Selections and Attribution

`ExternalEquipLearner::OnExternalEquip` handles a `TESEquipEvent` Huginn did not
cause. It first asks **`PlayerInputGate`** (`src/learning/PlayerInputGate.h`)
whether the player made it -- an Inventory, Favorites or Magic menu open, or
closed within `MENU_CLOSE_INPUT_WINDOW_MS` (2 s; LoreRim closes the inventory
as the player drinks, before the potion's equip event arrives), a
vanilla favourites hotkey (`Hotkey1`-`Hotkey8`) within
`PLAYER_INPUT_WINDOW_MS` (1 s), a pick of that item off one of the player's
own Wheeler wheels, or any Wheeler wheel open. No input, no selection: a
script's equip teaches nothing (LoreRim's auto-quaff trained the learner as the
player until this gate).

The open-wheel rule exists because Wheeler equips an item **before** it calls
Huginn back (measured on LoreRim 2026-10-02: equip event at .096, callback at
.102). So every Huginn-wheel pick first arrives here as an outside selection;
when Huginn's own selection of the same item follows, `SelectionTracker::Select`
relabels the pending record as `Wheeler` and drops its attribution. If the pick
was a Remembrance swap-back, the callback **withdraws** the record instead
(`SelectionTracker::Withdraw`), so an undo is never rewarded.

An equip event for an item that already has a pending selection is part of it
and is ignored here, before the input gate. That is the common case for a
Huginn key's potion, whose equip event lands ~0.85 s after the press -- past
`EquipSourceTracker`'s 400 ms mark. The A-E case is recorded
to the soak telemetry only when an outside selection **confirms**, so neither a
relabelled Huginn pick nor an unconfirmed one reaches accept%.

It then labels the equip with what the pipeline "thought" of it
(`PipelineStateCache`). The label goes to the soak telemetry (accept%, goal 1)
and the selection log. **It is not a reward weight** -- the multipliers that
used to scale these cases (0.2x-1.0x, and 0 for case E) were removed with the
one selection path:

```
Case A: Not a candidate
Case B-low: Low rank (far miss)
Case B-med: Mid rank
Case C: Near-miss (not shown)
Case D: Displayed, page changed since the snapshot
Case E: Displayed, current page (Huginn already surfaced it)
```

**Case D is narrower than it reads.** `PipelineStateCache` only records
assignments for the page that was current when it snapshotted, so an item shown
on some *other* page never enters the displayed set at all. What separates D
from E is comparing the snapshot's `displayPage` against the **live** page
(`m_env.currentDisplayPage()`), so D fires only when the player changed pages
between the last pipeline run and the equip.

**Rank thresholds** are slot-relative. With `displayedCount` slots shown,
`overshoot = max(0, rank - displayedCount)`:

- `overshoot <= NEAR_MISS_SLOTS` (2) → Case C, near-miss
- `overshoot <= FAR_MISS_SLOTS` (5) → Case B-med, mid rank
- `overshoot > 5` → Case B-low

**Skip filters** (`ShouldSkip`). Each returns a one-character reason code that
is recorded by `SoakMetrics::RecordEquipSkip`, so the soak heartbeat can
distinguish "nobody equipped anything" from "every equip was filtered":

| Code | Filter |
|---|---|
| `x` | Master toggle `bLearnFromExternalEquips` is off |
| `n` | A consumable with no player input behind it (a script) -- counted as `skipped=input` |
| `e` | A non-consumable with no input: the engine refilling the quiver, a bound weapon arriving. Debug log only, not counted |
| `s` | Pipeline cache older than `fExternalEquipTimeWindow` |

The wheel-open (`w`) and anti-spam (`a`) filters were removed in v0.22.9:
Huginn's own wheel picks are marked Huginn equips and never get here, the
player's own wheel picks are now selections, and a repeat event for one pick
merges into its pending selection.

Two further guards sit around this path:

- **Environment wiring gate.** `currentDisplayPage` is injected by the
  composition root in `Main.cpp` rather than reached for upwards. If it is
  unwired, `OnExternalEquip` bails *before* recording any telemetry, so a
  wiring fault can't be mistaken for a recommendation hit.
- **Huginn equips are not outside selections** -- `EquipSourceTracker`
  (`src/learning/EquipSourceTracker.h`): Huginn equip sites call
  `MarkHuginnEquip(formID)` before triggering the equip, and the external
  listener checks `IsRecentHuginnEquip(formID)` within a
  `DEFAULT_WINDOW_MS = 400` window. Matching is **per-FormID** (a fixed 8-entry
  ring), so a Huginn equip of X can't suppress a genuine external equip of Y,
  and entries are kept until expiry rather than consumed on first match —
  the game can fire multiple `TESEquipEvent`s for one action.

### Weight Decay

FeatureBanditLearner uses **two complementary decay mechanisms**:

**1. L2 regularization (on every update).**
The `- LEARNING_RATE * L2_LAMBDA * w[i]` term in `Update` pulls weights toward
zero. Items that keep being equipped outpace this pull; items that stop
receiving updates keep their last-trained weights until time decay takes over.

**2. Lazy time-based decay (once per scoring pass).**
`MaybeDecayBatch` (`FeatureBanditLearner.cpp:54`) is called once per
`ScoreCandidates` with the whole candidate pool
(`UtilityScorer.cpp:56-63`). Items idle longer than
`DECAY_THRESHOLD_MINUTES` (5) have their weights multiplied by
`(1 - DECAY_RATE_PER_HOUR)^elapsedHours` (2%/hr), then `lastUpdate` is stamped
to `now` so the next pass doesn't re-decay the same interval. This is lazy — it
only touches items about to be scored, never a global sweep.

It is also **two-phase**: one `shared_lock` collects the items that cross the
threshold, and the `unique_lock` phase is skipped entirely when nothing
qualifies (the common case). The second phase re-checks each item, because an
`Update` from the game thread may have refreshed it between the locks.

```cpp
// src/Config.h
inline constexpr float DECAY_RATE_PER_HOUR    = 0.02f;  // 2%/hr exponential decay
inline constexpr float DECAY_THRESHOLD_MINUTES = 5.0f;  // Don't decay within 5 min
```

**Why skip penalties were removed (v0.12.x):**
- Skip penalties punished correct recommendations during state transitions (e.g. combat ending while wheel was open)
- All visible items received identical -1.0 regardless of rank
- L2 regularization + time-based decay provide the same "drift toward zero" signal without these failure modes

`SKIP_PENALTY` no longer exists anywhere in `src/`.

### Misclick Detection (removed in v0.22.9)

`UsageMemory` used to penalise "a different item, same context hash, within
3 s" by -3.0. The consumption event arrived late, so "drink, then equip a
spell" penalised the potion and then the spell, and stamina was not in the hash.
The selection confirm window replaced it: an item swapped away inside the
window is never confirmed, so it earns nothing -- and nothing is penalised.

### Wheeler Callback Integration

Huginn receives feedback through Wheeler's callback system, registered in
`src/wheeler/WheelerConnection.cpp`:

```cpp
api->RegisterItemActivatedCallback(itemCb);   // WheelerClient::OnItemActivated
api->RegisterWheelStateCallback(wheelCb);     // WheelerClient::OnWheelStateChanged
```

**ItemActivatedCallback:** fired when the player selects an item from any wheel.
On a Huginn wheel it is a selection (`SelectionTracker::Select`,
`EquipSource::Wheeler`). On one of the player's own wheels it is noted as player
input (`PlayerInputGate::NoteOwnWheelPick`), so the equip event that follows
counts as an outside selection.

**WheelStateCallback:** fired when the wheel opens/closes. Used for Wheeler UX
behaviour (page sync, focus, edit-mode handling). It applies **no** learning
penalty.

**Version support:** `WheelerAPI::API_VERSION_MIN = 1`,
`API_VERSION_MAX = 4` (`src/wheeler/WheelerAPI.h:23-24`). Both callbacks are
registered on any accepted version; v2+ adds subtext, v3 batch delete, v4 batch
lookup plus managed wheels surviving Wheeler's load-time reset. See
[Mod Compatibility](https://github.com/LandingCrew/huginn/wiki/Mod-Compatibility#wheeler-integration)
for version differences.

---

## Learning Signals

| Signal | Value | Source | Purpose |
|--------|-------|--------|---------|
| Equip reward | +8.0 | A confirmed selection of a worn/held item, any device | One reward per selection |
| Consume reward | +5.0 | A confirmed selection of a consumable, any device | One reward per selection |
| L2 regularization | Continuous | Applied during each weight update | Pulls weights toward zero |
| Time-based decay | Lazy | `MaybeDecayBatch` before scoring | 2%/hr exponential decay on idle items |

**Removed signals:** Skip penalty (-1.0) removed in v0.12.x (replaced by implicit decay via L2 regularization + time-based decay). In v0.22.9: the misclick penalty (-3.0), the separate consumption reward that made every drink train twice, and the external attribution multipliers.

### Feedback Loop

```
+-----------------------------------------------------------------------------+
|                     CONTEXTUAL BANDIT FEEDBACK LOOP                          |
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
|                    |    (18-float context)   |                               |
|                    +-----------+-------------+                               |
|                                |                                             |
|            +-------------------+-------------------+                         |
|            |                   |                   |                         |
|            v                   v                   v                         |
|   +----------------+  +----------------+  +----------------+                 |
|   | Context Weight |  | Reward est. Q  |  |  Exploration   |                 |
|   |  (heuristic)   |  |  (w . phi)     |  |    (UCB)       |                 |
|   +-------+--------+  +-------+--------+  +-------+--------+                 |
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
|                    +-----------+-----------+                                 |
|                    |                       |                                 |
|                    v                       v                                 |
|             +-------------+        +-------------+                           |
|             |  SELECT     |        |   IGNORE    |                           |
|             | (confirmed: |        |  (time decay|                           |
|             |  +8 or +5)  |        |   handles)  |                           |
|             +------+------+        +-------------+                           |
|                    |                                                         |
|                    v                                                         |
|          +-------------------------+                                         |
|          |   EQUIP EVENT BUS       |                                         |
|          |  -> bandit reward          |                                         |
|          |  -> Usage memory        |                                         |
|          +-----------+-------------+                                         |
|                      |                                                       |
|                      v                                                       |
|          +-------------------------+                                         |
|          |   WEIGHT UPDATE         |                                         |
|          | w += alpha(r - w.phi)phi |                                        |
|          |     - alpha*lambda*w    |                                         |
|          +-------------------------+                                         |
|                                                                              |
+-----------------------------------------------------------------------------+
```

Note the loop closes on **immediate reward only**. The "PLAYER ACTION" box feeds
a reward back into the weight update for the *same* context that produced the
recommendation; no arrow carries a successor state forward.

---

## Interpreting Weights

The learned weights are interpretable, and `hg weights <hex FormID>` prints them
per feature (`src/console/ConsoleCommands.cpp`, `kFeatureNames`, which carries a
`static_assert` tying it to `NUM_FEATURES`):

```
  healthPct     [0]      inCombat    [3]      tgtNone      [6]      melee  [13]
  magickaPct    [1]      isSneaking  [4]      tgtHumanoid  [7]      bow    [14]
  staminaPct    [2]      distNorm    [5]      tgtUndead    [8]      spell  [15]
                                              tgtBeast     [9]      shield [16]
                                              tgtConstruct [10]     bias   [17]
                                              tgtDragon    [11]
                                              tgtDaedra    [12]
```

The command also prints the live `Q`, `conf` and `ucb` for that item evaluated
against the *current* state, and reports "no training data" when
`trainCount == 0`.

Example interpretation:
```
Fireball weights:
  inCombat:       2.1    -> "Used in combat"
  distNorm:       1.5    -> "Used at range"
  tgtUndead:     -0.5    -> "Not used against undead"
  bias:           0.3    -> "Generally useful"
```

---

## Utility Calculation

Final utility combines context weights (heuristic), reward estimates (learned),
priors, exploration, recency, and favorites. The formula lives in exactly one
place — `UtilityScorer::ComputeUtility` (`src/learning/UtilityScorer.cpp:318`) —
used by both the normal path and the cold-start fallback.

```cpp
// Step 1: Heuristic relevance. Evaluated ONCE per pass for all candidates
//         (ContextRuleEngine::EvaluateRules), then mapped per candidate.
float contextWeight = Context::WeightForCandidate(candidate, weights);

// Step 2: Learning metrics — from the LockedReader in the batch path,
//         or FeatureBanditLearner::GetMetrics on the single-candidate path.
//         metrics.rewardEstimate, metrics.ucb, metrics.confidence

// Step 3: Intrinsic quality prior. NOTE: no GameState parameter —
//         priors are deliberately not context-aware.
float prior = m_priorCalc.CalculatePrior(player, candidate);

// Step 4: learningScore = α*Q + (1-α)*prior + β*UCB
float alpha = metrics.confidence;
float beta  = m_config.explorationWeight;          // fExplorationWeight = 0.2
float learningScore = alpha * metrics.rewardEstimate
                    + (1.0f - alpha) * prior
                    + beta * metrics.ucb;

// Step 4b: Recency boost from UsageMemory (event-driven short-term recall).
//          ADDITIVE to learningScore — context weight still gates final utility.
learningScore += recencyBoost;

// Steps 5-6: correlation and potion multipliers
float correlationBonus = m_correlationBooster.CalculateBonus(player, targets, candidate);
float potionMultiplier = m_potionDiscrim.GetMultiplier(state, player, candidate);

// Step 7: Provisional favorites multiplier (rank 0 of 1 = favoritesBoostMax).
//         ScoreCandidates later replaces it with the rank-scaled value —
//         see ApplyFavoritesRankScaling.
float favoritesMultiplier = GetFavoritesMultiplier(candidate, 0, 1);

// Step 8: adaptive lambda + final utility
float lambda = ComputeAdaptiveLambda(metrics.confidence);
// λ(confidence) = lambdaMin + confidence * (lambdaMax - lambdaMin)
// confidence=0 → λ = 0.5 (context dominates); confidence=1 → λ = 3.0 (6x amplification)

return contextWeight * (1.0f + lambda * learningScore)
       * correlationBonus * potionMultiplier * favoritesMultiplier;
```

`recencyBoost` is `UsageMemory::RECENCY_BOOST` (1.5), granted when at least
`MATCH_THRESHOLD` (3) events for the same FormID *and* the same context hash
sit in the 20-slot ring buffer. `UsageMemory` is read through a
`SnapshotReader` that copies the ring under a brief shared lock and then scans
the local copy with no lock held, so scoring never blocks `RecordUsage` on the
equip path.

### PriorCalculator

`PriorCalculator` (`src/learning/PriorCalculator.h/.cpp`) answers "which item is
intrinsically better?" — never "is it relevant now?". Its signature takes
`PlayerActorState` only (for the ammo/equipped-weapon compatibility check) and
deliberately **no** `GameState`, because accepting one would falsely suggest
priors were context-aware. It returns `[0, 1]` from `BASE_PRIOR = 0.3` adjusted
by intrinsic properties only:

| Factor | Constant | Effect |
|---|---|---|
| Potion magnitude (log-scaled) | `MAGNITUDE_REFERENCE_VALUE` 100, `MAGNITUDE_SCALE_FACTOR` 0.15 | up to +0.15 |
| Spell cost | `MAX_REASONABLE_SPELL_COST` 200, `COST_SCALE_FACTOR` 0.1 | up to +0.1 |
| Inventory scarcity | `LOW_COUNT_THRESHOLD` 5, `COUNT_PENALTY_SCALE` 0.1 | up to -0.1 |
| Weapon charge depletion | `CHARGE_PENALTY_SCALE` 0.2 | up to -0.2 |
| Ammo scarcity | `AMMO_LOW_COUNT_THRESHOLD` 20, `AMMO_SCARCITY_SCALE` 0.1 | up to +0.1 |
| Ammo fits equipped weapon | `AMMO_TYPE_MATCH_BONUS` 0.15 | +0.15 |

Because `alpha = confidence`, an untrained item scores almost entirely on its
prior; a well-trained one scores almost entirely on learned weights.

### CorrelationBooster and PotionDiscriminator

Both sit *outside* the `(1 + λ·learningScore)` bracket as plain multipliers, so
they scale the whole utility rather than the learned part. `CorrelationBooster`
encodes equipment/target synergies whose values live in `[Scoring]`:
`fBowArrowBonus` and `fCrossbowBoltBonus` (2.0), `fMeleeDefensiveBonus` (1.5),
`fSilverUndeadBonus` (2.0), `fFortifySchoolBonus` (2.0),
`fStaffLowMagickaBonus` (1.5), `fTwoHandedDefensiveBonus` (1.2).
`PotionDiscriminator` handles combat-timing discrimination
(`fCombatStartWindow` 10s, `fRegenPotionCombatStartMult` 1.5,
`fFlatRestoreLowResourceMult` 1.5) and returns 1.0 for non-potions. Ordering
the strengths of one potion is `UtilityScorer::ApplyPotionTierPreference`
(`sPotionTierPreference`, default Higher), which needs the whole list. Neither feeds back into the learner.

---

## Cold-Start UCB Fallback (Phase 1.5)

With the multiplicative formula, items with no context signal only receive
`baseRelevanceWeight = 0.05`. Even with maximum UCB and prior, the final utility
caps below `fMinimumUtility = 0.1` — resulting in empty slots on all-Regular
pages despite hundreds of items in the registries.

**Solution:** when fewer candidates pass the normal scoring path than
`topNCandidates` (10), a second pass runs with UCB-boosted context weights
(`src/learning/UtilityScorer.cpp:112-175`):

```cpp
boostedContext = max(contextWeight, coldStartUCBBoost * metrics.ucb);
```

- Untried items have UCB exactly 1.0 → `boostedContext = 0.2`, above
  `fMinimumContextWeight = 0.05`, and the resulting utility clears
  `fMinimumUtility`
- As items accumulate trains, UCB drops → the fallback self-heals
- The pass dedups against already-scored candidates by linear scan (cheap:
  the branch only runs when fewer than 10 candidates scored) and stops as soon
  as `topNCandidates` is reached
- Empty Regular slots show `(Learning...)` (`src/slot/SlotUtils.h:58`)
- Configured via `fColdStartUCBBoost = 0.2` in `[Scoring]` (0.0 disables)
- `isColdStartBoosted` on `ScoredCandidate` flags it; `hg recs` prints `[COLD]`

---

## Two Exploration Mechanisms

Huginn explores in two independent places, and it is worth keeping them apart:

| | UCB term | Wildcards |
|---|---|---|
| Owner | `FeatureBanditLearner::ComputeUCB` | `WildcardManager` (`src/learning/WildcardManager.h/.cpp`) |
| Granularity | Per item, continuous | Per slot, stochastic |
| Where it acts | Inside `learningScore` (β·UCB) and the cold-start context floor | *After* the top-N sort, by swapping a lower-ranked candidate up into a slot |
| Decays with | Per-item train count | A cooldown timer, not learning |

`WildcardManager::ApplyWildcards` runs at the end of `ScoreCandidates`, against
the `WildcardPage` the *pipeline snapshot* says this tick allocates — never a
live `SlotAllocator` read, because a page switch landing mid-tick would
otherwise roll wildcards against one page while the pipeline allocates another.

Since v0.19.6 the wildcard cache is **per page**: each page owns its slot array,
its cooldown and its refractory timer, and each array records the page *shape*
(`slotCount`, `wildcardSlots`) it was rolled against, so an INI hot-reload that
resizes a page or toggles `bWildcardsEnabled` discards that page's cache
wholesale. This closed a family of "a cached wildcard nothing can display" bugs
that suppressed re-rolls.

Configured in `[Wildcards]`:

```ini
; P(slot i) = fBaseProbability * i, capped at fMaxProbability.
; Slot 0 is always excluded (it is the top-scored pick).
fBaseProbability = 0.165
fMaxProbability = 0.5
fCooldownSeconds = 30
fRefractorySeconds = 60
```

Slot classification and the multi-page layout are covered in
[5-slots.md](5-slots.md).

---

## Favorites Multiplier

The favorites system is **separate from the bandit** — it applies a
post-processing multiplier to utility based on whether the player marked an item
as favorited in Skyrim's own favorites menu.

### Architectural Separation

```
+-------------------------------------------------------------+
|  BANDIT (learns from actions)                               |
|  v                                                          |
|  learningScore = alpha*Q + (1-alpha)*prior + beta*UCB       |
|                  + recencyBoost                             |
|                                                             |
|  FAVORITES (manual preference override)                     |
|  v                                                          |
|  favoritesMultiplier = 1.0 (non-favorited)                  |
|                     or 1.3-2.5 (favorited, rank-scaled)     |
|                                                             |
|  COMBINED                                                   |
|  v                                                          |
|  utility = contextWeight * (1 + lambda*learningScore)       |
|            * correlationBonus * potionMultiplier            |
|            * favoritesMultiplier  <-- Applied at the end    |
+-------------------------------------------------------------+
```

**Key point:** favoriting an item does NOT affect learned weights. It only
scales the final utility.

Favorites also get two membership guarantees in `ScoreCandidates`:

1. They **bypass the `minimumContextWeight` early filter**, so an explicitly
   favorited item stays observable by the learner even when context weight is
   low.
2. `ApplyFavoritesRankScaling` deliberately does **not** re-filter on
   `minimumUtility` after rescaling. Rank scaling corrects *order*, not
   membership — erasing there would under-fill slots after the cold-start
   fallback had already run, and near-threshold favorites would flicker in and
   out across ticks.

### Rank scaling (implemented — `ApplyFavoritesRankScaling`)

`ScoreCandidateInternal` Step 7 assigns every favorite the same *provisional*
multiplier (`rank 0 of 1` = `favoritesBoostMax`). After the scoring loop and the
cold-start fallback, and before the top-N sort,
`UtilityScorer::ApplyFavoritesRankScaling`
(`src/learning/UtilityScorer.cpp:377`) replaces it with the rank-scaled value:

```cpp
// Collect favorite indices, sort them by current utility, rewrite multipliers.
for (size_t rank = 0; rank < total; ++rank) {
    entry.breakdown.favoritesMultiplier = m_config.GetFavoritesMultiplier(rank, total);
    entry.utility = ComputeUtility(entry.breakdown);
}
```

Two properties make this cheap and correct:

- Because Step 7 gave every favorite the *same* provisional multiplier, ranking
  them by current utility equals ranking them by pre-boost utility.
- The multiplier is monotone non-increasing in rank, so rewriting utilities
  cannot reorder favorites relative to each other — no re-sort is needed inside
  the helper; the caller's `partial_sort` establishes final order.

With 0 or 1 favorites the pass returns immediately: a single favorite is defined
as rank 0 of 1, which is what Step 7 already wrote. The single-candidate
`ScoreCandidate` path has no cohort to rank against, so `favoritesBoostMax` is
its final value there.

### Three Modes

Configured via `[Favorites]` in `Huginn.ini`, loaded by `ScorerSettings`:

| Mode | Behavior | Use Case |
|------|----------|----------|
| **Boost** (default) | Favorited items get a 1.3x–2.5x multiplier, scaled by rank among favorites | Surface preferred items alongside learned recommendations |
| **Off** | Favorites flag is ignored (multiplier 1.0) | Pure learning-driven recommendations |
| **Suppress** | Favorited items get multiplier **0.0** | "I already have these in my favorites menu, show me other options" |

Suppress zeroes the utility rather than skipping the candidate outright, so the
item is filtered by `minimumUtility` on the way out.

### Boost Multiplier Formula

`ScorerConfig::GetFavoritesMultiplier` (`src/learning/ScorerConfig.h:133`):

```cpp
float GetFavoritesMultiplier(size_t rank, size_t totalItems) const noexcept {
    if (favoritesMode != FavoritesMode::Boost || totalItems == 0) return 1.0f;
    float t = (totalItems > 1)
        ? static_cast<float>(rank) / static_cast<float>(totalItems - 1)
        : 0.0f;
    return favoritesBoostMax - t * (favoritesBoostMax - favoritesBoostMin);
    // Default: 2.5 - t * (2.5 - 1.3)
}
```

`rank` and `totalItems` are **relative to the favorites cohort**, not to the
whole candidate list: the strongest favorite gets 2.5x, the weakest 1.3x, linear
in between.

### Limited Scope

Only **spells** and **weapons** carry a favorites flag
(`Candidate::IsFavorited`, `src/candidate/CandidateTypes.h:254`):

```cpp
[[nodiscard]] inline bool IsFavorited(const CandidateVariant& v) noexcept {
    return std::visit([](const auto& c) -> bool {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SpellCandidate> ||
                      std::is_same_v<T, WeaponCandidate>) {
            return c.isFavorited;
        } else {
            return false;  // Items, scrolls, ammo don't have favorites
        }
    }, v);
}
```

Potions, items, scrolls, and ammo therefore always receive
`favoritesMultiplier = 1.0`.

### INI Configuration

```ini
[Favorites]
; sFavoritesMode: How to handle favorited items
;   Boost    - Favorited items get a utility multiplier (default)
;   Off      - Ignore favorites flag entirely
;   Suppress - Skip favorited items (they're already in your favorites menu)
;
; fFavoritesBoostMin/Max (Boost mode): favorites are ranked by utility each
; update; the strongest favorite's utility is multiplied by Max, the weakest
; by Min, linear in between. A single favorite gets Max.

sFavoritesMode = Boost
fFavoritesBoostMin = 1.3
fFavoritesBoostMax = 2.5
```

### Example: Close Wounds (Favorited)

Scenario: player has favorited "Close Wounds", health at 40%, and it is the
strongest of their favorites this tick.

**Without favorites (mode = Off):**
```
contextWeight = healingWeight (e.g. 2.43 from the health formula)
learningScore = 0.7*2.5 + 0.3*1.2 + 0.2*0.3 = 2.17
lambda = ComputeAdaptiveLambda(0.7) = 0.5 + 0.7*(3.0-0.5) = 2.25
utility = 2.43 * (1 + 2.25*2.17) * 1.0 * 1.0 * 1.0 = 14.3
```

**With favorites boost (mode = Boost, rank 0 of the favorites cohort):**
```
Same calculation, but final step:
utility = 14.3 * 2.5 = 35.75  <-- 2.5x boost pushes to top
```

A favorite ranked *last* among N favorites would instead take 1.3x.

### Why Separate from Learning?

**The bandit learns:** "This item works well in this situation" (context-dependent, data-driven)

**Favorites declare:** "I like this item generally" (context-independent, player-driven)

Keeping them separate allows:
1. **Cold-start assistance** — favorited items surface even with no learned data
2. **Manual override** — the player can boost items the learner hasn't discovered yet
3. **Clean learning** — reward estimates reflect actual use, not favorited status
4. **User control** — `sFavoritesMode = Off` gives pure learning-driven recs

---

## The Bandit is Slot-Agnostic

Important: the learner learns item preference in **game context**, not slot
context.

`EquipEvent` carries a `FormID`, an `EquipSource` label, the selection kind,
the 18-float context and the discretized `GameState`. Its page snapshot (for the
selection log) has slot indices, but the learner never reads them. Nothing in
`FeatureBanditLearner`, `StateFeatures` or the cosave record references a slot or a
page. The learner is told *what* was equipped in *what situation*; the slot only
determined visibility.

```cpp
// The Wheeler selection site (src/Main.cpp) — note the absence of a slot index
.publishWheelerEquip = [](RE::FormID formID) {
    Learning::SelectionTracker::GetSingleton().Select(
        formID, Learning::EquipSource::Wheeler, "Huginn wheel");
},
```

Page identity reaches only `ExternalEquipLearner`'s Case D/E label, which is
telemetry -- since v0.22.9 it no longer scales any reward.

---

## Persistence

### SKSE Cosave

All learning data is persisted via SKSE's cosave system, which saves/loads
alongside the player's save files. `BanditSerializer`
(`src/persist/BanditSerializer.h/.cpp`) uses the static buffer pattern,
handling the case where `Load` fires before the global `g_featureBanditLearner`
exists: `LoadCallback` fills `s_pendingBanditData`, and `ApplyPendingBanditData` moves
it into the learner once `Main.cpp` has constructed it.

**Record types:**
- `HCID` — which character the save belongs to and how far its learning had
  got: a 64-bit character ID and the learner's 64-bit learning clock
  (`kRecordType_CharacterID = 'DICH'`, `'HCID'` on disk; v2, 0.22.11; v1 held
  the ID only and is still read)
- `BNDW` — FeatureBanditLearner weight vectors plus the global train count
  (`kRecordType_BanditWeights = 'WDNB'`, `'BNDW'` on disk;
  `kUniqueID = 'QCNO'`, `'ONCQ'` on disk)

**Serialization callbacks** (registered from `SKSEPlugin_Load` via
`RegisterSerialization`):
- `Save` → write the character ID and learning clock, then export learner weights
- `Load` → read them and import into the static buffer
- `Revert` → drop the buffer. It no longer clears the learner (0.22.11).

**Learning survives a reload** (0.22.11, `Persist::ResolveLoadedLearner`, called
at `kNewGame` / `kPostLoadGame`). The learner keeps a **learning clock** that
ticks on every `Update()` and every `Clear()`; it is saved with the character
ID. Then:

| Load | Learner after it |
|---|---|
| New game | Cleared; new random character ID |
| Load that failed (`kPostLoadGame` data false) | Unchanged, ID unchanged |
| Same character, save's clock NOT ahead of memory (a reload) | In-memory learning kept; dynamic-form (`0xFF`) entries taken from the save, because the engine can reuse those IDs for different forms |
| Different character, first load since launch, or a LATER save of the same character (its clock is ahead) | The save's learner, as before |

So a death-and-reload keeps the fight, loading a later save does not throw that
save's learning away, and `hg reset weights` (a `Clear()`, so a clock tick)
cannot be undone by reloading an older save. A save from before 0.22.11 gets an
ID derived from the player's name and race at its first load -- stable across
loads of that character's old saves -- and train counts stand in for the clock
until its next save writes one.

**BNDW record format (version 2):**
```
[version: uint32]                = 2   (also in the SKSE record header; cross-checked)
[numFeatures: uint32]            = 18  (validated, then MIGRATED if it differs)
[totalTrainCount: uint32]        = global train counter
[numItems: uint32]
Then ONE contiguous blob of numItems fixed-stride entries:
  [formID: uint32]               -> resolved via ResolveFormID
  [weights: float[numFeatures]]  -> per-item weight vector
  [trainCount: uint32]           -> per-item training count
  [minutesSinceLastUpdate: u32]  -> v2: minutes idle at save time
```

The entry array is written as a single bulk blob rather than 21 calls per item.
Two `static_assert`s lock that in: `SerializedEntry` must be trivially copyable
and tightly packed to the exact field sum, so adding or reordering a field
fails to compile instead of silently writing an incompatible blob. The byte
order is identical to the older per-field layout, so v2 saves round-trip
between the batch and per-field code paths.

**Load-side robustness:**

| Guard | Behaviour |
|---|---|
| Version | Accepts 1 and 2. v1 entries lack `minutesSinceLastUpdate` and are read per field, treated as fresh (`0` minutes). A SKSE-header/in-data version mismatch is logged, and the in-data version is trusted |
| Feature count | **Migrated positionally, not rejected.** Fewer features on disk → tail zero-pads (new features start untrained); more → tail truncates. Sound only because the vector is APPEND-ONLY. `numFeatures` outside `[1, kMaxBanditFeatures=256]` is treated as corrupt and the record is skipped |
| Item cap | `numItems > kMaxBanditItems` (50,000) → record skipped wholesale |
| Short read | v2 bulk read or v1 per-field read that comes up short rejects the record wholesale — never a silent partial import |
| Non-finite weights | Entries containing any non-finite weight are dropped before they can reach the scorer |
| FormID resolution | `ResolveFormID` on every FormID (mod reordering); unresolvable entries are dropped and counted |
| Train-count repair | `totalTrainCount` is **recomputed** as the sum of surviving entries, so trains belonging to dropped/unresolvable items can't inflate the UCB exploration term |

`ExportData` takes a `shared_lock`, `ImportData` a `unique_lock` (and clears
first). `ImportData` reconstructs each `lastUpdate` as
`now - minutes(minutesSinceLastUpdate)`, which is what lets `MaybeDecayBatch`
apply time-based decay across a save/load boundary — items idle before saving
keep decaying proportionally after loading.

`hg reset weights` and the dMenu "reset learning data" button both route through
`SettingsReloader::ResetLearningData`, which runs `FeatureBanditLearner::Clear()`
under the update handler's exclusive lock and also resets `SlotLocker` — without
that, locked slots would keep pinning recommendations scored by the
just-cleared table for the remainder of their lock duration.

---

## Hyperparameters

### FeatureBanditLearner (compile-time, `src/learning/FeatureBanditLearner.h:128-133`)

| Parameter | Default | Description |
|-----------|---------|-------------|
| LEARNING_RATE | 0.1 | Semi-gradient update step size |
| L2_LAMBDA | 0.01 | L2 regularization (implicit weight decay on update) |
| WEIGHT_CLAMP | +/-10.0 | Hard bounds on individual weights |
| CONFIDENCE_MIDPOINT | 5.0 | 50% confidence at 5 trains per item |
| CONFIDENCE_STEEPNESS | 0.3 | Sigmoid steepness (≈82% at 10 trains, ≈95% at 15) |
| UCB_NORMALIZATION_FACTOR | 0.2 | Scales the UCB1 bonus into [0, 1] |

### Rewards and decay (`src/Config.h`)

| Parameter | Default | Description |
|-----------|---------|-------------|
| EQUIP_REWARD | 8.0 | Reward for one confirmed equip selection |
| CONSUME_REWARD | 5.0 | Reward for one confirmed consumable selection |
| DECAY_RATE_PER_HOUR | 0.02 | Exponential weight decay for idle items |
| DECAY_THRESHOLD_MINUTES | 5.0 | Don't decay if updated within this window |
| CONSUMPTION_HUGINN_WINDOW_MS | 2500 | A consumable selection must see its count drop within this |
| SELECTION_CONFIRM_MS | 3000 | An equip selection must still be equipped after this |
| PLAYER_INPUT_WINDOW_MS | 1000 | How recent a vanilla hotkey / own-wheel pick must be for an outside equip |

### UsageMemory (`src/learning/UsageMemory.h`)

| Parameter | Default | Description |
|-----------|---------|-------------|
| BUFFER_CAPACITY | 20 | Ring buffer of recent usage events |
| MATCH_THRESHOLD | 3 | Matching (formID, contextHash) events needed for a boost |
| RECENCY_BOOST | 1.5 | Additive boost to `learningScore` |

### ExternalEquipLearner (`src/learning/ExternalEquipLearner.h`)

| Parameter | Default | Description |
|-----------|---------|-------------|
| NEAR_MISS_SLOTS | 2 | Overshoot ≤ this → Case C |
| FAR_MISS_SLOTS | 5 | Overshoot ≤ this → Case B-med, beyond → B-low |
| MAX_ANTI_SPAM_ENTRIES | 200 | Anti-spam map size before cleanup |
| CLEANUP_AGE_SECONDS | 600 | Age at which anti-spam entries are pruned |
| `EquipSourceTracker::DEFAULT_WINDOW_MS` | 400 | Huginn-equip suppression window (per FormID) |

**Removed parameters:**

| Parameter | Old Default | Reason Removed |
|-----------|-------------|----------------|
| SKIP_PENALTY | -1.0 | Replaced by L2 regularization + time-based decay. Skip penalties punished correct recommendations during state transitions. |
| Tabular hyperparameters | various | Tabular learner fully removed in v0.13.x (alpha, activeDecayRate, passiveDecayRate, etc.) |

---

## INI Configuration

### [Scoring] section

Loaded by `ScorerSettings` into `ScorerConfig`. The parameters this document
depends on:

```ini
[Scoring]
; Formula: utility = contextWeight × (1 + λ(confidence) × learningScore)
;                    × correlationBonus × potionMultiplier × favoritesMultiplier
; Where λ(confidence) = lambdaMin + confidence × (lambdaMax - lambdaMin)

fLambdaMin = 0.5              ; learning weight at zero confidence
fLambdaMax = 3.0              ; learning weight at full confidence
fExplorationWeight = 0.2      ; beta — UCB exploration bonus

fMinimumUtility = 0.1
fMinimumContextWeight = 0.05
fColdStartUCBBoost = 0.2      ; 0.0 disables the cold-start fallback

iTopNCandidates = 10          ; must be >= max slots per page
```

### [Learning] section

Outside-selection parameters, loaded by the `LearningSettings` singleton
and snapshotted into `LearningConfig` (a POD copy, so `ExternalEquipLearner`
reads are race-free). Re-snapshotted on `hg reload` via `SetConfig`.

```ini
[Learning]
; bLearnFromExternalEquips: Master toggle for learning from outside selections
bLearnFromExternalEquips = true

; fExternalEquipTimeWindow: Max cache age for pipeline state to be valid (ms).
; The shipped configs/Huginn.ini uses 500; the compiled default is 2000.
fExternalEquipTimeWindow = 500.0
```

`fExternalEquipMinInterval` and the five `f*RewardMult` keys were removed in
v0.22.9 with the one selection path; an INI that still has them is ignored.

---

## Inspecting and Resetting Learned Weights

### Initial heuristic defaults

Before enough training data exists, `PriorCalculator` supplies the intrinsic
quality term and `ContextRuleEngine` supplies relevance, so recommendations are
usable from the first minute. Because `alpha = confidence`, the learner's own
opinion is weighted in only as it earns it.

### Inspecting

| Command | What it shows |
|---|---|
| `hg status` | Learner item count and total trains |
| `hg weights <hex FormID>` | Per-feature weights, train count, live Q / confidence / UCB |
| `hg recs [N]` | Top-N breakdown (N = 1–50, default 10), plus the current slot assignments; `[WC]` marks wildcards, `[COLD]` cold-start boosts |

`hg recs` queues a **one-shot full-detail dump** that bypasses the verbosity
gate, the 5-second throttle and the membership-change dedup applied to the
periodic `[Recs]` line, then forces a pipeline pass so it logs immediately
(`PipelineCoordinator::LogRecommendations`). Full detail means
`ScoreBreakdown::ToDetailString`: `ctx`, `λ`, `learn` and then the inputs that
produced `learn` — `Q`, `P`, `UCB`, `α`, and `rec` when a recency boost applied.

The periodic line is gated by `DebugSettings::recLogVerbosity`
(0 = off, 1 = compact, 2 = detail) and prints the top 5 at most every 5 seconds.

### When recommendations don't match your playstyle

<!-- UNVERIFIED: the old doc claimed "50-100 actions plus a few hours of
     gameplay" to adapt. Nothing in src/ or the test suite establishes that
     figure, so it has been replaced by the confidence curve, which is
     verifiable. -->

The per-item confidence curve is the honest answer to "how long until it
learns": an item reaches 50% confidence at **5** rewarded uses and ~95% at
**15**. Until then its score is dominated by its prior and its context weight.

1. **Train the system** — use the recommended items when appropriate
2. **Favorite preferred equipment** — favorited spells/weapons get up to a 2.5x
   utility multiplier (see [Favorites Multiplier](#favorites-multiplier)), which
   surfaces them before the learner has data
3. **Reset** — if the learned table is genuinely wrong, clear it (below)

### Reset options

> **WARNING:** Resetting learned weights is **destructive** — it erases learned
> preferences for every item. An external backup of your save is recommended.

| Action | Effect |
|---|---|
| `hg reset weights` | `FeatureBanditLearner::Clear()` + slot-lock reset. Config untouched |
| `hg reset all` | The above, plus a full registry rebuild and a reset of every stateful pipeline subsystem |
| dMenu "reset learning data" | Same path as `hg reset weights` (`SettingsReloader::ResetLearningData`) |
| dMenu "reset to defaults" | Settings only — learned weights untouched |
| `hg reload` | Re-reads the INI; learned weights untouched |

There is no "randomize" or "jumble weights" operation — `Clear()` empties the
map, and an item with no entry scores `Q = 0` with `UCB = 1.0` (maximum
exploration), which is what makes a cleared learner recover quickly rather than
start from noise.

Cleared data is not written back to the cosave until the next save. A reload of
an older save of the same character keeps the cleared state (the clear ticked
the learning clock past it); a new game also clears the learner.

---

## See Also

- [0-pipeline.md](0-pipeline.md) — Overall pipeline flow
- [1-states.md](1-states.md) — State models (WorldState, PlayerActorState, TargetCollection)
- [3-candidate-filtering.md](3-candidate-filtering.md) — Where the candidates being scored come from
- [5-slots.md](5-slots.md) — Slot classification, locking and the multi-page layout
- [Console commands](https://github.com/LandingCrew/huginn/wiki/Console-Commands) — Full console command reference
- [Mod Compatibility](https://github.com/LandingCrew/huginn/wiki/Mod-Compatibility#wheeler-integration) — Wheeler integration notes

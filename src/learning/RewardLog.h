#pragma once

#include "EquipEvent.h"

namespace Huginn::Learning
{
    class FeatureBanditLearner;

    // =========================================================================
    // REWARD LOG - what the pipeline thought at the moment of a reward
    // =========================================================================
    // Logging only; changes nothing. Written for the soak run ahead of the
    // learning rework ("Learning swamps context" in docs/roadmap.md), which
    // turns each reward into a choice: the chosen item against the items shown
    // for the same need and passed over. These lines are the data that sizes
    // that rework -- the weight of a passed-over item, the surprise factor k --
    // and the input for replaying a session offline against a new learner.
    //
    // One header line for the chosen item, then one line per item on the
    // current page:
    //   [Reward] 0003EADE 'Potion of Healing' src=Hotkey reward=+8.0 mult=1.00
    //            rank=3 util=1.23 ctx=0.80 pred=6.20 need=HealingAny
    //            over=0003EAE3 'Potion of Minor Healing' shown=7 page=0 age=40ms
    //   [Reward]   s2 * 0003EADE 'Potion of Healing' need=HealingAny rank=3 ...
    //   [Reward]   s0 = 0003EAE3 'Potion of Minor Healing' need=HealingAny rank=0 ...
    //   [Reward]   s1   00012EB7 'Iron Sword' need=WeaponsMelee rank=1 ...
    // `*` is the chosen item, `=` an item shown for the same need -- the
    // would-be negatives. `over` names the best-ranked of those: what the
    // choice displaced. `pred` is the learner's estimate for THIS state
    // before the update, the term a choice target would correct.
    //
    // "Need" is the candidate's primary slot class (SlotClassifier::Classify),
    // which pairs a healing spell with a health potion and a resist potion with
    // a ward. Coarse on purpose: every shown item is logged with its class, so
    // a finer grouping can be applied when the data is read.
    //
    // Only the current page is known (PipelineStateCache). A Wheeler pick from
    // another page logs as not shown.
    // =========================================================================

    /// Call immediately BEFORE the learner update for `event`, so `pred` is the
    /// estimate the reward is about to correct.
    void LogRewardContext(const EquipEvent& event, float reward, const FeatureBanditLearner& learner);
}

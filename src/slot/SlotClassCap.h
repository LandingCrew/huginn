#pragma once

#include "SlotConfig.h"
#include "learning/ScoredCandidate.h"

namespace Huginn::Slot
{
    // =========================================================================
    // SLOT CLASS CAP -- a soft cap per slot class on Regular keys
    // =========================================================================
    // One strong slot class could take the whole page: a hungry character at an
    // alchemy lab got eight foods on eight keys and none of the craft gear
    // (2026-10-05). The job-per-key layout used to give the page its variety by
    // construction; the all-Regular default (0.22.10) does not.
    //
    // On a Regular key, the first `free` items of a class on the page compete at
    // full score, and each one past that at + ln d, + 2 ln d, ... (the old
    // x discount, x discount^2 on the utility, made additive in R7 so that a
    // negative score is pushed down, not promoted). It decides PLACEMENT only:
    // ranking, the learner and the selection log keep every item's true score,
    // so learning never sees an item as worse because a sibling took a key
    // first. Keys with a job are never capped.
    //
    // Measured before it shipped (tools/replay, 653 picks since 2026-10-03, the
    // shipped learner): first 3 free, then x0.5, kept top-8 at 76.3% (76.7%
    // without) and menu picks at 30.8% (29.7%), and no page held five of one
    // class (196 did without). Discounting from the 2nd item -- the first
    // design -- cost 3-10 points overall and up to 25 on potions: players do
    // pick the second and third item of a class.
    //
    // The counting and the arithmetic live in the slot core
    // (src/core/SlotAllocCore.h, Core::ClassCapTerm); what stays here is the
    // GROUPING: the class is SlotClassifier::Classify, except that all food and
    // drink is one class. (Called the "need" before 0.23.8; "need" now means the
    // need vector of the engine rewrite. The selection log's JSONL keeps its
    // `need` key for tools/replay.) Foods classify by effect (HealingAny,
    // DefensiveAny, BuffsAny, FoodAny), so a page of seven foods never had more
    // than three of a class.
    // =========================================================================
    class SlotClassCap
    {
    public:
        /// The slot class `c` counts against.
        [[nodiscard]] static SlotClassification ClassOf(const Scoring::ScoredCandidate& c) noexcept;
    };

}  // namespace Huginn::Slot

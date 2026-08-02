#pragma once

#include "ContextReason.h"
#include "candidate/CandidateTypes.h"   // Candidate::CandidateVariant

namespace Huginn::Context
{
    // =========================================================================
    // CONTEXT REASON ATTRIBUTION (#64)
    // =========================================================================
    // DominantReason answers "why does the situation matter RIGHT NOW" — one
    // reason per tick, for the whole page. That is the wrong granularity for a
    // per-slot subtext: stamping it on every slot makes the label say *why now*
    // when the player reading a slot is asking *why this item*. Standing at an
    // enchanter it produced `At Enchanter(Pickaxe)`; in combat it produced eight
    // identical `Fire Damage` labels, one of them on a greatsword.
    //
    // This narrows it to attribution: a reason may speak for a slot only if the
    // candidate in it actually draws weight from the field that reason is read
    // off. Everything else falls through to the next rung of the ladder
    // (`Favorite`, then no label), which is what the out-of-combat lines already
    // showed was the right shape.
    //
    // Deliberate consequence: a reason nothing can draw from labels NOTHING,
    // ever. That covers the three signal-only reasons with no weight at all
    // (AllyInjured, LookingAtOre, InDarkness) and the weights no candidate
    // mapping reads yet (unlock, slow-fall, anti-dragon — see the capacity TODOs
    // in ContextWeightForCandidate.cpp). Those reasons were never ranking
    // anything, so the label was never true; silence is the honest report, and
    // it makes the gap visible as absence instead of hiding it behind a lie.
    // =========================================================================

    /// True when `candidate` draws context weight from the field behind
    /// `reason` — i.e. that reason is part of why this item ranked, not merely
    /// what was happening around it.
    ///
    /// Pure function of (candidate, reason): the tick's actual weight values are
    /// not needed. DominantReason only reports a reason whose weight already
    /// cleared its threshold, so the remaining question is purely structural —
    /// does this candidate's type/tag mapping read that field at all.
    [[nodiscard]] bool ReasonAppliesTo(
        const Candidate::CandidateVariant& candidate,
        ContextReason reason);
}

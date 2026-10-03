#pragma once

#include "EquipEvent.h"

namespace Huginn::Learning
{
    // =========================================================================
    // SHADOW ARM -- learning on vs learning off, on the same playthrough
    // =========================================================================
    // THROWAWAY DEBUG CODE for the long soak run (roadmap Phase 2, "a
    // learning-off arm", decided 2026-10-02). Debug builds only; delete it,
    // with its one call in SelectionLog, once the run is analysed.
    //
    // Nothing has measured context-only Huginn. Instead of alternating
    // sessions with the learner off, this ranks the SAME pipeline run twice at
    // every confirmed selection and asks whether each arm had the chosen item
    // on the page:
    //   A   the live page, as shown (seating, holds and all)
    //   A*  the open slots filled by the live utility, ranked plainly
    //   B   context only: ctx x corr x potion x fav -- the whole learning
    //       factor removed, as fLambdaMin = fLambdaMax = 0 would. That also
    //       drops the PriorCalculator prior and the recency boost, which are
    //       NOT learned (#164 review).
    //   B'  no learned weights, everything else kept: every item untrained
    //       (confidence 0, so lambda = lambdaMin; UCB 1.0), so
    //       ctx x (1 + lambdaMin x (prior + beta + recency)) x mults.
    // Overrides, Remembrance holds and wildcards keep their slots in every
    // arm -- none of them comes from the learner. A* against B' isolates the
    // learner; B against B' is what the prior (and recency) add; A against
    // A* shows what seating and the hold cost.
    //
    // Like the live page, the open slots skip what was in the player's hands
    // when they chose (bSkipEquipped is on for every shipped slot). Only
    // selections on page 1 -- the plain page -- are tallied: page 2's slots
    // are classified, which plain ranking cannot model.
    //
    // One line per confirmed selection in Huginn_AB.log (SKSE log folder),
    // with a running tally for this game launch, overall and for OUTSIDE
    // picks (menu / vanilla hotkey / own wheel): an outside pick that B or B'
    // had and A did not is a reach-in that arm would have saved -- goal 1, per
    // arm. Approximate by construction: B and B' are ranked from scores the
    // live run computed, so potion-tier ordering and cold-start context
    // boosts carry A's learning in.
    // =========================================================================
    namespace ShadowArm
    {
        /// Call at each confirmed selection, with the event the learner is about
        /// to see. Debug builds only; a no-op in Release.
        void Record(const EquipEvent& event);
    }
}

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
    //   B   the open slots filled with the learned term removed:
    //       ctx x corr x potion x fav, i.e. utility / (1 + lambda x learn)
    // Overrides, Remembrance holds and wildcards keep their slots in both
    // arms -- none of them comes from the learner. A* against B isolates the
    // learner; A against A* shows what seating and the hold cost.
    //
    // One line per confirmed selection in Huginn_AB.log (SKSE log folder),
    // with a running tally for this game launch, overall and for OUTSIDE
    // picks (menu / vanilla hotkey / own wheel): an outside pick that B had
    // and A did not is a reach-in context-only Huginn would have saved -- goal
    // 1, per arm. Approximate by construction: B is ranked from scores the
    // live run computed, so tier ordering and cold-start boosts are A's.
    // =========================================================================
    namespace ShadowArm
    {
        /// Call at each confirmed selection, with the event the learner is about
        /// to see. Debug builds only; a no-op in Release.
        void Record(const EquipEvent& event);
    }
}

#pragma once

#include "slot/SlotAssignment.h"
#include "learning/ScoredCandidate.h"
#include "override/OverrideConditions.h"
#include "state/PlayerActorState.h"
#include "state/WorldState.h"

#include <chrono>
#include <vector>

namespace Huginn::Display
{
    // =========================================================================
    // DisplayContext — Data bundle passed to all display backends per frame.
    // =========================================================================
    // Collects every piece of data a backend might need so that Push() has a
    // single, stable signature.  Adding a field here is the only change needed
    // when the pipeline produces new data a backend might consume.
    // =========================================================================

    struct DisplayContext
    {
        const Slot::SlotAssignments& assignments;
        const std::vector<Scoring::ScoredCandidate>& scoredCandidates;
        const Override::OverrideCollection& overrides;
        const State::PlayerActorState& playerState;
        const State::WorldState& worldState;
        bool hasUrgentOverride;
        std::chrono::steady_clock::time_point now;
    };

    // =========================================================================
    // IDisplayBackend — Interface for recommendation display targets.
    // =========================================================================
    // Each concrete backend (Wheeler, IntuitionMenu, future SkyUI overlay, ...)
    // implements Push() to render the current slot assignments in its own way.
    //
    // IsEnabled() is a coarse gate — return false to skip the backend entirely
    // (e.g., Wheeler not installed).  Fine-grained checks (wheel open, menu
    // hidden) belong inside Push().
    // =========================================================================

    class IDisplayBackend
    {
    public:
        virtual ~IDisplayBackend() = default;

        /// Push current slot assignments to this display target.
        virtual void Push(const DisplayContext& ctx) = 0;

        /// Coarse enable check — false skips this backend entirely.
        [[nodiscard]] virtual bool IsEnabled() const = 0;
    };

}  // namespace Huginn::Display

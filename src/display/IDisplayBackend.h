#pragma once

#include "slot/SlotAssignment.h"
#include "learning/ScoredCandidate.h"
#include "context/ContextReason.h"      // Context::ContextReason
#include "override/OverrideConditions.h"
#include "state/PlayerActorState.h"
#include "state/WorldState.h"

#include <chrono>
#include <string_view>
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

        // Resolved page state for THIS push. The coordinator reads it once from
        // the allocator (after ResolveDisplayPage) so backends don't each re-fetch
        // page/name/count from SlotAllocator/SlotSettings singletons at push time.
        // pageName is a view into a string owned by the caller for the push's
        // duration (PipelineCoordinator::PushDisplay).
        size_t pageIndex;
        size_t pageCount;
        size_t slotCount;          // slot count on the current page
        std::string_view pageName; // current page's display name

        // Dominant reason the current context matters, derived once per tick from
        // the scorer's own weight map (Display::DeriveExplanationLabel turns it
        // into text). Override candidates carry their own reason on the
        // assignment and take priority over this one.
        Context::ContextReason contextReason = Context::ContextReason::None;

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

        /// The page this backend wants displayed, or -1 if it doesn't drive page
        /// selection. The coordinator resolves the active page from these BEFORE
        /// allocation so every backend renders one consistent page (see
        /// PipelineCoordinator::ResolveDisplayPage). Default: no opinion.
        [[nodiscard]] virtual int GetDesiredPage() const { return -1; }
    };

}  // namespace Huginn::Display

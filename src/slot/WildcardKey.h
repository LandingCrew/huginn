#pragma once

#include "core/SlotAllocCore.h"
#include "core/WildcardPick.h"
#include "learning/ScoredCandidate.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace Huginn::Slot
{
    // =========================================================================
    // THE WILDCARD KEY (R10) -- what passes between the manager and the slots
    // =========================================================================
    // Scoring::WildcardManager (learning/) owns the per-page cache and the
    // clocks; SlotAllocator places and draws (core/SlotAllocCore.h). These two
    // records are all either side needs of the other, so learning/ does not
    // depend on the allocator (critique #10).
    // =========================================================================

    /// R10: what the wildcard key asks of one allocation
    /// (Scoring::WildcardManager::Plan). Default: nothing.
    struct WildcardRequest
    {
        RE::FormID pinForm = 0;          // the page's cached wildcard, 0 = none
        uint16_t pinUniqueID = 0;        // its stack (a row of the same form otherwise)
        bool roll = false;               // nothing cached, the clocks allow one: roll
        double uRoll = 1.0;              // uniforms in [0, 1)
        double uDraw = 0.0;
        Core::Wildcard::Params params;
        // Forms a slot lock holds on the page's keys (PipelineCoordinator, from
        // SlotLocker::GetLockSnapshot): kept out of the draw's pool.
        std::vector<RE::FormID> heldForms;
    };

    /// ...and what the allocation did with it.
    struct WildcardResult
    {
        Core::SlotAlloc::WildcardOut core;   // the seat, the pin's fate, the roll's numbers
        bool pinFound = false;               // the cached wildcard had a row in the page's list
        std::optional<Scoring::ScoredCandidate> drawn;   // the row a roll put on the seat
    };

}  // namespace Huginn::Slot

#pragma once

// =============================================================================
// DECISION CAPTURE (R4) -- what the selection log v3 took at a press
// =============================================================================
// Taken when the player chooses (SelectionTracker::Select, or the v3-only
// path for picks the frozen learner drops), carried on the EquipEvent until
// the selection confirms, then written (SelectionLogV3::OnConfirmed). Logging
// only: no subscriber reads it.
// =============================================================================

#include "core/DecisionLog.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Huginn::Learning
{
    struct DecisionCapture
    {
        /// The logged context the press is joined to: the live one, or for a
        /// pick from a selection menu the one taken when that menu opened.
        std::shared_ptr<const Core::DecisionLog::Context> ctx;
        float ageMs = 0.0f;               // press time minus the context's time
        std::vector<std::uint8_t> open;   // needs with an open episode at the press
        double pressSec = 0.0;            // steady seconds at the press
    };
}

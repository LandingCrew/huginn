#pragma once

#include "learning/ScoredCandidate.h"

#include <cstddef>

namespace Huginn::Display
{
    // =========================================================================
    // Weapons and armour with no uniqueID stay off a page that feeds a wheel
    // =========================================================================
    // Wheeler cannot show a weapon or armour without an ExtraUniqueID
    // (Wheeler::WheelSync::RequiresUniqueID), and the widget page and the
    // wheel page share one allocation. So while Wheeler drives a page's wheel,
    // that page is allocated without those rows and the key takes the next
    // item: the widget and the wheel show the same thing. The rule and its
    // gate (Wheeler connected, the page has a wheel, the weapon registry past
    // its post-load degraded scan) are core/WheelUidGate.h.
    //
    // Update thread only (the pipeline, and WheelerBackend::Push for the pages
    // the player is not viewing); the log's dedup state takes its own lock
    // anyway.
    // =========================================================================

    /// Whether page `page`'s allocation leaves out uid-less weapons and armour
    /// this tick. Read once per page per tick and pass the answer along, so the
    /// wildcard roll, the allocation and the lock check agree.
    [[nodiscard]] bool HidesUidlessGear(size_t page);

    /// Whether any page hides uid-less gear: Wheeler connected with a wheel
    /// of ours, and the weapon registry past its post-load scan. For callers
    /// off the pipeline (the weapon equip listener).
    [[nodiscard]] bool AnyPageHidesUidlessGear();

    /// A weapon or armour row with no uniqueID: what Wheeler refuses.
    [[nodiscard]] bool IsUidlessGear(const Candidate::CandidateVariant& candidate) noexcept;

    /// The list page `page` is allocated from: `all` itself when `hides` is
    /// false or no row is kept off (no copy), otherwise `all` without the
    /// uid-less gear, built in `scratch`. Logs at debug when what a page keeps
    /// off changes, once per (page, form).
    [[nodiscard]] const Scoring::ScoredCandidateList& CandidatesForWheelPage(size_t page, bool hides,
        const Scoring::ScoredCandidateList& all, Scoring::ScoredCandidateList& scratch);
}

#pragma once

// Where a held (locked) slot's display data comes from this run.
//
// A lock keeps a key's ITEM; it must not keep the item's numbers. SlotLocker
// re-emits the assignment it stored when the lock was taken, candidate and
// all, and the widget reads the stack count, the staff's charge and the ammo
// count off that candidate. Under the combat freeze that froze them too: drink
// one of five healing potions and the key showed [5] until combat ended
// (verifier, PR #207).
//
// Two lists to look in, because the scored list is not every item: the scorer
// drops anything under fMinimumContextWeight / fMinimumUtility, and a healing
// potion's weight falls to its floor the moment health is back up -- exactly
// after the player drank one. So:
//   - in this run's SCORED list (same form and stack): take its item data and
//     its utility; its score too, unless the row was added for a Remembrance
//     hold only (an unranked -inf score must not land on an ordinary key) or
//     the held key is pinned (an override's score is the pinned +inf);
//   - else in this run's GENERATED list (every candidate the generator made,
//     before the scorer's floors): take its item data only, keep the utility
//     and score the key had;
//   - else keep everything: nothing fresher exists (a spell the affordability
//     filter dropped while the freeze holds it on its key, for one).
//
// Pure: standard library only, host-tested in tests/core/HeldRefreshTests.cpp.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Huginn::Core
{
    struct ItemKey
    {
        uint32_t formID = 0;
        uint16_t uniqueID = 0;
        bool rememberedOnly = false;   ///< scored rows only: added for a Remembrance hold, unranked
    };

    /// Index of the row that is exactly this item (form and stack), or nullopt.
    /// A stack-less item (uniqueID 0) matches only a stack-less row: the
    /// tempered dagger's numbers must not be shown on the plain one's key.
    [[nodiscard]] inline std::optional<std::size_t> FindHeldRefresh(
        uint32_t formID, uint16_t uniqueID, const std::vector<ItemKey>& rows)
    {
        if (formID == 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].formID == formID && rows[i].uniqueID == uniqueID) {
                return i;
            }
        }
        return std::nullopt;
    }

    struct HeldRefreshPlan
    {
        /// Take the item data (and, per the flags, utility/score) from this
        /// scored row.
        std::optional<std::size_t> scoredIndex;
        /// Not scored: take the item data only from this generated row.
        std::optional<std::size_t> generatedIndex;
        bool refreshUtility = false;
        bool refreshScore = false;

        [[nodiscard]] bool Any() const noexcept { return scoredIndex || generatedIndex; }
    };

    [[nodiscard]] inline HeldRefreshPlan PlanHeldRefresh(uint32_t formID, uint16_t uniqueID, bool heldPinned,
        const std::vector<ItemKey>& scored, const std::vector<ItemKey>& generated)
    {
        HeldRefreshPlan plan;
        if (const auto s = FindHeldRefresh(formID, uniqueID, scored)) {
            plan.scoredIndex = s;
            const bool ranked = !scored[*s].rememberedOnly;
            plan.refreshUtility = ranked;
            plan.refreshScore = ranked && !heldPinned;
            return plan;
        }
        plan.generatedIndex = FindHeldRefresh(formID, uniqueID, generated);
        return plan;
    }
}

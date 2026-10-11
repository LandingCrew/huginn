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
// Two sources, because the scored list is not every item: the scorer drops
// anything under fMinimumContextWeight / fMinimumUtility, and the candidate
// filters drop the very item just used -- a potion on cooldown (3 s), a health
// potion at full health, a resist potion while its resist is up. All of them
// are exactly the moment the count changed. So:
//   - in this run's SCORED list (same form and stack): take its item data and
//     its utility; its score too, unless the row was added for a Remembrance
//     hold only (an unranked -inf score must not land on an ordinary key) or
//     the held key is pinned (an override's score is the pinned +inf);
//   - else the item's CURRENT data, read from what the generator gathered from
//     the registries this run BEFORE any filter (the caller's lookup,
//     SlotLocker::RunContext::lookupItem): item data only, keep the utility and
//     score the key had;
//   - else keep everything: nothing fresher exists (the item is gone -- the
//     Gone path frees the key -- or not gathered at all).
// An OVERRIDE key is different: only OverrideManager's own candidate carries
// the override's reason (CandidateBase::overrideReason, the "Low HP" / "Poisoned"
// / "Underwater" label), and scored or gathered rows carry none. So an override
// key takes the override's current candidate first, and whatever source is
// used, the stored reason is kept if the new data has none (keepOverrideReason).
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
        /// Not scored: take the item data only from the current (pre-filter)
        /// lookup.
        bool useCurrent = false;
        /// An override key: take the data from the override's own current
        /// candidate (it carries the reason).
        bool useOverride = false;
        bool refreshUtility = false;
        bool refreshScore = false;
        /// Keep the stored override reason if the new data carries none.
        bool keepOverrideReason = false;

        [[nodiscard]] bool Any() const noexcept { return scoredIndex.has_value() || useCurrent || useOverride; }
    };

    /// `current`: the key of what the pre-filter lookup returned for this item,
    /// if anything. Used only when it is exactly this item (form and stack).
    /// `heldIsOverride`: the key shows an override's item. `overrideCurrent`:
    /// an active override carries exactly this item right now.
    [[nodiscard]] inline HeldRefreshPlan PlanHeldRefresh(uint32_t formID, uint16_t uniqueID, bool heldPinned,
        const std::vector<ItemKey>& scored, std::optional<ItemKey> current,
        bool heldIsOverride = false, bool overrideCurrent = false)
    {
        HeldRefreshPlan plan;
        plan.keepOverrideReason = heldIsOverride;
        if (heldIsOverride && overrideCurrent && formID != 0) {
            plan.useOverride = true;
            return plan;
        }
        if (const auto s = FindHeldRefresh(formID, uniqueID, scored)) {
            plan.scoredIndex = s;
            const bool ranked = !scored[*s].rememberedOnly;
            plan.refreshUtility = ranked;
            plan.refreshScore = ranked && !heldPinned;
            return plan;
        }
        plan.useCurrent = formID != 0 && current && current->formID == formID && current->uniqueID == uniqueID;
        return plan;
    }
}

#pragma once

// Where a held (locked) slot's display data comes from this run.
//
// A lock keeps a key's ITEM; it must not keep the item's numbers. SlotLocker
// re-emits the assignment it stored when the lock was taken, candidate and
// all, and the widget reads the stack count, the staff's charge and the ammo
// count off that candidate. Under the combat freeze that froze them too: drink
// one of five healing potions and the key showed [5] until combat ended
// (verifier, PR #207). So every run a held key looks for its own item in this
// run's candidates -- same form AND same stack -- and takes the fresh data from
// there, keeping the key, its type and its name. Not found (an unaffordable
// spell the freeze is holding, say) keeps the stored data: nothing fresher
// exists.
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
    };

    /// Index of the candidate that is exactly this item (form and stack), or
    /// nullopt. A stack-less item (uniqueID 0) matches only a stack-less
    /// candidate: the tempered dagger's numbers must not be shown on the plain
    /// one's key.
    [[nodiscard]] inline std::optional<std::size_t> FindHeldRefresh(
        uint32_t formID, uint16_t uniqueID, const std::vector<ItemKey>& candidates)
    {
        if (formID == 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].formID == formID && candidates[i].uniqueID == uniqueID) {
                return i;
            }
        }
        return std::nullopt;
    }
}

#pragma once

// Which keys SlotLocker's post-lock dedup clears (SlotLocker::DedupePreferLocked).
//
// A lock can keep an item on one key while the allocator places it -- or another
// form with the same display name -- on a second key; the page must not show
// one name twice. Dedup is by NAME, because two forms with one name read as the
// same item to the player.
//
// Rules, strongest first:
//   1. An override or Remembrance item (pinned) is never cleared for a
//      non-pinned copy: a frozen key holding a different form that shares the
//      override's name used to win the "first seen" rule and hide the urgent
//      potion for the whole fight (verifier, PR #207). Between two pinned
//      copies the first is kept.
//   2. A non-pinned copy of a pinned item's name is cleared, locked or not.
//   3. A locked key that already showed the item last run (the incumbent)
//      beats any other non-pinned copy, locked or not: the other copy is the
//      item arriving somewhere it was not, and a lower index winning evicted the
//      incumbent -- two changes for none.
//   4. An unlocked copy of a locked key's name is cleared.
//   5. Otherwise the first occurrence is kept.
//
// Pure: standard library only, host-tested in tests/core/DedupRulesTests.cpp.

#include <cstddef>
#include <set>
#include <string_view>
#include <vector>

namespace Huginn::Core
{
    struct DedupRow
    {
        std::string_view name;   ///< display name (the dedup key)
        bool empty = true;
        bool pinned = false;     ///< override or Remembrance item
        bool locked = false;     ///< the key's lock holds this run
        bool incumbent = false;  ///< locked, and showed this same item last run
    };

    enum class DedupClear : unsigned char
    {
        Keep,
        PinnedDuplicate,   ///< a second pinned copy (rule 1)
        PinnedElsewhere,   ///< a pinned item shows this name (rule 2)
        IncumbentElsewhere,///< a locked incumbent shows this name (rule 3)
        LockedElsewhere,   ///< unlocked, and a locked key shows this name (rule 4)
        Duplicate,         ///< a later copy (rule 5)
    };

    [[nodiscard]] inline std::vector<DedupClear> DedupDecide(const std::vector<DedupRow>& rows)
    {
        std::set<std::string_view> pinnedNames, incumbentNames, lockedNames;
        for (const auto& r : rows) {
            if (r.empty) continue;
            if (r.pinned) pinnedNames.insert(r.name);
            if (r.locked) lockedNames.insert(r.name);
            if (r.incumbent && !r.pinned) incumbentNames.insert(r.name);
        }
        std::vector<DedupClear> out(rows.size(), DedupClear::Keep);
        std::set<std::string_view> pinnedSeen, seen;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            if (r.empty) continue;
            if (r.pinned) {
                if (!pinnedSeen.insert(r.name).second) {
                    out[i] = DedupClear::PinnedDuplicate;
                    continue;
                }
                seen.insert(r.name);
                continue;
            }
            if (pinnedNames.contains(r.name)) {
                out[i] = DedupClear::PinnedElsewhere;
                continue;
            }
            if (!r.incumbent && incumbentNames.contains(r.name)) {
                out[i] = DedupClear::IncumbentElsewhere;
                continue;
            }
            if (!r.locked && lockedNames.contains(r.name)) {
                out[i] = DedupClear::LockedElsewhere;
                continue;
            }
            if (!seen.insert(r.name).second) {
                out[i] = DedupClear::Duplicate;
            }
        }
        return out;
    }
}

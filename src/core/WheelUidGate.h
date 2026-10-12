#pragma once

// Weapons and armour with no uniqueID stay off a page that feeds a Wheeler
// wheel (0.23.40).
//
// Wheeler identifies a weapon or armour instance by its ExtraUniqueID and by
// nothing else, so it refuses uniqueID 0 for those two form types
// (WheelSync::RequiresUniqueID). A plain stacked weapon that was never
// equipped, tempered or enchanted has no ExtraDataList and so no uniqueID.
// Since #118 the Wheeler push blanked such an item's wheel tile while the
// widget, sharing the same allocation, still showed it: on 2026-10-12 a mage's
// never-equipped Iron Dagger (0001397E/uid0) held key 4 through a fight as a
// wildcard, and the wheel showed a blank tile in that slot (08:58:59).
//
// So the allocation of such a page leaves those rows out, and the key gets
// the next-best item instead: the widget and the wheel always match. Only
// when
//   - Wheeler is connected, and the page has a wheel of ours (a hotkey-only
//     player keeps the item: EquipManager equips a uid-0 weapon by base form);
//   - the weapon registry has read extra data since the last load. For about
//     a second after every load every weapon reads uniqueID 0 (WheelSync.cpp,
//     #74) until the primed reconcile reads the extra lists; filtering then
//     would empty the page of weapons and bring them all back a second later.
// An item gains a uniqueID the first time it is equipped; the registry learns
// it at its next reconcile (30 s, or sooner on a departure), and the item is
// eligible again from that pass.
//
// Pure: standard library only, host-tested in tests/core/WheelUidGateTests.cpp.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace Huginn::Core
{
    struct WheelUidGate
    {
        bool wheelerConnected = false;     ///< WheelerClient::IsConnected()
        bool pageHasWheel = false;         ///< the page's wheel index is valid
        bool instanceDetailKnown = false;  ///< WeaponRegistry::HasInstanceDetail(): extra data read since the load
    };

    /// Whether this page's allocation leaves out instance items with no uniqueID.
    [[nodiscard]] constexpr bool HidesUidlessGear(const WheelUidGate& g) noexcept
    {
        return g.wheelerConnected && g.pageHasWheel && g.instanceDetailKnown;
    }

    /// One row: `instanceItem` = a weapon or armour (identified per instance).
    [[nodiscard]] constexpr bool KeptOffWheelPage(bool hides, bool instanceItem, std::uint16_t uniqueID) noexcept
    {
        return hides && instanceItem && uniqueID == 0;
    }

    /// One row as the filter sees it.
    struct GearRow
    {
        bool instanceItem = false;    ///< a weapon or armour (identified per instance)
        std::uint16_t uniqueID = 0;
    };

    /// The rows of a page's list that the allocation leaves out, ascending.
    /// `proj` turns an element of `rows` into a GearRow. Empty when the page
    /// does not hide (the caller then allocates from the list itself, no copy).
    template <class Rows, class Proj>
    [[nodiscard]] std::vector<std::size_t> KeptOffRows(bool hides, const Rows& rows, Proj proj)
    {
        std::vector<std::size_t> out;
        if (!hides) {
            return out;
        }
        std::size_t i = 0;
        for (const auto& row : rows) {
            const GearRow g = proj(row);
            if (KeptOffWheelPage(hides, g.instanceItem, g.uniqueID)) {
                out.push_back(i);
            }
            ++i;
        }
        return out;
    }

    /// A cached wildcard on a page that hides uid-less gear (WildcardManager):
    /// Use it if some row of its form can be shown; Drop it if its rows exist
    /// but none can (it would otherwise sit in the cache for its whole
    /// cooldown, showing nothing and blocking a re-roll); Absent if no row of
    /// it is in the list this run (kept, as before: it may come back).
    enum class CachedWildcardFate : std::uint8_t { Use, Absent, Drop };

    [[nodiscard]] constexpr CachedWildcardFate FateOfCachedWildcard(bool anyRow, bool anyShowableRow) noexcept
    {
        if (anyShowableRow) {
            return CachedWildcardFate::Use;
        }
        return anyRow ? CachedWildcardFate::Drop : CachedWildcardFate::Absent;
    }

    /// The log's dedup, per page: which forms were kept off last pass. Update()
    /// takes this pass's forms (any order, repeats allowed) and returns which
    /// are new and which are no longer kept off, so the log line fires on the
    /// change only.
    struct KeptOffLog
    {
        std::vector<std::uint32_t> forms;   ///< sorted, unique

        struct Change
        {
            std::vector<std::uint32_t> added;
            std::vector<std::uint32_t> removed;
            [[nodiscard]] bool Empty() const noexcept { return added.empty() && removed.empty(); }
        };

        Change Update(std::vector<std::uint32_t> now)
        {
            std::sort(now.begin(), now.end());
            now.erase(std::unique(now.begin(), now.end()), now.end());
            Change c;
            std::set_difference(now.begin(), now.end(), forms.begin(), forms.end(), std::back_inserter(c.added));
            std::set_difference(forms.begin(), forms.end(), now.begin(), now.end(), std::back_inserter(c.removed));
            forms = std::move(now);
            return c;
        }
    };
}

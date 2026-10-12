#pragma once

// A weapon stack gaining its uniqueID is not the weapon leaving (0.23.40).
//
// A plain stack that was never equipped, tempered or enchanted has no
// ExtraUniqueID, and the weapon registry keys it (form, 0). Its first equip
// gives it one, and the next reconcile sees (form, 0) gone and (form, N) new.
// Reported as a departure, (form, 0) reached SlotLocker::OnItemUsed(form, 0,
// Gone) -- and uid 0 there means "every lock on the form": the Sticky
// activation lock, a combat freeze, an undo settle all let go, and the key the
// player had just pressed refilled about 100 ms later (the misclick churn of
// #207/#208). The same held for every weapon at the primed reconcile a second
// after each load, where the degraded (form, 0) records give way to the real
// ones. So a removed uid-0 record whose form is still carried under a uniqueID
// is an UPGRADE: not reported as a departure, and what named it (a lock, the
// allocator's seats) is re-pointed to the new uniqueID when there is exactly
// one to point at.
//
// Also here: whether a weapon equip should ask for that reconcile at all
// (WeaponDepartureListener), and the dedup-key rewrite the re-point uses.
//
// Pure: standard library only, host-tested in tests/core/StackIdentityTests.cpp.

#include <cstdint>
#include <vector>

namespace Huginn::Core
{
    struct StackKey
    {
        std::uint32_t formID = 0;
        std::uint16_t uniqueID = 0;
        friend constexpr bool operator==(const StackKey&, const StackKey&) = default;
    };

    /// A uid-0 record that gave way to the same form under a uniqueID.
    /// newUniqueID 0 = the form is carried under SEVERAL uniqueIDs and none was
    /// added this pass: not a departure, but nothing to re-point to (a key
    /// naming uid 0 still equips by base form).
    struct StackUpgrade
    {
        std::uint32_t formID = 0;
        std::uint16_t newUniqueID = 0;
        friend constexpr bool operator==(const StackUpgrade&, const StackUpgrade&) = default;
    };

    struct StackRemovals
    {
        std::vector<StackKey> departed;       ///< really left: break their locks
        std::vector<StackUpgrade> upgraded;   ///< (form, 0) -> (form, N): keep the keys
    };

    /// Classify the records a reconcile removed. `current` is every record
    /// after the pass, `added` the ones this pass inserted. A removed record
    /// with a uniqueID is always a departure (two instances are two items). A
    /// removed uid-0 record is an upgrade when the form is still carried under
    /// some uniqueID: re-pointed to the one added this pass if exactly one
    /// was, else to the only one carried, else to none.
    [[nodiscard]] inline StackRemovals ClassifyRemovedStacks(const std::vector<StackKey>& removed,
        const std::vector<StackKey>& current, const std::vector<StackKey>& added)
    {
        StackRemovals out;
        for (const auto& r : removed) {
            if (r.uniqueID != 0) {
                out.departed.push_back(r);
                continue;
            }
            std::uint16_t onlyCarried = 0, onlyAdded = 0;
            int carried = 0, addedCount = 0;
            for (const auto& c : current) {
                if (c.formID == r.formID && c.uniqueID != 0) {
                    ++carried;
                    onlyCarried = c.uniqueID;
                }
            }
            if (carried == 0) {
                out.departed.push_back(r);
                continue;
            }
            for (const auto& a : added) {
                if (a.formID == r.formID && a.uniqueID != 0) {
                    ++addedCount;
                    onlyAdded = a.uniqueID;
                }
            }
            const std::uint16_t to = addedCount == 1 ? onlyAdded : carried == 1 ? onlyCarried : 0;
            out.upgraded.push_back({ r.formID, to });
        }
        return out;
    }

    /// The allocator's seat keys (CandidateBase::GetDeduplicationKey:
    /// [sourceType:8][uniqueID:16][formID:32]) for (formID, uid 0) become
    /// (formID, newUniqueID); every other key is returned as it was. A form is
    /// one type, so the form ID alone names the item.
    [[nodiscard]] constexpr std::uint64_t RekeyDedupKey(std::uint64_t key, std::uint32_t formID,
        std::uint16_t newUniqueID) noexcept
    {
        const bool sameForm = static_cast<std::uint32_t>(key & 0xFFFFFFFFull) == formID;
        const bool plain = ((key >> 32) & 0xFFFFull) == 0;
        if (key == 0 || !sameForm || !plain || newUniqueID == 0) {
            return key;
        }
        return key | (static_cast<std::uint64_t>(newUniqueID) << 32);
    }

    /// A weapon equip: should it ask for a reconcile now?
    struct EquipReconcileInput
    {
        bool pageHides = false;            ///< some page hides uid-less gear (Wheeler drives a wheel)
        bool extraReadable = false;        ///< SelectionLogV3::IsInventoryExtraReadable(): no load under way, past the window
        bool detailKnown = false;          ///< WeaponRegistry::HasInstanceDetail()
        bool plainRecordHeld = false;      ///< the registry holds (form, 0)
        std::uint16_t eventUniqueID = 0;   ///< the equip event's stack (0 = it carried none)
        bool eventStackIndexed = false;    ///< the registry already holds (form, eventUniqueID)
        bool alreadyRequested = false;     ///< (form, eventUniqueID) asked for already this load
    };

    /// Only where it matters (a page hides uid-less gear: without Wheeler
    /// nothing changes), only when the inventory may be read, only for a form
    /// the registry holds plain and does not already know this stack of, and
    /// once per (form, stack) per load -- a mod re-equipping every tick must
    /// not buy an inventory walk per tick.
    [[nodiscard]] constexpr bool ShouldRequestEquipReconcile(const EquipReconcileInput& in) noexcept
    {
        if (!in.pageHides || !in.extraReadable || !in.detailKnown || !in.plainRecordHeld || in.alreadyRequested) {
            return false;
        }
        return in.eventUniqueID == 0 || !in.eventStackIndexed;
    }
}

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
// ones. So a removed uid-0 record is an UPGRADE -- not reported as a
// departure, and what named it (a lock, the allocator's seats) re-pointed to
// the new uniqueID when there is exactly one to point at -- in two cases only:
//   - a uniqueID of the same form was ADDED by this pass (the equip), or
//   - the record was DEGRADED: written by a scan that could not read extra
//     data (the load window), so it never was a stack of its own.
// Anything else is a real departure, even with uid-bearing copies of the form
// still carried: the player sold or dropped the PLAIN dagger while keeping a
// tempered one. Reported exact-stack (LockMatchesStack): uid 0 there matches
// only locks naming uid 0, so the tempered copy's own lock survives.
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

    /// A record the reconcile removed. `degraded`: it was written by a scan
    /// that could not read extra data (one uid-0 record per form, the
    /// post-load window), so it was never a stack of its own.
    struct RemovedStack
    {
        std::uint32_t formID = 0;
        std::uint16_t uniqueID = 0;
        bool degraded = false;
    };

    struct StackRemovals
    {
        std::vector<StackKey> departed;       ///< really left: break their locks
        std::vector<StackUpgrade> upgraded;   ///< (form, 0) -> (form, N): keep the keys
    };

    /// Classify the records a reconcile removed. `current` is every record
    /// after the pass, `added` the ones this pass inserted.
    ///   - A removed record with a uniqueID: a departure (two instances are
    ///     two items).
    ///   - A removed uid-0 record with a uniqueID of its form ADDED this pass:
    ///     the plain stack was equipped and got one. Upgrade, re-pointed to it
    ///     when exactly one was added.
    ///   - A removed DEGRADED uid-0 record whose form is carried under a
    ///     uniqueID: the post-load placeholder giving way to the real record.
    ///     Upgrade, re-pointed when exactly one is carried.
    ///   - Otherwise a departure: the plain copy was sold or dropped, whatever
    ///     tempered copies of the form remain.
    [[nodiscard]] inline StackRemovals ClassifyRemovedStacks(const std::vector<RemovedStack>& removed,
        const std::vector<StackKey>& current, const std::vector<StackKey>& added)
    {
        StackRemovals out;
        for (const auto& r : removed) {
            const StackKey key{ r.formID, r.uniqueID };
            if (r.uniqueID != 0) {
                out.departed.push_back(key);
                continue;
            }
            std::uint16_t onlyAdded = 0, onlyCarried = 0;
            int addedCount = 0, carried = 0;
            for (const auto& a : added) {
                if (a.formID == r.formID && a.uniqueID != 0) {
                    ++addedCount;
                    onlyAdded = a.uniqueID;
                }
            }
            if (addedCount > 0) {
                out.upgraded.push_back({ r.formID, addedCount == 1 ? onlyAdded : std::uint16_t{ 0 } });
                continue;
            }
            for (const auto& c : current) {
                if (c.formID == r.formID && c.uniqueID != 0) {
                    ++carried;
                    onlyCarried = c.uniqueID;
                }
            }
            if (r.degraded && carried > 0) {
                out.upgraded.push_back({ r.formID, carried == 1 ? onlyCarried : std::uint16_t{ 0 } });
                continue;
            }
            out.departed.push_back(key);
        }
        return out;
    }

    /// Does a lock on (lockForm, lockUid) match an item event for (formID,
    /// uniqueID)? A named stack matches only itself. uid 0 is "every stack of
    /// the form" for a use or an inventory count (potions, ammo, a Wheeler
    /// activation -- the form is the identity there), but for a weapon
    /// DEPARTURE (`exactStack`) it names the plain stack and matches only a
    /// lock on uid 0: selling the plain dagger must not free the tempered
    /// one's key.
    [[nodiscard]] constexpr bool LockMatchesStack(std::uint32_t lockForm, std::uint16_t lockUid,
        std::uint32_t formID, std::uint16_t uniqueID, bool exactStack) noexcept
    {
        if (lockForm != formID) {
            return false;
        }
        if (uniqueID != 0 || exactStack) {
            return lockUid == uniqueID;
        }
        return true;
    }

    /// A lock or shown record on (lockForm, lockUid) after (formID, 0) became
    /// (formID, newUniqueID): its new uniqueID, unchanged unless it named the
    /// plain stack of that form.
    [[nodiscard]] constexpr std::uint16_t RekeyedUniqueID(std::uint32_t lockForm, std::uint16_t lockUid,
        std::uint32_t formID, std::uint16_t newUniqueID) noexcept
    {
        return (lockForm == formID && lockUid == 0 && newUniqueID != 0) ? newUniqueID : lockUid;
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

    /// RequestReconcileForEquip's memo. A stack with a uniqueID is asked for
    /// once per load. An event that carried no uniqueID cannot be told from
    /// the next plain copy of the same form, so it is not remembered for the
    /// load -- only rate-limited, once per form per kUidlessRepeatMs.
    struct EquipAskMemo
    {
        static constexpr std::int64_t kUidlessRepeatMs = 2000;

        struct Asked
        {
            StackKey key;
            std::int64_t atMs = 0;
        };
        std::uint32_t generation = 0;
        std::vector<Asked> asked;

        /// Already asked for (load `gen`, clock `nowMs`)? A new load forgets
        /// everything.
        [[nodiscard]] bool AlreadyAsked(std::uint32_t gen, StackKey key, std::int64_t nowMs)
        {
            if (gen != generation) {
                generation = gen;
                asked.clear();
            }
            for (const auto& a : asked) {
                if (!(a.key == key)) continue;
                if (key.uniqueID != 0) return true;
                if (nowMs - a.atMs < kUidlessRepeatMs) return true;
            }
            return false;
        }

        void Note(StackKey key, std::int64_t nowMs)
        {
            for (auto& a : asked) {
                if (a.key == key) {
                    a.atMs = nowMs;
                    return;
                }
            }
            asked.push_back({ key, nowMs });
        }
    };

    /// A weapon equip: should it ask for a reconcile now?
    struct EquipReconcileInput
    {
        bool pageHides = false;            ///< some page hides uid-less gear (Wheeler drives a wheel)
        bool extraReadable = false;        ///< SelectionLogV3::IsInventoryExtraReadable(): no load under way, past the window
        bool detailKnown = false;          ///< WeaponRegistry::HasInstanceDetail()
        bool plainRecordHeld = false;      ///< the registry holds (form, 0)
        std::uint16_t eventUniqueID = 0;   ///< the equip event's stack (0 = it carried none)
        bool eventStackIndexed = false;    ///< the registry already holds (form, eventUniqueID)
        bool alreadyRequested = false;     ///< EquipAskMemo::AlreadyAsked
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

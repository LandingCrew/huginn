// Host tests for src/core/StackIdentity.h.

#include "core/StackIdentity.h"

#include <doctest/doctest.h>

#include <vector>

using namespace Huginn::Core;

namespace
{
    constexpr std::uint32_t kDagger = 0x0001397E;
    constexpr std::uint32_t kMace = 0x00013982;
}

TEST_CASE("stack identity: a lone plain stack equipped once is an upgrade, not a departure")
{
    // Before: (dagger, 0). The equip gives it uid 7; the reconcile adds
    // (dagger, 7) and removes (dagger, 0).
    const auto r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kDagger, 7 } }, { { kDagger, 7 } });
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kDagger, 7 });
}

TEST_CASE("stack identity: with plain copies left, the uid-0 record stays, so nothing is removed")
{
    // Two plain daggers, one equipped: the scan emits (dagger, 7) for it and
    // (dagger, 0) for the rest, so the reconcile removes nothing.
    const auto r = ClassifyRemovedStacks({}, { { kDagger, 0 }, { kDagger, 7 } }, { { kDagger, 7 } });
    CHECK(r.departed.empty());
    CHECK(r.upgraded.empty());
}

TEST_CASE("stack identity: a plain stack that really left is a departure")
{
    // Sold the only dagger: no record of the form remains.
    const auto r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kMace, 3 } }, {});
    REQUIRE(r.departed.size() == 1);
    CHECK(r.departed[0] == StackKey{ kDagger, 0 });
    CHECK(r.upgraded.empty());
}

TEST_CASE("stack identity: a stack with a uniqueID that left is always a departure")
{
    // A tempered dagger (uid 4) sold while a new one (uid 9) was picked up:
    // two instances, two items.
    const auto r = ClassifyRemovedStacks({ { kDagger, 4 } }, { { kDagger, 9 } }, { { kDagger, 9 } });
    REQUIRE(r.departed.size() == 1);
    CHECK(r.departed[0] == StackKey{ kDagger, 4 });
    CHECK(r.upgraded.empty());
}

TEST_CASE("stack identity: the primed reconcile after a load drops the degraded record as an upgrade")
{
    // The load-time scan added a DEGRADED (mace, 0) beside the real (mace, 3)
    // kept from before (a save load reconciles, it does not rebuild); the
    // primed reconcile removes (mace, 0) and adds nothing.
    const auto r = ClassifyRemovedStacks({ { kMace, 0, true } }, { { kMace, 3 } }, {});
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kMace, 3 });   // the only one carried

    // Several carried: still the placeholder going, but nothing to point at.
    const auto two = ClassifyRemovedStacks({ { kMace, 0, true } }, { { kMace, 3 }, { kMace, 5 } }, {});
    CHECK(two.departed.empty());
    REQUIRE(two.upgraded.size() == 1);
    CHECK(two.upgraded[0] == StackUpgrade{ kMace, 0 });

    // A degraded record whose form is gone altogether: a departure.
    const auto gone = ClassifyRemovedStacks({ { kMace, 0, true } }, {}, {});
    REQUIRE(gone.departed.size() == 1);
    CHECK(gone.upgraded.empty());
}

TEST_CASE("stack identity: the plain copy sold beside a tempered one is a departure (a)")
{
    // Plain (dagger, 0) on key K, tempered (dagger, 4) carried all along; the
    // player sells the plain one. Nothing added, the record was real.
    const auto r = ClassifyRemovedStacks({ { kDagger, 0, false } }, { { kDagger, 4 } }, {});
    REQUIRE(r.departed.size() == 1);
    CHECK(r.departed[0] == StackKey{ kDagger, 0 });
    CHECK(r.upgraded.empty());   // K's lock is NOT handed to the tempered copy
}

TEST_CASE("stack identity: the plain copy sold beside two tempered ones is a departure (b)")
{
    const auto r = ClassifyRemovedStacks({ { kDagger, 0, false } }, { { kDagger, 4 }, { kDagger, 7 } }, {});
    REQUIRE(r.departed.size() == 1);
    CHECK(r.departed[0] == StackKey{ kDagger, 0 });
    CHECK(r.upgraded.empty());
}

TEST_CASE("stack identity: the added count is per form")
{
    // A uniqueID added for ANOTHER form says nothing about this one. Degraded
    // with two carried: an upgrade with nothing to point at, never to Y's 9.
    constexpr std::uint32_t kY = 0x00012EB7;
    auto r = ClassifyRemovedStacks({ { kDagger, 0, true } }, { { kDagger, 4 }, { kDagger, 7 }, { kY, 9 } },
        { { kY, 9 } });
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0].newUniqueID == 0);
    // Not degraded: a departure, whatever Y did.
    r = ClassifyRemovedStacks({ { kDagger, 0, false } }, { { kDagger, 4 }, { kDagger, 7 }, { kY, 9 } }, { { kY, 9 } });
    REQUIRE(r.departed.size() == 1);
    CHECK(r.upgraded.empty());
}

TEST_CASE("stack identity: forms that share their low bits under another plugin index are other forms")
{
    constexpr std::uint32_t kOther = 0x0201397E;   // same low 24 bits as the dagger, another plugin
    // The dagger's plain record left; only the other plugin's form gained a uid.
    const auto r = ClassifyRemovedStacks({ { kDagger, 0, false } }, { { kOther, 5 } }, { { kOther, 5 } });
    REQUIRE(r.departed.size() == 1);
    CHECK(r.upgraded.empty());
    // Locks and seats: the full 32-bit form is compared.
    CHECK_FALSE(LockMatchesStack(kOther, 0, kDagger, 0, false));
    CHECK(RekeyedUniqueID(kOther, 0, kDagger, 5) == 0);
    const std::uint64_t otherSeat = (3ull << 48) | kOther;
    CHECK(RekeyDedupKey(otherSeat, kDagger, 5) == otherSeat);
}

TEST_CASE("stack identity: several uniqueIDs carried -- the one added this pass, else none")
{
    // Equipped the last plain dagger (now uid 7) while a tempered one (uid 4)
    // was already known: re-point to the one that just appeared.
    auto r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kDagger, 4 }, { kDagger, 7 } }, { { kDagger, 7 } });
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kDagger, 7 });


    // Two added at once and two carried: ambiguous as well.
    r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kDagger, 4 }, { kDagger, 7 } }, { { kDagger, 4 }, { kDagger, 7 } });
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0].newUniqueID == 0);
}

TEST_CASE("stack identity: mixed removals are classified one by one")
{
    const auto r = ClassifyRemovedStacks({ { kDagger, 0 }, { kMace, 0, true }, { kMace, 2 } },
        { { kDagger, 7 } }, { { kDagger, 7 } });
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kDagger, 7 });
    REQUIRE(r.departed.size() == 2);
    CHECK(r.departed[0] == StackKey{ kMace, 0 });
    CHECK(r.departed[1] == StackKey{ kMace, 2 });
}

TEST_CASE("stack identity: seat keys of (form, 0) are re-pointed, nothing else")
{
    // [sourceType:8][uniqueID:16][formID:32], sourceType 3 = Weapon.
    const std::uint64_t plain = (3ull << 48) | kDagger;
    const std::uint64_t staff = (7ull << 48) | kDagger;   // the type bits are kept as they are
    CHECK(RekeyDedupKey(plain, kDagger, 7) == ((3ull << 48) | (7ull << 32) | kDagger));
    CHECK(RekeyDedupKey(staff, kDagger, 7) == ((7ull << 48) | (7ull << 32) | kDagger));
    const std::uint64_t tempered = (3ull << 48) | (4ull << 32) | kDagger;
    CHECK(RekeyDedupKey(tempered, kDagger, 7) == tempered);       // already a stack: not this one
    const std::uint64_t mace = (3ull << 48) | kMace;
    CHECK(RekeyDedupKey(mace, kDagger, 7) == mace);               // another form
    CHECK(RekeyDedupKey(0, kDagger, 7) == 0u);                    // an empty seat stays empty
    CHECK(RekeyDedupKey(plain, kDagger, 0) == plain);             // nothing to point at
    // A uniqueID with only high bits set (0x100) is a stack, not plain: the
    // plain check reads all 16 bits.
    const std::uint64_t high = (3ull << 48) | (0x100ull << 32) | kDagger;
    CHECK(RekeyDedupKey(high, kDagger, 7) == high);
}

TEST_CASE("stack identity: a departure releases exactly its stack; a use of uid 0 the whole form")
{
    // lockForm, lockUid, formID, uniqueID, exactStack
    // The plain dagger sold: the plain key lets go, the tempered one's stays.
    CHECK(LockMatchesStack(kDagger, 0, kDagger, 0, true));
    CHECK_FALSE(LockMatchesStack(kDagger, 4, kDagger, 0, true));
    // A potion drunk, ammo spent, a Wheeler activation: uid 0 is the form.
    CHECK(LockMatchesStack(kDagger, 0, kDagger, 0, false));
    CHECK(LockMatchesStack(kDagger, 4, kDagger, 0, false));
    // A named stack matches only itself, either way.
    CHECK(LockMatchesStack(kDagger, 4, kDagger, 4, false));
    CHECK_FALSE(LockMatchesStack(kDagger, 7, kDagger, 4, false));
    CHECK_FALSE(LockMatchesStack(kDagger, 0, kDagger, 4, true));
    CHECK_FALSE(LockMatchesStack(kMace, 0, kDagger, 0, false));
}

TEST_CASE("stack identity: a re-point changes only the plain stack of that form")
{
    // lockForm, lockUid, formID, newUniqueID
    CHECK(RekeyedUniqueID(kDagger, 0, kDagger, 7) == 7);    // the key that held the plain dagger
    CHECK(RekeyedUniqueID(kDagger, 4, kDagger, 7) == 4);    // the tempered copy keeps its own
    CHECK(RekeyedUniqueID(kMace, 0, kDagger, 7) == 0);      // another form
    CHECK(RekeyedUniqueID(kDagger, 0, kDagger, 0) == 0);    // nothing to point at
}

TEST_CASE("stack identity: the equip memo -- once per stack per load, uid-0 events rate-limited")
{
    EquipAskMemo memo;
    const StackKey named{ kDagger, 7 }, plain{ kDagger, 0 };
    CHECK_FALSE(memo.AlreadyAsked(1, named, 0));
    memo.Note(named, 0);
    CHECK(memo.AlreadyAsked(1, named, 1000000));   // the whole load
    CHECK_FALSE(memo.AlreadyAsked(2, named, 1000000));   // a new load forgets

    // An event with no uniqueID: a second plain copy of the form may need its
    // own request, so it is only held off for kUidlessRepeatMs.
    CHECK_FALSE(memo.AlreadyAsked(2, plain, 5000));
    memo.Note(plain, 5000);
    CHECK(memo.AlreadyAsked(2, plain, 5000 + EquipAskMemo::kUidlessRepeatMs - 1));
    CHECK_FALSE(memo.AlreadyAsked(2, plain, 5000 + EquipAskMemo::kUidlessRepeatMs));
    // Another form is its own entry.
    CHECK_FALSE(memo.AlreadyAsked(2, StackKey{ kMace, 0 }, 5001));
}

TEST_CASE("stack identity: an equip asks for a reconcile only where and when it helps")
{
    EquipReconcileInput in;
    in.pageHides = true;
    in.extraReadable = true;
    in.detailKnown = true;
    in.plainRecordHeld = true;
    in.eventUniqueID = 7;
    in.eventStackIndexed = false;
    CHECK(ShouldRequestEquipReconcile(in));   // the first equip of the plain dagger

    auto off = in;
    off.pageHides = false;            // no Wheeler page: behave exactly as before
    CHECK_FALSE(ShouldRequestEquipReconcile(off));
    off = in;
    off.extraReadable = false;        // a load under way / the post-load window
    CHECK_FALSE(ShouldRequestEquipReconcile(off));
    off = in;
    off.detailKnown = false;          // the primed reconcile is coming anyway
    CHECK_FALSE(ShouldRequestEquipReconcile(off));
    off = in;
    off.plainRecordHeld = false;      // no plain stack of it: nothing to learn
    CHECK_FALSE(ShouldRequestEquipReconcile(off));
    off = in;
    off.eventStackIndexed = true;     // a tempered copy beside plain ones: known already
    CHECK_FALSE(ShouldRequestEquipReconcile(off));
    off = in;
    off.alreadyRequested = true;      // asked this load: a mod re-equipping every tick buys nothing
    CHECK_FALSE(ShouldRequestEquipReconcile(off));

    auto noUid = in;
    noUid.eventUniqueID = 0;          // the event carried no stack: ask (once)
    noUid.eventStackIndexed = false;
    CHECK(ShouldRequestEquipReconcile(noUid));
}

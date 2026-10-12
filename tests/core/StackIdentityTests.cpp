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
    // The load-time scan added (mace, 0) beside the real (mace, 3) kept from
    // before; the primed reconcile removes (mace, 0) and adds nothing.
    const auto r = ClassifyRemovedStacks({ { kMace, 0 } }, { { kMace, 3 } }, {});
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kMace, 3 });   // the only one carried
}

TEST_CASE("stack identity: several uniqueIDs carried -- the one added this pass, else none")
{
    // Equipped the last plain dagger (now uid 7) while a tempered one (uid 4)
    // was already known: re-point to the one that just appeared.
    auto r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kDagger, 4 }, { kDagger, 7 } }, { { kDagger, 7 } });
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kDagger, 7 });

    // Nothing added and two carried (a degraded record after a load): not a
    // departure, nothing to re-point to.
    r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kDagger, 4 }, { kDagger, 7 } }, {});
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0] == StackUpgrade{ kDagger, 0 });

    // Two added at once and two carried: ambiguous as well.
    r = ClassifyRemovedStacks({ { kDagger, 0 } }, { { kDagger, 4 }, { kDagger, 7 } }, { { kDagger, 4 }, { kDagger, 7 } });
    CHECK(r.departed.empty());
    REQUIRE(r.upgraded.size() == 1);
    CHECK(r.upgraded[0].newUniqueID == 0);
}

TEST_CASE("stack identity: mixed removals are classified one by one")
{
    const auto r = ClassifyRemovedStacks({ { kDagger, 0 }, { kMace, 0 }, { kMace, 2 } },
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

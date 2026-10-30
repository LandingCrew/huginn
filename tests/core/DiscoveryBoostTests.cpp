// Host tests for the discovery boost (0.24.12, core/DiscoveryBoost.h) and its
// cosave record DISC (core/ChoiceCosave.h): the multiplier and its decay, the
// expiry, the draw variance and what it does to the wildcard weight, the
// tracker's rules (present at load: never boosted; a form seen once is never
// new again; the first pick ends a boost; a load under way drops gains), the
// save state's round trip and the reload's dynamic forms, and the codec.

#include "core/ChoiceCosave.h"
#include "core/ChoiceUpdate.h"
#include "core/DiscoveryBoost.h"
#include "core/WildcardPick.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace D = Huginn::Core::Discovery;
namespace CC = Huginn::Core::ChoiceCosave;
namespace C = Huginn::Core::Choice;
namespace W = Huginn::Core::Wildcard;

namespace
{
    constexpr double kHour = 3600.0;
    using Gain = D::Tracker::Gain;

    template <class T>
    void Poke(std::vector<std::byte>& blob, std::size_t at, T v)
    {
        REQUIRE(at + sizeof(T) <= blob.size());
        std::memcpy(blob.data() + at, &v, sizeof(T));
    }

    D::Params Defaults() { return D::Params{}; }
}

// ---------------------------------------------------------------------------
// The math
// ---------------------------------------------------------------------------

TEST_CASE("discovery: the defaults are on, x4 on b's variance (sd x2), halving every hour of play")
{
    const auto p = Defaults();
    CHECK(p.enabled);
    CHECK(p.varMult == doctest::Approx(4.0));
    CHECK(p.halfLifeSec == doctest::Approx(kHour));
    CHECK(p == p.Clamped());
    // sd at the gain: twice b's prior sd, 0.5 -> 1.0
    CHECK(std::sqrt(C::kPriorVarB * D::Mult(p, 0.0)) == doctest::Approx(1.0));
}

TEST_CASE("discovery: m(age) = 1 + (M - 1) 2^(-age/H), and 1 with the boost off")
{
    const auto p = Defaults();
    CHECK(D::Mult(p, 0.0) == doctest::Approx(4.0));
    CHECK(D::Mult(p, kHour) == doctest::Approx(2.5));
    CHECK(D::Mult(p, 2 * kHour) == doctest::Approx(1.75));
    CHECK(D::Mult(p, 100 * kHour) == doctest::Approx(1.0));
    CHECK(D::Mult(p, -50.0) == doctest::Approx(4.0));   // a clock that stepped back reads as the gain
    for (double age = 0.0; age < 20 * kHour; age += 600.0) {
        CHECK(D::Mult(p, age + 600.0) < D::Mult(p, age));   // strictly decays
        CHECK(D::Mult(p, age) >= 1.0);
    }
    D::Params off = p;
    off.enabled = false;
    CHECK(D::Mult(off, 0.0) == 1.0);
    CHECK(D::Excess(off, 0.0) == doctest::Approx(3.0));   // the expiry still runs on it
}

TEST_CASE("discovery: out-of-range and non-finite settings clamp")
{
    D::Params p;
    p.varMult = 0.2;
    p.halfLifeSec = 1.0;
    CHECK(p.Clamped().varMult == 1.0);
    CHECK(p.Clamped().halfLifeSec == 60.0);
    p.varMult = 1e9;
    p.halfLifeSec = 1e12;
    CHECK(p.Clamped().varMult == 100.0);
    CHECK(p.Clamped().halfLifeSec == 360000.0);
    p.varMult = std::numeric_limits<double>::quiet_NaN();
    p.halfLifeSec = std::numeric_limits<double>::infinity();
    CHECK(p.Clamped().varMult == 4.0);
    CHECK(p.Clamped().halfLifeSec == 3600.0);
    // M = 1 is no boost at all
    D::Params one;
    one.varMult = 1.0;
    CHECK(D::Mult(one, 0.0) == 1.0);
}

TEST_CASE("discovery: a boost is over once its excess is under 1% -- 8.2 half-lives at the defaults")
{
    const auto p = Defaults();
    CHECK_FALSE(D::Expired(p, 0.0));
    CHECK_FALSE(D::Expired(p, 8.0 * kHour));    // 3 * 2^-8 = 0.0117
    CHECK(D::Expired(p, 8.3 * kHour));          // 3 * 2^-8.3 = 0.0095
    CHECK(std::log2(3.0 / D::kExpireExcess) == doctest::Approx(8.23).epsilon(0.001));
}

TEST_CASE("discovery: the draw variance scales b's part only, and the wildcard weight grows by sqrt of it")
{
    const double thetaVar = 0.09, bVar = C::kPriorVarB;
    const double rowVar = thetaVar + bVar;
    CHECK(D::ExploreVar(rowVar, bVar, 1.0) == rowVar);
    CHECK(D::ExploreVar(rowVar, bVar, 4.0) == doctest::Approx(thetaVar + 4.0 * bVar));
    CHECK(D::ExploreVar(rowVar, bVar, 0.5) == rowVar);   // never below the posterior
    CHECK(D::ExploreVar(rowVar, 0.0, 4.0) == rowVar);
    CHECK(D::ExploreVar(rowVar, bVar, std::numeric_limits<double>::quiet_NaN()) == rowVar);
    CHECK(D::ExploreVar(rowVar, std::numeric_limits<double>::infinity(), 4.0) == rowVar);

    // An unpicked item and a newly gained one, equally relevant: the new one
    // weighs sqrt(var_w / var) as much in the draw (R10: w = rel x sd).
    const double rel = 0.4;
    const double wOld = W::Weight(rel, std::sqrt(rowVar));
    const double wNew = W::Weight(rel, std::sqrt(D::ExploreVar(rowVar, bVar, D::Mult(Defaults(), 0.0))));
    CHECK(wNew / wOld == doctest::Approx(std::sqrt((thetaVar + 4.0 * bVar) / rowVar)));
    CHECK(wNew > wOld);
}

// ---------------------------------------------------------------------------
// The tracker
// ---------------------------------------------------------------------------

TEST_CASE("discovery tracker: an item present at load gets no boost; one gained after it does")
{
    D::Tracker t;
    const auto p = Defaults();
    const std::vector<std::uint32_t> atLoad{ 0x00012EB7u, 0x0003EADDu, 0x00000000u };   // form 0 ignored
    CHECK(t.Prime(atLoad) == 2);
    CHECK(t.Live());
    CHECK(t.SeenCount() == 2);
    CHECK(t.BoostedCount() == 0);
    // the potion the save held is used up and bought again: not new
    CHECK(t.OnGained(0x0003EADDu, 100.0) == Gain::Known);
    CHECK(t.MultFor(p, 0x0003EADDu, 100.0) == 1.0);
    // a sword picked up for the first time
    CHECK(t.OnGained(0x0001397Eu, 100.0) == Gain::New);
    CHECK(t.Boosted(0x0001397Eu));
    CHECK(t.MultFor(p, 0x0001397Eu, 100.0) == doctest::Approx(4.0));
    CHECK(t.MultFor(p, 0x0001397Eu, 100.0 + kHour) == doctest::Approx(2.5));
    // a second stack of it (a count change): no new boost, the clock is not reset
    CHECK(t.OnGained(0x0001397Eu, 100.0 + kHour) == Gain::Known);
    CHECK(t.MultFor(p, 0x0001397Eu, 100.0 + kHour) == doctest::Approx(2.5));
}

TEST_CASE("discovery tracker: nothing counts before the first prime or while a load is under way")
{
    D::Tracker t;
    CHECK_FALSE(t.Live());
    CHECK(t.OnGained(0x10u, 0.0) == Gain::Dropped);   // before any load
    CHECK_FALSE(t.Seen(0x10u));
    t.Prime({});
    CHECK(t.OnGained(0x10u, 0.0) == Gain::New);
    t.Suspend();   // SKSE's revert: a load starts
    CHECK(t.OnGained(0x20u, 1.0) == Gain::Dropped);
    CHECK_FALSE(t.Seen(0x20u));
    // the load restores 0x20 into the inventory: present, so never boosted
    const std::vector<std::uint32_t> present{ 0x20u };
    t.Prime(present);
    CHECK(t.OnGained(0x20u, 2.0) == Gain::Known);
    CHECK(t.OnGained(0u, 2.0) == Gain::Dropped);
}

TEST_CASE("discovery tracker: the first pick ends the boost, and a form once seen is never new again")
{
    D::Tracker t;
    const auto p = Defaults();
    t.Prime({});
    REQUIRE(t.OnGained(0x00101234u, 10.0) == Gain::New);
    CHECK(t.EndOnPick(0x00101234u));
    CHECK_FALSE(t.EndOnPick(0x00101234u));
    CHECK(t.MultFor(p, 0x00101234u, 10.0) == 1.0);
    // sold, then bought back: still seen
    CHECK(t.OnGained(0x00101234u, 500.0) == Gain::Known);
    CHECK_FALSE(t.Boosted(0x00101234u));
    // a pick of something never boosted changes nothing
    CHECK_FALSE(t.EndOnPick(0x00999999u));
}

TEST_CASE("discovery tracker: any decision but 'nothing' ends the boost -- the model need not be ready")
{
    // ChoiceLearner::OnPick calls OnDecision before its Ready() check, so a
    // pick in the fallback ranking ends the boost too; this is the rule it runs.
    D::Tracker t;
    t.Prime({});
    REQUIRE(t.OnGained(0x51u, 0.0) == Gain::New);
    REQUIRE(t.OnGained(0x52u, 0.0) == Gain::New);
    CHECK_FALSE(t.OnDecision(/*nothing=*/true, 0x51u));   // a need episode with no press
    CHECK(t.Boosted(0x51u));
    CHECK_FALSE(t.OnDecision(false, 0u));                  // no form
    CHECK(t.OnDecision(false, 0x51u));                     // a pick (usable or not)
    CHECK_FALSE(t.Boosted(0x51u));
    CHECK_FALSE(t.OnDecision(false, 0x51u));               // a second pick: nothing left
    CHECK(t.Boosted(0x52u));
}

TEST_CASE("discovery tracker: dynamic forms that no longer exist are dropped; static ones never")
{
    D::Tracker t;
    t.Prime(std::vector<std::uint32_t>{ 0x01u, 0xFF000001u, 0xFF000002u });
    t.OnGained(0xFF000003u, 0.0);
    t.OnGained(0x02u, 0.0);
    // the "lookup" says every form is gone: only the dynamic ones may go
    const auto n = t.DropDynamicIf([](std::uint32_t) { return true; });
    CHECK(n == 3);
    CHECK(t.Seen(0x01u));
    CHECK(t.Seen(0x02u));
    CHECK(t.Boosted(0x02u));
    CHECK_FALSE(t.Seen(0xFF000001u));
    CHECK_FALSE(t.Boosted(0xFF000003u));
    // a reused ID is new again
    CHECK(t.OnGained(0xFF000001u, 5.0) == Gain::New);
    // and one that still exists stays
    D::Tracker u;
    u.Prime(std::vector<std::uint32_t>{ 0xFF000010u, 0xFF000011u });
    CHECK(u.DropDynamicIf([](std::uint32_t f) { return f == 0xFF000011u; }) == 1);
    CHECK(u.Seen(0xFF000010u));
}

TEST_CASE("discovery tracker: Expire drops the boosts that ran out and keeps the rest, and the seen set")
{
    D::Tracker t;
    const auto p = Defaults();
    t.Prime({});
    t.OnGained(0xA1u, 0.0);
    t.OnGained(0xA2u, 5.0 * kHour);
    CHECK(t.Expire(p, 8.0 * kHour) == 0);
    CHECK(t.Expire(p, 8.3 * kHour) == 1);   // A1 at 8.3 h
    CHECK_FALSE(t.Boosted(0xA1u));
    CHECK(t.Boosted(0xA2u));
    CHECK(t.Seen(0xA1u));                    // the seen set is never pruned
    CHECK(t.OnGained(0xA1u, 9.0 * kHour) == Gain::Known);
    CHECK(t.Expire(p, 14.0 * kHour) == 1);
    CHECK(t.BoostedCount() == 0);
    CHECK(t.SeenCount() == 2);
}

TEST_CASE("discovery tracker: a boost the save carried survives the load's prime of a present form")
{
    // Saved 2 min after the sword was picked up, then that save loaded: the
    // sword is in the inventory (present) and still newly gained.
    D::Saved s;
    s.seen = { 0x11u, 0x22u };
    s.boosted = { { 0x22u, 120u } };
    D::Tracker t;
    t.Import(s, 1000.0);
    const std::vector<std::uint32_t> present{ 0x11u, 0x22u, 0x33u };
    CHECK(t.Prime(present) == 1);   // 0x33 is new to the set, but not boosted
    CHECK(t.Boosted(0x22u));
    CHECK_FALSE(t.Boosted(0x33u));
    CHECK(t.MultFor(Defaults(), 0x22u, 1000.0) == doctest::Approx(D::Mult(Defaults(), 120.0)));
}

TEST_CASE("discovery tracker: Export and Import round-trip the sets, ages in whole play seconds")
{
    D::Tracker t;
    t.Prime(std::vector<std::uint32_t>{ 0x05u, 0x03u });
    t.OnGained(0x07u, 10.0);
    t.OnGained(0xFF000801u, 70.5);
    const auto s = t.Export(100.0);
    CHECK(s.seen == std::vector<std::uint32_t>{ 0x03u, 0x05u, 0x07u, 0xFF000801u });   // sorted
    REQUIRE(s.boosted.size() == 2);
    CHECK(s.boosted[0] == std::pair<std::uint32_t, std::uint32_t>{ 0x07u, 90u });
    CHECK(s.boosted[1] == std::pair<std::uint32_t, std::uint32_t>{ 0xFF000801u, 29u });

    D::Tracker u;
    u.Import(s, 5000.0);   // another session's clock: ages are relative
    CHECK(u.SeenCount() == 4);
    CHECK(u.BoostedCount() == 2);
    CHECK(u.MultFor(Defaults(), 0x07u, 5000.0) == doctest::Approx(D::Mult(Defaults(), 90.0)));
    CHECK_FALSE(u.Live());   // an import does not open the gate: the prime does

    // a boosted form missing from the seen list is seen anyway
    D::Saved odd;
    odd.boosted = { { 0x44u, 0u } };
    u.Import(odd, 0.0);
    CHECK(u.Seen(0x44u));
    CHECK(u.SeenCount() == 1);   // Import replaces
}

TEST_CASE("discovery tracker: a reload keeps memory but takes dynamic forms (0xFF) from the save")
{
    D::Tracker t;
    t.Prime(std::vector<std::uint32_t>{ 0x01u, 0xFF000001u });
    t.OnGained(0x02u, 0.0);
    t.OnGained(0xFF000002u, 0.0);   // a potion brewed after the save
    D::Saved s;
    s.seen = { 0x01u, 0xFF000001u, 0xFF000009u };
    s.boosted = { { 0xFF000009u, 60u } };
    const auto [removed, added] = t.ReplaceDynamic(s, 100.0);
    CHECK(removed == 2);
    CHECK(added == 2);
    CHECK(t.Seen(0x02u));                  // static: memory kept
    CHECK(t.Boosted(0x02u));
    CHECK_FALSE(t.Seen(0xFF000002u));      // the engine may reuse that ID
    CHECK_FALSE(t.Boosted(0xFF000002u));
    CHECK(t.Boosted(0xFF000009u));
    CHECK(t.MultFor(Defaults(), 0xFF000009u, 100.0) == doctest::Approx(D::Mult(Defaults(), 60.0)));
}

TEST_CASE("discovery tracker: Clear forgets the character (a new game)")
{
    D::Tracker t;
    t.Prime(std::vector<std::uint32_t>{ 0x01u });
    t.OnGained(0x02u, 0.0);
    t.Clear();
    CHECK(t.SeenCount() == 0);
    CHECK(t.BoostedCount() == 0);
}

// ---------------------------------------------------------------------------
// The DISC record
// ---------------------------------------------------------------------------

TEST_CASE("cosave DISC: a round trip keeps the seen forms and the boosts with their ages")
{
    CC::DiscoveryRecord r;
    r.seen = { 0x00012EB7u, 0x0003EADDu, 0xFF000801u };
    r.boosted = { { 0x0003EADDu, 0u }, { 0xFF000801u, 4'000'000'000u } };
    const auto blob = CC::EncodeDiscovery(r);
    CHECK(blob.size() == 4 + 4 + r.seen.size() * CC::kSeenStride + 4 + r.boosted.size() * CC::kBoostStride);
    std::string why;
    const auto back = CC::DecodeDiscovery(blob, CC::kDiscoveryVersion, &why);
    REQUIRE_MESSAGE(back.has_value(), why);
    CHECK(back->seen == r.seen);
    CHECK(back->boosted == r.boosted);
    CHECK(back->dropped == 0);

    const auto empty = CC::DecodeDiscovery(CC::EncodeDiscovery(CC::DiscoveryRecord{}), CC::kDiscoveryVersion);
    REQUIRE(empty.has_value());
    CHECK(empty->seen.empty());
    CHECK(empty->boosted.empty());
}

TEST_CASE("cosave DISC: the tracker's export goes through the codec and back unchanged")
{
    D::Tracker t;
    t.Prime(std::vector<std::uint32_t>{ 0x05u, 0x03u });
    t.OnGained(0x07u, 10.0);
    const auto s = t.Export(70.0);
    CC::DiscoveryRecord r;
    r.seen = s.seen;
    r.boosted = s.boosted;
    const auto back = CC::DecodeDiscovery(CC::EncodeDiscovery(r), CC::kDiscoveryVersion);
    REQUIRE(back.has_value());
    D::Saved s2{ back->seen, back->boosted };
    D::Tracker u;
    u.Import(s2, 70.0);
    CHECK(u.Export(70.0).seen == s.seen);
    CHECK(u.Export(70.0).boosted == s.boosted);
}

TEST_CASE("cosave DISC: a version this build does not read is rejected, in the blob or in the record header")
{
    CC::DiscoveryRecord r;
    r.seen = { 1u, 2u };
    auto blob = CC::EncodeDiscovery(r);
    std::string why;
    CHECK_FALSE(CC::DecodeDiscovery(blob, CC::kDiscoveryVersion + 1, &why).has_value());
    CHECK(why.find("version") != std::string::npos);
    Poke<std::uint32_t>(blob, 0, CC::kDiscoveryVersion + 1);
    CHECK_FALSE(CC::DecodeDiscovery(blob, CC::kDiscoveryVersion).has_value());
}

TEST_CASE("cosave DISC: a truncated or padded blob, or a count over its cap, is rejected")
{
    CC::DiscoveryRecord r;
    r.seen = { 1u, 2u, 3u };
    r.boosted = { { 2u, 5u } };
    const auto good = CC::EncodeDiscovery(r);
    for (std::size_t cut = 0; cut < good.size(); ++cut) {
        CHECK_FALSE(CC::DecodeDiscovery(std::span<const std::byte>(good.data(), cut), CC::kDiscoveryVersion).has_value());
    }
    auto longBlob = good;
    longBlob.push_back(std::byte{ 0 });
    CHECK_FALSE(CC::DecodeDiscovery(longBlob, CC::kDiscoveryVersion).has_value());

    std::string why;
    auto hugeSeen = CC::EncodeDiscovery(CC::DiscoveryRecord{});
    Poke<std::uint32_t>(hugeSeen, 4, CC::kMaxSeenEntries + 1);
    CHECK_FALSE(CC::DecodeDiscovery(hugeSeen, CC::kDiscoveryVersion, &why).has_value());
    CHECK(why.find("cap") != std::string::npos);
    auto hugeBoost = CC::EncodeDiscovery(CC::DiscoveryRecord{});
    Poke<std::uint32_t>(hugeBoost, 8, CC::kMaxBoostEntries + 1);
    CHECK_FALSE(CC::DecodeDiscovery(hugeBoost, CC::kDiscoveryVersion, &why).has_value());
    CHECK(why.find("cap") != std::string::npos);

    // the cap the plugin checks before allocating holds the largest valid record
    CHECK(CC::EncodeDiscovery(CC::DiscoveryRecord{}).size() +
              static_cast<std::size_t>(CC::kMaxSeenEntries) * CC::kSeenStride +
              static_cast<std::size_t>(CC::kMaxBoostEntries) * CC::kBoostStride ==
          CC::kMaxDiscoveryBlobBytes);
    static_assert(CC::kMaxDiscoveryBlobBytes < 0xFFFFFFFFull);
}

TEST_CASE("cosave DISC: form 0 is dropped and counted, the rest kept")
{
    CC::DiscoveryRecord r;
    r.seen = { 0u, 7u };
    r.boosted = { { 0u, 1u }, { 7u, 2u } };
    const auto back = CC::DecodeDiscovery(CC::EncodeDiscovery(r), CC::kDiscoveryVersion);
    REQUIRE(back.has_value());
    CHECK(back->seen == std::vector<std::uint32_t>{ 7u });
    REQUIRE(back->boosted.size() == 1);
    CHECK(back->boosted[0].first == 7u);
    CHECK(back->dropped == 2);
}

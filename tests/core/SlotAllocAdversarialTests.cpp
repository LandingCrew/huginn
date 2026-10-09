// Named adversarial cases for the slot allocation core (R7): each a small page
// with a known answer, run under the old arithmetic and the sign-safe one.
// The golden test covers volume; these pin the cases worth naming, and the
// two places the old and new code legitimately part ways.

#include "LegacySlotPolicy.h"
#include "SlotAllocTestSupport.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

using namespace Huginn::Test;
using Huginn::Core::SlotAlloc::Allocate;
using Huginn::Core::SlotAlloc::LogScorePolicy;

namespace
{
    SA::SlotRec Regular(std::int8_t priority = 0)
    {
        SA::SlotRec s;
        s.classIndex = kRegular;
        s.regular = true;
        s.priority = priority;
        s.overrideAccept = 0;
        s.skipEquipped = false;
        return s;
    }

    SA::SlotRec Job(std::uint8_t cls, std::int8_t priority = 0)
    {
        SA::SlotRec s = Regular(priority);
        s.classIndex = cls;
        s.regular = false;
        return s;
    }

    SA::CandidateRec Cand(std::uint32_t id, float utility, std::uint8_t capClass = 0, std::uint32_t extraClasses = 0)
    {
        SA::CandidateRec c;
        c.formID = id;
        c.dedupKey = id;
        c.name = "C" + std::to_string(id);
        c.utility = utility;
        c.score = Huginn::Core::BridgeScore(utility);
        c.matchMask = (1u << kRegular) | extraClasses;
        c.capClass = capClass;
        return c;
    }

    SA::Input Page(std::vector<SA::SlotRec> slots, std::vector<SA::CandidateRec> cands)
    {
        SA::Input in;
        in.nowNs = 5'000'000'000'000LL;
        in.slots = std::move(slots);
        in.candidates = std::move(cands);
        in.settings.keepSlotPositions = true;
        in.settings.holdSeatedItems = true;
        return in;
    }

    std::vector<std::uint32_t> Shown(const SA::Input& in, const SA::Output& out)
    {
        std::vector<std::uint32_t> ids;
        for (const auto& p : SA::PageOf(in, out)) ids.push_back(p.formID);
        return ids;
    }

    /// Both arithmetics, same page; returns it.
    std::vector<std::uint32_t> Both(const SA::Input& in)
    {
        const auto oldOut = Allocate(in, LegacySlotPolicy::From(in.settings));
        const auto newOut = Allocate(in, LogScorePolicy::From(in.settings));
        CHECK(DiffOutputs(in, oldOut, newOut).empty());
        return Shown(in, newOut);
    }
}  // namespace

TEST_CASE("slot adversarial: exact ties keep list order")
{
    auto in = Page({ Regular(), Regular() }, { Cand(1, 0.5f), Cand(2, 0.5f), Cand(3, 0.5f) });
    CHECK(Both(in) == std::vector<std::uint32_t>{ 1, 2 });
}

TEST_CASE("slot adversarial: zero and denormal utilities rank last, ties among them in order")
{
    auto in = Page({ Regular(), Regular(), Regular() },
        { Cand(1, 0.0f), Cand(2, 1e-40f), Cand(3, 0.0f), Cand(4, 0.2f) });
    // The fill takes the list in order (the scorer sorted it; this list is not):
    // a zero utility is still a candidate.
    CHECK(Both(in) == std::vector<std::uint32_t>{ 1, 2, 3 });
}

TEST_CASE("slot adversarial: the class cap with a 2x tie: capped 0.8 equals uncapped 0.4")
{
    // Three of class 1 shown (free = 3), a fourth of class 1 at 0.8 (x0.5 =
    // 0.4) against class 2 at 0.4: an exact tie in the old float arithmetic,
    // ~1e-16 apart in the logs. A tie keeps the earlier pick.
    auto in = Page({ Regular(4), Regular(3), Regular(2), Regular(1) },
        { Cand(1, 3.0f, 1), Cand(2, 2.0f, 1), Cand(3, 1.0f, 1), Cand(4, 0.8f, 1), Cand(5, 0.4f, 2) });
    CHECK(Both(in) == std::vector<std::uint32_t>{ 1, 2, 3, 4 });
    in.candidates[4].utility = 0.40000004f;   // one ulp more: now it wins
    in.candidates[4].score = Huginn::Core::BridgeScore(in.candidates[4].utility);
    CHECK(Both(in) == std::vector<std::uint32_t>{ 1, 2, 3, 5 });
}

TEST_CASE("slot adversarial: an override takes its matching slot; a pinned vital its configured one")
{
    auto in = Page({ Regular(2), Job(6, 1), Regular(0) }, { Cand(1, 1.0f, 0, 1u << 6), Cand(2, 0.9f), Cand(3, 0.8f) });
    in.slots[1].overrideAccept = 1u << 0;   // HP
    in.overridesActive = true;
    SA::OverrideRec o;
    o.formID = 9;
    o.dedupKey = 9;
    o.name = "Potion";
    o.category = 0;
    o.pinnedToSlot = true;
    o.matchMask = (1u << kRegular) | (1u << 6);
    in.overrides.push_back(o);
    const auto page = Both(in);
    CHECK(page == std::vector<std::uint32_t>{ 1, 9, 2 });
}

TEST_CASE("slot adversarial: a remembered-only row is shown only by its hold, and loses to a ranked stack")
{
    auto ranked = Cand(7, 0.3f);
    ranked.uniqueID = 1;
    ranked.dedupKey = (1ull << 32) | 7;
    auto unranked = Cand(7, 0.0f);
    unranked.isRememberedOnly = true;
    unranked.score = Huginn::Core::kUnrankedScore;
    auto in = Page({ Regular(1), Regular(0) }, { Cand(1, 1.0f), unranked, ranked });
    CHECK(Both(in) == std::vector<std::uint32_t>{ 1, 7 });   // the ranked stack, by the fill
    in.holds[1] = { true, 7 };
    const auto out = Allocate(in);
    const auto page = SA::PageOf(in, out);
    CHECK(page[1].kind == SA::Kind::Remembered);
    CHECK(page[1].key == ranked.dedupKey);   // the best-scoring stack, never the unranked row
    // Under scores of any sign: a ranked stack at -40 still beats the unranked row.
    in.candidates[2].score = -40.0;
    const auto out2 = Allocate(in);
    CHECK(SA::PageOf(in, out2)[1].key == ranked.dedupKey);
}

TEST_CASE("slot adversarial: past the sorted prefix the fill takes list order, so the scorer must sort")
{
    // Twelve items; the job slot's only matches sit at 10 and 11, the worse
    // one first (a partial sort's unsorted tail). The slot code takes the
    // first match in list order: it never re-ranks. With the full sort the
    // scorer now does (UtilityScorer), the better one comes first.
    std::vector<SA::CandidateRec> cands;
    for (std::uint32_t i = 1; i <= 10; ++i) cands.push_back(Cand(i, 2.0f - 0.1f * static_cast<float>(i)));
    cands.push_back(Cand(11, 0.2f, 0, 1u << 3));
    cands.push_back(Cand(12, 0.9f, 0, 1u << 3));
    auto in = Page({ Job(3) }, cands);
    CHECK(Both(in) == std::vector<std::uint32_t>{ 11 });   // the partial sort's pick
    std::stable_sort(in.candidates.begin(), in.candidates.end(),
        [](const SA::CandidateRec& a, const SA::CandidateRec& b) { return a.utility > b.utility; });
    CHECK(Both(in) == std::vector<std::uint32_t>{ 12 });   // the full sort's: the better match
}

TEST_CASE("slot adversarial: a challenger ON the hold's margin holds, in both arithmetics")
{
    // The old hold asked u_c > u_i * 1.5f in float, so a challenger exactly on
    // the margin was decided by rounding. Not measure zero: the old potion
    // tier preference divides a family's next tier by POTION_TIER_STEP = 1.5,
    // the hold's own 1.5, and a recorded play snapshot had Potion of
    // Plentiful Magicka (0.207614) against Minor (0.13840933 = 0.207614 / 1.5)
    // on exactly that boundary. The new test has a tie band of about four
    // float ulps (kHoldTieEpsilon): on the margin, the holder holds.
    using Huginn::Core::BridgeScore;
    const auto legacy = LegacySlotPolicy::From(SA::Settings{});
    const auto fresh = LogScorePolicy::From(SA::Settings{});
    CHECK_FALSE(legacy.Exceeds(0.207614f, 0.13840933f));
    CHECK_FALSE(fresh.Exceeds(BridgeScore(0.207614f), BridgeScore(0.13840933f)));

    // Every challenger exactly float(1.5 * h), wherever that product rounds:
    // the old code holds (c > c is false), and so does the new.
    std::vector<Disagreement> log;
    const auto dual = DualPolicy::From(SA::Settings{}, &log);
    int checked = 0;
    for (float h = 0.5f; h < 0.5005f; h = std::nextafter(h, 2.0f)) {
        const float c = h * 1.5f;
        CHECK_FALSE(dual.Exceeds({ c, BridgeScore(c) }, { h, BridgeScore(h) }));
        ++checked;
    }
    CHECK(log.empty());
    // A potion family's tiers, top / 1.5 / 1.5 ..., as the scorer builds them.
    for (float top = 0.1f; top < 3.0f; top *= 1.013f) {
        const float next = top / 1.5f;
        (void)dual.Exceeds({ top, BridgeScore(top) }, { next, BridgeScore(next) });
        ++checked;
    }
    MESSAGE("on-the-margin pairs checked: ", checked, ", answered differently: ", log.size());
    for (const auto& d : log) CHECK(IsRoundingBoundary(d));
}

TEST_CASE("slot adversarial: rounding boundaries exist for any discount and margin, and are classified")
{
    // Wherever the old code compared rounded float products (u * d^k under
    // the cap, u * m in the hold) and the two sides met within the rounding,
    // the old answer is the rounding's and the new one the exact
    // comparison's -- for any discount and margin, not only the shipped ones.
    // The hold's tie band absorbs the hold's (above); the cap's scan has none,
    // and underflow is its own case. IsRoundingBoundary must recognise each.
    using Huginn::Core::BridgeScore;
    std::vector<Disagreement> log;

    // (a) The hold at margin 0.1 and 1.0: at the boundary both hold (the band).
    for (const float margin : { 0.1f, 1.0f }) {
        SA::Settings s;
        s.challengerMargin = margin;
        const auto dual = DualPolicy::From(s, &log);
        const float m = 1.0f + margin;
        const auto before = log.size();
        for (float h = 0.5f; h < 0.5005f; h = std::nextafter(h, 2.0f)) {
            const float c = h * m;
            CHECK_FALSE(dual.Exceeds({ c, BridgeScore(c) }, { h, BridgeScore(h) }));
        }
        CHECK(log.size() == before);
    }
    // (b) The cap's scan at discount 0.7: an uncapped item exactly equal to
    // float(0.7 * u) of a capped one, where that product rounded up.
    {
        SA::Settings s;
        s.classDiscount = 0.7f;
        const auto dual = DualPolicy::From(s, &log);
        const auto cap = dual.CapFor(s.classFree);   // one step: x0.7, + ln 0.7
        bool found = false;
        for (float u = 0.5f; u < 1.0f && !found; u = std::nextafter(u, 2.0f)) {
            const float p = u * 0.7f;
            if (static_cast<double>(p) > static_cast<double>(u) * static_cast<double>(0.7f) * (1.0 + 1e-9)) {
                const auto capped = dual.Apply({ u, BridgeScore(u) }, cap);
                const auto before = log.size();
                CHECK_FALSE(dual.Greater({ p, BridgeScore(p) }, capped));   // a tie in float
                REQUIRE(log.size() == before + 1);
                CHECK(IsRoundingBoundary(log.back()));
                found = true;
            }
        }
        CHECK(found);
    }
    // (c) Underflow: at discount 1e-30 two steps of the old factor are 0 in
    // float, so every capped item read 0 and tied; the logs still order them.
    {
        SA::Settings s;
        s.classDiscount = 1e-30f;
        s.classFree = 0;
        const auto dual = DualPolicy::From(s, &log);
        const auto cap = dual.CapFor(1);   // two steps
        CHECK(cap.f == 0.0f);
        const auto a = dual.Apply({ 2.0f, BridgeScore(2.0f) }, cap);
        const auto b = dual.Apply({ 1.0f, BridgeScore(1.0f) }, cap);
        const auto before = log.size();
        CHECK_FALSE(dual.Greater(a, b));   // old: 0 > 0
        REQUIRE(log.size() == before + 1);
        CHECK(IsRoundingBoundary(log.back()));
    }
    // A NaN margin (the INI now reads it as the default; the core agrees with
    // the old arithmetic anyway): nothing beats the holder.
    {
        SA::Settings s;
        s.challengerMargin = std::numeric_limits<float>::quiet_NaN();
        const auto dual = DualPolicy::From(s, &log);
        const auto before = log.size();
        CHECK_FALSE(dual.Exceeds({ 9.0f, BridgeScore(9.0f) }, { 0.1f, BridgeScore(0.1f) }));
        CHECK(log.size() == before);
    }
}

TEST_CASE("slot adversarial: negative scores (past the bridge) rank the way their sign says")
{
    // Beyond R7's bridge: what the new scorer will hand over. The cap must
    // lower a negative score and the hold must use the difference.
    auto in = Page({ Regular(3), Regular(2), Regular(1), Regular(0) },
        { Cand(1, 1.0f, 1), Cand(2, 1.0f, 1), Cand(3, 1.0f, 1), Cand(4, 1.0f, 1), Cand(5, 1.0f, 2) });
    const double scores[] = { -0.5, -0.6, -0.7, -1.0, -1.5 };
    for (std::size_t i = 0; i < 5; ++i) in.candidates[i].score = scores[i];
    // The 4th of class 1 at -1.0 + ln 0.5 = -1.69 loses to class 2 at -1.5.
    const auto out = Allocate(in);
    CHECK(Shown(in, out) == std::vector<std::uint32_t>{ 1, 2, 3, 5 });
    // (The old multiplier would have made it -0.5 and kept it.)

    // The hold: seated item 1 at -2.0 against a challenger at -1.7 (0.3 < ln 1.5) holds;
    // against -1.5 (0.5 > ln 1.5) gives way.
    auto hold = Page({ Regular() }, { Cand(2, 1.0f), Cand(1, 1.0f) });
    hold.memory.seats[0] = 1;
    hold.candidates[0].score = -1.7;
    hold.candidates[1].score = -2.0;
    CHECK(Shown(hold, Allocate(hold)) == std::vector<std::uint32_t>{ 1 });
    hold.candidates[0].score = -1.5;
    CHECK(Shown(hold, Allocate(hold)) == std::vector<std::uint32_t>{ 2 });
}

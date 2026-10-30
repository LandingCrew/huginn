// =============================================================================
// R10: wildcards by uncertainty (map Phase 8)
// =============================================================================
// The math of the draw (src/core/WildcardPick.h) and the wildcard key in the
// slot core (src/core/SlotAllocCore.h: SeatOf, PlaceCachedWildcard,
// DrawWildcard): a reserved key per page, the cached wildcard pinned there,
// a roll that draws from what the page does not show weighted by
// relevance x sd plus a uniform share, and an exact propensity.
// =============================================================================

#include "SlotAllocTestSupport.h"
#include "core/WildcardPick.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

using namespace Huginn::Test;
using Huginn::Core::SlotAlloc::Allocate;
namespace W = Huginn::Core::Wildcard;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();

    SA::SlotRec Key(std::int8_t priority, bool wildcards = true)
    {
        SA::SlotRec s;
        s.classIndex = kRegular;
        s.regular = true;
        s.priority = priority;
        s.overrideAccept = 0;
        s.skipEquipped = false;
        s.wildcardsEnabled = wildcards;
        return s;
    }

    /// Eight Regular keys, priority 7..0: the shipped Main page.
    std::vector<SA::SlotRec> MainPage(std::size_t n = 8)
    {
        std::vector<SA::SlotRec> slots;
        for (std::size_t i = 0; i < n; ++i) slots.push_back(Key(static_cast<std::int8_t>(n - 1 - i)));
        return slots;
    }

    /// A candidate with a choice score (mu) and its sd.
    SA::CandidateRec Row(std::uint32_t id, double mu, double sd = 0.0, std::uint32_t extraClasses = 0)
    {
        SA::CandidateRec c;
        c.formID = id;
        c.dedupKey = id;
        c.name = "R" + std::to_string(id);
        c.score = mu;
        c.sd = sd;
        c.matchMask = (1u << kRegular) | extraClasses;
        return c;
    }

    /// Rows 1..n scored n, n-1, ..., 1 (sorted, as the scorer hands them).
    std::vector<SA::CandidateRec> Ranked(std::uint32_t n, double sd = 0.0)
    {
        std::vector<SA::CandidateRec> v;
        for (std::uint32_t i = 1; i <= n; ++i) v.push_back(Row(i, static_cast<double>(n + 1 - i), sd));
        return v;
    }

    SA::Input Page(std::vector<SA::SlotRec> slots, std::vector<SA::CandidateRec> cands)
    {
        SA::Input in;
        in.nowNs = 5'000'000'000'000LL;
        in.slots = std::move(slots);
        in.candidates = std::move(cands);
        return in;
    }

    std::vector<std::uint32_t> Shown(const SA::Input& in, const SA::Output& out)
    {
        std::vector<std::uint32_t> ids;
        for (const auto& p : SA::PageOf(in, out)) ids.push_back(p.formID);
        return ids;
    }

    std::size_t IndexOf(const SA::Input& in, std::uint32_t form)
    {
        for (std::size_t i = 0; i < in.candidates.size(); ++i) {
            if (in.candidates[i].formID == form) return i;
        }
        return SIZE_MAX;
    }
}  // namespace

// -----------------------------------------------------------------------------
// The draw's math
// -----------------------------------------------------------------------------

TEST_CASE("wildcard math: relevance is the pairwise choice probability against the cut")
{
    CHECK(W::Relevance(2.0, 2.0) == doctest::Approx(0.5));
    CHECK(W::Relevance(1.0, 2.0) == doctest::Approx(1.0 / (1.0 + std::exp(1.0))));
    CHECK(W::Relevance(-40.0, 2.0) < 1e-17);
    CHECK(W::Relevance(5.0, 2.0) > 0.95);
    // Any sign (R7): a negative mu against a negative cut.
    CHECK(W::Relevance(-3.0, -2.0) == doctest::Approx(W::Relevance(2.0, 3.0)));
    // Edges: an unranked row is 0; no cut (an empty seat, nothing ranked shown) is 1.
    CHECK(W::Relevance(-kInf, 0.0) == 0.0);
    CHECK(W::Relevance(std::nan(""), 0.0) == 0.0);
    CHECK(W::Relevance(0.5, -kInf) == 1.0);
    CHECK(W::Weight(0.5, 0.5) == doctest::Approx(W::kWeightRef));
    CHECK(W::Weight(0.5, -1.0) == 0.0);
    CHECK(W::Weight(std::nan(""), 1.0) == 0.0);
}

TEST_CASE("wildcard math: the roll's chance runs from base (nothing uncertain) to max (an unknown item at the edge)")
{
    const W::Params p{ 0.165, 0.5, 0.1 };
    CHECK(W::RollProbability(p, 0.0) == doctest::Approx(0.165));
    CHECK(W::RollProbability(p, W::kWeightRef) == doctest::Approx(0.5));
    CHECK(W::RollProbability(p, 10.0) == doctest::Approx(0.5));
    CHECK(W::RollProbability(p, W::kWeightRef / 2) == doctest::Approx(0.165 + 0.335 * 0.5));
    // Bad odds are clamped: max under base reads as base, out of [0, 1] clamps.
    CHECK(W::RollProbability({ 0.4, 0.1, 0.1 }, 1.0) == doctest::Approx(0.4));
    CHECK(W::RollProbability({ -1.0, 2.0, 0.1 }, 1.0) == doctest::Approx(1.0));
    CHECK(W::RollProbability({ 1.0, 1.0, 0.1 }, 0.0) == doctest::Approx(1.0));
}

TEST_CASE("wildcard math: the draw is a mixture -- every row keeps at least share/N, the rest by weight")
{
    const std::vector<double> w{ 0.3, 0.1, 0.0, 0.0 };
    const auto p = W::Distribution(w, 0.1);
    REQUIRE(p.size() == 4);
    CHECK(std::accumulate(p.begin(), p.end(), 0.0) == doctest::Approx(1.0));
    CHECK(p[0] == doctest::Approx(0.1 / 4 + 0.9 * 0.75));
    CHECK(p[1] == doctest::Approx(0.1 / 4 + 0.9 * 0.25));
    CHECK(p[2] == doctest::Approx(0.025));   // a sure row (weight 0) keeps the uniform share
    // No weight at all (the fallback ranking has no sd): uniform.
    const auto u = W::Distribution(std::vector<double>{ 0.0, 0.0, 0.0 }, 0.1);
    for (const double x : u) CHECK(x == doctest::Approx(1.0 / 3));
    // Share 0: pure weight; share 1: pure uniform.
    CHECK(W::Distribution(w, 0.0)[2] == 0.0);
    CHECK(W::Distribution(w, 1.0)[0] == doctest::Approx(0.25));
    CHECK(W::Distribution(std::vector<double>{}, 0.1).empty());
}

TEST_CASE("wildcard math: the inverse CDF picks by uniform, and a form's share sums its stacks")
{
    const std::vector<double> p{ 0.5, 0.0, 0.3, 0.2 };
    CHECK(W::Draw(p, 0.0) == 0u);
    CHECK(W::Draw(p, 0.4999) == 0u);
    CHECK(W::Draw(p, 0.5) == 2u);   // the zero entry is never drawn
    CHECK(W::Draw(p, 0.79) == 2u);
    CHECK(W::Draw(p, 0.81) == 3u);
    CHECK(W::Draw(p, 1.0) == 3u);   // rounding past the total: the last entry with a share
    CHECK(W::Draw(std::vector<double>{}, 0.3) == static_cast<std::size_t>(-1));
    const std::vector<std::uint32_t> forms{ 7, 8, 7, 9 };
    CHECK(W::FormShare(p, forms, 7) == doctest::Approx(0.8));   // two stacks of one form
    CHECK(W::FormShare(p, forms, 9) == doctest::Approx(0.2));
    CHECK(W::FormShare(p, forms, 1) == 0.0);
}

// -----------------------------------------------------------------------------
// The wildcard key
// -----------------------------------------------------------------------------

TEST_CASE("wildcard key: the last key in fill order that takes wildcards, never the first")
{
    CHECK(SA::SeatOf(MainPage()) == 7u);              // the shipped pages: key 8
    auto slots = MainPage();
    slots[7].wildcardsEnabled = false;
    CHECK(SA::SeatOf(slots) == 6u);                   // the player turns key 8 off: key 7
    for (auto& s : slots) s.wildcardsEnabled = false;
    CHECK(SA::SeatOf(slots) == SA::npos);             // off everywhere: no wildcard
    slots[0].wildcardsEnabled = true;
    CHECK(SA::SeatOf(slots) == SA::npos);             // only the top key: never the first in fill order
    CHECK(SA::SeatOf(MainPage(1)) == SA::npos);       // a one-key page has none
    CHECK(SA::SeatOf({}) == SA::npos);
    // Fill order, not slot order: ties keep slot order, so the highest index of
    // the lowest priority is last.
    std::vector<SA::SlotRec> tied{ Key(0), Key(5), Key(0), Key(5) };
    CHECK(SA::SeatOf(tied) == 2u);
    std::vector<SA::SlotRec> reversed{ Key(0), Key(1), Key(2) };
    CHECK(SA::SeatOf(reversed) == 0u);                // slot 0 filled last here: it is the seat
}

TEST_CASE("wildcard key: a cached wildcard is pinned on its seat; the fill serves the other keys as before")
{
    auto in = Page(MainPage(), Ranked(12));
    const auto plain = Allocate(in);
    CHECK(Shown(in, plain) == std::vector<std::uint32_t>{ 1, 2, 3, 4, 5, 6, 7, 8 });
    CHECK(plain.wildcard.seat == 7u);
    CHECK(plain.wildcard.pin == SA::PinFate::None);

    in.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(in, 11));
    const auto out = Allocate(in);
    const auto page = SA::PageOf(in, out);
    CHECK(out.wildcard.pin == SA::PinFate::Placed);
    CHECK(page[7].kind == SA::Kind::Wildcard);
    CHECK(page[7].formID == 11u);
    // Keys 1-7 are the ranked fill; item 8 lost the seat it would have had.
    CHECK(Shown(in, out) == std::vector<std::uint32_t>{ 1, 2, 3, 4, 5, 6, 7, 11 });
}

TEST_CASE("wildcard key: the pinned wildcard is never challenged, so it holds its key for its whole cooldown")
{
    // The old rank-swapped wildcard scored low by design, so the slot hold
    // found it beaten and moved it every pass until a special case exempted
    // it (2026-09-27 19:02:40-46). Pinned, it is outside the hold: over many
    // passes with the seating memory carried on, with scores that wobble, the
    // wildcard stays on key 8 and the other keys are untouched by it.
    auto in = Page(MainPage(), Ranked(14, 0.3));
    in.settings.keepSlotPositions = true;
    in.settings.holdSeatedItems = true;
    in.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(in, 14));   // the worst row
    SA::PageMemory memory;
    bool generation = false;
    for (int pass = 0; pass < 20; ++pass) {
        in.memory = memory;
        in.generationMatches = generation;
        in.nowNs += 300'000'000LL;
        for (std::size_t i = 0; i < in.candidates.size(); ++i) {
            in.candidates[i].score = static_cast<double>(15 - i) + 0.4 * std::sin(pass * 1.7 + static_cast<double>(i));
        }
        const auto out = Allocate(in);
        const auto page = SA::PageOf(in, out);
        CHECK(page[7].kind == SA::Kind::Wildcard);
        CHECK(page[7].formID == 14u);
        for (std::size_t j = 0; j < 7; ++j) CHECK(page[j].kind == SA::Kind::Normal);
        memory = out.memory;
        generation = out.generationMatches;
    }
}

TEST_CASE("wildcard key: when the wildcard ends, the key's owner comes home")
{
    auto in = Page(MainPage(), Ranked(12));
    in.settings.keepSlotPositions = true;
    SA::PageMemory memory;
    bool generation = false;
    auto pass = [&](std::uint32_t pinned) {
        in.memory = memory;
        in.generationMatches = generation;
        in.nowNs += 500'000'000LL;
        in.wildcard.pinned = pinned;
        const auto out = Allocate(in);
        memory = out.memory;
        generation = out.generationMatches;
        return SA::PageOf(in, out);
    };
    CHECK(pass(SA::kNone)[7].formID == 8u);
    CHECK(pass(SA::kNone)[7].formID == 8u);
    const auto during = pass(static_cast<std::uint32_t>(IndexOf(in, 11)));
    CHECK(during[7].formID == 11u);
    CHECK(during[7].kind == SA::Kind::Wildcard);
    const auto after = pass(SA::kNone);
    CHECK(after[7].formID == 8u);
    CHECK(after[7].kind == SA::Kind::Normal);
    for (std::size_t j = 0; j < 7; ++j) CHECK(after[j].formID == j + 1);
}

TEST_CASE("wildcard key: an override on the seat wins the run (kept), a refused item is dropped")
{
    // An override that takes the seat (its only accepting key).
    auto in = Page(MainPage(), Ranked(12));
    in.slots[7].overrideAccept = 1u;
    in.overridesActive = true;
    SA::OverrideRec o;
    o.formID = 0x99;
    o.dedupKey = 0x99;
    o.name = "Potion";
    o.category = 0;
    o.matchMask = 1u << kRegular;
    in.overrides.push_back(o);
    in.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(in, 11));
    auto out = Allocate(in);
    CHECK(out.wildcard.pin == SA::PinFate::SeatBusy);
    CHECK(SA::PageOf(in, out)[7].kind == SA::Kind::Override);
    // ...and a roll waits too: the seat is pinned by the override.
    in.wildcard.pinned = SA::kNone;
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    out = Allocate(in);
    CHECK_FALSE(out.wildcard.rollAsked);
    CHECK(out.wildcard.drawn == SA::kNone);

    // The wildcard is now in the player's hands, on a key that skips what is
    // held: refused, and the seat shows the ranked fill.
    auto held = Page(MainPage(), Ranked(12));
    held.slots[7].skipEquipped = true;
    held.candidates[IndexOf(held, 11)].playerEquipped = true;
    held.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(held, 11));
    out = Allocate(held);
    CHECK(out.wildcard.pin == SA::PinFate::Refused);
    CHECK(SA::PageOf(held, out)[7].formID == 8u);

    // A seat of a class the item does not have: refused.
    auto job = Page(MainPage(), Ranked(12));
    job.slots[7].classIndex = 3;
    job.slots[7].regular = false;
    job.candidates[IndexOf(job, 12)].matchMask |= 1u << 3;
    job.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(job, 11));
    CHECK(Allocate(job).wildcard.pin == SA::PinFate::Refused);
    job.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(job, 12));
    CHECK(Allocate(job).wildcard.pin == SA::PinFate::Placed);
}

TEST_CASE("wildcard roll: draws from what the page does not show, onto the seat, with an exact propensity")
{
    auto in = Page(MainPage(), Ranked(12, 0.5));
    in.wildcard.roll = true;
    in.wildcard.params = { 0.165, 0.5, 0.1 };
    in.wildcard.uRoll = 0.0;   // the roll comes up
    std::map<std::uint32_t, double> pDraw;
    for (int k = 0; k < 100; ++k) {
        in.wildcard.uDraw = (k + 0.5) / 100.0;
        const auto out = Allocate(in);
        REQUIRE(out.wildcard.rollAsked);
        REQUIRE(out.wildcard.rolled);
        REQUIRE(out.wildcard.drawn != SA::kNone);
        CHECK(out.wildcard.pool == 4u);   // rows 9-12: everything the page does not show
        const auto page = SA::PageOf(in, out);
        const auto form = in.candidates[out.wildcard.drawn].formID;
        CHECK(form >= 9u);
        CHECK(page[7].kind == SA::Kind::Wildcard);
        CHECK(page[7].formID == form);
        for (std::size_t j = 0; j < 7; ++j) CHECK(page[j].formID == j + 1);
        // Against the seat's occupant (row 8, mu 5): w = logistic(mu - 5) * 0.5.
        const double mu = in.candidates[out.wildcard.drawn].score;
        CHECK(out.wildcard.rel == doctest::Approx(W::Logistic(mu - 5.0)));
        CHECK(out.wildcard.weight == doctest::Approx(0.5 * W::Logistic(mu - 5.0)));
        CHECK(out.wildcard.topWeight == doctest::Approx(0.5 * W::Logistic(-1.0)));
        CHECK(out.wildcard.pRoll == doctest::Approx(0.165 + 0.335 * W::Uncertainty(0.5 * W::Logistic(-1.0))));
        CHECK(out.wildcard.Propensity() == doctest::Approx(out.wildcard.pRoll * out.wildcard.pDraw));
        pDraw[form] = out.wildcard.pDraw;
    }
    // A grid of uniforms draws each row in proportion to its share: the
    // closest row (9, one logit under the cut) most, the farthest (12) least.
    double total = 0.0;
    for (const auto& [form, p] : pDraw) total += p;
    CHECK(total == doctest::Approx(1.0));
    CHECK(pDraw[9] > pDraw[10]);
    CHECK(pDraw[10] > pDraw[11]);
    CHECK(pDraw[11] > pDraw[12]);
    CHECK(pDraw[12] > 0.1 / 4);   // the uniform share's floor, and then some
}

TEST_CASE("wildcard roll: uncertainty, not score, decides -- a sure item near the cut loses to an unknown one below it")
{
    // Row 9 sits one logit under the cut (row 8, mu 5) but the model is sure
    // of it (sd 0.01); row 10 is two logits under but unknown (sd 0.8, about a
    // never-picked item's prior sd of b).
    auto in = Page(MainPage(), Ranked(12, 0.01));
    in.candidates[IndexOf(in, 10)].sd = 0.8;
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    in.wildcard.params = { 0.165, 0.5, 0.1 };
    in.wildcard.uDraw = 0.5;
    const auto out = Allocate(in);
    REQUIRE(out.wildcard.drawn != SA::kNone);
    CHECK(in.candidates[out.wildcard.drawn].formID == 10u);
    CHECK(out.wildcard.pDraw > 0.85);
    // ...and the roll is likelier for it: its weight is near the reference.
    CHECK(out.wildcard.pRoll > 0.165 + 0.335 * 0.25);
}

TEST_CASE("wildcard roll: the roll can fail, and an empty pool draws nothing")
{
    auto in = Page(MainPage(), Ranked(12, 0.0));   // no sd: the base odds
    in.wildcard.roll = true;
    in.wildcard.params = { 0.165, 0.5, 0.1 };
    in.wildcard.uRoll = 0.2;   // above 0.165
    auto out = Allocate(in);
    CHECK(out.wildcard.rollAsked);
    CHECK(out.wildcard.pRoll == doctest::Approx(0.165));
    CHECK_FALSE(out.wildcard.rolled);
    CHECK(out.wildcard.drawn == SA::kNone);
    CHECK(Shown(in, out) == std::vector<std::uint32_t>{ 1, 2, 3, 4, 5, 6, 7, 8 });
    // With no sd the draw is uniform over the pool.
    in.wildcard.uRoll = 0.1;
    out = Allocate(in);
    REQUIRE(out.wildcard.drawn != SA::kNone);
    CHECK(out.wildcard.pDraw == doctest::Approx(0.25));
    // Eight rows on eight keys: nothing left to draw.
    auto full = Page(MainPage(), Ranked(8, 0.5));
    full.wildcard.roll = true;
    full.wildcard.uRoll = 0.0;
    out = Allocate(full);
    CHECK(out.wildcard.rollAsked);
    CHECK(out.wildcard.pool == 0u);
    CHECK(out.wildcard.drawn == SA::kNone);
    CHECK(Shown(full, out) == std::vector<std::uint32_t>{ 1, 2, 3, 4, 5, 6, 7, 8 });
}

TEST_CASE("wildcard roll: any item type -- the same-type draw is gone -- but only what the seat takes")
{
    // Row 9 matches class 3 only through the extra mask; the seat is a class-3
    // job key, so only row 9 (and 12, also class 3) may be drawn, whatever
    // the top row's type.
    auto in = Page(MainPage(), Ranked(12, 0.5));
    in.slots[7].classIndex = 3;
    in.slots[7].regular = false;
    for (const std::uint32_t f : { 8u, 9u, 12u }) in.candidates[IndexOf(in, f)].matchMask |= 1u << 3;
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    for (int k = 0; k < 20; ++k) {
        in.wildcard.uDraw = (k + 0.5) / 20.0;
        const auto out = Allocate(in);
        REQUIRE(out.wildcard.drawn != SA::kNone);
        const auto form = in.candidates[out.wildcard.drawn].formID;
        CHECK((form == 9u || form == 12u));
        CHECK(out.wildcard.pool == 2u);
    }
    // Two stacks of one form in the pool: its propensity is both shares.
    auto twins = Page(MainPage(), Ranked(10, 0.5));
    auto copy = twins.candidates[IndexOf(twins, 9)];
    copy.uniqueID = 2;
    copy.dedupKey = (2ull << 32) | copy.formID;
    copy.score -= 0.5;
    twins.candidates.push_back(copy);
    twins.wildcard.roll = true;
    twins.wildcard.uRoll = 0.0;
    twins.wildcard.uDraw = 0.0;
    const auto out = Allocate(twins);
    REQUIRE(out.wildcard.drawn != SA::kNone);
    CHECK(twins.candidates[out.wildcard.drawn].formID == 9u);
    CHECK(out.wildcard.pool == 3u);
    // Rows 9, 10 and the copy of 9.
    const double w9 = 0.5 * W::Logistic(2.0 - 3.0), w10 = 0.5 * W::Logistic(1.0 - 3.0), wc = 0.5 * W::Logistic(1.5 - 3.0);
    const double sum = w9 + w10 + wc;
    CHECK(out.wildcard.pDraw == doctest::Approx(0.1 * 2.0 / 3.0 + 0.9 * (w9 + wc) / sum));
}

TEST_CASE("wildcard roll: an empty seat has nothing to draw -- the fill already gave it whatever it takes")
{
    // Seven rows the keys take for eight keys (rows 8 and 9 are in hand, and
    // every key skips what is held): key 8 is empty, the roll is asked, and the
    // pool is empty, because any row the seat would take the fill put there.
    auto in = Page(MainPage(), Ranked(9, 0.5));
    for (auto& s : in.slots) s.skipEquipped = true;
    in.candidates[IndexOf(in, 8)].isEquipped = true;
    in.candidates[IndexOf(in, 9)].isEquipped = true;
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    const auto out = Allocate(in);
    CHECK(SA::PageOf(in, out)[7].kind == SA::Kind::Empty);
    CHECK(out.wildcard.rollAsked);
    CHECK(out.wildcard.pool == 0u);
    CHECK(out.wildcard.drawn == SA::kNone);
}

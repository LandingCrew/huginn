// =============================================================================
// R10: wildcards by uncertainty (map Phase 8)
// =============================================================================
// The math of the draw (src/core/WildcardPick.h) and the wildcard key in the
// slot core (src/core/SlotAllocCore.h: SeatOf, PlaceCachedWildcard,
// DrawWildcard): a reserved key per page, the cached wildcard pinned there,
// a roll that draws from what the page does not show weighted by
// relevance x sd plus a uniform share, and an exact propensity. And the gate
// around them (src/core/WildcardGate.h): each page's cache and clocks, one
// roll per refractory window, only the displayed page rolling.
// =============================================================================

#include "SlotAllocTestSupport.h"
#include "core/WildcardGate.h"
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
        CHECK(out.wildcard.movedTo == SA::npos);   // row 8 is the weakest: it leaves, nothing moves
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

TEST_CASE("wildcard key: a pinned wildcard whose item has a seat elsewhere stays on the wildcard key; no other key moves")
{
    // The pin's guard (SlotState::IsPinned counts Kind::Wildcard): seating
    // never moves a pinned row and it claims no seat. Item 3 is seated on key
    // 3, then pinned as the wildcard (in play: a departed item that home keys
    // would bring back). Seating must not carry the wildcard home to key 3,
    // which would leave key 8 to the ranked fill.
    auto in = Page(MainPage(), Ranked(12));
    in.settings.keepSlotPositions = true;
    in.settings.holdSeatedItems = true;
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
    for (int k = 0; k < 2; ++k) {
        const auto before = pass(SA::kNone);
        for (std::size_t j = 0; j < 8; ++j) CHECK(before[j].formID == j + 1);
    }
    const auto three = static_cast<std::uint32_t>(IndexOf(in, 3));
    for (int k = 0; k < 4; ++k) {
        const auto during = pass(three);
        CHECK(during[7].formID == 3u);
        CHECK(during[7].kind == SA::Kind::Wildcard);
        CHECK(during[2].formID != 3u);
        CHECK(during[2].kind == SA::Kind::Normal);
        // Every other key keeps its item.
        for (const std::size_t j : { 0u, 1u, 3u, 4u, 5u, 6u }) CHECK(during[j].formID == j + 1);
    }
    // The wildcard ends: item 3 comes home to key 3, and key 8's owner to key 8.
    const auto after = pass(SA::kNone);
    for (std::size_t j = 0; j < 8; ++j) CHECK(after[j].formID == j + 1);
    CHECK(after[7].kind == SA::Kind::Normal);
}

TEST_CASE("wildcard roll: with seating off the roll still draws, onto the seat of the final page")
{
    // bKeepSlotPositions = false: no seating, so the draw runs straight after
    // the fill (PASS 4w in the no-seating branch).
    auto in = Page(MainPage(), Ranked(12, 0.5));
    in.settings.keepSlotPositions = false;
    in.wildcard.roll = true;
    in.wildcard.params = { 0.165, 0.5, 0.1 };
    in.wildcard.uRoll = 0.0;
    for (int k = 0; k < 20; ++k) {
        in.wildcard.uDraw = (k + 0.5) / 20.0;
        const auto out = Allocate(in);
        REQUIRE(out.wildcard.rolled);
        REQUIRE(out.wildcard.drawn != SA::kNone);
        CHECK(out.wildcard.pool == 4u);
        const auto page = SA::PageOf(in, out);
        const auto form = in.candidates[out.wildcard.drawn].formID;
        CHECK(form >= 9u);
        CHECK(page[7].kind == SA::Kind::Wildcard);
        CHECK(page[7].formID == form);
        for (std::size_t j = 0; j < 7; ++j) CHECK(page[j].formID == j + 1);
    }
    // ...and with the job-key pull on (the other no-seating branch).
    in.settings.fillJobKeysFromRegular = true;
    in.wildcard.uDraw = 0.5;
    const auto out = Allocate(in);
    REQUIRE(out.wildcard.drawn != SA::kNone);
    CHECK(SA::PageOf(in, out)[7].kind == SA::Kind::Wildcard);
}

TEST_CASE("wildcard roll: a row named like one the page shows is not drawn (a second enchanted copy)")
{
    // Row 10 is another form with row 3's name (two enchanted copies of one
    // weapon): the fill dedups by name, and so does the draw's pool.
    auto in = Page(MainPage(), Ranked(12, 0.5));
    in.candidates[IndexOf(in, 10)].name = "R3";
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    for (const bool keep : { true, false }) {
        in.settings.keepSlotPositions = keep;
        for (int k = 0; k < 40; ++k) {
            in.wildcard.uDraw = (k + 0.5) / 40.0;
            const auto out = Allocate(in);
            REQUIRE(out.wildcard.drawn != SA::kNone);
            CHECK_MESSAGE(out.wildcard.pool == 3u, "keep=", keep);   // rows 9, 11, 12
            CHECK(in.candidates[out.wildcard.drawn].formID != 10u);
            const auto page = SA::PageOf(in, out);
            int named = 0;
            for (const auto& p : page) named += p.name == "R3" ? 1 : 0;
            CHECK(named == 1);
        }
    }
    // A pinned wildcard named like a ranked row is placed before the fill
    // (PASS 1w), so the fill passes the twin over: the name is shown once,
    // on the wildcard key.
    in.settings.keepSlotPositions = true;
    in.wildcard.roll = false;
    in.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(in, 10));
    const auto out = Allocate(in);
    CHECK(out.wildcard.pin == SA::PinFate::Placed);
    const auto page = SA::PageOf(in, out);
    CHECK(page[7].formID == 10u);
    for (std::size_t j = 0; j < 7; ++j) CHECK(page[j].name != "R3");
}

TEST_CASE("wildcard key: the page's best row seated on the wildcard key is not pushed off the page")
{
    // With seating the seat's owner is whoever got there first, and the slot
    // hold (R9's challenger rule) keeps it: it can be the page's best row.
    // The wildcard takes the seat, not that row's place on the page: the row
    // moves to the key of the weakest row shown, and that row leaves instead.
    // Before this, the page lost row 1 for the whole cooldown while rows 2-8
    // kept keys 1-7 (the verifier's probe on PR #230).
    auto base = Page(MainPage(), Ranked(12, 0.5));
    base.settings.keepSlotPositions = true;
    base.settings.holdSeatedItems = true;
    base.memory.seats[7] = 1;                                            // row 1, the best, owns key 8
    for (std::size_t j = 0; j < 7; ++j) base.memory.seats[j] = j + 2;    // rows 2..8 own keys 1..7
    const auto one = static_cast<std::uint32_t>(IndexOf(base, 1));
    const auto seven = static_cast<std::uint32_t>(IndexOf(base, 7));
    const auto eight = static_cast<std::uint32_t>(IndexOf(base, 8));

    // No wildcard: the page as seated.
    CHECK(Shown(base, Allocate(base)) == std::vector<std::uint32_t>{ 2, 3, 4, 5, 6, 7, 8, 1 });

    SUBCASE("a cached wildcard pinned: row 1 takes key 7 from row 8, held there, home when it ends")
    {
        auto in = base;
        in.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(in, 12));
        const auto out = Allocate(in);
        CHECK(out.wildcard.pin == SA::PinFate::Placed);
        CHECK(Shown(in, out) == std::vector<std::uint32_t>{ 2, 3, 4, 5, 6, 7, 1, 12 });
        CHECK(out.wildcard.movedTo == 6u);
        CHECK(out.wildcard.moved == one);
        CHECK(out.wildcard.left == eight);
        // The next run holds it where it moved to (a guest): nothing moves.
        in.memory = out.memory;
        in.generationMatches = out.generationMatches;
        in.nowNs += 500'000'000LL;
        const auto out2 = Allocate(in);
        CHECK(Shown(in, out2) == std::vector<std::uint32_t>{ 2, 3, 4, 5, 6, 7, 1, 12 });
        CHECK(out2.wildcard.movedTo == SA::npos);
        // The wildcard ends: row 1 goes home to key 8, row 8 back to key 7.
        in.memory = out2.memory;
        in.generationMatches = out2.generationMatches;
        in.nowNs += 500'000'000LL;
        in.wildcard.pinned = SA::kNone;
        const auto out3 = Allocate(in);
        CHECK(Shown(in, out3) == std::vector<std::uint32_t>{ 2, 3, 4, 5, 6, 7, 8, 1 });
    }

    SUBCASE("a roll: the same move, and the cut is row 8 (mu 5), the row that leaves")
    {
        auto in = base;
        in.wildcard.roll = true;
        in.wildcard.uRoll = 0.0;
        in.wildcard.params = { 0.165, 0.5, 0.1 };
        for (int k = 0; k < 20; ++k) {
            in.wildcard.uDraw = (k + 0.5) / 20.0;
            const auto out = Allocate(in);
            REQUIRE(out.wildcard.drawn != SA::kNone);
            const auto form = in.candidates[out.wildcard.drawn].formID;
            CHECK(form >= 9u);
            CHECK(out.wildcard.pool == 4u);
            const auto page = SA::PageOf(in, out);
            for (std::size_t j = 0; j < 6; ++j) CHECK(page[j].formID == j + 2);
            CHECK(page[6].formID == 1u);
            CHECK(page[6].kind == SA::Kind::Normal);
            CHECK(page[7].kind == SA::Kind::Wildcard);
            CHECK(page[7].formID == form);
            CHECK(out.wildcard.movedTo == 6u);
            CHECK(out.wildcard.moved == one);
            CHECK(out.wildcard.left == eight);
            const double mu = in.candidates[out.wildcard.drawn].score;
            CHECK(out.wildcard.rel == doctest::Approx(W::Logistic(mu - 5.0)));
            CHECK(out.wildcard.weight == doctest::Approx(0.5 * W::Logistic(mu - 5.0)));
        }
    }

    SUBCASE("a key whose row a slot lock holds is passed over, and a held form is never drawn")
    {
        // Row 8 is lock-held on key 7, and row 9 on some key of the page (the
        // lock shows it, not the allocation): row 1 takes key 6 from row 7
        // (mu 6), the cut is 6, and the pool is rows 10-12.
        auto in = base;
        in.wildcard.roll = true;
        in.wildcard.uRoll = 0.0;
        in.wildcard.heldForms = { 8u, 9u };
        for (int k = 0; k < 20; ++k) {
            in.wildcard.uDraw = (k + 0.5) / 20.0;
            const auto out = Allocate(in);
            REQUIRE(out.wildcard.drawn != SA::kNone);
            const auto form = in.candidates[out.wildcard.drawn].formID;
            CHECK(form >= 10u);
            CHECK(out.wildcard.pool == 3u);
            const auto page = SA::PageOf(in, out);
            CHECK(page[5].formID == 1u);
            CHECK(page[6].formID == 8u);
            CHECK(out.wildcard.movedTo == 5u);
            CHECK(out.wildcard.left == seven);
            const double mu = in.candidates[out.wildcard.drawn].score;
            CHECK(out.wildcard.rel == doctest::Approx(W::Logistic(mu - 6.0)));
        }
    }

    SUBCASE("a locked key is passed over though the allocation put another row on it (a roll)")
    {
        // The lock on key 7 holds some other form (it lost the key this
        // run): ApplyLocks shows that form there, whatever row 8 does, so
        // row 1 moves to key 6 (row 7 leaves) -- the same move as above,
        // read from the key, not from the row on it.
        auto in = base;
        in.wildcard.roll = true;
        in.wildcard.uRoll = 0.0;
        in.wildcard.heldKeys = 1u << 6;
        in.wildcard.heldForms = { 99u };
        for (int k = 0; k < 20; ++k) {
            in.wildcard.uDraw = (k + 0.5) / 20.0;
            const auto out = Allocate(in);
            REQUIRE(out.wildcard.drawn != SA::kNone);
            const auto page = SA::PageOf(in, out);
            CHECK(page[5].formID == 1u);
            CHECK(out.wildcard.movedTo == 5u);
            CHECK(out.wildcard.left == seven);
        }
    }

    SUBCASE("a locked key is passed over on a pinned run too")
    {
        auto in = base;
        in.wildcard.pinned = static_cast<std::uint32_t>(IndexOf(in, 12));
        in.wildcard.heldKeys = 1u << 6;
        const auto out = Allocate(in);
        CHECK(out.wildcard.pin == SA::PinFate::Placed);
        CHECK(Shown(in, out) == std::vector<std::uint32_t>{ 2, 3, 4, 5, 6, 1, 8, 12 });
        CHECK(out.wildcard.movedTo == 5u);
        CHECK(out.wildcard.moved == one);
        CHECK(out.wildcard.left == seven);
        // Every key a lock holds: the row has nowhere to go and leaves.
        in.wildcard.heldKeys = 0x7Fu;
        const auto out2 = Allocate(in);
        CHECK(out2.wildcard.pin == SA::PinFate::Placed);
        CHECK(out2.wildcard.movedTo == SA::npos);
        CHECK(Shown(in, out2) == std::vector<std::uint32_t>{ 2, 3, 4, 5, 6, 7, 8, 12 });
    }
}

TEST_CASE("wildcard roll: the cut is the row that leaves the page -- the seat's occupant when no other key takes it")
{
    // Key 8 is a class-3 job key and row 3 is class 3 only, so no other key
    // takes it: it stands on key 8 though it is the page's third-best row,
    // and a wildcard puts it off the page. The weights read against row 3
    // (mu 10), not against the page's lowest row (row 8, mu 5) -- with
    // seating off and on (the seat's occupant then sits there by seating).
    for (const bool keep : { false, true }) {
        auto in = Page(MainPage(), Ranked(12, 0.5));
        in.settings.keepSlotPositions = keep;
        in.settings.holdSeatedItems = keep;
        in.slots[7].classIndex = 3;
        in.slots[7].regular = false;
        in.candidates[IndexOf(in, 3)].matchMask = 1u << 3;
        for (const std::uint32_t f : { 9u, 10u }) in.candidates[IndexOf(in, f)].matchMask |= 1u << 3;
        in.wildcard.roll = true;
        in.wildcard.uRoll = 0.0;
        in.wildcard.params = { 0.165, 0.5, 0.1 };
        for (int k = 0; k < 20; ++k) {
            in.wildcard.uDraw = (k + 0.5) / 20.0;
            const auto out = Allocate(in);
            REQUIRE(out.wildcard.drawn != SA::kNone);
            const auto form = in.candidates[out.wildcard.drawn].formID;
            CHECK_MESSAGE((form == 9u || form == 10u), "keep=", keep);
            CHECK(out.wildcard.pool == 2u);
            CHECK(out.wildcard.movedTo == SA::npos);
            const auto page = SA::PageOf(in, out);
            const std::uint32_t expect[7] = { 1, 2, 4, 5, 6, 7, 8 };
            for (std::size_t j = 0; j < 7; ++j) CHECK(page[j].formID == expect[j]);
            CHECK(page[7].formID == form);
            const double mu = in.candidates[out.wildcard.drawn].score;
            CHECK_MESSAGE(out.wildcard.rel == doctest::Approx(W::Logistic(mu - 10.0)), "keep=", keep);
            CHECK(out.wildcard.weight == doctest::Approx(0.5 * W::Logistic(mu - 10.0)));
            CHECK(out.wildcard.topWeight == doctest::Approx(0.5 * W::Logistic(4.0 - 10.0)));   // row 9, mu 4
        }
    }
}

// -----------------------------------------------------------------------------
// The gate: each page's cached wildcard and its clocks (core/WildcardGate.h)
// -----------------------------------------------------------------------------

namespace
{
    using Gate = W::Gate<4>;
    constexpr std::int64_t kSec = 1'000'000'000LL;

    auto UseAll()
    {
        return [](std::uint32_t) { return Huginn::Core::CachedWildcardFate::Use; };
    }

    W::CommitIn Drew(std::uint32_t form, double p = 0.25)
    {
        W::CommitIn c;
        c.rollAsked = true;
        c.drawnForm = form;
        c.drawnUniqueID = 3;
        c.propensity = p;
        return c;
    }

    W::CommitIn Missed()
    {
        W::CommitIn c;
        c.rollAsked = true;
        return c;
    }

    Gate Fresh(double cooldown = 30.0, double refractory = 60.0)
    {
        Gate g;
        g.settings = { cooldown, refractory };
        g.Reset(0);
        return g;
    }

    constexpr W::GatePage kMain{ .index = 0, .slotCount = 8, .seat = 7 };
}  // namespace

TEST_CASE("wildcard gate: the displayed page rolls once the refractory is over; a drawn wildcard is pinned for its cooldown")
{
    auto g = Fresh();
    CHECK_FALSE(g.Plan(kMain, 59 * kSec, UseAll()).roll);   // the first window runs from the reset
    auto plan = g.Plan(kMain, 60 * kSec, UseAll());
    CHECK(plan.roll);
    CHECK(plan.pinForm == 0u);
    CHECK(g.Commit(0, Drew(0x1008), 60 * kSec) == W::CommitFate::Cached);
    REQUIRE(g.Page(0) != nullptr);
    CHECK(g.Page(0)->formID == 0x1008u);
    CHECK(g.Page(0)->propensity == doctest::Approx(0.25));
    // Pinned, never rolled, until the cooldown lapses.
    plan = g.Plan(kMain, 89 * kSec, UseAll());
    CHECK_FALSE(plan.roll);
    CHECK(plan.pinForm == 0x1008u);
    CHECK(plan.pinUniqueID == 3u);
    plan = g.Plan(kMain, 90 * kSec, UseAll());
    CHECK(plan.expired == 0x1008u);
    CHECK(plan.pinForm == 0u);
    CHECK_FALSE(plan.roll);   // the refractory restarts at the expiry
    CHECK(g.Plan(kMain, 150 * kSec, UseAll()).roll);
}

TEST_CASE("wildcard gate: one roll per refractory window -- a roll that draws nothing is spent")
{
    auto g = Fresh();
    REQUIRE(g.Plan(kMain, 60 * kSec, UseAll()).roll);
    CHECK(g.Commit(0, Missed(), 60 * kSec) == W::CommitFate::RollSpent);
    // No retry on the next runs: the window is over.
    CHECK_FALSE(g.Plan(kMain, 61 * kSec, UseAll()).roll);
    CHECK_FALSE(g.Plan(kMain, 119 * kSec, UseAll()).roll);
    CHECK(g.Plan(kMain, 120 * kSec, UseAll()).roll);
    // A run whose seat was busy (an override) never evaluated the roll: not
    // spent, the next run in the window rolls.
    CHECK(g.Commit(0, W::CommitIn{}, 120 * kSec) == W::CommitFate::None);
    CHECK(g.Plan(kMain, 121 * kSec, UseAll()).roll);

    // The window, not the run, is what P(roll) is per: over many windows the
    // share that show a wildcard is P(roll), whatever the run rate.
    auto h = Fresh(30.0, 60.0);
    const double pRoll = 0.3;
    int windows = 0;
    int shown = 0;
    std::int64_t now = 60 * kSec;
    for (int run = 0; run < 200000; ++run, now += kSec / 10) {
        const auto plan2 = h.Plan(kMain, now, UseAll());
        if (!plan2.roll) continue;
        ++windows;
        // A deterministic low-discrepancy uniform for the roll.
        const double u = std::fmod(windows * 0.6180339887498949, 1.0);
        if (u < pRoll) {
            ++shown;
            (void)h.Commit(0, Drew(0x2000, pRoll), now);
        } else {
            (void)h.Commit(0, Missed(), now);
        }
    }
    REQUIRE(windows > 100);
    CHECK(static_cast<double>(shown) / windows == doctest::Approx(pRoll).epsilon(0.05));
}

TEST_CASE("wildcard gate: only the displayed page rolls; an off-screen page pins only its own wildcard (the Wheeler leak)")
{
    auto g = Fresh(1000.0, 0.0);
    REQUIRE(g.Plan(kMain, kSec, UseAll()).roll);
    (void)g.Commit(0, Drew(0x1008), kSec);
    // Page 1 off screen: no roll, and not page 0's wildcard.
    const W::GatePage wheel{ .index = 1, .slotCount = 8, .seat = 7, .display = false };
    auto plan = g.Plan(wheel, 2 * kSec, UseAll());
    CHECK_FALSE(plan.roll);
    CHECK(plan.pinForm == 0u);
    // Page 1 on screen rolls its own; off screen again it pins that one.
    const W::GatePage shown1{ .index = 1, .slotCount = 8, .seat = 7 };
    REQUIRE(g.Plan(shown1, 3 * kSec, UseAll()).roll);
    (void)g.Commit(1, Drew(0x3000), 3 * kSec);
    plan = g.Plan(wheel, 4 * kSec, UseAll());
    CHECK(plan.pinForm == 0x3000u);
    CHECK(g.Plan(kMain, 4 * kSec, UseAll()).pinForm == 0x1008u);
    // A page with no wildcard key: nothing; past the page count: nothing.
    plan = g.Plan({ .index = 2, .slotCount = 1, .seat = W::kNoSeat }, 5 * kSec, UseAll());
    CHECK_FALSE(plan.roll);
    CHECK(plan.pinForm == 0u);
    CHECK_FALSE(g.Plan({ .index = 9, .slotCount = 8, .seat = 7 }, 5 * kSec, UseAll()).roll);
    CHECK(g.Commit(9, Drew(1), 5 * kSec) == W::CommitFate::None);
}

TEST_CASE("wildcard gate: combat drops every page's wildcard and places none; its end restarts every refractory")
{
    auto g = Fresh(1000.0, 10.0);
    REQUIRE(g.Plan(kMain, 10 * kSec, UseAll()).roll);
    (void)g.Commit(0, Drew(0x1008), 10 * kSec);
    const W::GatePage shown1{ .index = 1, .slotCount = 8, .seat = 7 };
    REQUIRE(g.Plan(shown1, 20 * kSec, UseAll()).roll);
    (void)g.Commit(1, Drew(0x3000), 20 * kSec);

    W::GatePage fight = kMain;
    fight.inCombat = true;
    auto plan = g.Plan(fight, 30 * kSec, UseAll());
    CHECK_FALSE(plan.roll);
    CHECK(plan.pinForm == 0u);
    CHECK(plan.combatDropped.size() == 2u);
    CHECK_FALSE(g.Page(0)->Active());
    CHECK_FALSE(g.Page(1)->Active());
    // Off-screen pages in combat: nothing either.
    const W::GatePage wheelFight{ .index = 1, .slotCount = 8, .seat = 7, .inCombat = true, .display = false };
    CHECK(g.Plan(wheelFight, 31 * kSec, UseAll()).pinForm == 0u);
    // Long after the fight began, still nothing.
    CHECK_FALSE(g.Plan(fight, 100 * kSec, UseAll()).roll);
    // Combat over: the refractory restarts on every page, so no roll yet.
    plan = g.Plan(kMain, 101 * kSec, UseAll());
    CHECK(plan.combatOver);
    CHECK_FALSE(plan.roll);
    CHECK_FALSE(g.Plan(kMain, 110 * kSec, UseAll()).roll);
    CHECK(g.Plan(kMain, 111 * kSec, UseAll()).roll);
}

TEST_CASE("wildcard gate: a reshape, a refusal and an unshowable stack drop the wildcard")
{
    auto g = Fresh(1000.0, 10.0);
    REQUIRE(g.Plan(kMain, 10 * kSec, UseAll()).roll);
    (void)g.Commit(0, Drew(0x1008), 10 * kSec);
    // The seat moves under an INI reload: dropped, no refractory owed.
    const W::GatePage moved{ .index = 0, .slotCount = 8, .seat = 6 };
    auto plan = g.Plan(moved, 11 * kSec, UseAll());
    CHECK(plan.reshapeDropped == 0x1008u);
    CHECK(plan.roll);

    // The key refused the pinned wildcard (now in hand under skip-equipped):
    // dropped, and the refractory restarts.
    (void)g.Commit(0, Drew(0x1009), 11 * kSec);
    REQUIRE(g.Plan(moved, 12 * kSec, UseAll()).pinForm == 0x1009u);
    W::CommitIn refused;
    refused.pinFound = true;
    refused.pin = SA::PinFate::Refused;
    CHECK(g.Commit(0, refused, 12 * kSec) == W::CommitFate::Refused);
    CHECK_FALSE(g.Page(0)->Active());
    CHECK_FALSE(g.Plan(moved, 21 * kSec, UseAll()).roll);
    CHECK(g.Plan(moved, 22 * kSec, UseAll()).roll);
    // A refusal for a wildcard whose row was not in the list is not one.
    (void)g.Commit(0, Drew(0x100A), 22 * kSec);
    refused.pinFound = false;
    CHECK(g.Commit(0, refused, 23 * kSec) == W::CommitFate::None);
    CHECK(g.Page(0)->Active());
    // SeatBusy keeps it.
    W::CommitIn busy;
    busy.pinFound = true;
    busy.pin = SA::PinFate::SeatBusy;
    CHECK(g.Commit(0, busy, 24 * kSec) == W::CommitFate::None);
    CHECK(g.Page(0)->formID == 0x100Au);

    // The page's wheel cannot show any row of it (uid-less gear): dropped, and
    // a roll may follow at once; a form absent from the list is kept.
    plan = g.Plan(moved, 25 * kSec, [](std::uint32_t) { return Huginn::Core::CachedWildcardFate::Absent; });
    CHECK(plan.pinForm == 0x100Au);
    plan = g.Plan(moved, 26 * kSec, [](std::uint32_t) { return Huginn::Core::CachedWildcardFate::Drop; });
    CHECK(plan.uidDropped == 0x100Au);
    CHECK(plan.pinForm == 0u);
}

TEST_CASE("wildcard gate: expiry reaches every page, and a reset starts a fresh window")
{
    auto g = Fresh(30.0, 0.0);
    const W::GatePage shown1{ .index = 1, .slotCount = 8, .seat = 7 };
    REQUIRE(g.Plan(shown1, kSec, UseAll()).roll);
    (void)g.Commit(1, Drew(0x3000), kSec);
    CHECK_FALSE(g.UpdateExpiry(30 * kSec));
    CHECK(g.UpdateExpiry(31 * kSec));
    CHECK_FALSE(g.Page(1)->Active());
    CHECK_FALSE(g.UpdateExpiry(32 * kSec));

    (void)g.Commit(1, Drew(0x3001), 40 * kSec);
    g.settings.refractorySec = 60.0;
    g.Reset(50 * kSec);
    CHECK_FALSE(g.Page(1)->Active());
    CHECK_FALSE(g.Plan(kMain, 109 * kSec, UseAll()).roll);
    CHECK(g.Plan(kMain, 110 * kSec, UseAll()).roll);
}

// -----------------------------------------------------------------------------
// 0.24.6: a wildcard counts only once the page shows it
// -----------------------------------------------------------------------------

TEST_CASE("wildcard roll: a row named like an item a slot lock holds is not drawn (the post-lock dedup compares names)")
{
    // A lock on some key holds a form named "R10" that is not row 10 (two
    // enchanted copies of one weapon): SlotLocker's post-lock dedup clears
    // by name (core/DedupRules.h), so a drawn row 10 would be cleared from
    // the seat -- drawn, cached and counted, never seen (R10's verifier).
    auto in = Page(MainPage(), Ranked(12, 0.5));
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    in.wildcard.heldNames = { "R10" };
    for (int k = 0; k < 40; ++k) {
        in.wildcard.uDraw = (k + 0.5) / 40.0;
        const auto out = Allocate(in);
        REQUIRE(out.wildcard.drawn != SA::kNone);
        CHECK(out.wildcard.pool == 3u);   // rows 9, 11, 12
        CHECK(in.candidates[out.wildcard.drawn].formID != 10u);
    }
    // Without the held name, row 10 is in the pool.
    in.wildcard.heldNames.clear();
    in.wildcard.uDraw = 0.5;
    CHECK(Allocate(in).wildcard.pool == 4u);
}

TEST_CASE("wildcard roll: a row another of its dedup key shadows is never drawn")
{
    // Row 9's key on a second row (another name): one owner per key
    // (Engine::MarkShadowed), so the copy is not in the pool -- its own
    // row is.
    auto in = Page(MainPage(), Ranked(10, 0.5));
    auto copy = Row(9, 0.5, 0.9);
    copy.name = "R9 copy";
    in.candidates.push_back(copy);
    in.wildcard.roll = true;
    in.wildcard.uRoll = 0.0;
    for (int k = 0; k < 40; ++k) {
        in.wildcard.uDraw = (k + 0.5) / 40.0;
        const auto out = Allocate(in);
        REQUIRE(out.wildcard.drawn != SA::kNone);
        CHECK(out.wildcard.pool == 2u);   // rows 9 and 10, not the copy
        CHECK(out.wildcard.drawn != 10u);
    }
}

TEST_CASE("wildcard gate: a draw the slot locks did not show counts for nothing, and the roll waits")
{
    // The page's seat after ApplyLocks: the drawn stack as the wildcard, or not.
    CHECK(W::DrawnShown(true, 0x1008, 3, 0x1008, 3));
    CHECK_FALSE(W::DrawnShown(false, 0x1008, 3, 0x1008, 3));   // the lock kept the old item (a Normal row)
    CHECK_FALSE(W::DrawnShown(true, 0x1008, 0, 0x1008, 3));    // another stack of the form
    CHECK_FALSE(W::DrawnShown(true, 0x2000, 3, 0x1008, 3));    // another item
    CHECK_FALSE(W::DrawnShown(false, 0, 0, 0x1008, 3));        // the post-lock dedup cleared the seat
    CHECK_FALSE(W::DrawnShown(true, 0, 0, 0, 0));              // nothing drawn

    auto g = Fresh();
    REQUIRE(g.Plan(kMain, 60 * kSec, UseAll()).roll);
    auto hidden = Drew(0x1008);
    hidden.drawnShown = false;
    CHECK(g.Commit(0, hidden, 60 * kSec) == W::CommitFate::NotShown);
    // Nothing cached (no cooldown on a key that shows something else) ...
    REQUIRE(g.Page(0) != nullptr);
    CHECK_FALSE(g.Page(0)->Active());
    CHECK(g.Page(0)->formID == 0u);
    // ... and the window is not spent: the next run rolls again.
    const auto plan = g.Plan(kMain, 61 * kSec, UseAll());
    CHECK(plan.roll);
    CHECK(plan.pinForm == 0u);
    // Shown on that run, it is cached with its own propensity.
    CHECK(g.Commit(0, Drew(0x2000, 0.4), 61 * kSec) == W::CommitFate::Cached);
    CHECK(g.Page(0)->formID == 0x2000u);
    CHECK(g.Page(0)->propensity == doctest::Approx(0.4));
    // A pinned wildcard is untouched by the flag (nothing was drawn).
    W::CommitIn pinned;
    pinned.pinFound = true;
    pinned.pin = SA::PinFate::Placed;
    pinned.drawnShown = false;
    CHECK(g.Commit(0, pinned, 62 * kSec) == W::CommitFate::None);
    CHECK(g.Page(0)->formID == 0x2000u);
}

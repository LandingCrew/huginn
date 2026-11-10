// Host tests for src/core/CombatUpsert.h (0.24.13: the challenger rule at the
// combat confidence, bTestAllChallengers).

#include "core/CombatUpsert.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace Huginn::Core;

namespace
{
    constexpr double kInf = std::numeric_limits<double>::infinity();

    CombatUpsertGateInput OpenKey()
    {
        CombatUpsertGateInput in;
        in.enabled = true;
        in.combatLocked = true;
        in.graceMs = 600.0;
        in.sincePressMs = 5000.0;
        in.sinceShownMs = 5000.0;
        in.cooldownMs = 10000.0;
        in.sinceUpsertMs = -1.0;
        return in;
    }
}

TEST_CASE("combat upsert: the barrier is the multiplier times ln(1 + margin)")
{
    CHECK(CombatUpsertBarrier(3.0f, 0.5f) == doctest::Approx(3.0 * std::log(1.5)));
    CHECK(CombatUpsertBarrier(3.0f, 0.5f) == doctest::Approx(1.2164).epsilon(1e-4));
    CHECK(CombatUpsertBarrier(2.0f, 0.5f) == doctest::Approx(2.0 * std::log(1.5)));
    CHECK(CombatUpsertBarrier(1.0f, 0.5f) == doctest::Approx(LogHoldMargin(0.5f)));
}

TEST_CASE("combat upsert: a multiplier of 0 (or below, or NaN) turns it off")
{
    CHECK(CombatUpsertBarrier(0.0f, 0.5f) == kInf);
    CHECK(CombatUpsertBarrier(-1.0f, 0.5f) == kInf);
    CHECK(CombatUpsertBarrier(std::numeric_limits<float>::quiet_NaN(), 0.5f) == kInf);
    CHECK(CombatUpsertBarrier(std::numeric_limits<float>::infinity(), 0.5f) == kInf);
    CHECK_FALSE(CombatUpsertBeats(100.0, -100.0, CombatUpsertBarrier(0.0f, 0.5f)));
}

TEST_CASE("combat upsert: a NaN margin means nothing beats the frozen item; margin 0 means any better one does")
{
    CHECK(CombatUpsertBarrier(3.0f, std::numeric_limits<float>::quiet_NaN()) == kInf);
    CHECK(CombatUpsertBarrier(3.0f, 0.0f) == 0.0);
    CHECK(CombatUpsertBeats(0.1, 0.0, CombatUpsertBarrier(3.0f, 0.0f)));
    CHECK_FALSE(CombatUpsertBeats(0.0, 0.0, CombatUpsertBarrier(3.0f, 0.0f)));
    // A negative margin (clamped away by SlotSettings) never makes a worse item win.
    CHECK_FALSE(CombatUpsertBeats(-0.1, 0.0, CombatUpsertBarrier(3.0f, -0.5f)));
}

TEST_CASE("combat upsert: the challenger must clear the barrier, not reach it")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    CHECK(CombatUpsertBeats(b + 0.001, 0.0, b));
    CHECK_FALSE(CombatUpsertBeats(b, 0.0, b));                // exactly on it holds
    CHECK_FALSE(CombatUpsertBeats(b - 0.001, 0.0, b));
    // Works the same for negative scores: a difference, not a ratio of signs.
    CHECK(CombatUpsertBeats(-1.0 + b + 0.01, -1.0, b));
    CHECK_FALSE(CombatUpsertBeats(-1.0 + b - 0.01, -1.0, b));
    // The [Hold] give-ways while frozen (margins 0.4-0.9) are far below 3 x ln 1.5.
    CHECK_FALSE(CombatUpsertBeats(0.467, -0.369, b));   // 12:45:03.586, +0.836
    // 12:54:47 replay example: Lesser Ward +2.202 over Oakflesh -0.475 (+2.677).
    CHECK(CombatUpsertBeats(2.202, -0.475, b));
}

TEST_CASE("combat upsert: no measurement against an unranked or pinned score")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    CHECK_FALSE(CombatUpsertBeats(1.0, kUnrankedScore, b));   // frozen item never scored
    CHECK_FALSE(CombatUpsertBeats(kUnrankedScore, -5.0, b));
    CHECK_FALSE(CombatUpsertBeats(kPinnedScore, 0.0, b));
    CHECK_FALSE(CombatUpsertBeats(1.0, kPinnedScore, b));
    CHECK_FALSE(CombatUpsertBeats(std::nan(""), 0.0, b));
}

TEST_CASE("combat upsert: an open key passes the gate")
{
    CHECK(DecideCombatUpsertGate(OpenKey()) == CombatUpsertGate::Open);
}

TEST_CASE("combat upsert: off, not combat-locked, wheel open")
{
    auto in = OpenKey();
    in.enabled = false;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Off);
    in = OpenKey();
    in.combatLocked = false;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::NotCombatLocked);
    in = OpenKey();
    in.wheelOpen = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::WheelOpen);
}

TEST_CASE("combat upsert: never an override, a Swap Back item or a key with a hold of its own")
{
    auto in = OpenKey();
    in.shownPinned = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Pinned);
    in = OpenKey();
    in.remembranceHold = true;   // a hold for this key not shown here (refused by the lock)
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::RemembranceHold);
}

TEST_CASE("combat upsert: never a wildcard")
{
    auto in = OpenKey();
    in.shownWildcard = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Wildcard);
}

TEST_CASE("combat upsert: none right after the Wheeler wheel closes")
{
    CHECK(WheelClosedRecently(0.0, 600.0));
    CHECK(WheelClosedRecently(599.0, 600.0));
    CHECK_FALSE(WheelClosedRecently(600.0, 600.0));
    // fPressGraceMs = 0 (or under the floor): the press floor still holds.
    CHECK(WheelClosedRecently(100.0, 0.0));
    CHECK(WheelClosedRecently(kCombatUpsertPressFloorMs - 1.0, 0.0));
    CHECK_FALSE(WheelClosedRecently(kCombatUpsertPressFloorMs, 0.0));
    CHECK_FALSE(WheelClosedRecently(-1.0, 600.0));   // never closed
}

TEST_CASE("combat upsert: press grace -- pressed or changed less than fPressGraceMs ago")
{
    auto in = OpenKey();
    in.sincePressMs = 599.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
    in.sincePressMs = 600.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
    in.sincePressMs = -1.0;   // never pressed
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);

    in = OpenKey();
    in.sinceShownMs = 100.0;   // the key's item arrived 0.1 s ago
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);

    in.graceMs = 0.0;          // fPressGraceMs = 0: no grace gate
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
}

TEST_CASE("combat upsert: at most one per key per cooldown")
{
    auto in = OpenKey();
    in.sinceUpsertMs = 9999.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Cooldown);
    in.sinceUpsertMs = 10000.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
    in.sinceUpsertMs = 0.0;
    in.cooldownMs = 0.0;   // fCombatUpsertCooldownSec = 0: no limit
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
}

TEST_CASE("combat upsert: a key that just took an upsert is in grace and in cooldown")
{
    // The upsert's own change stamps the key's shown time: grace blocks the
    // next run's second change before the cooldown is even asked.
    auto in = OpenKey();
    in.sinceShownMs = 100.0;
    in.sinceUpsertMs = 100.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
    in.sinceShownMs = 700.0;
    in.sinceUpsertMs = 700.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Cooldown);
}

TEST_CASE("combat upsert: planning pairs the widest margins, one challenger per key")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);   // 1.216
    // Keys 0 and 2 open, key 1 not.
    const std::vector<std::optional<SlotScore>> frozen{ -1.0, std::nullopt, 0.0 };
    const std::vector<SlotScore> challengers{ 1.5, 0.5, 2.0 };
    const std::vector<std::vector<bool>> fits{
        { true, true, true },
        { true, true, true },
        { true, true, true },
    };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, b);
    REQUIRE(picks.size() == 2);
    // Widest first: challenger 2 (2.0) on key 0 (-1.0), +3.0.
    CHECK(picks[0].key == 0);
    CHECK(picks[0].challenger == 2);
    CHECK(picks[0].margin == doctest::Approx(3.0));
    // Then challenger 0 (1.5) on key 2 (0.0): +1.5 > 1.216. Challenger 1 never clears.
    CHECK(picks[1].key == 2);
    CHECK(picks[1].challenger == 0);
}

TEST_CASE("combat upsert: a challenger goes only where it fits, and none past the barrier means none")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    const std::vector<std::optional<SlotScore>> frozen{ -1.0, -1.0 };
    const std::vector<SlotScore> challengers{ 2.0 };
    const std::vector<std::vector<bool>> fits{ { false }, { true } };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, b);
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].key == 1);

    const std::vector<SlotScore> weak{ 0.2 };   // +1.2 < 1.216
    CHECK(PlanCombatUpserts(frozen, weak, fits, b).empty());
    // Off: nothing at all.
    CHECK(PlanCombatUpserts(frozen, challengers, fits, CombatUpsertBarrier(0.0f, 0.5f)).empty());
}

TEST_CASE("combat upsert: ties go to the lower key, then the lower challenger")
{
    const double b = 1.0;
    const std::vector<std::optional<SlotScore>> frozen{ 0.0, 0.0 };
    const std::vector<SlotScore> challengers{ 2.0, 2.0 };
    const std::vector<std::vector<bool>> fits{ { true, true }, { true, true } };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, b);
    REQUIRE(picks.size() == 2);
    CHECK(picks[0].key == 0);
    CHECK(picks[0].challenger == 0);
    CHECK(picks[1].key == 1);
    CHECK(picks[1].challenger == 1);
}

TEST_CASE("combat upsert: short fits rows and missing keys read as no")
{
    const std::vector<std::optional<SlotScore>> frozen{ -5.0, -5.0, -5.0 };
    const std::vector<SlotScore> challengers{ 2.0, 3.0 };
    const std::vector<std::vector<bool>> fits{ { true } };   // key 0: challenger 0 only; keys 1, 2: none
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, 1.0);
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].key == 0);
    CHECK(picks[0].challenger == 0);
}

TEST_CASE("combat upsert: a key the allocator wants for an override or hold is left to it")
{
    // A non-urgent override waiting out fMinLockDurationMs: an upsert first
    // would be C -> B -> override on one key, and would restart the wait.
    auto in = OpenKey();
    in.wantedPinned = true;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::WantedPinned);
}

TEST_CASE("combat upsert: a press holds its key for the floor even with fPressGraceMs = 0")
{
    auto in = OpenKey();
    in.graceMs = 0.0;
    in.sincePressMs = 250.0;   // a tap waiting out the 300 ms double-tap window
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
    in.sincePressMs = kCombatUpsertPressFloorMs;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::Open);
    // A longer double-tap window asks for longer (NoteKeyPress at key up).
    in.pressFloorMs = 700.0;
    in.sincePressMs = 650.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
    // fPressGraceMs above the floor wins.
    in = OpenKey();
    in.graceMs = 1500.0;
    in.sincePressMs = 1000.0;
    CHECK(DecideCombatUpsertGate(in) == CombatUpsertGate::PressGrace);
}

TEST_CASE("combat upsert: at most maxPicks per run")
{
    const std::vector<std::optional<SlotScore>> frozen{ -5.0, -5.0 };
    const std::vector<SlotScore> challengers{ 2.0, 3.0 };
    const std::vector<std::vector<bool>> fits{ { true, true }, { true, true } };
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, 1.0, 1);
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].key == 0);         // widest: 3.0 on key 0 (+8.0)
    CHECK(picks[0].challenger == 1);
}

TEST_CASE("combat upsert memory: the page takes one upsert per spacing")
{
    CombatUpsertMemory mem;
    CHECK(mem.PageOpen(1000.0, kCombatUpsertPageSpacingMs));
    mem.NoteUpsert(1000.0);
    CHECK_FALSE(mem.PageOpen(1100.0, kCombatUpsertPageSpacingMs));
    CHECK_FALSE(mem.PageOpen(1000.0 + kCombatUpsertPageSpacingMs - 1.0, kCombatUpsertPageSpacingMs));
    CHECK(mem.PageOpen(1000.0 + kCombatUpsertPageSpacingMs, kCombatUpsertPageSpacingMs));
    mem.EndFight();
    CHECK(mem.PageOpen(1100.0, kCombatUpsertPageSpacingMs));
}

TEST_CASE("combat upsert memory: what an upsert displaced stays out for the fight; any departure for a while")
{
    CombatUpsertMemory mem;
    mem.NoteLeft({ 0xA, 0, "Potion A" }, 1000.0, /*byUpsert=*/true);
    mem.NoteLeft({ 0xC, 0, "Spell C" }, 1000.0, /*byUpsert=*/false);
    CHECK(mem.Excluded(0xA, 0, "Potion A", 600000.0, 10000.0));    // ten minutes on: still out
    CHECK(mem.Excluded(0xC, 0, "Spell C", 5000.0, 10000.0));       // left 4 s ago
    CHECK_FALSE(mem.Excluded(0xC, 0, "Spell C", 11000.0, 10000.0));
    CHECK(mem.Excluded(0xF, 0, "Potion A", 2000.0, 10000.0));      // same name, other form
    CHECK_FALSE(mem.Excluded(0xB, 0, "Other", 2000.0, 10000.0));
    mem.Prune(11000.0, 10000.0);
    CHECK(mem.Size() == 1);                                        // C pruned, A kept
    mem.EndFight();
    CHECK_FALSE(mem.Excluded(0xA, 0, "Potion A", 600000.0, 10000.0));
}

namespace
{
    // One run of SlotLocker::ApplyCombatUpserts's sequence on frozen keys,
    // through the real core pieces: page spacing, the departure memory, the
    // planner with one pick per run.
    struct Page
    {
        struct Key
        {
            uint32_t formID = 0;
            std::string name;
            SlotScore score = 0.0;
            bool fitsAll = true;   ///< fits every key's class (else only the key the allocator placed it on)
        };
        std::vector<Key> keys;
        CombatUpsertMemory mem;
        int changes = 0;

        void Run(double nowMs, const std::vector<Key>& wanted, double barrier, double recentMs)
        {
            mem.Prune(nowMs, recentMs);
            if (!mem.PageOpen(nowMs, kCombatUpsertPageSpacingMs)) return;
            std::vector<std::optional<SlotScore>> frozen;
            for (const auto& k : keys) frozen.push_back(k.score);
            std::vector<SlotScore> scores;
            std::vector<std::size_t> src;
            for (std::size_t j = 0; j < wanted.size(); ++j) {
                const auto& c = wanted[j];
                bool shown = false;
                for (const auto& k : keys) shown = shown || k.formID == c.formID;
                if (shown || mem.Excluded(c.formID, 0, c.name, nowMs, recentMs)) continue;
                src.push_back(j);
                scores.push_back(c.score);
            }
            std::vector<std::vector<bool>> fits(keys.size(), std::vector<bool>(src.size(), false));
            for (std::size_t k = 0; k < keys.size(); ++k) {
                for (std::size_t c = 0; c < src.size(); ++c) {
                    fits[k][c] = src[c] == k || wanted[src[c]].fitsAll;
                }
            }
            for (const auto& p : PlanCombatUpserts(frozen, scores, fits, barrier, 1)) {
                auto& key = keys[p.key];
                mem.NoteLeft({ key.formID, 0, key.name }, nowMs, true);
                mem.NoteUpsert(nowMs);
                key = wanted[src[p.challenger]];
                ++changes;
            }
        }
    };
}

TEST_CASE("combat upsert: no cascade through the item an upsert displaced (verifier, PR #208)")
{
    // Key k (Regular) frozen on healing potion A (s=0); key j (Healing) frozen
    // on C (s=-1.5). The seating wants B (damage, s=+1.5, fits only k) on k and
    // A on j. Run 1: B takes k. After that A, shown nowhere and the allocator's
    // pick for j, must NOT take j: A moving k -> j in 0.1 s is the home-key
    // move the combat lock defers, as two changes.
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    const double recentMs = 10000.0;
    Page page;
    page.keys = { { 0xA, "Potion A", 0.0, true }, { 0xC, "Spell C", -1.5, true } };
    const std::vector<Page::Key> wanted{ { 0xB, "Spell B", 1.5, false }, { 0xA, "Potion A", 0.0, true } };
    page.Run(1000.0, wanted, b, recentMs);
    REQUIRE(page.changes == 1);
    CHECK(page.keys[0].formID == 0xB);
    for (double t = 1100.0; t <= 61000.0; t += 100.0) {   // a minute of runs, past spacing and cooldown
        page.Run(t, wanted, b, recentMs);
    }
    CHECK(page.changes == 1);
    CHECK(page.keys[1].formID == 0xC);
}

TEST_CASE("combat upsert: two keys clearing the barrier at once change one at a time")
{
    const double b = CombatUpsertBarrier(3.0f, 0.5f);
    Page page;
    page.keys = { { 0x1, "Old 1", -2.0, true }, { 0x2, "Old 2", -2.0, true } };
    const std::vector<Page::Key> wanted{ { 0x3, "New 3", 1.0, true }, { 0x4, "New 4", 1.0, true } };
    page.Run(1000.0, wanted, b, 10000.0);
    CHECK(page.changes == 1);
    page.Run(1100.0, wanted, b, 10000.0);       // inside the page spacing
    CHECK(page.changes == 1);
    page.Run(1000.0 + kCombatUpsertPageSpacingMs, wanted, b, 10000.0);
    CHECK(page.changes == 2);
}

// =============================================================================
// 0.24.13: the Bayesian challenger rule at the combat confidence
// =============================================================================

namespace
{
    constexpr double kNoBelief = std::numeric_limits<double>::quiet_NaN();

    /// The 0.24.13-0.24.28 line (0.24.29 passes a fixed margin instead): the multiplier 3 (the fixed barrier for a
    /// pair no belief covers), fChallengerMinGap 0, the confidence
    /// 0.995 (z 2.576).
    CombatUpsertRule Shipped(bool testAll = false)
    {
        CombatUpsertRule r;
        r.barrier = CombatUpsertBarrier(3.0f, 0.5f);
        r.minGap = 0.0;
        r.z = Challenger::ZForConfidence(0.995);
        r.testAll = testAll;
        return r;
    }

    /// The pre-0.24.13 planner, verbatim: every pair past the fixed barrier,
    /// greedy on the margin (stable: lower key, then lower challenger).
    std::vector<CombatUpsertPick> OldPlanner(const std::vector<std::optional<SlotScore>>& frozen,
        const std::vector<SlotScore>& challengers, const std::vector<std::vector<bool>>& fits, double barrier,
        std::size_t maxPicks)
    {
        std::vector<CombatUpsertPick> pairs;
        for (std::size_t k = 0; k < frozen.size(); ++k) {
            if (!frozen[k] || k >= fits.size()) continue;
            for (std::size_t c = 0; c < challengers.size(); ++c) {
                if (c >= fits[k].size() || !fits[k][c]) continue;
                if (!CombatUpsertBeats(challengers[c], *frozen[k], barrier)) continue;
                pairs.push_back({ k, c, challengers[c] - *frozen[k] });
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(), [](const CombatUpsertPick& a, const CombatUpsertPick& b) {
            return a.margin > b.margin;
        });
        std::vector<CombatUpsertPick> picks;
        std::vector<bool> keyTaken(frozen.size(), false), challengerTaken(challengers.size(), false);
        for (const auto& p : pairs) {
            if (picks.size() >= maxPicks) break;
            if (keyTaken[p.key] || challengerTaken[p.challenger]) continue;
            keyTaken[p.key] = true;
            challengerTaken[p.challenger] = true;
            picks.push_back(p);
        }
        return picks;
    }
}

TEST_CASE("combat upsert (0.24.13): the shipped confidence is 0.995, z 2.576 -- about 3x the calm z")
{
    const double z = Challenger::ZForConfidence(0.995);
    CHECK(z == doctest::Approx(2.5758293035489).epsilon(1e-10));
    // The old upsert's barrier was 3x the calm hold's margin; 3 x z(0.8) is
    // z(0.9942), and 0.995 is the nearest round confidence.
    CHECK(Challenger::NormalCdf(3.0 * Challenger::ZForConfidence(0.8)) == doctest::Approx(0.99421).epsilon(1e-4));
    CHECK(z / Challenger::ZForConfidence(0.8) == doctest::Approx(3.06).epsilon(0.005));
}

TEST_CASE("combat upsert (0.24.13): a clearly better challenger takes the key, an uncertain one does not")
{
    const auto rule = Shipped();
    double sd = 0.0;
    // Lead 1.0 at sd 0.2: line 2.576 x 0.2 = 0.515, P = Phi(5) -- takes it.
    CHECK(CombatUpsertTakes(1.0, 0.0, 0.2, rule, &sd));
    CHECK(sd == 0.2);
    // The same lead at sd 0.5: line 1.288, P = Phi(2) = 0.977 < 0.995 -- holds.
    CHECK_FALSE(CombatUpsertTakes(1.0, 0.0, 0.5, rule, &sd));
    CHECK(sd == 0.5);
    // A lead the old fixed barrier (1.216) let through, but the beliefs are
    // wide (sd 0.6: line 1.545): the rule keeps the key.
    CHECK(CombatUpsertBeats(1.3, 0.0, rule.barrier));
    CHECK_FALSE(CombatUpsertTakes(1.3, 0.0, 0.6, rule));
    // ...and a lead under the old barrier that the beliefs are sure of (sd
    // 0.1: line 0.258) takes it.
    CHECK_FALSE(CombatUpsertBeats(0.5, 0.0, rule.barrier));
    CHECK(CombatUpsertTakes(0.5, 0.0, 0.1, rule));
    // Stricter than the calm hold: a lead the calm rule (z 0.84) lets through
    // at sd 0.3 (line 0.25) is under the combat line (0.77).
    CHECK(Challenger::Exceeds(0.5, 0.0, 0.3, 0.0, Challenger::ZForConfidence(0.8)));
    CHECK_FALSE(CombatUpsertTakes(0.5, 0.0, 0.3, rule));
    // ln m adds to the line.
    auto gap = rule;
    gap.minGap = 0.4;
    CHECK(CombatUpsertTakes(0.6, 0.0, 0.0, rule));
    CHECK_FALSE(CombatUpsertTakes(0.4, 0.0, 0.0, gap));
    // A certain gap (sd 0): any lead past ln m and the tie band.
    CHECK(CombatUpsertTakes(1e-3, 0.0, 0.0, rule));
    CHECK_FALSE(CombatUpsertTakes(0.0, 0.0, 0.0, rule));
    // Never a worse item, never a non-finite score.
    CHECK_FALSE(CombatUpsertTakes(-0.1, 0.0, 0.0, rule));
    CHECK_FALSE(CombatUpsertTakes(1.0, kUnrankedScore, 0.2, rule));
    CHECK_FALSE(CombatUpsertTakes(kInf, 0.0, 0.2, rule));
}

TEST_CASE("combat upsert (0.24.13): no belief for the pair -- the fixed barrier, exactly as before")
{
    const auto rule = Shipped();
    double sd = 0.0;
    for (const double s : { kNoBelief, -1.0 }) {
        CHECK(CombatUpsertTakes(rule.barrier + 0.001, 0.0, s, rule, &sd));
        CHECK(sd < 0.0);
        CHECK_FALSE(CombatUpsertTakes(rule.barrier, 0.0, s, rule, &sd));
        CHECK(sd < 0.0);
    }
    // The planner with no sigma, either testAll setting, is the old planner
    // on random pages (the fallback ranking carries no beliefs at all).
    std::mt19937 rng(1310);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int picksSeen = 0;
    for (int it = 0; it < 4000; ++it) {
        const std::size_t keys = 1 + static_cast<std::size_t>(u(rng) * 6);
        const std::size_t chal = static_cast<std::size_t>(u(rng) * 7);
        std::vector<std::optional<SlotScore>> frozen(keys);
        for (auto& f : frozen) {
            if (u(rng) < 0.8) f = std::round((u(rng) * 4.0 - 2.0) * 4.0) / 4.0;   // quarter steps: ties happen
        }
        std::vector<SlotScore> scores(chal);
        for (auto& s : scores) s = u(rng) < 0.05 ? kUnrankedScore : std::round((u(rng) * 4.0 - 1.0) * 4.0) / 4.0;
        std::vector<std::vector<bool>> fits(keys, std::vector<bool>(chal));
        for (auto& row : fits) {
            for (std::size_t c = 0; c < chal; ++c) row[c] = u(rng) < 0.7;
        }
        const double barrier = u(rng) < 0.1 ? CombatUpsertBarrier(0.0f, 0.5f)
            : CombatUpsertBarrier(static_cast<float>(u(rng) * 4.0), u(rng) < 0.2 ? 0.0f : 0.5f);
        const std::size_t maxPicks = u(rng) < 0.5 ? 1 : 10;
        const auto old = OldPlanner(frozen, scores, fits, barrier, maxPicks);
        for (const bool testAll : { false, true }) {
            CombatUpsertRule rule2;
            rule2.barrier = barrier;
            rule2.z = Challenger::ZForConfidence(0.995);
            rule2.testAll = testAll;
            const auto now = PlanCombatUpserts(frozen, scores, fits, {}, rule2, maxPicks);
            REQUIRE(now.size() == old.size());
            for (std::size_t i = 0; i < now.size(); ++i) {
                CHECK(now[i].key == old[i].key);
                CHECK(now[i].challenger == old[i].challenger);
                CHECK(now[i].margin == old[i].margin);
                CHECK(now[i].sd < 0.0);
                CHECK(now[i].passedOver == 0);
            }
            // The fixed-barrier overload is the same planner.
            const auto overload = PlanCombatUpserts(frozen, scores, fits, barrier, maxPicks);
            REQUIRE(overload.size() == old.size());
            for (std::size_t i = 0; i < overload.size(); ++i) {
                CHECK(overload[i].key == old[i].key);
                CHECK(overload[i].challenger == old[i].challenger);
            }
        }
        picksSeen += static_cast<int>(old.size());
    }
    MESSAGE("fallback planner: " << picksSeen << " picks over 4000 random pages, identical to the old planner");
    CHECK(picksSeen > 1000);
}

TEST_CASE("combat upsert (0.24.13): off (an infinite barrier) stays off with beliefs")
{
    auto rule = Shipped();
    rule.barrier = CombatUpsertBarrier(0.0f, 0.5f);
    const std::vector<std::optional<SlotScore>> frozen{ -5.0 };
    const std::vector<SlotScore> challengers{ 5.0 };
    const std::vector<std::vector<bool>> fits{ { true } };
    const std::vector<std::vector<double>> sigma{ { 0.0 } };   // a certain gap of 10
    CHECK(PlanCombatUpserts(frozen, challengers, fits, sigma, rule).empty());
    CHECK_FALSE(PlanCombatUpserts(frozen, challengers, fits, sigma, Shipped()).empty());
}

TEST_CASE("combat upsert (0.24.13): best challenger only, or every challenger in rank order (bTestAllChallengers)")
{
    // Key 0 frozen at 0.0. Challenger 0 ranks first (+1.0) but the beliefs
    // are wide (sd 0.6: line 1.55); challenger 1 (+0.8) is a sure thing
    // (sd 0.1: line 0.26); challenger 2 (+0.9) does not fit the key.
    const std::vector<std::optional<SlotScore>> frozen{ 0.0 };
    const std::vector<SlotScore> challengers{ 1.0, 0.8, 0.9 };
    const std::vector<std::vector<bool>> fits{ { true, true, false } };
    const std::vector<std::vector<double>> sigma{ { 0.6, 0.1, 0.0 } };
    // Off: only the best fitting challenger is asked, and it misses.
    CHECK(PlanCombatUpserts(frozen, challengers, fits, sigma, Shipped(false)).empty());
    // On: the next one in rank order clears the line.
    const auto picks = PlanCombatUpserts(frozen, challengers, fits, sigma, Shipped(true));
    REQUIRE(picks.size() == 1);
    CHECK(picks[0].challenger == 1);
    CHECK(picks[0].passedOver == 1);
    CHECK(picks[0].sd == 0.1);
    CHECK(picks[0].margin == doctest::Approx(0.8));
    // Off, and the best one sure: it takes the key, nothing passed over.
    const std::vector<std::vector<double>> sure{ { 0.1, 0.1, 0.0 } };
    const auto best = PlanCombatUpserts(frozen, challengers, fits, sure, Shipped(false));
    REQUIRE(best.size() == 1);
    CHECK(best[0].challenger == 0);
    CHECK(best[0].passedOver == 0);
    // A challenger already placed on another key this run is not "the best"
    // for the next: two keys, one sure challenger each.
    const std::vector<std::optional<SlotScore>> two{ 0.0, -0.1 };
    const std::vector<std::vector<bool>> fitsTwo{ { true, true, false }, { true, true, false } };
    const std::vector<std::vector<double>> sigmaTwo{ { 0.1, 0.1, 0.0 }, { 0.1, 0.1, 0.0 } };
    const auto both = PlanCombatUpserts(two, challengers, fitsTwo, sigmaTwo, Shipped(false));
    REQUIRE(both.size() == 2);
    CHECK(both[0].key == 1);          // +1.1, the widest margin
    CHECK(both[0].challenger == 0);
    CHECK(both[1].key == 0);
    CHECK(both[1].challenger == 1);   // the best one left for key 0
}

TEST_CASE("combat upsert (0.24.13): no swap-back on fixed beliefs (property test)")
{
    // Random items with fixed scores and a fixed symmetric sigma_Delta per
    // pair; a page of frozen keys run upsert after upsert with no memory
    // (CombatUpsertMemory would bar the displaced item anyway -- this tests
    // the rule alone). No upsert may undo an earlier one, and the page must
    // settle: every upsert raises the page's total score by more than 0.
    std::mt19937 rng(20261030);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int upserts = 0, viaBelief = 0, viaBarrier = 0, passedOver = 0, unsettled = 0, swapBacks = 0;
    for (int world = 0; world < 1500; ++world) {
        const std::size_t items = 4 + static_cast<std::size_t>(u(rng) * 12);
        const std::size_t keys = 1 + static_cast<std::size_t>(u(rng) * static_cast<double>(std::min<std::size_t>(items - 1, 6)));
        std::vector<double> score(items);
        for (auto& s : score) s = std::normal_distribution<double>(0.0, 0.8)(rng);
        std::vector<std::vector<double>> sd(items, std::vector<double>(items, kNoBelief));
        for (std::size_t a = 0; a < items; ++a) {
            for (std::size_t b = a + 1; b < items; ++b) {
                // Mostly beliefs, some pairs without (the fixed barrier).
                const double v = u(rng) < 0.85 ? u(rng) * 0.8 : kNoBelief;
                sd[a][b] = v;
                sd[b][a] = v;
            }
        }
        std::vector<std::vector<bool>> fitsItem(keys, std::vector<bool>(items));
        for (auto& row : fitsItem) {
            for (std::size_t i = 0; i < items; ++i) row[i] = u(rng) < 0.75;
        }
        CombatUpsertRule rule;
        rule.barrier = CombatUpsertBarrier(static_cast<float>(u(rng) * 4.0), 0.5f);
        rule.minGap = u(rng) < 0.5 ? 0.0 : 0.3 * u(rng);
        rule.z = Challenger::ZForConfidence(0.5 + 0.499 * u(rng));
        rule.testAll = u(rng) < 0.5;
        std::vector<std::size_t> shown(keys);
        for (std::size_t k = 0; k < keys; ++k) shown[k] = k;   // the first `keys` items, frozen
        std::set<std::pair<std::size_t, std::size_t>> beaten;   // (winner item, loser item)
        bool settled = false;
        for (int run = 0; run < 200; ++run) {
            std::vector<std::optional<SlotScore>> frozen(keys);
            for (std::size_t k = 0; k < keys; ++k) frozen[k] = score[shown[k]];
            std::vector<std::size_t> pool;
            std::vector<SlotScore> chScores;
            for (std::size_t i = 0; i < items; ++i) {
                if (std::find(shown.begin(), shown.end(), i) != shown.end()) continue;
                pool.push_back(i);
                chScores.push_back(score[i]);
            }
            std::vector<std::vector<bool>> fits(keys, std::vector<bool>(pool.size()));
            std::vector<std::vector<double>> sigma(keys, std::vector<double>(pool.size()));
            for (std::size_t k = 0; k < keys; ++k) {
                for (std::size_t c = 0; c < pool.size(); ++c) {
                    fits[k][c] = fitsItem[k][pool[c]];
                    sigma[k][c] = sd[shown[k]][pool[c]];
                }
            }
            const auto picks = PlanCombatUpserts(frozen, chScores, fits, sigma, rule, 1);
            if (picks.empty()) {
                settled = true;
                break;
            }
            const auto& p = picks[0];
            const std::size_t winner = pool[p.challenger], loser = shown[p.key];
            if (beaten.contains({ loser, winner })) ++swapBacks;
            beaten.insert({ winner, loser });
            shown[p.key] = winner;
            ++upserts;
            if (p.sd >= 0.0) {
                ++viaBelief;
            } else {
                ++viaBarrier;
            }
            passedOver += p.passedOver > 0 ? 1 : 0;
        }
        unsettled += settled ? 0 : 1;
    }
    MESSAGE("combat upsert, fixed beliefs: " << upserts << " upserts (" << viaBelief << " by the rule, " << viaBarrier
            << " by the barrier, " << passedOver << " past a higher challenger), " << swapBacks << " swap-backs, "
            << unsettled << " unsettled");
    CHECK(swapBacks == 0);
    CHECK(unsettled == 0);
    CHECK(viaBelief > 500);
    CHECK(viaBarrier > 20);
    CHECK(passedOver > 20);
}

TEST_CASE("combat upsert (0.24.13): the displaced item cannot take its key back, named case")
{
    // One key; B (+0.6 at sd 0.1) takes it from A. With A then the
    // challenger against B on the same beliefs, the line is >= 0 and A is
    // behind: never, at any confidence, either testAll setting.
    for (const bool testAll : { false, true }) {
        for (const double tau : { 0.5, 0.8, 0.995, 0.999 }) {
            CombatUpsertRule rule = Shipped(testAll);
            rule.z = Challenger::ZForConfidence(tau);
            const std::vector<std::vector<bool>> fits{ { true } };
            const std::vector<std::vector<double>> sigma{ { 0.1 } };
            const std::vector<std::optional<SlotScore>> onA{ 0.0 };
            const std::vector<SlotScore> b{ 0.6 };
            REQUIRE(PlanCombatUpserts(onA, b, fits, sigma, rule).size() == 1);
            const std::vector<std::optional<SlotScore>> onB{ 0.6 };
            const std::vector<SlotScore> a{ 0.0 };
            CHECK(PlanCombatUpserts(onB, a, fits, sigma, rule).empty());
        }
    }
}


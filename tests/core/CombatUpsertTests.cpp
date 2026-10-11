// Host tests for src/core/CombatUpsert.h.

#include "core/CombatUpsert.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

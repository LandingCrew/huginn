#pragma once

#include <cstdint>
#include <vector>

#include "core/SlotAllocCore.h"
#include "core/SlotClassCapMath.h"
#include "core/SlotScoreMath.h"

// =============================================================================
// The OLD slot arithmetic, as a policy for Core::SlotAlloc (tests only)
// =============================================================================
// SlotAllocator 0.23.9 ranked on the float utility: the class cap multiplied
// it by ClassCapFactor (d^k), the slot hold asked u_c * f_c > u_i * f_i * m
// with m = 1.0f + fChallengerMargin, and every comparison was a float `>`.
// This policy is exactly that, so the core run with it IS the old code's
// decision path; SlotAllocGoldenTests.cpp checks it against pages the old
// game code recorded in play, then checks the sign-safe LogScorePolicy gives
// the same pages.
//
// DualPolicy runs both at once: every comparison is made both ways, the old
// answer is followed, and any comparison where the two answers differ is
// recorded with its operands. A run that records nothing took exactly the
// path the new arithmetic would have taken, so its page IS the new page; a
// recorded disagreement must be a float-rounding (or underflow) boundary of
// the old arithmetic, which the golden test checks.
// =============================================================================

namespace Huginn::Test
{
    namespace SA = Core::SlotAlloc;

    struct LegacySlotPolicy
    {
        using Score = float;
        using Cap = float;   // a multiplier, <= 1

        float discount = 1.0f;
        std::uint32_t freePerClass = 0;
        float factor = 1.5f;

        [[nodiscard]] static LegacySlotPolicy From(const SA::Settings& s) noexcept
        {
            return { Core::ClampClassCapDiscount(s.classDiscount), s.classFree, 1.0f + s.challengerMargin };
        }
        [[nodiscard]] bool CapActive() const noexcept { return Core::ClassCapActive(discount); }
        [[nodiscard]] Score Raw(const SA::CandidateRec& c) const noexcept { return c.utility; }
        [[nodiscard]] Cap NoCap() const noexcept { return 1.0f; }
        [[nodiscard]] Cap CapFor(std::uint32_t shown) const noexcept { return Core::ClassCapFactor(discount, freePerClass, shown); }
        [[nodiscard]] bool IsCapped(Cap c) const noexcept { return c < 1.0f; }
        [[nodiscard]] Score Apply(Score s, Cap c) const noexcept { return s * c; }
        [[nodiscard]] bool Greater(Score a, Score b) const noexcept { return a > b; }
        [[nodiscard]] bool RawGreater(Score a, Score b) const noexcept { return a > b; }
        [[nodiscard]] bool Exceeds(Score c, Score h) const noexcept { return c > h * factor; }
        [[nodiscard]] static double ToDouble(float v) noexcept { return v; }
    };

    /// One comparison the two arithmetics answered differently.
    struct Disagreement
    {
        enum class Op { IsCapped, Greater, RawGreater, Exceeds } op;
        float oldA = 0, oldB = 0;      // the old operands (B already times m for Exceeds)
        double newA = 0, newB = 0;     // the new operands (B already plus ln m for Exceeds)
        bool oldAnswer = false;
    };

    struct DualPolicy
    {
        struct Score { float u; double s; };
        struct Cap { float f; double t; };

        LegacySlotPolicy legacy;
        SA::LogScorePolicy fresh;
        std::vector<Disagreement>* log = nullptr;

        [[nodiscard]] static DualPolicy From(const SA::Settings& s, std::vector<Disagreement>* log)
        {
            return { LegacySlotPolicy::From(s), SA::LogScorePolicy::From(s), log };
        }

        void Record(Disagreement d) const
        {
            if (log) log->push_back(d);
        }

        [[nodiscard]] bool CapActive() const noexcept { return legacy.CapActive(); }
        [[nodiscard]] Score Raw(const SA::CandidateRec& c) const noexcept { return { legacy.Raw(c), fresh.Raw(c) }; }
        [[nodiscard]] Cap NoCap() const noexcept { return { legacy.NoCap(), fresh.NoCap() }; }
        [[nodiscard]] Cap CapFor(std::uint32_t shown) const noexcept { return { legacy.CapFor(shown), fresh.CapFor(shown) }; }
        [[nodiscard]] bool IsCapped(Cap c) const
        {
            const bool o = legacy.IsCapped(c.f), n = fresh.IsCapped(c.t);
            if (o != n) Record({ Disagreement::Op::IsCapped, c.f, 1.0f, c.t, 0.0, o });
            return o;
        }
        [[nodiscard]] Score Apply(Score s, Cap c) const noexcept { return { legacy.Apply(s.u, c.f), fresh.Apply(s.s, c.t) }; }
        [[nodiscard]] bool Greater(Score a, Score b) const
        {
            const bool o = legacy.Greater(a.u, b.u), n = fresh.Greater(a.s, b.s);
            if (o != n) Record({ Disagreement::Op::Greater, a.u, b.u, a.s, b.s, o });
            return o;
        }
        [[nodiscard]] bool RawGreater(Score a, Score b) const
        {
            const bool o = legacy.RawGreater(a.u, b.u), n = fresh.RawGreater(a.s, b.s);
            if (o != n) Record({ Disagreement::Op::RawGreater, a.u, b.u, a.s, b.s, o });
            return o;
        }
        [[nodiscard]] bool Exceeds(Score c, Score h) const
        {
            const bool o = legacy.Exceeds(c.u, h.u), n = fresh.Exceeds(c.s, h.s);
            if (o != n) Record({ Disagreement::Op::Exceeds, c.u, h.u * legacy.factor, c.s, h.s + fresh.logMargin, o });
            return o;
        }
        [[nodiscard]] static double ToDouble(Score v) noexcept { return v.s; }
        [[nodiscard]] static double ToDouble(Cap v) noexcept { return v.t; }
    };
}  // namespace Huginn::Test

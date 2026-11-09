#pragma once

// =============================================================================
// COVERAGE (0.23.46) -- covered(i): how much of an item's effect is already up
// =============================================================================
// Doc 11 (section 2.3, step 3): one runtime cross-feature, logged on every
// selection log v3 row, saying how much of item i's effect is already running
// on the player. Logged from 0.23.46; since R8.3 (0.24.0) the choice score
// weighs it with theta_covered (core/ChoiceScore.h).
//
//   covered(i) = max over the specific columns j of cap(i) of
//                min(1, active_j / cap_j(i)) * left(r_j)
//
//   active_j   the strongest beneficial active effect on the player whose
//              magic effect maps to column j (core/EffectRules.h, the R2
//              classes), graded as cap is: the value column j has in the cap
//              of the item that cast it (the spell, potion or scroll; the staff
//              or the worn piece for an enchantment). A source outside the
//              catalog, or whose cap lacks j (a perk row), has an unknown
//              strength: Effect::kNeutralStrength, as the mapper grades one.
//   r_j        the remaining seconds of the longest-lived such effect
//              (duration - elapsed); kNoTimer for a constant effect (worn
//              gear, an ability-free permanent buff), which never runs out.
//   left(r)    the refresh prior of doc 11's decision 4: a logistic centred at
//              [Scoring] fRefreshPriorSec (R8.1, 0.23.55; default
//              kRefreshPriorSec = 5 s) with scale kRefreshPriorScaleSec = 2 s,
//              1 with no timer. A buff with a few seconds left reads partly
//              uncovered. A PRIOR, labelled as one: R8 fits the curve from the
//              player's refresh picks; the logged `cr` (r of the arg max) lets
//              the fit undo it exactly, covered / left(cr).
//
// Summons (summon_creature*, summon_reanimate) are counted, not graded: with n
// living actors the player commands and the perk limit L (the commanded-actor
// entry point; Twin Souls makes it 2),
//
//   covered = min(1, n / L)           times left(r_out) once n >= L,
//
// r_out the remaining seconds of the summon a new cast would dismiss (the
// oldest). Doc 11's greedy upgrade gate (a better summon at the limit reads
// uncovered) needs the R8 score and is NOT applied here; the context's summon
// list carries what the fit needs to apply it (each summon's source, kind and
// remaining time).
//
// Only specific columns count: families repeat their specifics, and the item
// features (delivery, school, kind ...) describe the item, not an effect.
//
// Known limits: a buff that targets others (Oakflesh on Target) reads covered
// by the player's own Oakflesh; detrimental effects never cover (a fire spell
// is not covered by the player burning).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/EffectColumns.h"
#include "core/EffectMapper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Huginn::Core::Coverage
{
    /// A remaining time that never runs out: a constant effect, or no factor.
    inline constexpr float kNoTimer = -1.0f;

    /// Doc 11 decision 4's refresh prior: the remaining seconds at which a buff
    /// reads half uncovered (the default of [Scoring] fRefreshPriorSec since
    /// R8.1, 0.23.55, which the callers pass as `centreSec`), and the
    /// logistic's scale. R8 fits both from refresh picks.
    inline constexpr float kRefreshPriorSec = 5.0f;
    inline constexpr float kRefreshPriorScaleSec = 2.0f;

    /// left(r): 1 with no timer; at the default centre ~0.08 at 0 s, 0.5 at
    /// 5 s, 0.99 at 15 s. `centreSec` is [Scoring] fRefreshPriorSec.
    [[nodiscard]] inline float LeftPrior(float remainingSec, float centreSec) noexcept
    {
        if (remainingSec < 0.0f) return 1.0f;
        return 1.0f / (1.0f + std::exp(-(remainingSec - centreSec) / kRefreshPriorScaleSec));
    }

    /// The summon columns covered by the count, not by a graded strength.
    [[nodiscard]] constexpr bool IsSummonCountColumn(Effect::Col c) noexcept
    {
        using C = Effect::Col;
        return c == C::summon_creature_fire || c == C::summon_creature_frost || c == C::summon_creature_shock ||
               c == C::summon_creature || c == C::summon_reanimate;
    }

    /// The summon limit as a count: the entry point's float, truncated, at
    /// least 1 (an entry point can set or multiply as well as add, so its
    /// value is not assumed to be 1 + n; a fraction is taken to round down).
    [[nodiscard]] inline int SummonLimitCount(float limit) noexcept
    {
        if (!(limit >= 1.0f)) return 1;   // NaN and anything under 1
        return static_cast<int>(std::floor(limit + 1e-3f));
    }

    /// Is the player's summon limit reached? The summon filter's test (0.23.46:
    /// it compared against 1 whatever the perks said -- the Twin Souls bug).
    [[nodiscard]] inline bool SummonLimitReached(int summons, float limit) noexcept
    {
        return summons > 0 && summons >= SummonLimitCount(limit);
    }

    /// What is active on the player, in cap(i)'s column space.
    struct Active
    {
        std::array<float, Effect::kColumnCount> strength{};    // 0 = nothing of the column
        std::array<float, Effect::kColumnCount> remaining{};   // valid where strength > 0; kNoTimer = constant
        int summons = 0;                // living actors the player commands (the summon filter's count)
        float summonLimit = 1.0f;       // the commanded-actor limit (perk entry point)
        float summonRemaining = kNoTimer;  // the oldest summon's remaining seconds (kNoTimer: none, or no timer)

        /// One active effect of column `c`: the strongest strength, and the
        /// longest remaining time (a constant one outlasts any timer).
        void Add(Effect::Col c, float s, float remainingSec) noexcept
        {
            const auto i = static_cast<std::size_t>(c);
            if (i >= Effect::kColumnCount || !(s > 0.0f)) return;
            const bool had = strength[i] > 0.0f;
            strength[i] = std::max(strength[i], s);
            if (!had) {
                remaining[i] = remainingSec;
            }
            else if (remaining[i] >= 0.0f) {
                remaining[i] = remainingSec < 0.0f ? kNoTimer : std::max(remaining[i], remainingSec);
            }
        }
    };

    /// One living actor the player commands (the player's commanded-actor
    /// list), with the effect that made it: logged in every v3 context so the
    /// fit can recount (thralls apart from summons, doc 11 decision 3) and
    /// apply the upgrade gate (each summon's source, doc 11 decision 2).
    struct CommandedActor
    {
        enum class Kind : std::uint8_t { Summon, Reanimate, Other };
        std::uint32_t source = 0;      // the spell or scroll that cast it, or the staff (0: unknown)
        std::uint32_t actor = 0;       // the commanded actor's reference
        Kind kind = Kind::Other;       // its effect's archetype: summon, reanimate, anything else
        float remaining = kNoTimer;    // seconds left on that effect (kNoTimer: no timer)
        float elapsed = 0.0f;          // seconds since it was cast (the oldest is dismissed first)
    };

    /// The remaining seconds of the summon a new cast would dismiss: the
    /// oldest (largest elapsed) summon or thrall; kNoTimer when there is none.
    template <class Range>
    [[nodiscard]] float OldestRemaining(const Range& commanded) noexcept
    {
        const CommandedActor* oldest = nullptr;
        for (const auto& c : commanded) {
            if (c.kind == CommandedActor::Kind::Other) continue;
            if (!oldest || c.elapsed > oldest->elapsed) oldest = &c;
        }
        return oldest ? oldest->remaining : kNoTimer;
    }

    struct Result
    {
        float value = 0.0f;          // covered(i), 0..1
        float remaining = kNoTimer;  // r of the column that gave it (kNoTimer: no timer, or no factor)
    };

    /// covered(i), its left(r) centred at `refreshCentreSec` ([Scoring] fRefreshPriorSec).
    [[nodiscard]] inline Result Covered(const Effect::Cap& cap, const Active& a, float refreshCentreSec) noexcept
    {
        Result best;
        for (const auto& v : cap) {
            const auto i = static_cast<std::size_t>(v.col);
            if (i >= Effect::kColumnCount || !(v.v > 0.0f)) continue;
            if (Effect::kColumns[i].level != Effect::Level::Specific) continue;
            float share = 0.0f;
            float r = kNoTimer;
            if (IsSummonCountColumn(v.col)) {
                if (a.summons <= 0) continue;
                const int limit = SummonLimitCount(a.summonLimit);
                share = std::min(1.0f, static_cast<float>(a.summons) / static_cast<float>(limit));
                if (a.summons >= limit) r = a.summonRemaining;
            }
            else {
                const float act = a.strength[i];
                if (!(act > 0.0f)) continue;
                share = std::min(1.0f, act / v.v);
                r = a.remaining[i];
            }
            const float value = share * LeftPrior(r, refreshCentreSec);
            if (value > best.value) best = Result{ value, r };
        }
        return best;
    }

    /// covered(i) in two parts, for doc 11's greedy upgrade gate (R8.2,
    /// core/ChoiceScore.h): the best value over the non-summon specific
    /// columns, and the summon count's own term, which the gate replaces at
    /// the limit. max(other, summonValue) equals Covered(...).value.
    struct Parts
    {
        float other = 0.0f;             // the best non-summon column's value (share * left)
        bool summon = false;            // the cap has a summon count column (summons up or not)
        bool atLimit = false;           // ... and the summon count has reached the limit
        float summonValue = 0.0f;       // min(1, n / L), times left(r_out) at the limit
        float summonLeft = 1.0f;        // left(r_out) at the limit, else 1
    };

    [[nodiscard]] inline Parts CoveredParts(const Effect::Cap& cap, const Active& a, float refreshCentreSec) noexcept
    {
        Parts p;
        for (const auto& v : cap) {
            const auto i = static_cast<std::size_t>(v.col);
            if (i >= Effect::kColumnCount || !(v.v > 0.0f)) continue;
            if (Effect::kColumns[i].level != Effect::Level::Specific) continue;
            if (IsSummonCountColumn(v.col)) {
                p.summon = true;
                if (a.summons <= 0) continue;
                const int limit = SummonLimitCount(a.summonLimit);
                const float share = std::min(1.0f, static_cast<float>(a.summons) / static_cast<float>(limit));
                float left = 1.0f;
                if (a.summons >= limit) {
                    p.atLimit = true;
                    left = LeftPrior(a.summonRemaining, refreshCentreSec);
                }
                p.summonLeft = left;
                p.summonValue = std::max(p.summonValue, share * left);
                continue;
            }
            const float act = a.strength[i];
            if (!(act > 0.0f)) continue;
            const float value = std::min(1.0f, act / v.v) * LeftPrior(a.remaining[i], refreshCentreSec);
            p.other = std::max(p.other, value);
        }
        return p;
    }

    /// The source of a_out, the summon a new cast would dismiss (the oldest
    /// summon or thrall, whose timer is Covered's r_out); 0 when there is none
    /// or its source is unknown.
    template <class Range>
    [[nodiscard]] std::uint32_t OldestSource(const Range& commanded) noexcept
    {
        const CommandedActor* oldest = nullptr;
        for (const auto& c : commanded) {
            if (c.kind == CommandedActor::Kind::Other) continue;
            if (!oldest || c.elapsed > oldest->elapsed) oldest = &c;
        }
        return oldest ? oldest->source : 0;
    }
}

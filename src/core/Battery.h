#pragma once

// =============================================================================
// BATTERY (R8.2) -- memory with a useful life of play
// =============================================================================
// The useful-life math of the old learner (roadmap Phase 3 #3a, 0.23.0), moved
// here from the old learner so the new learner's per-item b
// (ChoiceLearner, doc 9's battery) runs the same arithmetic on the same play
// clock. The old learner calls RetentionAt with lifeMult = 1, which is
// exactly its old expression (the float part is evaluated as before, then
// multiplied by 1.0, which is exact).
//
//   retention(idle) = min(1, (1 + e^(-L/s)) / (1 + e^((idle - L)/s)))
//   L = (T0 + k ln(1 + n)) * lifeMult        hours of play
//
// full strength for about L hours of play after the last pick, then a knee of
// width s. n is the item's evidence (picks); lifeMult > 1 lengthens the life
// (favourites in Boost mode: [Favorites] fFavoriteLifeMult, doc 9's open
// point, taken by the lifetime route).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <cmath>

namespace Huginn::Core::Battery
{
    // Memory with a useful life (roadmap Phase 3 #3a): an item's evidence
    // keeps full strength for a while after its last pick, then fades back to
    // the prior; below forgetBelow the entry is deleted. `enabled` false:
    // nothing fades and nothing is forgotten.
    struct MemoryLife
    {
        bool enabled = true;
        float lifeHours = 8.0f;          // T0: full strength this long after a pick
        float lifePerPickHours = 2.0f;   // k: each doubling of the picks adds ~1.4 k
        float fadeHours = 1.0f;          // s: the width of the knee
        float forgetBelow = 0.05f;       // retention under this deletes the entry

        // The one copy of the allowed ranges (the INI loader and the learners
        // both clamp through this).
        [[nodiscard]] MemoryLife Clamped() const noexcept
        {
            MemoryLife c = *this;
            c.lifeHours = std::clamp(lifeHours, 0.1f, 10000.0f);
            c.lifePerPickHours = std::clamp(lifePerPickHours, 0.0f, 1000.0f);
            c.fadeHours = std::clamp(fadeHours, 0.05f, 1000.0f);
            c.forgetBelow = std::clamp(forgetBelow, 0.0f, 0.5f);
            return c;
        }
    };

    /// Retention at play time `now` (seconds) of an entry last chosen at
    /// `chosenAt` with evidence `trainCount`.
    [[nodiscard]] inline float RetentionAt(const MemoryLife& life, double now, double chosenAt, float trainCount,
                                           double lifeMult = 1.0) noexcept
    {
        if (!life.enabled) {
            return 1.0f;
        }
        // Hours, in double: the clock runs for days.
        const double idle = std::max(0.0, now - chosenAt) / 3600.0;
        const double L = static_cast<double>(life.lifeHours + life.lifePerPickHours * std::log1p(std::max(0.0f, trainCount)))
                         * lifeMult;
        const double s = life.fadeHours;
        const double x = (idle - L) / s;
        if (x > 60.0) {
            return 0.0f;
        }
        return static_cast<float>(std::min(1.0, (1.0 + std::exp(-L / s)) / (1.0 + std::exp(x))));
    }

    /// ForgetFaded's test: the entry's retention is under forgetBelow (never
    /// while the life is off).
    [[nodiscard]] inline bool Faded(const MemoryLife& life, double now, double chosenAt, float trainCount,
                                    double lifeMult = 1.0) noexcept
    {
        return life.enabled && RetentionAt(life, now, chosenAt, trainCount, lifeMult) < life.forgetBelow;
    }
}

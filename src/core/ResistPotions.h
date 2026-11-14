#pragma once

// =============================================================================
// RESIST POTIONS (0.24.29) -- what stacks: potion with spell, not potion with potion
// =============================================================================
// Resist potions of one element do not stack: they share the game's resist
// effect, so a second one only replaces the first. A spell's resist is another
// effect and stacks with a potion's; recasting the same spell only refreshes
// its own. The candidate filters (CandidateFilters) therefore:
//   - drop every resist potion of an element while a POTION's resistance to
//     it is running on the player (Reason::resist, an active-buff drop, so a
//     combat key showing one is covered) -- until that effect is within
//     [Scoring] fRefreshPriorSec of its end (the refresh pass, R8.1);
//   - otherwise, under [Candidates] sResistPotionPick = Greedy (the default),
//     offer only the strongest of the element's potions that pass every other
//     filter (cap(i)'s resist_<element>, which grows with the magnitude;
//     Reason::resist_weaker for the rest; WeakerAmong). All: every one, the
//     score decides.
//   - drop a resist spell while one of ITS OWN resist effects runs on the
//     player (CastBlocked, by the effect's form), until that effect is within
//     fRefreshPriorSec of its end: Fire Shell stacks with a potion, and a
//     cloak's resist rider does not hide another spell;
// A spell, gear, food or a cloak's rider never blocks a potion, and is not
// picked among them. There is no threshold on the player's resistance
// (0.24.29 removed the 50 % one): what the player wears does not stop a
// potion or spell that would stack.
//
// Pure: standard library and core only (src/core/README.md).
// =============================================================================

#include "core/CapFacets.h"
#include "core/RefreshPass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Huginn::Core::ResistPotions
{
    using Element = Refresh::Element;
    inline constexpr std::size_t kElementCount = Refresh::kElementCount;

    /// One bit per element (Element order).
    using Bits = std::uint8_t;
    static_assert(kElementCount <= sizeof(Bits) * 8);

    [[nodiscard]] constexpr Bits Bit(Element e) noexcept
    {
        return static_cast<Bits>(1u << static_cast<unsigned>(e));
    }

    /// The elements a potion's resistance runs for, not yet due for its
    /// refresh: per element, the timer of the player's potion resist effects
    /// (Refresh::Timer: the block ends when the last one does).
    [[nodiscard]] constexpr Bits RunningBits(const std::array<Refresh::Timer, kElementCount>& timers,
                                             float refreshSec) noexcept
    {
        Bits bits = 0;
        for (std::size_t e = 0; e < kElementCount; ++e) {
            const auto& t = timers[e];
            if (t.any && !Refresh::RefreshDue(t.remaining, refreshSec)) bits |= Bit(static_cast<Element>(e));
        }
        return bits;
    }

    /// The elements an item's resistance answers (the resist filter's facets).
    [[nodiscard]] constexpr Bits ElementsOf(const Facets::Set& f) noexcept
    {
        using F = Facets::F;
        Bits bits = 0;
        if (f.Has(F::ResistFire)) bits |= Bit(Element::fire);
        if (f.Has(F::ResistFrost)) bits |= Bit(Element::frost);
        if (f.Has(F::ResistShock)) bits |= Bit(Element::shock);
        if (f.Has(F::ResistPoison)) bits |= Bit(Element::poison);
        if (f.Has(F::ResistMagic)) bits |= Bit(Element::magic);
        return bits;
    }

    /// A resist potion whose element a running potion already resists: it
    /// would only replace that one.
    [[nodiscard]] constexpr bool Blocked(const Facets::Set& f, Bits running) noexcept
    {
        return Facets::ResistJudgeable(f) && (ElementsOf(f) & running) != 0;
    }

    /// A potion's resistance per element (cap(i)'s resist_<element>; 0: none).
    using Strengths = std::array<float, kElementCount>;

    [[nodiscard]] constexpr bool Any(const Strengths& s) noexcept
    {
        for (const float v : s) {
            if (v > 0.0f) return true;
        }
        return false;
    }

    /// The strongest on hand per element, one potion at a time.
    constexpr void NoteBest(Strengths& best, const Strengths& s) noexcept
    {
        for (std::size_t e = 0; e < kElementCount; ++e) {
            if (s[e] > best[e]) best[e] = s[e];
        }
    }

    /// Greedy: not the strongest for any element it resists. One that ties
    /// the best is kept (two stacks of one strength both stay; the score
    /// decides between them).
    [[nodiscard]] constexpr bool Weaker(const Strengths& s, const Strengths& best) noexcept
    {
        bool resists = false;
        for (std::size_t e = 0; e < kElementCount; ++e) {
            if (!(s[e] > 0.0f)) continue;
            resists = true;
            if (s[e] >= best[e]) return false;
        }
        return resists;
    }

    /// Greedy over the resist potions that passed every other filter (`kept`:
    /// each one's strengths): true for each that is not the strongest kept for
    /// any element it resists (Weaker; a tie keeps both). The best is chosen
    /// among those survivors only (review of PR #254): a stronger potion that
    /// another filter drops -- favourite-suppressed, or blocked because a
    /// running potion covers another of its elements -- is not on offer, and
    /// must not drop the weaker ones, or no potion of the element is offered.
    [[nodiscard]] inline std::vector<bool> WeakerAmong(std::span<const Strengths> kept)
    {
        Strengths best{};
        for (const auto& s : kept) NoteBest(best, s);
        std::vector<bool> weaker(kept.size(), false);
        for (std::size_t i = 0; i < kept.size(); ++i) weaker[i] = Weaker(kept[i], best);
        return weaker;
    }

    /// The resist effects (their forms) a spell, scroll or staff put on the
    /// player that still run and are not due for their refresh.
    struct CastEffects
    {
        static constexpr std::size_t kMax = 8;
        std::array<std::uint32_t, kMax> ids{};
        std::uint8_t n = 0;

        constexpr void Add(std::uint32_t id) noexcept
        {
            for (std::size_t i = 0; i < n; ++i) {
                if (ids[i] == id) return;
            }
            if (n < kMax) ids[n++] = id;
        }

        [[nodiscard]] constexpr bool Has(std::uint32_t id) const noexcept
        {
            for (std::size_t i = 0; i < n; ++i) {
                if (ids[i] == id) return true;
            }
            return false;
        }

        constexpr bool operator==(const CastEffects& o) const noexcept
        {
            if (n != o.n) return false;
            for (std::size_t i = 0; i < n; ++i) {
                if (!o.Has(ids[i])) return false;
            }
            return true;
        }
    };

    /// A resist spell one of whose resist effects (`own`, its effects' forms)
    /// already runs: casting it again only refreshes that one.
    [[nodiscard]] constexpr bool CastBlocked(std::span<const std::uint32_t> own, const CastEffects& running) noexcept
    {
        for (const auto id : own) {
            if (running.Has(id)) return true;
        }
        return false;
    }
}

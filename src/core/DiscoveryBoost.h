#pragma once

// =============================================================================
// DISCOVERY BOOST (0.24.14) -- a newly gained item starts more uncertain
// =============================================================================
// R10's wildcard key draws an off-page row by relevance x sd
// (core/WildcardPick.h). A spell learned or an item found during play has no
// b entry yet, so its b reads the prior N(0, 0.25) -- the same sd as every
// other never-picked item, of which a character carries hundreds. Nothing
// marks it as new, and the draw gives it no more chance than an old potion
// the player has walked past for forty hours. That is the bootstrap's
// limitation the Nexus page names: the model starts from a fit, and a fit
// knows nothing of what the player gains afterwards.
//
// The boost: an item the player newly GAINS during play gets, for the
// wildcard draw only, b's variance multiplied by
//
//     m(age) = 1 + (M - 1) * 2^(-age / H)          age = play seconds since gained
//
// (M = [Learning] fDiscoveryBoostVarMult, H = fDiscoveryBoostHalfLifeSec),
// so its sd starts sqrt(M) times b's and decays back to it; its FIRST PICK
// ends the boost at once (the player found it; from then on the learner's
// evidence speaks). The mean is untouched, so the ranking (mu) and relevance
// are untouched. The row's draw variance is
//
//     var_w = var + (m - 1) * var_b                (ExploreVar)
//
// var the row's score variance (core/ChoiceScore.h), var_b b's variance as
// read now (the battery applied). Only b's part is scaled: the theta part is
// the need x effect pairing's uncertainty, which the item's novelty says
// nothing about.
//
// WHY THE WILDCARD ONLY, not the score's variance. The score's sigma feeds
// two other rules, and in both a larger sigma works AGAINST surfacing:
//   - the challenger rule (core/ChallengerRule.h): c takes i's key only when
//     mu_c - mu_i > ln m + z sigma_Delta, z >= 0. A larger var(b_c) raises
//     that bar, so a boosted challenger would take a held key LATER, and a
//     boosted holder would be harder to move -- the opposite of discovery;
//   - doc 11's upgrade gate at the summon limit (ChoiceScore.h
//     ApplyUpgradeGate): covered reads (1 - logistic(Delta / sigma_Delta)),
//     so a larger var(b) pulls covered toward 0.5 and MOVES mu.
// So the boost is an exploration term on the draw, not a belief: the
// learner's update, the challenger rule, the gate and the logged sd all read
// the unboosted posterior.
//
// WHAT COUNTS AS GAINED (the plugin decides; this file only keeps the sets):
//   - an item entering the player's inventory (TESContainerChangedEvent) whose
//     form this character has never held;
//   - a spell the spell registry adds in play (not one a load brought:
//     SpellRegistry::TakeNewlyLearned);
// and NOT: anything present when a character or a save loads (Prime puts it
// in the seen set with no boost), anything b holds an entry for (picked or
// passed over: the character held it, even if it is used up or sold now), a
// stack count change of a form already seen, or a form gained again after it
// left (sold, dropped, used up, a spell removed and re-added: Arcane
// Anchor's churn). What sits in a chest and has no b entry cannot be primed
// (the plugin reads only the player's inventory), so on the first load with
// DISC such an item reads as new when it comes back.
//
// Dynamic forms (0xFF......) the save names that no longer exist are dropped
// at the load (DropDynamicIf): the engine reuses those IDs.
//
// Tracker: the seen set (every form this character has held or known since
// the feature existed: the first load primes it with the inventory and the
// spells) and the boosted map (form -> play seconds when gained). Gains
// before the load's Prime (a load under way: Suspend) are dropped, so a load
// can never boost what it restored. The seen set is never pruned: the b
// battery's forget pass returns a long-unused item's b to its prior, and that
// item is not new. A boost ends at the first pick or when its excess falls
// under kExpireExcess (Expire, the forget sweep's minute cadence); with the
// defaults (M 4, H 1 h) that is 8.2 h of play, about b's useful life T0.
//
// bDiscoveryBoost = 0 turns the multiplier off (m = 1); the tracker keeps
// running, so switching it back on does not boost a backlog.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Huginn::Core::Discovery
{
    /// A boost whose excess (m - 1) is under this is over (Expire).
    inline constexpr double kExpireExcess = 0.01;

    /// The [Learning] keys.
    struct Params
    {
        bool enabled = true;          // bDiscoveryBoost
        double varMult = 4.0;         // fDiscoveryBoostVarMult: M (sd x 2 at the gain)
        double halfLifeSec = 3600.0;  // fDiscoveryBoostHalfLifeSec: H (one hour of play)

        /// The one copy of the allowed ranges (the INI reader clamps here).
        [[nodiscard]] Params Clamped() const noexcept
        {
            Params c = *this;
            c.varMult = std::isfinite(varMult) ? std::clamp(varMult, 1.0, 100.0) : 4.0;
            c.halfLifeSec = std::isfinite(halfLifeSec) ? std::clamp(halfLifeSec, 60.0, 360000.0) : 3600.0;
            return c;
        }
        friend bool operator==(const Params&, const Params&) = default;
    };

    /// m(age)'s excess, (M - 1) 2^(-age / H), whatever `enabled` says (the
    /// expiry runs on it either way). A negative age reads as 0.
    [[nodiscard]] inline double Excess(const Params& p, double ageSec) noexcept
    {
        const Params c = p.Clamped();
        const double age = ageSec > 0.0 ? ageSec : 0.0;
        return (c.varMult - 1.0) * std::exp2(-age / c.halfLifeSec);
    }

    /// The multiplier on b's variance `ageSec` play seconds after the gain;
    /// 1 with the boost off.
    [[nodiscard]] inline double Mult(const Params& p, double ageSec) noexcept
    {
        if (!p.enabled) return 1.0;
        return 1.0 + Excess(p, ageSec);
    }

    /// A boost this old is over.
    [[nodiscard]] inline bool Expired(const Params& p, double ageSec) noexcept
    {
        return Excess(p, ageSec) < kExpireExcess;
    }

    /// The wildcard draw's variance of a row: its score variance with b's
    /// part scaled by `mult` (header). mult <= 1 or a non-finite input leaves
    /// rowVar unchanged.
    [[nodiscard]] inline double ExploreVar(double rowVar, double bVar, double mult) noexcept
    {
        if (!(mult > 1.0) || !std::isfinite(mult) || !(bVar > 0.0) || !std::isfinite(bVar)) return rowVar;
        return rowVar + (mult - 1.0) * bVar;
    }

    /// Dynamic forms (0xFF......): the engine reuses their IDs, so a reload
    /// takes them from the save (as b's ReplaceDynamicB).
    [[nodiscard]] constexpr bool IsDynamic(std::uint32_t form) noexcept
    {
        return (form >> 24) == 0xFF;
    }

    /// What the cosave keeps (DISC, core/ChoiceCosave.h).
    struct Saved
    {
        std::vector<std::uint32_t> seen;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> boosted;   // form, play seconds since gained
    };

    class Tracker
    {
    public:
        enum class Gain : std::uint8_t
        {
            New,       // first time on this character: boosted from now
            Known,     // in the seen set already: nothing
            Dropped,   // no Prime since the last Suspend (a load under way), or form 0
        };

        /// A form the player just gained, at play second `now`.
        Gain OnGained(std::uint32_t form, double now)
        {
            if (!m_live || form == 0) return Gain::Dropped;
            if (!m_seen.insert(form).second) return Gain::Known;
            m_boosted[form] = now;
            return Gain::New;
        }

        /// A load starts (SKSE's revert): gains are dropped until Prime.
        void Suspend() noexcept { m_live = false; }
        [[nodiscard]] bool Live() const noexcept { return m_live; }

        /// What the load found (the inventory, the spells): seen, never
        /// boosted; a boost the save carried for one of them stays. Gains
        /// count again from here. Returns how many were not seen before.
        std::size_t Prime(std::span<const std::uint32_t> present)
        {
            std::size_t added = 0;
            for (const auto f : present) {
                if (f != 0 && m_seen.insert(f).second) ++added;
            }
            m_live = true;
            return added;
        }

        /// The item's first pick ends its boost. True when it had one.
        bool EndOnPick(std::uint32_t form) { return m_boosted.erase(form) != 0; }

        /// A decision of the selection log: any outcome but "nothing" with a
        /// form is a pick and ends that form's boost -- whether or not the
        /// learner can use the decision, and whether or not the model is
        /// ready (the fallback ranking still has picks). True when it did.
        bool OnDecision(bool nothing, std::uint32_t form)
        {
            return !nothing && form != 0 && EndOnPick(form);
        }

        /// Drops the dynamic forms (0xFF......) `gone` says no longer exist
        /// from the seen set and the boosts: the engine reuses such IDs, so a
        /// later item under one must not read as already seen. Returns how
        /// many seen entries went.
        template <class Gone>
        std::size_t DropDynamicIf(Gone&& gone)
        {
            std::size_t n = 0;
            for (auto it = m_seen.begin(); it != m_seen.end();) {
                if (IsDynamic(*it) && gone(*it)) {
                    m_boosted.erase(*it);
                    it = m_seen.erase(it);
                    ++n;
                }
                else {
                    ++it;
                }
            }
            return n;
        }

        /// Drops the boosts that are over at `now`; returns how many.
        std::size_t Expire(const Params& p, double now)
        {
            std::size_t n = 0;
            for (auto it = m_boosted.begin(); it != m_boosted.end();) {
                if (Expired(p, now - it->second)) {
                    it = m_boosted.erase(it);
                    ++n;
                }
                else {
                    ++it;
                }
            }
            return n;
        }

        /// The multiplier `form` reads at `now` (1 when not boosted).
        [[nodiscard]] double MultFor(const Params& p, std::uint32_t form, double now) const
        {
            const auto it = m_boosted.find(form);
            return it == m_boosted.end() ? 1.0 : Mult(p, now - it->second);
        }

        [[nodiscard]] bool Seen(std::uint32_t form) const { return m_seen.contains(form); }
        [[nodiscard]] bool Boosted(std::uint32_t form) const { return m_boosted.contains(form); }
        [[nodiscard]] std::size_t SeenCount() const noexcept { return m_seen.size(); }
        [[nodiscard]] std::size_t BoostedCount() const noexcept { return m_boosted.size(); }
        [[nodiscard]] const std::unordered_map<std::uint32_t, double>& BoostedMap() const noexcept { return m_boosted; }

        /// A new character: nothing seen, nothing boosted (Prime follows).
        void Clear()
        {
            m_seen.clear();
            m_boosted.clear();
        }

        /// The state for the cosave, the boosts' ages in whole play seconds.
        [[nodiscard]] Saved Export(double now) const
        {
            Saved s;
            s.seen.assign(m_seen.begin(), m_seen.end());
            std::sort(s.seen.begin(), s.seen.end());
            s.boosted.reserve(m_boosted.size());
            for (const auto& [f, at] : m_boosted) s.boosted.emplace_back(f, AgeOf(now, at));
            std::sort(s.boosted.begin(), s.boosted.end());
            return s;
        }

        /// Replaces the state with a save's (a load of another character, or
        /// a later save). A boosted form is seen whether or not the record
        /// lists it.
        void Import(const Saved& s, double now)
        {
            Clear();
            for (const auto f : s.seen) {
                if (f != 0) m_seen.insert(f);
            }
            for (const auto& [f, age] : s.boosted) {
                if (f == 0) continue;
                m_seen.insert(f);
                m_boosted[f] = now - static_cast<double>(age);
            }
        }

        /// A reload of the same character keeps the in-memory state except
        /// dynamic forms, which come from the save. Returns {removed, added}
        /// (seen entries).
        std::pair<std::size_t, std::size_t> ReplaceDynamic(const Saved& s, double now)
        {
            std::size_t removed = 0, added = 0;
            for (auto it = m_seen.begin(); it != m_seen.end();) {
                if (IsDynamic(*it)) {
                    it = m_seen.erase(it);
                    ++removed;
                }
                else {
                    ++it;
                }
            }
            for (auto it = m_boosted.begin(); it != m_boosted.end();) {
                it = IsDynamic(it->first) ? m_boosted.erase(it) : std::next(it);
            }
            for (const auto f : s.seen) {
                if (IsDynamic(f) && m_seen.insert(f).second) ++added;
            }
            for (const auto& [f, age] : s.boosted) {
                if (!IsDynamic(f)) continue;
                if (m_seen.insert(f).second) ++added;
                m_boosted[f] = now - static_cast<double>(age);
            }
            return { removed, added };
        }

    private:
        static std::uint32_t AgeOf(double now, double at) noexcept
        {
            const double age = std::floor(std::max(0.0, now - at));
            return static_cast<std::uint32_t>(std::min(age, 4.0e9));
        }

        std::unordered_set<std::uint32_t> m_seen;
        std::unordered_map<std::uint32_t, double> m_boosted;
        bool m_live = false;
    };
}  // namespace Huginn::Core::Discovery

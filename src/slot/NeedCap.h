#pragma once

#include "SlotAssignment.h"
#include "SlotConfig.h"
#include "learning/ScoredCandidate.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Huginn::Slot
{
    // =========================================================================
    // NEED CAP -- a soft cap per need on Regular keys
    // =========================================================================
    // One strong need could take the whole page: a hungry character at an
    // alchemy lab got eight foods on eight keys and none of the craft gear
    // (2026-10-05). The job-per-key layout used to give the page its variety by
    // construction; the all-Regular default (0.22.10) does not.
    //
    // On a Regular key, the first `free` items of a need on the page compete at
    // full utility, and each one past that at x discount, x discount^2, ... It
    // decides PLACEMENT only: ranking, the learner and the selection log keep
    // every item's true utility, so learning never sees an item as worse
    // because a sibling took a key first. Keys with a job are never capped.
    //
    // Measured before it shipped (tools/replay, 653 picks since 2026-10-03, the
    // shipped learner): first 3 free, then x0.5, kept top-8 at 76.3% (76.7%
    // without) and menu picks at 30.8% (29.7%), and no page held five of one
    // need (196 did without). Discounting from the 2nd item -- the first
    // design -- cost 3-10 points overall and up to 25 on potions: players do
    // pick the second and third item of a need.
    //
    // A need is SlotClassifier::Classify, except that all food and drink is one
    // need. Foods classify by effect (HealingAny, DefensiveAny, BuffsAny,
    // FoodAny), so a page of seven foods never had more than three of a class.
    // =========================================================================
    class NeedCap
    {
    public:
        /// `discount` >= 1 turns it off: Factor() is then always 1 and nothing
        /// is classified. `candidates`, when given, lets Factor() classify each
        /// candidate of that list once per allocation instead of per call.
        NeedCap(float discount, uint32_t freePerNeed,
            const Scoring::ScoredCandidateList* candidates = nullptr);

        [[nodiscard]] bool Active() const noexcept { return m_discount < 1.0f; }

        /// The need `c` counts against.
        [[nodiscard]] static SlotClassification NeedOf(const Scoring::ScoredCandidate& c) noexcept;

        /// Multiplier on `c`'s utility for one more item of its need on this
        /// page: 1 while its need has fewer than `free` on the page.
        [[nodiscard]] float Factor(const Scoring::ScoredCandidate& c) const;

        /// Count what the page already shows (overrides, Remembrance, holds).
        void Recount(const SlotAssignments& assignments);

        /// One more item of `c`'s need is on the page.
        void Add(const Scoring::ScoredCandidate& c);

        /// One fewer: a holder taken out of the count while it is judged.
        void Remove(const Scoring::ScoredCandidate& c);

        /// The pick for a Regular key went past `skipped` (the higher utility)
        /// because of the cap. Kept for the transition log.
        void NoteSkipped(const Scoring::ScoredCandidate& skipped);

        /// Items the cap kept off the page: noted as skipped and not shown in
        /// the end. Empty when the cap changed nothing. Sorted, so equal pages
        /// give equal strings.
        [[nodiscard]] std::string Summary(const SlotAssignments& assignments) const;

    private:
        [[nodiscard]] SlotClassification CachedNeed(const Scoring::ScoredCandidate& c) const;

        float m_discount;
        uint32_t m_free;
        std::array<uint8_t, SLOT_CLASSIFICATION_COUNT> m_onPage{};
        const Scoring::ScoredCandidateList* m_candidates;
        mutable std::vector<uint8_t> m_needCache;  // per index of m_candidates; kUnknown = not yet
        std::vector<std::pair<RE::FormID, SlotClassification>> m_skipped;
        std::vector<std::string> m_skippedNames;
    };

}  // namespace Huginn::Slot

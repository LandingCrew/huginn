#pragma once

#include "SlotAssignment.h"
#include "core/SlotClassCapMath.h"
#include "SlotConfig.h"
#include "learning/ScoredCandidate.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Huginn::Slot
{
    // =========================================================================
    // SLOT CLASS CAP -- a soft cap per slot class on Regular keys
    // =========================================================================
    // One strong slot class could take the whole page: a hungry character at an
    // alchemy lab got eight foods on eight keys and none of the craft gear
    // (2026-10-05). The job-per-key layout used to give the page its variety by
    // construction; the all-Regular default (0.22.10) does not.
    //
    // On a Regular key, the first `free` items of a class on the page compete at
    // full utility, and each one past that at x discount, x discount^2, ... It
    // decides PLACEMENT only: ranking, the learner and the selection log keep
    // every item's true utility, so learning never sees an item as worse
    // because a sibling took a key first. Keys with a job are never capped.
    //
    // Measured before it shipped (tools/replay, 653 picks since 2026-10-03, the
    // shipped learner): first 3 free, then x0.5, kept top-8 at 76.3% (76.7%
    // without) and menu picks at 30.8% (29.7%), and no page held five of one
    // class (196 did without). Discounting from the 2nd item -- the first
    // design -- cost 3-10 points overall and up to 25 on potions: players do
    // pick the second and third item of a class.
    //
    // The class is SlotClassifier::Classify, except that all food and drink is
    // one class. (Called the "need" before 0.23.8; "need" now means the need
    // vector of the engine rewrite. The selection log's JSONL keeps its `need`
    // key for tools/replay.) Foods classify by effect (HealingAny, DefensiveAny, BuffsAny,
    // FoodAny), so a page of seven foods never had more than three of a class.
    // =========================================================================
    class SlotClassCap
    {
    public:
        /// `discount` >= 1 turns it off: Factor() is then always 1 and nothing
        /// is classified. `candidates`, when given, lets Factor() classify each
        /// candidate of that list once per allocation instead of per call.
        SlotClassCap(float discount, uint32_t freePerClass,
            const Scoring::ScoredCandidateList* candidates = nullptr);

        [[nodiscard]] bool Active() const noexcept { return Core::ClassCapActive(m_discount); }

        /// The slot class `c` counts against.
        [[nodiscard]] static SlotClassification ClassOf(const Scoring::ScoredCandidate& c) noexcept;

        /// Multiplier on `c`'s utility for one more item of its class on this
        /// page: 1 while its class has fewer than `free` on the page.
        [[nodiscard]] float Factor(const Scoring::ScoredCandidate& c) const;

        /// Count what the page already shows (overrides, Remembrance, holds).
        void Recount(const SlotAssignments& assignments);

        /// One more item of `c`'s class is on the page.
        void Add(const Scoring::ScoredCandidate& c);

        /// One fewer: a holder taken out of the count while it is judged.
        void Remove(const Scoring::ScoredCandidate& c);

        /// The pick for a Regular key went past `skipped` (the higher utility)
        /// because of the cap. Kept for the transition log.
        void NoteSkipped(const Scoring::ScoredCandidate& skipped);

        /// The slot hold's challenger search is hypothetical: when the holder
        /// stays, the notes that search made are dropped unless the skipped
        /// item, uncapped, would have beaten the holder (`threshold`).
        [[nodiscard]] size_t SkipMark() const noexcept { return m_skipped.size(); }
        void DropSkipsSince(size_t mark, float threshold);

        /// Items the cap kept off the page: noted as skipped and not shown in
        /// the end. Empty when the cap changed nothing. Sorted, so equal pages
        /// give equal strings.
        [[nodiscard]] std::string Summary(const SlotAssignments& assignments) const;

    private:
        [[nodiscard]] SlotClassification CachedClass(const Scoring::ScoredCandidate& c) const;

        float m_discount;
        uint32_t m_free;
        std::array<uint8_t, SLOT_CLASSIFICATION_COUNT> m_onPage{};
        const Scoring::ScoredCandidateList* m_candidates;
        mutable std::vector<uint8_t> m_classCache;  // per index of m_candidates; kUnknown = not yet
        struct Skipped
        {
            RE::FormID formID;
            SlotClassification slotClass;
            float utility;
            std::string name;
        };
        std::vector<Skipped> m_skipped;
    };

}  // namespace Huginn::Slot

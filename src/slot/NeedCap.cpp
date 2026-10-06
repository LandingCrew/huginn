#include "NeedCap.h"
#include "SlotClassifier.h"
#include <algorithm>
#include <format>

namespace Huginn::Slot
{
    namespace
    {
        constexpr uint8_t kUnknown = 0xFF;
        static_assert(SLOT_CLASSIFICATION_COUNT < kUnknown);
    }

    NeedCap::NeedCap(float discount, uint32_t freePerNeed, const Scoring::ScoredCandidateList* candidates) :
        m_discount(std::clamp(discount, 0.0f, 1.0f)),
        m_free(freePerNeed),
        m_candidates(candidates)
    {
        if (Active() && m_candidates) {
            m_needCache.assign(m_candidates->size(), kUnknown);
        }
    }

    SlotClassification NeedCap::NeedOf(const Scoring::ScoredCandidate& c) noexcept
    {
        if (c.GetSourceType() == Candidate::SourceType::Food) {
            return SlotClassification::FoodAny;
        }
        return SlotClassifier::Classify(c);
    }

    SlotClassification NeedCap::CachedNeed(const Scoring::ScoredCandidate& c) const
    {
        // A candidate of the list this allocation runs on is classified once;
        // a copy (FindBestCandidate returns one) is classified on the spot.
        if (m_candidates && !m_needCache.empty()) {
            const auto* first = m_candidates->data();
            if (&c >= first && &c < first + m_candidates->size()) {
                auto& slot = m_needCache[static_cast<size_t>(&c - first)];
                if (slot == kUnknown) {
                    slot = static_cast<uint8_t>(NeedOf(c));
                }
                return static_cast<SlotClassification>(slot);
            }
        }
        return NeedOf(c);
    }

    float NeedCap::Factor(const Scoring::ScoredCandidate& c) const
    {
        if (!Active()) {
            return 1.0f;
        }
        const uint32_t shown = m_onPage[static_cast<size_t>(CachedNeed(c))];
        float factor = 1.0f;
        for (uint32_t n = m_free; n <= shown; ++n) {
            factor *= m_discount;
        }
        return factor;
    }

    void NeedCap::Recount(const SlotAssignments& assignments)
    {
        if (!Active()) {
            return;
        }
        m_onPage.fill(0);
        for (const auto& a : assignments) {
            if (!a.IsEmpty() && a.candidate) {
                Add(*a.candidate);
            }
        }
    }

    void NeedCap::Add(const Scoring::ScoredCandidate& c)
    {
        if (!Active()) {
            return;
        }
        auto& count = m_onPage[static_cast<size_t>(CachedNeed(c))];
        if (count < UINT8_MAX) {
            ++count;
        }
    }

    void NeedCap::Remove(const Scoring::ScoredCandidate& c)
    {
        if (!Active()) {
            return;
        }
        auto& count = m_onPage[static_cast<size_t>(CachedNeed(c))];
        if (count > 0) {
            --count;
        }
    }

    void NeedCap::NoteSkipped(const Scoring::ScoredCandidate& skipped)
    {
        const RE::FormID id = skipped.GetFormID();
        if (std::none_of(m_skipped.begin(), m_skipped.end(), [id](const Skipped& s) { return s.formID == id; })) {
            m_skipped.push_back({ id, CachedNeed(skipped), skipped.utility, std::string(skipped.GetName()) });
        }
    }

    void NeedCap::DropSkipsSince(size_t mark, float threshold)
    {
        if (mark >= m_skipped.size()) {
            return;
        }
        m_skipped.erase(std::remove_if(m_skipped.begin() + static_cast<std::ptrdiff_t>(mark), m_skipped.end(),
                            [threshold](const Skipped& s) { return s.utility <= threshold; }),
            m_skipped.end());
    }

    std::string NeedCap::Summary(const SlotAssignments& assignments) const
    {
        std::vector<std::string> kept;
        for (const auto& s : m_skipped) {
            const RE::FormID id = s.formID;
            const bool shown = std::any_of(assignments.begin(), assignments.end(),
                [id](const SlotAssignment& a) { return !a.IsEmpty() && a.formID == id; });
            if (!shown) {
                kept.push_back(std::format("'{}' ({})", s.name, SlotClassificationToString(s.need)));
            }
        }
        std::sort(kept.begin(), kept.end());
        std::string out;
        for (const auto& k : kept) {
            if (!out.empty()) out += ", ";
            out += k;
        }
        return out;
    }

}  // namespace Huginn::Slot

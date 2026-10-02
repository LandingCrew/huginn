#include "RewardLog.h"
#include "FeatureBanditLearner.h"
#include "PipelineStateCache.h"

#include <format>
#include <string>

namespace Huginn::Learning
{
    namespace
    {
        // A tail rank is "not in the sorted prefix", not a number to compare.
        std::string RankString(bool wasCandidate, size_t rank)
        {
            if (!wasCandidate) return "-";
            if (rank >= PipelineStateCache::kUnrankedTail) return "tail";
            return std::to_string(rank);
        }

        std::string FormName(RE::FormID formID)
        {
            const auto* form = RE::TESForm::LookupByID(formID);
            const char* name = form ? form->GetName() : nullptr;
            return (name && *name) ? name : "?";
        }
    }

    void LogRewardContext(const EquipEvent& event, float reward, const FeatureBanditLearner& learner)
    {
        const auto snap = PipelineStateCache::GetSingleton().GetRewardSnapshot(event.formID);
        const float pred = learner.GetRewardEstimate(event.formID, event.features);

        // What the choice displaced: the best-ranked item shown for the same
        // need that was not chosen. Only meaningful when the chosen item was a
        // candidate -- otherwise it has no need to compare on.
        const PipelineStateCache::ShownEntry* over = nullptr;
        if (snap.wasCandidate) {
            for (const auto& e : snap.shown) {
                if (e.formID == event.formID || !e.wasCandidate || e.need != snap.need) continue;
                if (!over || e.rank < over->rank) over = &e;
            }
        }

        const std::string ctxPart = snap.wasCandidate
            ? std::format("rank={} util={:.2f} ctx={:.2f}", RankString(true, snap.rank), snap.utility, snap.contextWeight)
            : std::string("not a candidate");
        const std::string overPart = over
            ? std::format("{:08X} '{}'", over->formID, over->name)
            : std::string("-");

        logger::info("[Reward] {:08X} '{}' src={} reward={:+.1f} mult={:.2f} {} pred={:.2f} need={} "
                     "over={} shown={} page={} age={:.0f}ms"sv,
            event.formID, FormName(event.formID), EquipSourceToString(event.source),
            reward, event.rewardMultiplier, ctxPart, pred,
            snap.wasCandidate ? Slot::SlotClassificationToString(snap.need) : "-"sv,
            overPart, snap.shown.size(), snap.page, snap.ageMs);

        for (const auto& e : snap.shown) {
            const char mark = (e.formID == event.formID) ? '*'
                : (snap.wasCandidate && e.wasCandidate && e.need == snap.need) ? '='
                : ' ';
            logger::info("[Reward]   s{} {} {:08X} '{}' need={} rank={} util={:.2f} ctx={:.2f} pred={:.2f}"sv,
                e.slotIndex, mark, e.formID, e.name,
                e.wasCandidate ? Slot::SlotClassificationToString(e.need) : "-"sv,
                RankString(e.wasCandidate, e.rank), e.utility, e.contextWeight,
                learner.GetRewardEstimate(e.formID, event.features));
        }
    }
}

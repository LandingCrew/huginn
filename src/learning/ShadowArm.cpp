#include "ShadowArm.h"
#include "slot/SlotSettings.h"
#include "UtilityScorer.h"
#include "Globals.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// THROWAWAY DEBUG CODE -- see ShadowArm.h. Delete after the soak run.

namespace Huginn::Learning
{
#ifdef NDEBUG
    void ShadowArm::Record(const EquipEvent&) {}
#else
    namespace
    {
        using Row = PipelineStateCache::ScoreRow;

        float ContextOnly(const Row& r)
        {
            const auto& b = r.breakdown;
            return b.contextWeight * b.correlationBonus * b.potionMultiplier * b.favoritesMultiplier;
        }

        // No learned weights: every item as if untrained -- confidence 0 so
        // lambda = lambdaMin, UCB 1.0 -- but the prior and the recency boost kept.
        float PriorKept(const Row& r, float lambdaMin, float beta)
        {
            const auto& b = r.breakdown;
            const float learn = b.prior + beta + b.recencyBoost;
            return ContextOnly(r) * (1.0f + lambdaMin * learn);
        }

        // Rank of `formID` among the open-slot rows under `key`, 0-based, or -1.
        template <typename Key>
        int RankAmong(const std::vector<const Row*>& rows, RE::FormID formID, Key key)
        {
            std::vector<const Row*> sorted = rows;
            std::stable_sort(sorted.begin(), sorted.end(),
                [&](const Row* a, const Row* b) { return key(*a) > key(*b); });
            for (size_t i = 0; i < sorted.size(); ++i) {
                if (sorted[i]->formID == formID) return static_cast<int>(i);
            }
            return -1;
        }

        struct Tally
        {
            uint32_t n = 0, a = 0, aStar = 0, b = 0, bp = 0;
            uint32_t bNotA = 0;    // on B's page but not the live one
            uint32_t bpNotA = 0;   // on B-prime's page but not the live one
        };

        std::mutex s_mutex;
        Tally s_all, s_outside;
        std::ofstream s_out;

        void Count(Tally& t, bool a, bool aStar, bool b, bool bp)
        {
            ++t.n;
            t.a += a;
            t.aStar += aStar;
            t.b += b;
            t.bp += bp;
            t.bNotA += (b && !a);
            t.bpNotA += (bp && !a);
        }

        std::string TallyString(const Tally& t)
        {
            return std::format("n={} A={} A*={} B={} B'={} B-not-A={} B'-not-A={}",
                t.n, t.a, t.aStar, t.b, t.bp, t.bNotA, t.bpNotA);
        }
    }

    void ShadowArm::Record(const EquipEvent& event)
    {
        const auto& snap = event.shown;
        if (!snap.valid) return;

        // The page's slot count, and the slots that are NOT ranked: overrides,
        // Remembrance holds, wildcards. Both arms keep those as they are.
        const auto page = Slot::SlotSettings::GetSingleton().GetPage(snap.page);
        const size_t slotCount = std::max(page.slots.size(), snap.shown.size());
        std::unordered_set<RE::FormID> fixed;
        bool chosenFixed = false;
        bool onLivePage = false;
        size_t liveSlot = 0;
        for (const auto& s : snap.shown) {
            if (s.formID == event.formID) {
                onLivePage = true;
                liveSlot = s.slotIndex;
            }
            if (s.type == Slot::AssignmentType::Override || s.type == Slot::AssignmentType::Remembered ||
                s.type == Slot::AssignmentType::Wildcard) {
                fixed.insert(s.formID);
                chosenFixed |= (s.formID == event.formID);
            }
        }
        const size_t openSlots = slotCount > fixed.size() ? slotCount - fixed.size() : 0;

        // Rows competing for the open slots: one per FormID (the first, best
        // row -- as PipelineStateCache's index keeps), not fixed, not a
        // Remembrance-only or wildcard row, and not something that was in the
        // player's hands when they chose -- every shipped slot skips those
        // (bSkipEquipped). The chosen item itself is never skipped: an outside
        // equip's event lands after the equip, so it can be "in hand" already.
        std::vector<const Row*> open;
        std::unordered_set<RE::FormID> seen;
        for (const auto& r : snap.scores) {
            if (r.isRememberedOnly || r.isWildcard || fixed.contains(r.formID)) continue;
            if (r.formID != event.formID &&
                std::find(event.handsAtPress.begin(), event.handsAtPress.end(), r.formID) != event.handsAtPress.end()) {
                continue;
            }
            if (!seen.insert(r.formID).second) continue;
            open.push_back(&r);
        }

        float lambdaMin = 0.0f, beta = 0.0f;
        if (g_utilityScorer) {
            lambdaMin = g_utilityScorer->GetConfig().lambdaMin;
            beta = g_utilityScorer->GetConfig().explorationWeight;
        }
        const int rankA = RankAmong(open, event.formID, [](const Row& r) { return r.utility; });
        const int rankB = RankAmong(open, event.formID, ContextOnly);
        const int rankBp = RankAmong(open, event.formID,
            [lambdaMin, beta](const Row& r) { return PriorKept(r, lambdaMin, beta); });
        auto onPage = [&](int rank) { return chosenFixed || (rank >= 0 && static_cast<size_t>(rank) < openSlots); };
        const bool onAStar = onPage(rankA);
        const bool onB = onPage(rankB);
        const bool onBp = onPage(rankBp);
        const bool outside = event.source == EquipSource::External;
        // Page 2 ("Jobs") classifies its slots; plain ranking cannot model that,
        // so only plain-page selections are tallied. Others are logged, marked.
        const bool tallied = snap.page == 0;

        const auto* form = RE::TESForm::LookupByID(event.formID);
        const char* name = form && form->GetName() && *form->GetName() ? form->GetName() : "?";
        const auto utc = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

        std::lock_guard lock(s_mutex);
        if (tallied) {
            Count(s_all, onLivePage, onAStar, onB, onBp);
            if (outside) Count(s_outside, onLivePage, onAStar, onB, onBp);
        }

        const std::string line = std::format(
            "{:%F %T} gen={} page={} {:08X} '{}' src={} via={}{}{} kind={} | A={} A*={} B={} B'={} (open slots {}, "
            "rank A*={} B={} B'={}{}){} | all: {} | outside: {}\n",
            utc, event.loadGeneration, snap.page, event.formID, name, EquipSourceToString(event.source), event.via,
            event.attribution.empty() ? "" : " case=", event.attribution, SelectionKindToString(event.kind),
            onLivePage ? std::format("s{}", liveSlot) : std::string("no"),
            onAStar ? "yes" : "no", onB ? "yes" : "no", onBp ? "yes" : "no", openSlots, rankA, rankB, rankBp,
            chosenFixed ? ", fixed slot" : "",
            tallied ? "" : " [labeled page: not tallied]",
            TallyString(s_all), TallyString(s_outside));

        if (!s_out.is_open()) {
            if (const auto logDir = SKSE::log::log_directory()) {
                s_out.open(*logDir / "Huginn_AB.log", std::ios::app | std::ios::binary);
            }
            if (!s_out.is_open()) return;
        }
        s_out << line;
        s_out.flush();
    }
#endif
}

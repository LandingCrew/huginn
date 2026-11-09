// Only compile in debug mode
#ifdef _DEBUG

#include "UtilityScorerDebugWidget.h"
#include "../slot/SlotClassifier.h"
#include "../slot/SlotConfig.h"
#include "../core/EffectColumns.h"   // the explanation's column names (R8.4)
#include "../core/NeedIds.h"         // ... and need names
#include <algorithm>
#include <cmath>
#include <format>
#include <shared_mutex>

namespace Huginn::UI
{
    // =============================================================================
    // COLOR CONSTANTS
    // =============================================================================

    namespace ScorerColors
    {
        // Source type colors
        constexpr ImVec4 SPELL_COLOR{0.5f, 0.7f, 1.0f, 1.0f};      // Blue
        constexpr ImVec4 POTION_COLOR{0.2f, 0.9f, 0.4f, 1.0f};     // Green
        constexpr ImVec4 WEAPON_COLOR{0.9f, 0.6f, 0.2f, 1.0f};     // Orange
        constexpr ImVec4 AMMO_COLOR{0.8f, 0.8f, 0.3f, 1.0f};       // Yellow
        constexpr ImVec4 SCROLL_COLOR{0.9f, 0.5f, 0.9f, 1.0f};     // Pink
        constexpr ImVec4 SOULGEM_COLOR{0.7f, 0.4f, 0.9f, 1.0f};    // Purple
        constexpr ImVec4 FOOD_COLOR{0.9f, 0.7f, 0.4f, 1.0f};       // Tan
        constexpr ImVec4 STAFF_COLOR{0.6f, 0.5f, 0.3f, 1.0f};      // Brown
        constexpr ImVec4 APPAREL_COLOR{0.79f, 0.61f, 1.0f, 1.0f};  // Soft violet — matches the widget (#65)

        // Score component colors (R8.4: the choice score's parts)
        constexpr ImVec4 TERM_COLOR{0.4f, 0.8f, 0.4f, 1.0f};       // Green: theta n cap terms
        constexpr ImVec4 BIAS_COLOR{0.4f, 0.6f, 1.0f, 1.0f};       // Blue: b
        constexpr ImVec4 LEGACY_COLOR{0.8f, 0.6f, 0.4f, 1.0f};     // Brown: the D2 legacy feature

        // Status colors
        constexpr ImVec4 FAVORITE_COLOR{1.0f, 0.4f, 0.4f, 1.0f};   // Red heart
        constexpr ImVec4 UTILITY_BAR{0.3f, 0.7f, 0.9f, 1.0f};      // Cyan

        // Text colors
        constexpr ImVec4 HEADER_COLOR{0.9f, 0.9f, 0.3f, 1.0f};     // Yellow
        constexpr ImVec4 LABEL_DIM{0.6f, 0.6f, 0.6f, 1.0f};        // Gray

        // Assignment type header colors
        constexpr ImVec4 OVERRIDE_HEADER{1.0f, 0.3f, 0.3f, 1.0f};  // Red
        constexpr ImVec4 WILDCARD_HEADER{1.0f, 0.8f, 0.2f, 1.0f};  // Gold
        constexpr ImVec4 NORMAL_HEADER{1.0f, 1.0f, 1.0f, 1.0f};    // White
        constexpr ImVec4 EMPTY_HEADER{0.5f, 0.5f, 0.5f, 1.0f};     // Gray
    }

    // =============================================================================
    // SINGLETON
    // =============================================================================

    UtilityScorerDebugWidget& UtilityScorerDebugWidget::GetSingleton() noexcept
    {
        static UtilityScorerDebugWidget instance;
        return instance;
    }

    // =============================================================================
    // PUBLIC INTERFACE
    // =============================================================================

    void UtilityScorerDebugWidget::UpdateSlotData(
        const Scoring::ScoredCandidateList& candidates,
        const Slot::SlotAssignments& assignments,
        size_t pageIndex,
        const std::string& pageName,
        size_t pageCount,
        size_t coldStartBoostedCount)
    {
        // Thread safety: Called from game update thread.
        // Build new lists outside lock to minimize critical section,
        // then swap in under unique_lock (writer).
        Scoring::ScoredCandidateList newCandidates;
        std::vector<std::string> newNames;
        const size_t count = std::min(m_maxDisplay, candidates.size());
        newCandidates.reserve(count);
        newNames.reserve(count);
        newCandidates.insert(newCandidates.end(),
                             candidates.begin(),
                             candidates.begin() + static_cast<ptrdiff_t>(count));

        // The copies' name views point into registry strings that a reconcile
        // can invalidate before the render thread reads them. Capture owned
        // copies now (views still valid on this thread), then blank the views.
        for (auto& sc : newCandidates) {
            newNames.emplace_back(sc.GetName());
            Candidate::GetBase(sc.candidate).name = {};
        }

        Slot::SlotAssignments newAssignments = assignments;  // Full copy
        for (auto& a : newAssignments) {
            // assignment.name is owned; only the embedded candidate holds a view
            if (a.candidate) {
                Candidate::GetBase(a.candidate->candidate).name = {};
            }
        }
        std::string newPageName = pageName;

        // Short critical section: swap in the new data
        std::unique_lock lock(m_mutex);
        m_cachedCandidates = std::move(newCandidates);
        m_cachedNames = std::move(newNames);
        m_cachedAssignments = std::move(newAssignments);
        m_cachedPageIndex = pageIndex;
        m_cachedPageName = std::move(newPageName);
        m_pageCount = pageCount;
        m_coldStartBoostedCount = coldStartBoostedCount;
    }

    void UtilityScorerDebugWidget::Draw()
    {
        if (!m_isVisible) {
            return;
        }

        // Thread safety: Called from render thread (D3D11 hook).
        // Copy-out pattern: Take short shared_lock to copy everything, then release.
        Scoring::ScoredCandidateList localCandidates;
        std::vector<std::string> localNames;
        Slot::SlotAssignments localAssignments;
        size_t localPageIndex = 0;
        std::string localPageName;
        size_t localPageCount = 0;
        size_t localColdStartCount = 0;
        {
            std::shared_lock lock(m_mutex);
            localCandidates = m_cachedCandidates;
            localNames = m_cachedNames;
            localAssignments = m_cachedAssignments;
            localPageIndex = m_cachedPageIndex;
            localPageName = m_cachedPageName;
            localPageCount = m_pageCount;
            localColdStartCount = m_coldStartBoostedCount;
        }

        // Position on right side, below Cache widget to avoid overlap
        ImGui::SetNextWindowPos(ImVec2(m_posX, m_posY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380.0f, 650.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

        ImGuiWindowFlags flags = 0;

        static const std::string windowTitle = std::format("Choice Scorer [v{} DEBUG]", Huginn::VERSION_STRING);
        if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, flags)) {
            // Header: Page info + slot count
            ImGui::TextColored(ScorerColors::HEADER_COLOR, "Page: \"%s\" (%zu/%zu)",
                              localPageName.c_str(),
                              localPageIndex + 1,
                              localPageCount);

            // The legacy floors' cold start (which rows get the D2 feature)
            if (localColdStartCount > 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), " [legacy cold start: %zu]",
                                  localColdStartCount);
            }

            ImGui::TextColored(ScorerColors::LABEL_DIM, "%zu slots, %zu candidates scored",
                              localAssignments.size(), localCandidates.size());
            ImGui::Separator();

            if (localAssignments.empty()) {
                ImGui::TextColored(ScorerColors::LABEL_DIM, "No slot assignments yet...");
            } else {
                // Per-slot collapsible sections
                for (size_t i = 0; i < localAssignments.size(); ++i) {
                    DrawSlotSection(localAssignments[i], localCandidates, localNames, i);
                }
            }
        }
        ImGui::End();
    }

    // =============================================================================
    // SECTION RENDERERS
    // =============================================================================

    void UtilityScorerDebugWidget::DrawSlotSection(
        const Slot::SlotAssignment& assignment,
        const Scoring::ScoredCandidateList& candidates,
        const std::vector<std::string>& names,
        size_t slotIdx)
    {
        ImGui::PushID(static_cast<int>(slotIdx));

        // Build header label: "Slot N [Classification] -> Winner"
        auto classLabel = Slot::SlotClassificationToString(assignment.classification);
        auto headerColor = GetAssignmentTypeColor(assignment.type);

        std::string winnerName = assignment.IsEmpty()
            ? std::format("(No {})", classLabel)
            : assignment.name;

        std::string headerLabel = std::format("Slot {} [{}] -> {}",
            slotIdx, classLabel, winnerName);

        ImGui::PushStyleColor(ImGuiCol_Text, headerColor);
        bool isOpen = ImGui::TreeNode("##slot", "%s", headerLabel.c_str());
        ImGui::PopStyleColor();

        if (isOpen) {
            // Filter candidates matching this slot's classification
            static const std::string kNoName{"???"};
            size_t shown = 0;
            size_t totalMatches = 0;
            for (size_t ci = 0; ci < candidates.size(); ++ci) {
                const auto& candidate = candidates[ci];
                if (!Slot::SlotClassifier::Matches(candidate, assignment.classification)) {
                    continue;
                }
                ++totalMatches;
                if (shown < m_candidatesPerSlot) {
                    DrawCandidateCard(candidate,
                        ci < names.size() ? names[ci] : kNoName, shown + 1);
                    ++shown;
                }
            }

            if (totalMatches == 0) {
                ImGui::TextColored(ScorerColors::LABEL_DIM, "(0 candidates match %.*s)",
                    static_cast<int>(classLabel.size()), classLabel.data());
            } else if (totalMatches > m_candidatesPerSlot) {
                ImGui::TextColored(ScorerColors::LABEL_DIM, "... +%zu more",
                    totalMatches - m_candidatesPerSlot);
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void UtilityScorerDebugWidget::DrawCandidateCard(
        const Scoring::ScoredCandidate& candidate,
        const std::string& name,
        size_t rank)
    {
        ImGui::PushID(static_cast<int>(rank));

        // Candidate header: Rank, Name, Type
        auto typeColor = GetSourceTypeColor(candidate.GetSourceType());
        const char* typeName = GetSourceTypeName(candidate.GetSourceType());

        // Slot indicator with status badges
        ImGui::TextColored(ScorerColors::HEADER_COLOR, "#%zu", rank);
        ImGui::SameLine();

        // Cold-start badge
        if (candidate.isColdStartBoosted) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[CS]");
            ImGui::SameLine();
        }

        // R10: no wildcard badge on a ranked row -- the wildcard is a key's
        // placement (the assignments below mark it), not a row's flag.

        // Favorite badge
        if (candidate.IsFavorited()) {
            ImGui::TextColored(ScorerColors::FAVORITE_COLOR, "[*]");
            ImGui::SameLine();
        }

        // Type badge
        ImGui::TextColored(typeColor, "[%s]", typeName);
        ImGui::SameLine();

        // Candidate name (owned snapshot — the candidate's own view is blanked;
        // see UpdateSlotData). Truncated if too long.
        if (name.length() > 20) {
            ImGui::Text("%.*s...", 17, name.data());
        } else {
            ImGui::Text("%.*s", static_cast<int>(name.length()), name.data());
        }

        // The score (mu, any sign) and its parts
        DrawScoreBar(candidate);
        DrawChoiceSummary(candidate);

        ImGui::PopID();
        ImGui::Separator();
    }

    void UtilityScorerDebugWidget::DrawChoiceSummary(const Scoring::ScoredCandidate& candidate)
    {
        // R8.4: the choice score's parts, as [Recs] prints them: the top three
        // theta[need][col] n a terms (the first is the explanation), b, and the
        // D2 legacy feature (lm on a row the old floors kept, "um" otherwise).
        // The fallback ranking (no bootstrap) has no choice parts: `choice` is
        // unset there, so it says so instead of printing "learned" and lm 0.
        if (candidate.fallbackRanked) {
            ImGui::TextColored(ScorerColors::LABEL_DIM, "fallback: s = ln u (no bootstrap)");
            return;
        }
        const auto& p = candidate.choice;
        bool any = false;
        for (const auto& t : p.top) {
            if (t.need < 0) break;
            const auto ni = static_cast<size_t>(t.need);
            const auto ci = static_cast<size_t>(t.col);
            const std::string_view need = ni < Core::Needs::kNeedCount ? Core::Needs::kNeeds[ni].id : std::string_view("?");
            const std::string_view col = ci < Core::Effect::kColumnCount ? Core::Effect::kColumns[ci].id : std::string_view("?");
            if (any) ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextColored(ScorerColors::TERM_COLOR, "%.*sx%.*s=%+.2f",
                static_cast<int>(need.size()), need.data(), static_cast<int>(col.size()), col.data(), t.value);
            any = true;
        }
        if (!any) {
            ImGui::TextColored(ScorerColors::LABEL_DIM, "learned");
        }
        ImGui::TextColored(ScorerColors::BIAS_COLOR, "b:%+.2f", p.b);
        ImGui::SameLine(0.0f, 6.0f);
        if (p.utilMissing != 0.0) {
            ImGui::TextColored(ScorerColors::LABEL_DIM, "lm: um");
        } else {
            ImGui::TextColored(ScorerColors::LEGACY_COLOR, "lm:%+.2f", p.lnLearnMult);
        }
        if (p.covered > 0.0) {
            ImGui::SameLine(0.0f, 6.0f);
            ImGui::TextColored(ScorerColors::LABEL_DIM, "covered:%.2f", p.covered);
        }
    }

    void UtilityScorerDebugWidget::DrawScoreBar(const Scoring::ScoredCandidate& candidate)
    {
        // The slot score, any sign: mu (R8.3), or s = ln u in the fallback.
        // The bar is the logistic of it (0.5 at 0), so it is defined for any
        // sign; an unranked row (remembered only) is an empty bar.
        const double s = candidate.SlotScore();
        const float fraction = std::isfinite(s) ? static_cast<float>(1.0 / (1.0 + std::exp(-s))) : (s > 0 ? 1.0f : 0.0f);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ScorerColors::UTILITY_BAR);
        char overlay[48];
        if (std::isfinite(s) && candidate.fallbackRanked) {
            snprintf(overlay, sizeof(overlay), "s %+.2f  fallback", s);   // ln u: no mu, no sd
        } else if (std::isfinite(s)) {
            snprintf(overlay, sizeof(overlay), "mu %+.2f  sd %.2f", s, std::sqrt(std::max(candidate.choice.var, 0.0)));
        } else {
            snprintf(overlay, sizeof(overlay), "%s", s > 0 ? "pinned" : "unranked");
        }
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::PopStyleColor();
    }

    // =============================================================================
    // HELPERS
    // =============================================================================

    const char* UtilityScorerDebugWidget::GetSourceTypeName(Candidate::SourceType type) const
    {
        switch (type) {
            case Candidate::SourceType::Spell:   return "Spell";
            case Candidate::SourceType::Potion:  return "Potion";
            case Candidate::SourceType::Scroll:  return "Scroll";
            case Candidate::SourceType::Weapon:  return "Weapon";
            case Candidate::SourceType::Ammo:    return "Ammo";
            case Candidate::SourceType::SoulGem: return "SoulGem";
            case Candidate::SourceType::Food:    return "Food";
            case Candidate::SourceType::Staff:   return "Staff";
            case Candidate::SourceType::Apparel: return "Apparel";
            case Candidate::SourceType::Torch:   return "Torch";
            default:                             return "???";
        }
    }

    ImVec4 UtilityScorerDebugWidget::GetSourceTypeColor(Candidate::SourceType type) const
    {
        switch (type) {
            case Candidate::SourceType::Spell:   return ScorerColors::SPELL_COLOR;
            case Candidate::SourceType::Potion:  return ScorerColors::POTION_COLOR;
            case Candidate::SourceType::Scroll:  return ScorerColors::SCROLL_COLOR;
            case Candidate::SourceType::Weapon:  return ScorerColors::WEAPON_COLOR;
            case Candidate::SourceType::Ammo:    return ScorerColors::AMMO_COLOR;
            case Candidate::SourceType::SoulGem: return ScorerColors::SOULGEM_COLOR;
            case Candidate::SourceType::Food:    return ScorerColors::FOOD_COLOR;
            case Candidate::SourceType::Staff:   return ScorerColors::STAFF_COLOR;
            case Candidate::SourceType::Apparel: return ScorerColors::APPAREL_COLOR;
            default:                             return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }

    ImVec4 UtilityScorerDebugWidget::GetAssignmentTypeColor(Slot::AssignmentType type) const
    {
        switch (type) {
            case Slot::AssignmentType::Override: return ScorerColors::OVERRIDE_HEADER;
            case Slot::AssignmentType::Wildcard: return ScorerColors::WILDCARD_HEADER;
            case Slot::AssignmentType::Normal:   return ScorerColors::NORMAL_HEADER;
            case Slot::AssignmentType::Empty:    return ScorerColors::EMPTY_HEADER;
            default:                             return ScorerColors::NORMAL_HEADER;
        }
    }

}  // namespace Huginn::UI

#endif  // _DEBUG

// Only compile in debug mode
#ifdef _DEBUG

#include "UtilityScorerDebugWidget.h"
#include "../slot/SlotClassifier.h"
#include "../slot/SlotConfig.h"
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

        // Score component colors
        constexpr ImVec4 CONTEXT_COLOR{0.4f, 0.8f, 0.4f, 1.0f};    // Green
        constexpr ImVec4 QVALUE_COLOR{0.4f, 0.6f, 1.0f, 1.0f};     // Blue
        constexpr ImVec4 PRIOR_COLOR{0.8f, 0.6f, 0.4f, 1.0f};      // Brown
        constexpr ImVec4 UCB_COLOR{0.9f, 0.9f, 0.4f, 1.0f};        // Yellow
        constexpr ImVec4 CORRELATION_COLOR{0.7f, 0.4f, 0.9f, 1.0f};// Purple
        constexpr ImVec4 MULTIPLIER_COLOR{0.9f, 0.5f, 0.5f, 1.0f}; // Red
        constexpr ImVec4 RECENCY_COLOR{1.0f, 0.7f, 0.2f, 1.0f};    // Warm amber

        // Status colors
        constexpr ImVec4 WILDCARD_COLOR{1.0f, 0.8f, 0.2f, 1.0f};   // Gold
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
        const size_t count = std::min(m_maxDisplay, candidates.size());
        newCandidates.reserve(count);
        newCandidates.insert(newCandidates.end(),
                             candidates.begin(),
                             candidates.begin() + static_cast<ptrdiff_t>(count));

        Slot::SlotAssignments newAssignments = assignments;  // Full copy
        std::string newPageName = pageName;

        // Short critical section: swap in the new data
        std::unique_lock lock(m_mutex);
        m_cachedCandidates = std::move(newCandidates);
        m_cachedAssignments = std::move(newAssignments);
        m_cachedPageIndex = pageIndex;
        m_cachedPageName = std::move(newPageName);
        m_pageCount = pageCount;
        m_coldStartBoostedCount = coldStartBoostedCount;
    }

    void UtilityScorerDebugWidget::UpdateUsageMemory(std::vector<Learning::UsageEvent> snapshot,
                                                      uint32_t currentContextHash)
    {
        std::unique_lock lock(m_mutex);
        m_cachedUsageSnapshot = std::move(snapshot);
        m_cachedContextHash = currentContextHash;
    }

    void UtilityScorerDebugWidget::Draw()
    {
        if (!m_isVisible) {
            return;
        }

        // Thread safety: Called from render thread (D3D11 hook).
        // Copy-out pattern: Take short shared_lock to copy everything, then release.
        Scoring::ScoredCandidateList localCandidates;
        Slot::SlotAssignments localAssignments;
        size_t localPageIndex = 0;
        std::string localPageName;
        size_t localPageCount = 0;
        size_t localColdStartCount = 0;
        std::vector<Learning::UsageEvent> localUsageSnapshot;
        uint32_t localContextHash = 0;
        {
            std::shared_lock lock(m_mutex);
            localCandidates = m_cachedCandidates;
            localAssignments = m_cachedAssignments;
            localPageIndex = m_cachedPageIndex;
            localPageName = m_cachedPageName;
            localPageCount = m_pageCount;
            localColdStartCount = m_coldStartBoostedCount;
            localUsageSnapshot = m_cachedUsageSnapshot;
            localContextHash = m_cachedContextHash;
        }

        // Position on right side, below Cache widget to avoid overlap
        ImGui::SetNextWindowPos(ImVec2(m_posX, m_posY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380.0f, 650.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

        ImGuiWindowFlags flags = 0;

        static const std::string windowTitle = std::format("Utility Scorer [v{} DEBUG]", Huginn::VERSION_STRING);
        if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, flags)) {
            // Header: Page info + slot count
            ImGui::TextColored(ScorerColors::HEADER_COLOR, "Page: \"%s\" (%zu/%zu)",
                              localPageName.c_str(),
                              localPageIndex + 1,
                              localPageCount);

            // Cold Start indicator
            if (localColdStartCount > 0) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), " [Cold Start: %zu]",
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
                    DrawSlotSection(localAssignments[i], localCandidates, i);
                }
            }

            // Event buffer section (unchanged from original)
            ImGui::Separator();
            ImGui::TextColored(ScorerColors::HEADER_COLOR, "Event Buffer (%zu/%zu)",
                              localUsageSnapshot.size(),
                              Learning::UsageMemory::BUFFER_CAPACITY);
            ImGui::SameLine();
            ImGui::TextColored(ScorerColors::LABEL_DIM, "ctx:0x%04X",
                              localContextHash & 0xFFFF);

            if (localUsageSnapshot.empty()) {
                ImGui::TextColored(ScorerColors::LABEL_DIM, "  (empty)");
            } else {
                // Newest events at top (reverse iteration)
                for (auto it = localUsageSnapshot.rbegin(); it != localUsageSnapshot.rend(); ++it) {
                    const auto& event = *it;
                    bool matches = (event.contextHash == localContextHash);

                    // Resolve item name via form lookup
                    const char* itemName = "???";
                    if (auto* form = RE::TESForm::LookupByID(event.formID)) {
                        itemName = form->GetName();
                    }

                    ImVec4 color = matches
                        ? ImVec4(0.9f, 0.9f, 0.9f, 1.0f)
                        : ScorerColors::LABEL_DIM;

                    ImGui::TextColored(color, "  %-18s [0x%04X]%s",
                                      itemName,
                                      event.contextHash & 0xFFFF,
                                      matches ? " \xE2\x97\x84" : "");
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
            size_t shown = 0;
            size_t totalMatches = 0;
            for (const auto& candidate : candidates) {
                if (!Slot::SlotClassifier::Matches(candidate, assignment.classification)) {
                    continue;
                }
                ++totalMatches;
                if (shown < m_candidatesPerSlot) {
                    DrawCandidateCard(candidate, shown + 1);
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

        // Wildcard badge
        if (candidate.isWildcard) {
            ImGui::TextColored(ScorerColors::WILDCARD_COLOR, "[W]");
            ImGui::SameLine();
        }

        // Favorite badge
        if (candidate.IsFavorited()) {
            ImGui::TextColored(ScorerColors::FAVORITE_COLOR, "[*]");
            ImGui::SameLine();
        }

        // Type badge
        ImGui::TextColored(typeColor, "[%s]", typeName);
        ImGui::SameLine();

        // Recency badge
        if (candidate.breakdown.recencyBoost > 0.0f) {
            ImGui::TextColored(ScorerColors::RECENCY_COLOR, "[REC]");
            ImGui::SameLine();
        }

        // Candidate name (truncated if too long)
        const auto name = candidate.GetName();
        if (name.length() > 20) {
            logger::warn("[ScorerWidget] Name truncated in display: {}", name);
            ImGui::Text("%.*s...", 17, name.data());
        } else {
            ImGui::Text("%.*s", static_cast<int>(name.length()), name.data());
        }

        // Utility bar
        DrawUtilityBar(candidate.utility, 15.0f);  // Max ~15 for typical utility

        // Compact inline score summary
        DrawScoreSummary(candidate.breakdown);

        ImGui::PopID();
        ImGui::Separator();
    }

    void UtilityScorerDebugWidget::DrawScoreSummary(const Scoring::ScoreBreakdown& bd)
    {
        // Compact single-line score summary: ctx:X.X learn:X.X [rec:X.X] [corr:X.X] [pot:Xx] [fav:Xx]
        ImGui::TextColored(ScorerColors::CONTEXT_COLOR, "ctx:%.1f", bd.contextWeight);
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextColored(ScorerColors::QVALUE_COLOR, "learn:%.1f", bd.learningScore);

        if (bd.recencyBoost > 0.0f) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(ScorerColors::RECENCY_COLOR, "rec:%.1f", bd.recencyBoost);
        }
        if (std::abs(bd.correlationBonus) > 0.01f) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(ScorerColors::CORRELATION_COLOR, "corr:%.1f", bd.correlationBonus);
        }
        if (std::abs(bd.potionMultiplier - 1.0f) > 0.01f) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(ScorerColors::MULTIPLIER_COLOR, "pot:%.1fx", bd.potionMultiplier);
        }
        if (std::abs(bd.favoritesMultiplier - 1.0f) > 0.01f) {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(ScorerColors::MULTIPLIER_COLOR, "fav:%.1fx", bd.favoritesMultiplier);
        }
    }

    void UtilityScorerDebugWidget::DrawUtilityBar(float utility, float maxUtility)
    {
        float fraction = (maxUtility > 0.0f) ? std::clamp(utility / maxUtility, 0.0f, 1.0f) : 0.0f;

        // Draw custom colored progress bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ScorerColors::UTILITY_BAR);

        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.2f", utility);
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

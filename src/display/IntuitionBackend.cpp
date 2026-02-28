#include "../PCH.h"
#include "IntuitionBackend.h"
#include "../Profiling.h"

#include "slot/SlotAllocator.h"
#include "slot/SlotSettings.h"
#include "slot/SlotUtils.h"
#include "ui/IntuitionMenu.h"
#include "ui/IntuitionSettings.h"
#include "wheeler/WheelerClient.h"

namespace Huginn::Display
{
    bool IntuitionBackend::IsEnabled() const
    {
        return true;  // Fine-grained checks (null singleton, wheel overlap) are in Push()
    }

    void IntuitionBackend::Push(const DisplayContext& ctx)
    {
        Huginn_ZONE_NAMED("Display::Intuition");
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        auto& slotAllocator = Slot::SlotAllocator::GetSingleton();

        // Track widget visibility vs Wheeler overlap
        bool wheelIsOpen = wheelerClient.HasRecommendationWheel() && wheelerClient.IsWheelOpen();

        auto* intuition = UI::IntuitionMenu::GetSingleton();
        if (!intuition) {
            // Throttled warning: log once per 5 seconds when singleton is null
            auto sinceWarn = std::chrono::duration_cast<std::chrono::seconds>(
                ctx.now - m_lastNullWarn);
            if (sinceWarn.count() >= 5) {
                auto* ui = RE::UI::GetSingleton();
                bool menuOpen = ui ? ui->IsMenuOpen(UI::IntuitionMenu::MENU_NAME) : false;
                logger::warn("[Intuition] singleton is NULL - menuOpen={}"sv, menuOpen);
                m_lastNullWarn = ctx.now;
            }
            return;
        }

        if (wheelIsOpen) {
            if (!m_widgetHiddenForWheel) {
                intuition->SetVisible(false);
                m_widgetHiddenForWheel = true;
            }
            return;  // Wheeler is visible and owns the display
        }

        if (m_widgetHiddenForWheel) {
            intuition->SetVisible(true);
            m_widgetHiddenForWheel = false;
        }

        // Determine display page: track Wheeler's active managed wheel if open,
        // otherwise fall back to slot allocator's current page.
        int displayPage = static_cast<int>(slotAllocator.GetCurrentPage());
        Slot::SlotAssignments displayAssignments = ctx.assignments;

        if (wheelerClient.IsConnected()) {
            int wheelerPage = wheelerClient.GetActiveManagedPage();
            if (wheelerPage >= 0 && wheelerPage != displayPage) {
                displayPage = wheelerPage;
                slotAllocator.SetCurrentPage(static_cast<size_t>(wheelerPage));
                displayAssignments = slotAllocator.AllocateSlotsForPage(
                    static_cast<size_t>(wheelerPage), ctx.scoredCandidates,
                    ctx.overrides, ctx.playerState, ctx.worldState);
            }
        }

        const auto displayPageName = Slot::SlotSettings::GetSingleton().GetPageName(
            static_cast<size_t>(displayPage));

        intuition->SetSlotCount(static_cast<int>(slotAllocator.GetSlotCount(
            static_cast<size_t>(displayPage))));
        intuition->SetPage(
            displayPage,
            static_cast<int>(slotAllocator.GetPageCount()),
            displayPageName);

        const auto displayMode = UI::IntuitionSettings::GetSingleton().GetDisplayMode();
        logger::trace("[Intuition] displayMode={}"sv,
            displayMode == UI::DisplayMode::Verbose ? "verbose" :
            displayMode == UI::DisplayMode::Normal ? "normal" : "minimal");

        for (const auto& assignment : displayAssignments) {
            auto content = Slot::ToSlotContent(assignment);
            auto type = UI::IntuitionMenu::MapSlotContentType(content.type);
            auto detail = UI::IntuitionMenu::BuildSlotDetail(
                assignment, displayMode, ctx.playerState);
            logger::trace("[Intuition] slot={} formID={:08X} name='{}' detail='{}' hasCand={}"sv,
                assignment.slotIndex, assignment.formID, content.name, detail, assignment.HasCandidate());
            intuition->SetSlot(
                static_cast<int>(assignment.slotIndex),
                content.name,
                type,
                static_cast<double>(content.confidence),
                detail,
                assignment.visualState);
        }
    }

}  // namespace Huginn::Display

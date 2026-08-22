#include "../PCH.h"
#include "IntuitionBackend.h"
#include "../Profiling.h"

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
            // Re-assert hidden every tick while the wheel is open. A game menu
            // opening/closing (inventory, map) makes HudVisibilityManager call
            // SetVisible(true); without re-hiding here the widget would surface
            // behind the wheel. SetVisible defers its GFx work to the UI task
            // queue and setting _root invisible is idempotent, so the repeated
            // call is trivially cheap.
            intuition->SetVisible(false);
            m_widgetHiddenForWheel = true;
            return;  // Wheeler is visible and owns the display
        }

        if (m_widgetHiddenForWheel) {
            intuition->SetVisible(true);
            m_widgetHiddenForWheel = false;
        }

        // Page state was resolved once by the coordinator (ResolveDisplayPage) and
        // handed to us on the context — no need to re-fetch from the allocator or
        // SlotSettings. We just render the assignments we're given for that page.
        const auto& displayAssignments = ctx.assignments;

        const auto displayMode = UI::IntuitionSettings::GetSingleton().GetDisplayMode();

        // #14: build what we WOULD send, and bail if it matches the last push.
        // Everything below this point allocates a UI task and copies strings into
        // it, so the comparison has to cover every field that reaches the widget —
        // including detail, which folds in displayMode, so a settings change still
        // repaints. Built into a reused member so the vector keeps its capacity.
        m_scratch.valid = true;
        m_scratch.slotCount = ctx.slotCount;
        m_scratch.pageIndex = ctx.pageIndex;
        m_scratch.pageCount = ctx.pageCount;
        m_scratch.pageName.assign(ctx.pageName);
        m_scratch.slots.clear();
        m_scratch.slots.reserve(displayAssignments.size());
        for (const auto& assignment : displayAssignments) {
            auto content = Slot::ToSlotContent(assignment);
            m_scratch.slots.push_back(SlotView{
                .name = content.name,
                .type = static_cast<int>(UI::IntuitionMenu::MapSlotContentType(content.type)),
                .confidence = static_cast<double>(content.confidence),
                .detail = UI::IntuitionMenu::BuildSlotDetail(assignment, displayMode, ctx.playerState),
                .visualState = static_cast<int>(assignment.visualState),
            });
        }

        if (m_lastPush.valid && m_lastPush == m_scratch) {
            return;  // identical frame — nothing for the widget to redraw
        }
        // Swap rather than assign: m_scratch inherits the old buffers, so neither
        // side reallocates on the next push.
        std::swap(m_lastPush, m_scratch);

        intuition->SetSlotCount(static_cast<int>(ctx.slotCount));
        intuition->SetPage(
            static_cast<int>(ctx.pageIndex),
            static_cast<int>(ctx.pageCount),
            ctx.pageName);  // SetPage takes string_view + copies into the UI task

        logger::trace("[Intuition] displayMode={}"sv,
            displayMode == UI::DisplayMode::Verbose ? "verbose" :
            displayMode == UI::DisplayMode::Normal ? "normal" : "minimal");

        // Send from m_lastPush, not by recomputing: ToSlotContent / BuildSlotDetail
        // already ran during the change check above, and running them twice would
        // hand the widget values the comparison never saw.
        for (size_t i = 0; i < displayAssignments.size() && i < m_lastPush.slots.size(); ++i) {
            const auto& assignment = displayAssignments[i];
            const auto& view = m_lastPush.slots[i];
            logger::trace("[Intuition] slot={} formID={:08X} name='{}' detail='{}' hasCand={}"sv,
                assignment.slotIndex, assignment.formID, view.name, view.detail, assignment.HasCandidate());
            intuition->SetSlot(
                static_cast<int>(assignment.slotIndex),
                view.name,
                static_cast<UI::IntuitionSlotType>(view.type),
                view.confidence,
                view.detail,
                assignment.visualState);
        }
    }

}  // namespace Huginn::Display

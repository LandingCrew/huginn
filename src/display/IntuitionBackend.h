#pragma once

#include "IDisplayBackend.h"

#include <string>
#include <vector>

namespace Huginn::Display
{
    /// Pushes slot assignments to the Scaleform IntuitionMenu HUD widget.
    class IntuitionBackend final : public IDisplayBackend
    {
    public:
        void Push(const DisplayContext& ctx) override;
        [[nodiscard]] bool IsEnabled() const override;

    private:
        /// Exactly what was last handed to the widget, so an identical frame can
        /// be skipped. #14: Push had no gating, so every run re-sent every slot —
        /// a heap-allocated UI task plus two string copies per SetSlot, and a full
        /// AS2 setPage indicator redraw — for byte-identical content.
        ///
        /// Worth doing even though Display::Intuition measures only ~84 us/call in
        /// Tracy: that zone times the ENQUEUE, not the GFx work the task later
        /// does on the UI thread, which is the part this actually removes.
        struct SlotView
        {
            std::string name;
            int type = 0;
            std::string detail;
            int visualState = 0;

            /// Sent to the widget but NOT compared. It is `assignment.utility`, a
            /// float that moves on essentially every scoring run, so including it
            /// made the cache miss every time — measured 2026-08-22: Display::
            /// Intuition count stayed equal to ScoreCandidates count (188/188) and
            /// MTPC went UP, 84 -> 92.76 us, because the comparison work was added
            /// without ever paying off. It is safe to exclude because AS2's
            /// applyItemContent (Intuition.as:707) takes the parameter and never
            /// reads it — confidence drives nothing on screen.
            double confidence = 0.0;

            bool operator==(const SlotView& o) const
            {
                return name == o.name && type == o.type
                    && detail == o.detail && visualState == o.visualState;
            }
        };
        struct PushView
        {
            bool valid = false;          ///< false until the first push populates it
            size_t slotCount = 0;
            size_t pageIndex = 0;
            size_t pageCount = 0;
            std::string pageName;
            std::vector<SlotView> slots;
            bool operator==(const PushView& o) const
            {
                return slotCount == o.slotCount && pageIndex == o.pageIndex
                    && pageCount == o.pageCount && pageName == o.pageName
                    && slots == o.slots;
            }
        };
        PushView m_lastPush;
        PushView m_scratch;              ///< built each push, swapped in on a change (keeps capacity)

        // Wheel-overlap visibility tracking (was a local static in PushToIntuition)
        bool m_widgetHiddenForWheel = false;

        // Throttled null-singleton warning
        std::chrono::steady_clock::time_point m_lastNullWarn = std::chrono::steady_clock::now();
    };

}  // namespace Huginn::Display

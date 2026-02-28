#pragma once

#include "IDisplayBackend.h"

namespace Huginn::Display
{
    /// Pushes slot assignments to the Scaleform IntuitionMenu HUD widget.
    class IntuitionBackend final : public IDisplayBackend
    {
    public:
        void Push(const DisplayContext& ctx) override;
        [[nodiscard]] bool IsEnabled() const override;

    private:
        // Wheel-overlap visibility tracking (was a local static in PushToIntuition)
        bool m_widgetHiddenForWheel = false;

        // Throttled null-singleton warning
        std::chrono::steady_clock::time_point m_lastNullWarn = std::chrono::steady_clock::now();
    };

}  // namespace Huginn::Display

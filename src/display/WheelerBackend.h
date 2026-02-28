#pragma once

#include "IDisplayBackend.h"

namespace Huginn::Display
{
    /// Pushes slot assignments to the Wheeler radial menu (all pages).
    class WheelerBackend final : public IDisplayBackend
    {
    public:
        void Push(const DisplayContext& ctx) override;
        [[nodiscard]] bool IsEnabled() const override;
    };

}  // namespace Huginn::Display

#pragma once

#include "IDisplayBackend.h"
#include "learning/ScoredCandidate.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Huginn::Display
{
    /// Pushes slot assignments to the Wheeler radial menu (all pages).
    class WheelerBackend final : public IDisplayBackend
    {
    public:
        void Push(const DisplayContext& ctx) override;
        [[nodiscard]] bool IsEnabled() const override;
        [[nodiscard]] int GetDesiredPage() const override;

    private:
        // Reusable per-page extraction scratch buffers, cleared and refilled for
        // each page. Members (not per-loop locals) so their heap capacity is
        // retained across ticks instead of re-allocating four vectors per page per
        // tick. Push runs single-threaded on the pipeline thread, so no guard needed.
        std::vector<RE::FormID>  m_formIDs;
        std::vector<bool>        m_wildcardFlags;
        std::vector<uint16_t>    m_uniqueIDs;
        std::vector<std::string> m_subtexts;
        // A page's candidates without the gear its wheel cannot show
        // (WheelUidFilter.h), when any was left out. Same reuse as above.
        Scoring::ScoredCandidateList m_pageCandidates;
    };

}  // namespace Huginn::Display

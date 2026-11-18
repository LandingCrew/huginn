#pragma once

// =============================================================================
// INI OVERLAY -- the player's file over the shipped one (0.24.10)
// =============================================================================
// Every settings INI Huginn reads is parsed as two layers into one CSimpleIniA:
// first the shipped file (compiled into the DLL, cmake/EmbedShipped.cmake),
// then the player's file on top. A key the player's file sets replaces the
// shipped value; a key it leaves out keeps the shipped value. So "the default"
// of a setting is the shipped file's line, not a constant in a loader, and a
// player whose Huginn.ini is missing, old or half-deleted runs on exactly what
// a fresh install would.
//
// The loaders' own fallback constants still exist, and still apply to a key
// that is in neither file, or whose value does not parse (a NaN, an unknown
// mode name). They no longer decide any default the shipped file states.
//
// Header-only and SimpleIni-only on purpose: huginn_core_tests includes this
// file and pins the layering (tests/core/ShippedDefaultsTests.cpp), so the
// semantics the plugin relies on -- SimpleIni appends a second LoadData to the
// first, and with multi-key off a repeated key REPLACES the earlier value --
// are tested on the code the plugin runs.
// =============================================================================

#include <string_view>
#include <SimpleIni.h>

namespace Huginn::Settings
{
    /// What LoadOverlay read.
    struct OverlayResult
    {
        SI_Error shipped = SI_OK;   ///< the shipped layer's parse (< 0: failed)
        SI_Error player = SI_OK;    ///< the player's layer's parse (SI_OK when there was none)
        [[nodiscard]] bool Ok() const noexcept { return shipped >= 0 && player >= 0; }
    };

    /// @brief Parse `shipped`, then `player` over it, into `out` (cleared first).
    /// @param player The player's file's bytes; empty for none (a missing file).
    ///        A UTF-8 byte-order mark is skipped, as SimpleIni's LoadFile does.
    inline OverlayResult LoadOverlay(CSimpleIniA& out, std::string_view shipped, std::string_view player)
    {
        out.Reset();
        out.SetUnicode(true);
        out.SetMultiKey(false);    // a repeated key replaces: the overlay itself
        out.SetMultiLine(false);
        OverlayResult r;
        r.shipped = out.LoadData(shipped.data(), shipped.size());
        if (!player.empty()) {
            r.player = out.LoadData(player.data(), player.size());
        }
        return r;
    }
}

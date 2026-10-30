#pragma once

// =============================================================================
// SHIPPED DEFAULTS -- one source for every setting's default (0.24.10)
// =============================================================================
// configs/Huginn.ini and the dMenu descriptor (Huginn.json) are compiled into
// the DLL (cmake/EmbedShipped.cmake). They ARE the defaults:
//
//   - Every main-INI read goes through LoadMainIni: the shipped Huginn.ini,
//     then the player's Data/SKSE/Plugins/Huginn.ini over it
//     (settings/IniOverlay.h). A key the player's file leaves out, or a
//     missing file, reads the shipped value.
//   - Every [Widget]/[Debug] read goes through LoadDMenuIni: the descriptor's
//     defaults (core/DMenuDefaults.h, what dMenu writes on a fresh install),
//     then dMenu's INI over them.
//   - dMenu's Reset to Defaults (SettingsReloader) loads the shipped layers
//     ALONE, and RestoreShippedFiles writes them back to disk, so the next
//     game load reads them too: the game runs as a fresh install would.
//
// Learned data (the cosave's THTA/BIAS/BNDW) is not a setting and is not
// touched here: that is Reset Learned Weights.
// =============================================================================

#include <filesystem>
#include <string>
#include <string_view>
#include <SimpleIni.h>

namespace Huginn::Settings
{
    /// configs/Huginn.ini as shipped, byte for byte.
    [[nodiscard]] std::string_view ShippedMainIni() noexcept;

    /// The dMenu descriptor's defaults as dMenu writes them: [Widget] and
    /// [Debug] (core/DMenuDefaults.h). Built once.
    [[nodiscard]] const std::string& ShippedDMenuIni();

    /// @brief The main INI every settings loader reads: the shipped Huginn.ini,
    /// then the player's Huginn.ini (GetMainIniPath()) over it.
    /// @return true when the player's file was found and read; false when it is
    /// missing or unreadable (`out` then holds the shipped file alone).
    bool LoadMainIni(CSimpleIniA& out, std::string_view tag);

    /// @brief [Widget]/[Debug]: the descriptor's defaults, then the file at
    /// `path` over them. Callers pass GetDMenuIniPath() (dMenu's INI, or the
    /// main INI when dMenu's does not exist).
    /// @return true when the file at `path` was found and read.
    bool LoadDMenuIni(CSimpleIniA& out, const std::filesystem::path& path, std::string_view tag);

    /// The shipped layers alone, no player file (Reset to Defaults).
    void LoadShippedMainIni(CSimpleIniA& out);
    void LoadShippedDMenuIni(CSimpleIniA& out);

    /// What RestoreShippedFiles did, per file.
    struct RestoreResult
    {
        enum class Outcome
        {
            Written,        ///< replaced (the old file kept as <name>.<stamp>.bak), or created
            AlreadyShipped, ///< already byte-identical: nothing written, no .bak made
            Absent,         ///< dMenu's INI only: dMenu has not created one (not installed,
                            ///< or never run); nothing to reset, and none is created
            Failed,         ///< could not back up or write; the file on disk is unchanged
                            ///< (or, if only the final write failed, see the log)
        };
        Outcome main = Outcome::Failed;
        Outcome dMenu = Outcome::Failed;
        [[nodiscard]] bool Ok() const noexcept { return main != Outcome::Failed && dMenu != Outcome::Failed; }
    };

    /// @brief Write the shipped files over the player's: the shipped Huginn.ini
    /// to Data/SKSE/Plugins/Huginn.ini and, when dMenu's INI exists, the
    /// descriptor's defaults to it. A file that differs is first copied to
    /// <name>.<YYYYMMDD-HHMMSS>.bak (local time; every backup kept), so a
    /// reset can be undone by hand; a file already the same (Huginn.ini byte
    /// for byte, dMenu's INI key by key) is left alone and gets no backup.
    /// The new file is written to <name>.tmp and renamed over the old one.
    /// Logs each step.
    RestoreResult RestoreShippedFiles();
}

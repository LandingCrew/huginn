#pragma once

#include <filesystem>
#include <string_view>
#include <SimpleIni.h>

// =============================================================================
// Shared INI parse front door
// =============================================================================
// Every settings loader routes its disk read through LoadIniFile so the
// exists-check / SetUnicode / LoadFile / error-logging boilerplate lives in one
// place. Kept in its own lightweight header (rather than Globals.h) so the
// settings translation units don't pull in the whole global-systems include
// graph just to parse a file.
// =============================================================================

/// @brief Log severity for the "file not found" case (a parse failure is always
/// an error). Optional settings sections use Info; a file that ships with the mod
/// and is expected to be present (e.g. Huginn_Overrides.ini) uses Warn.
enum class IniMissing { Info, Warn };

/// @brief Parse an INI file from disk into `out`.
/// @param out      CSimpleIniA to populate (left empty on failure).
/// @param path     INI file to read.
/// @param tag      Log prefix, e.g. "ScorerSettings".
/// @param missing  Severity for the not-found case (default Info).
/// @return true if the file was found and parsed; false (caller keeps defaults) otherwise.
[[nodiscard]] bool LoadIniFile(CSimpleIniA& out, const std::filesystem::path& path,
                               std::string_view tag, IniMissing missing = IniMissing::Info);

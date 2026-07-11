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

/// @brief Parse an INI file from disk into `out`.
/// @param out   CSimpleIniA to populate (left empty on failure).
/// @param path  INI file to read.
/// @param tag   Log prefix, e.g. "ScorerSettings".
/// @return true if the file was found and parsed; false (caller keeps defaults) otherwise.
[[nodiscard]] bool LoadIniFile(CSimpleIniA& out, const std::filesystem::path& path, std::string_view tag);

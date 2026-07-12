#pragma once

#include <algorithm>
#include <cmath>
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

/// @brief Read a float INI value and clamp it to [lo, hi].
/// @details Warns (prefixed with `tag`) when the raw value was outside the range,
/// so a typo'd or garbage INI edit (e.g. a negative scoring weight) is surfaced in
/// the log and degraded gracefully instead of silently poisoning recommendations.
[[nodiscard]] inline float ReadClampedFloat(const CSimpleIniA& ini, const char* section,
    const char* key, double defaultVal, float lo, float hi, std::string_view tag)
{
   const float raw = static_cast<float>(ini.GetDoubleValue(section, key, defaultVal));

   // NaN/inf slip through std::clamp (all comparisons with NaN are false → it
   // returns NaN, which would then poison every downstream utility). Fall back to
   // the compile-time default instead — this is the one case a clamp can't fix.
   if (!std::isfinite(raw)) {
      const float fallback = std::clamp(static_cast<float>(defaultVal), lo, hi);
      logger::warn("[{}] {} = {} is not finite, using default {:.3f}"sv, tag, key, raw, fallback);
      return fallback;
   }

   const float clamped = std::clamp(raw, lo, hi);
   if (clamped != raw) {
      logger::warn("[{}] {} = {:.3f} out of range [{:.1f}, {:.1f}], clamped to {:.3f}"sv,
         tag, key, raw, lo, hi, clamped);
   }
   return clamped;
}

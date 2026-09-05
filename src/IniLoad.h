#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
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

// =============================================================================
// Override-file section namespacing
// =============================================================================
// Huginn_Overrides.ini is shared by SpellOverrides and ItemOverrides, and both
// used to walk EVERY section. A section written for one domain therefore also
// registered in the other: its `type` and `tags` were parsed against the wrong
// vocabulary, and because several token names exist in both (RestoreHealth,
// Fear, Frenzy, Invisibility, and `type = buff`), a section could silently
// apply to a spell and a potion sharing a display name.
//
// Sections may now be namespaced -- `[Spell:Fireball]`, `[Item:Potion of
// Healing]` -- and a prefixed section is visible ONLY to its own domain. An
// unprefixed section stays visible to both, so existing files keep working;
// each loader reports how many it saw so the ambiguity is greppable.

enum class OverrideDomain { Spell, Item };

struct OverrideSectionMatch
{
   bool belongs = false;   ///< Does this section apply to the domain asked about?
   bool prefixed = false;  ///< Was it explicitly namespaced (vs. legacy shared)?
   std::string name;       ///< Section name with any prefix stripped and trimmed.
};

/// @brief Decide whether an override section belongs to `domain`, and strip its prefix.
/// @details Prefix match is case-insensitive and tolerates spaces around the colon.
/// A section carrying the OTHER domain's prefix returns `belongs = false`.
[[nodiscard]] inline OverrideSectionMatch MatchOverrideSection(
   std::string_view section, OverrideDomain domain)
{
   const auto trim = [](std::string_view v) {
      const auto first = v.find_first_not_of(" 	");
      if (first == std::string_view::npos) return std::string_view{};
      const auto last = v.find_last_not_of(" 	");
      return v.substr(first, last - first + 1);
   };

   const auto startsWithCI = [](std::string_view hay, std::string_view needle) {
      if (hay.size() < needle.size()) return false;
      for (size_t i = 0; i < needle.size(); ++i) {
         const auto a = static_cast<unsigned char>(hay[i]);
         const auto b = static_cast<unsigned char>(needle[i]);
         if (std::tolower(a) != std::tolower(b)) return false;
      }
      return true;
   };

   const std::string_view trimmed = trim(section);

   struct { std::string_view prefix; OverrideDomain domain; } kPrefixes[] = {
      { "Spell:"sv, OverrideDomain::Spell },
      { "Item:"sv,  OverrideDomain::Item  },
   };

   for (const auto& p : kPrefixes) {
      if (!startsWithCI(trimmed, p.prefix)) {
         continue;
      }
      const std::string_view bare = trim(trimmed.substr(p.prefix.size()));
      // A prefix with nothing after it names no item; treat it as not ours so
      // it cannot register an empty-named override in either domain.
      if (bare.empty()) {
         return { false, true, {} };
      }
      return { p.domain == domain, true, std::string(bare) };
   }

   // Unprefixed: legacy shared section, visible to both domains.
   return { true, false, std::string(trimmed) };
}

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

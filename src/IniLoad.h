#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <cstdint>
#include <optional>
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

/// @brief Does this `type =` token parse in BOTH override vocabularies?
/// @details The tag guard (engage only when something parsed) cannot help for
/// `type`: a token that parses is not evidence it was meant for this domain.
/// These two parse in both `ItemOverrides::ParseItemType` and
/// `SpellOverrides::ParseSpellType`, so an UNPREFIXED section carrying one sets
/// the type for a spell AND an item sharing the name. Ambiguous rather than
/// wrong, so it warns instead of being skipped -- the user may well mean both.
/// KEEP IN SYNC with those two functions; a test pins the current pair.
[[nodiscard]] inline bool IsAmbiguousTypeToken(std::string_view token)
{
   const auto eqCI = [](std::string_view a, std::string_view b) {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
         if (std::tolower(static_cast<unsigned char>(a[i])) !=
             std::tolower(static_cast<unsigned char>(b[i]))) return false;
      }
      return true;
   };
   return eqCI(token, "buff"sv) || eqCI(token, "unknown"sv);
}

/// @brief Does this `tags =` token parse in BOTH override vocabularies?
/// @details The larger half of the overlap: the tag guard only neutralises a
/// cross-domain section when NOTHING parses, so a token common to both still
/// applies to a spell and an item sharing a name. KEEP IN SYNC with both
/// `ParseSingleTag` implementations; a test pins the current set.
[[nodiscard]] inline bool IsAmbiguousTagToken(std::string_view token)
{
   const auto eqCI = [](std::string_view a, std::string_view b) {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
         if (std::tolower(static_cast<unsigned char>(a[i])) !=
             std::tolower(static_cast<unsigned char>(b[i]))) return false;
      }
      return true;
   };
   // Item ParseSingleTag accepts "paralysis" as an alias for Paralyze, and the
   // spell arm has SpellTag::Paralysis -- so it is shared despite the different
   // enumerator names.
   static constexpr std::string_view kShared[] = {
      "restorehealth"sv, "restoremagicka"sv, "restorestamina"sv,
      "fear"sv, "frenzy"sv, "invisibility"sv, "paralysis"sv,
   };
   for (const auto& t : kShared) {
      if (eqCI(token, t)) return true;
   }
   return false;
}

/// Whitespace trimmed from each token of a `tags =` list.
inline constexpr std::string_view kTagTrim = " \t\n\r"sv;

/// @brief Does any token in a comma-separated `tags =` list parse in both?
[[nodiscard]] inline bool AnyAmbiguousTagToken(std::string_view tagList)
{
   size_t start = 0;
   while (start <= tagList.size()) {
      const auto comma = tagList.find(',', start);
      const auto end = (comma == std::string_view::npos) ? tagList.size() : comma;
      std::string_view tok = tagList.substr(start, end - start);
      const auto f = tok.find_first_not_of(kTagTrim);
      if (f != std::string_view::npos) {
         const auto l = tok.find_last_not_of(kTagTrim);
         if (IsAmbiguousTagToken(tok.substr(f, l - f + 1))) return true;
      }
      if (comma == std::string_view::npos) break;
      start = comma + 1;
   }
   return false;
}

/// @brief Parse a section name as an 8-hex-digit FormID, strictly.
/// @details `std::stoul` stops at the first invalid character instead of
/// throwing, so a name like `Deadwood` used to register as FormID 0x0000DEAD
/// and match nothing. Requires all-hex and full consumption; an optional `0x`
/// prefix is allowed.
[[nodiscard]] inline std::optional<std::uint32_t> TryParseFormID(std::string_view name)
{
   std::string_view body = name;
   if (body.size() > 2 && (body[0] == '0') && (body[1] == 'x' || body[1] == 'X')) {
      body = body.substr(2);
   }
   if (body.empty() || body.size() > 8) return std::nullopt;
   std::uint32_t value = 0;
   for (const char c : body) {
      const auto u = static_cast<unsigned char>(c);
      int digit;
      if (u >= '0' && u <= '9')      digit = u - '0';
      else if (u >= 'a' && u <= 'f') digit = u - 'a' + 10;
      else if (u >= 'A' && u <= 'F') digit = u - 'A' + 10;
      else return std::nullopt;
      value = (value << 4) | static_cast<std::uint32_t>(digit);
   }
   return value;
}

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

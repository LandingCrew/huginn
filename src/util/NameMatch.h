#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace Huginn::Util
{
   // =========================================================================
   // NAME MATCHING
   // =========================================================================
   // Classifiers fall back to reading an item's NAME when the game data does
   // not describe the property they need. That fallback is only as safe as its
   // matching rule, which is why this lives in one place with one test rather
   // than once per registry.
   // =========================================================================

   /// Case-insensitive substring match where the match must START at a word
   /// boundary.
   ///
   /// "Quicksilver" contains "silver". Quicksilver is mercury and carries no
   /// anti-undead property, but a loose match tagged every Quicksilver weapon
   /// WeaponTag::Silver. That was harmless while nothing read the tag; once the
   /// anti-undead context started reading it, an observed Quicksilver Greatsword
   /// doubled its weight against draugr (ctx 0.30 → 0.60) (#81).
   ///
   /// Leading boundary ONLY — "Silvered Sword" must still match "silver", so
   /// requiring a trailing boundary would break the case this exists to keep.
   [[nodiscard]] inline bool NameContainsWord(std::string_view name, std::string_view keyword)
   {
      auto caseInsensitiveEqual = [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
      };

      for (auto it = name.begin(); it != name.end(); ++it) {
      it = std::search(it, name.end(), keyword.begin(), keyword.end(), caseInsensitiveEqual);
      if (it == name.end()) {
        return false;
      }
      const bool atWordStart = it == name.begin() ||
                               !std::isalnum(static_cast<unsigned char>(*(it - 1)));
      if (atWordStart) {
        return true;
      }
      }
      return false;
   }
}

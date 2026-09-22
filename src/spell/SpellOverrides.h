#pragma once

#include "SpellData.h"
#include <SimpleIni.h>
#include <mutex>
#include <unordered_set>

namespace Huginn::Spell
{
   // Spell override data loaded from INI file
   struct SpellOverride
   {
      std::string name;
      std::optional<SpellType> type;
      std::optional<SpellTag> tags;
   };

   // Manages spell classification overrides from INI file
   class SpellOverrides
   {
   public:
      SpellOverrides() = default;
      ~SpellOverrides() = default;

      // Load overrides from INI file
      // Returns true if file was loaded successfully
      bool LoadFromFile(const std::filesystem::path& iniPath);

      // Check if a spell has an override by name
      [[nodiscard]] bool HasOverride(const std::string& spellName) const;

      // Check if a spell has an override by FormID
      [[nodiscard]] bool HasOverride(RE::FormID formID) const;

      // Get override data by spell name
      [[nodiscard]] std::optional<SpellOverride> GetOverride(const std::string& spellName) const;

      // Get override data by FormID
      [[nodiscard]] std::optional<SpellOverride> GetOverride(RE::FormID formID) const;

      // Get number of loaded overrides
      [[nodiscard]] size_t GetOverrideCount() const { return m_nameOverrides.size() + m_formIDOverrides.size(); }

      // Record that a key actually matched a spell. Called by the classifier.
      void NoteMatched(const std::string& spellName) const;
      void NoteMatched(RE::FormID formID) const;

      /// Log how many overrides were written versus how many ever matched, and
      /// NAME the ones that did not.
      ///
      /// Without this an override file fails silently: a misspelled spell name
      /// or a stale FormID loads fine, matches nothing, and the log still says
      /// "Loaded N spell overrides". At one or two entries you notice. At a
      /// hundred -- which is what a heavily modded load order needs, since its
      /// script-driven spells can never be classified from effect data -- you
      /// are diffing CSVs to find the typo.
      void ReportUsage(std::string_view context) const;

   private:
      // Parse spell type from string
      static std::optional<SpellType> ParseSpellType(const std::string& typeStr);

      // Parse spell tags from comma-separated string
      static SpellTag ParseSpellTags(const std::string& tagsStr);

      // Parse a single tag name
      static std::optional<SpellTag> ParseSingleTag(const std::string& tagStr);

      // Storage
      std::unordered_map<std::string, SpellOverride> m_nameOverrides;  // Spell name -> override
      std::unordered_map<RE::FormID, SpellOverride> m_formIDOverrides;  // FormID -> override

      // Which keys have matched since the last load. Mutable because matching
      // happens during classification, which is const; guarded because
      // classification runs on the update thread and again from the console
      // thread when `hg dump spells` walks the whole form array.
      mutable std::mutex m_matchMutex;
      mutable std::unordered_set<std::string> m_matchedNames;
      mutable std::unordered_set<RE::FormID> m_matchedFormIDs;
   };
}

#pragma once

#include "SpellData.h"
#include <SimpleIni.h>

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
   };
}

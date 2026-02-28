#pragma once

#include "ItemData.h"
#include <SimpleIni.h>

namespace Huginn::Item
{
   // Item override data loaded from INI file
   struct ItemOverride
   {
      std::string name;
      std::optional<ItemType> type;
      std::optional<ItemTag> tags;
   };

   // Manages item classification overrides from INI file
   class ItemOverrides
   {
   public:
      ItemOverrides() = default;
      ~ItemOverrides() = default;

      // Load overrides from INI file
      // Returns true if file was loaded successfully
      bool LoadFromFile(const std::filesystem::path& iniPath);

      // Check if an item has an override by name
      [[nodiscard]] bool HasOverride(const std::string& itemName) const;

      // Check if an item has an override by FormID
      [[nodiscard]] bool HasOverride(RE::FormID formID) const;

      // Get override data by item name
      [[nodiscard]] std::optional<ItemOverride> GetOverride(const std::string& itemName) const;

      // Get override data by FormID
      [[nodiscard]] std::optional<ItemOverride> GetOverride(RE::FormID formID) const;

      // Get number of loaded overrides
      [[nodiscard]] size_t GetOverrideCount() const { return m_nameOverrides.size() + m_formIDOverrides.size(); }

   private:
      // Parse item type from string
      static std::optional<ItemType> ParseItemType(const std::string& typeStr);

      // Parse item tags from comma-separated string
      static ItemTag ParseItemTags(const std::string& tagsStr);

      // Parse a single tag name
      static std::optional<ItemTag> ParseSingleTag(const std::string& tagStr);

      // Storage
      std::unordered_map<std::string, ItemOverride> m_nameOverrides;  // Item name -> override
      std::unordered_map<RE::FormID, ItemOverride> m_formIDOverrides; // FormID -> override
   };
}

#include "ItemOverrides.h"
#include <algorithm>
#include <cctype>

namespace Huginn::Item
{
   bool ItemOverrides::LoadFromFile(const std::filesystem::path& iniPath)
   {
      if (!std::filesystem::exists(iniPath)) {
      logger::warn("Item overrides file not found: {}"sv, iniPath.string());
      return false;
      }

      CSimpleIniA ini;
      ini.SetUnicode();
      SI_Error rc = ini.LoadFile(iniPath.string().c_str());

      if (rc < 0) {
      logger::error("Failed to load item overrides file: {}"sv, iniPath.string());
      return false;
      }

      logger::info("Loading item overrides from: {}"sv, iniPath.string());

      // Iterate through all sections (each section is an item name or FormID)
      CSimpleIniA::TNamesDepend sections;
      ini.GetAllSections(sections);

      for (const auto& section : sections) {
      std::string sectionName = section.pItem;

      // Skip comments and empty sections
      if (sectionName.empty() || sectionName[0] == ';' || sectionName[0] == '#') {
        continue;
      }

      ItemOverride override;
      override.name = sectionName;

      // Read type
      const char* typeStr = ini.GetValue(sectionName.c_str(), "type", nullptr);
      if (typeStr) {
        override.type = ParseItemType(typeStr);
      }

      // Read tags
      const char* tagsStr = ini.GetValue(sectionName.c_str(), "tags", nullptr);
      if (tagsStr) {
        override.tags = ParseItemTags(tagsStr);
      }

      // Determine if this is a FormID or item name
      // FormIDs are hex strings like "00012FCD" or "0x00012FCD"
      if (sectionName.length() == 8 || sectionName.substr(0, 2) == "0x") {
        try {
           RE::FormID formID = std::stoul(sectionName, nullptr, 16);
           m_formIDOverrides[formID] = override;
           logger::debug("Loaded FormID override: {:08X}"sv, formID);
        } catch (const std::exception&) {
           // Not a valid FormID, treat as name
           m_nameOverrides[sectionName] = override;
           logger::debug("Loaded name override: {}"sv, sectionName);
        }
      } else {
        // Item name
        m_nameOverrides[sectionName] = override;
        logger::debug("Loaded name override: {}"sv, sectionName);
      }
      }

      logger::info("Loaded {} item overrides ({} by name, {} by FormID)"sv,
      GetOverrideCount(), m_nameOverrides.size(), m_formIDOverrides.size());

      return true;
   }

   bool ItemOverrides::HasOverride(const std::string& itemName) const
   {
      return m_nameOverrides.contains(itemName);
   }

   bool ItemOverrides::HasOverride(RE::FormID formID) const
   {
      return m_formIDOverrides.contains(formID);
   }

   std::optional<ItemOverride> ItemOverrides::GetOverride(const std::string& itemName) const
   {
      auto it = m_nameOverrides.find(itemName);
      if (it != m_nameOverrides.end()) {
      return it->second;
      }
      return std::nullopt;
   }

   std::optional<ItemOverride> ItemOverrides::GetOverride(RE::FormID formID) const
   {
      auto it = m_formIDOverrides.find(formID);
      if (it != m_formIDOverrides.end()) {
      return it->second;
      }
      return std::nullopt;
   }

   std::optional<ItemType> ItemOverrides::ParseItemType(const std::string& typeStr)
   {
      std::string lower = typeStr;
      std::transform(lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return std::tolower(c); });

      if (lower == "unknown") return ItemType::Unknown;
      if (lower == "healthpotion" || lower == "health") return ItemType::HealthPotion;
      if (lower == "magickapotion" || lower == "magicka") return ItemType::MagickaPotion;
      if (lower == "staminapotion" || lower == "stamina") return ItemType::StaminaPotion;
      if (lower == "resistpotion" || lower == "resist") return ItemType::ResistPotion;
      if (lower == "buffpotion" || lower == "buff" || lower == "fortify") return ItemType::BuffPotion;
      if (lower == "curepotion" || lower == "cure") return ItemType::CurePotion;
      if (lower == "poison") return ItemType::Poison;
      if (lower == "food") return ItemType::Food;
      if (lower == "alcohol") return ItemType::Alcohol;
      if (lower == "ingredient") return ItemType::Ingredient;

      logger::warn("Unknown item type: {}"sv, typeStr);
      return std::nullopt;
   }

   ItemTag ItemOverrides::ParseItemTags(const std::string& tagsStr)
   {
      ItemTag result = ItemTag::None;

      // Split by comma
      std::istringstream stream(tagsStr);
      std::string token;

      while (std::getline(stream, token, ',')) {
      // Trim whitespace
      token.erase(0, token.find_first_not_of(" \t\n\r"));
      token.erase(token.find_last_not_of(" \t\n\r") + 1);

      if (auto tag = ParseSingleTag(token)) {
        result |= *tag;  // Use defined operator|=
      }
      }

      return result;
   }

   std::optional<ItemTag> ItemOverrides::ParseSingleTag(const std::string& tagStr)
   {
      std::string lower = tagStr;
      std::transform(lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return std::tolower(c); });

      // Restoration effects
      if (lower == "restorehealth") return ItemTag::RestoreHealth;
      if (lower == "restoremagicka") return ItemTag::RestoreMagicka;
      if (lower == "restorestamina") return ItemTag::RestoreStamina;

      // Resistances
      if (lower == "resistfire") return ItemTag::ResistFire;
      if (lower == "resistfrost") return ItemTag::ResistFrost;
      if (lower == "resistshock") return ItemTag::ResistShock;
      if (lower == "resistmagic") return ItemTag::ResistMagic;
      if (lower == "resistpoison") return ItemTag::ResistPoison;
      if (lower == "resistdisease") return ItemTag::ResistDisease;

      // Fortifications - Vitals
      if (lower == "fortifyhealth") return ItemTag::FortifyHealth;
      if (lower == "fortifymagicka") return ItemTag::FortifyMagicka;
      if (lower == "fortifystamina") return ItemTag::FortifyStamina;

      // v0.8: Grouped fortify tags
      if (lower == "fortifymagicschool") return ItemTag::FortifyMagicSchool;
      if (lower == "fortifycombatskill") return ItemTag::FortifyCombatSkill;
      if (lower == "fortifyutilityskill") return ItemTag::FortifyUtilitySkill;
      if (lower == "fortifycarryweight") return ItemTag::FortifyCarryWeight;

      // v0.8: Deprecated - warn and default to combat skill
      if (lower == "fortifyskill") {
      logger::warn("'FortifySkill' is deprecated in v0.8. Use FortifyMagicSchool, FortifyCombatSkill, or FortifyUtilitySkill instead. Defaulting to FortifyCombatSkill."sv);
      return ItemTag::FortifyCombatSkill;
      }

      // Regeneration
      if (lower == "regenhealth") return ItemTag::RegenHealth;
      if (lower == "regenmagicka") return ItemTag::RegenMagicka;
      if (lower == "regenstamina") return ItemTag::RegenStamina;  // v0.8: NEW

      // Cures
      if (lower == "curedisease") return ItemTag::CureDisease;
      if (lower == "curepoison") return ItemTag::CurePoison;

      // Survival Mode
      if (lower == "satisfieshunger" || lower == "hunger") return ItemTag::SatisfiesHunger;
      if (lower == "satisfiescold" || lower == "warm" || lower == "warming") return ItemTag::SatisfiesCold;

      // Poison effects
      if (lower == "damagehealth") return ItemTag::DamageHealth;
      if (lower == "damagemagicka") return ItemTag::DamageMagicka;
      if (lower == "damagestamina") return ItemTag::DamageStamina;
      if (lower == "paralyze" || lower == "paralysis") return ItemTag::Paralyze;
      if (lower == "slow") return ItemTag::Slow;
      if (lower == "frenzy") return ItemTag::Frenzy;
      if (lower == "fear") return ItemTag::Fear;  // v0.8: NEW
      if (lower == "invisibility") return ItemTag::Invisibility;
      if (lower == "waterbreathing") return ItemTag::Waterbreathing;

      // v0.8: Lingering moved to ItemTagExt - warn user
      if (lower == "lingering") {
      logger::warn("'Lingering' tag moved to ItemTagExt in v0.8. INI override not yet supported for extended tags."sv);
      return std::nullopt;
      }

      logger::warn("Unknown item tag: {}"sv, tagStr);
      return std::nullopt;
   }
}

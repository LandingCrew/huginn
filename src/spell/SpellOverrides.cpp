#include "SpellOverrides.h"
#include <algorithm>
#include <cctype>

namespace Huginn::Spell
{
   bool SpellOverrides::LoadFromFile(const std::filesystem::path& iniPath)
   {
      if (!std::filesystem::exists(iniPath)) {
      logger::warn("Spell overrides file not found: {}"sv, iniPath.string());
      return false;
      }

      CSimpleIniA ini;
      ini.SetUnicode();
      SI_Error rc = ini.LoadFile(iniPath.string().c_str());

      if (rc < 0) {
      logger::error("Failed to load spell overrides file: {}"sv, iniPath.string());
      return false;
      }

      logger::info("Loading spell overrides from: {}"sv, iniPath.string());

      // Iterate through all sections (each section is a spell name or FormID)
      CSimpleIniA::TNamesDepend sections;
      ini.GetAllSections(sections);

      for (const auto& section : sections) {
      std::string sectionName = section.pItem;

      // Skip comments and empty sections
      if (sectionName.empty() || sectionName[0] == ';' || sectionName[0] == '#') {
        continue;
      }

      SpellOverride override;
      override.name = sectionName;

      // Read type
      const char* typeStr = ini.GetValue(sectionName.c_str(), "type", nullptr);
      if (typeStr) {
        override.type = ParseSpellType(typeStr);
      }

      // Read tags
      const char* tagsStr = ini.GetValue(sectionName.c_str(), "tags", nullptr);
      if (tagsStr) {
        override.tags = ParseSpellTags(tagsStr);
      }

      // Determine if this is a FormID or spell name
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
        // Spell name
        m_nameOverrides[sectionName] = override;
        logger::debug("Loaded name override: {}"sv, sectionName);
      }
      }

      logger::info("Loaded {} spell overrides ({} by name, {} by FormID)"sv,
      GetOverrideCount(), m_nameOverrides.size(), m_formIDOverrides.size());

      return true;
   }

   bool SpellOverrides::HasOverride(const std::string& spellName) const
   {
      return m_nameOverrides.contains(spellName);
   }

   bool SpellOverrides::HasOverride(RE::FormID formID) const
   {
      return m_formIDOverrides.contains(formID);
   }

   std::optional<SpellOverride> SpellOverrides::GetOverride(const std::string& spellName) const
   {
      auto it = m_nameOverrides.find(spellName);
      if (it != m_nameOverrides.end()) {
      return it->second;
      }
      return std::nullopt;
   }

   std::optional<SpellOverride> SpellOverrides::GetOverride(RE::FormID formID) const
   {
      auto it = m_formIDOverrides.find(formID);
      if (it != m_formIDOverrides.end()) {
      return it->second;
      }
      return std::nullopt;
   }

   std::optional<SpellType> SpellOverrides::ParseSpellType(const std::string& typeStr)
   {
      std::string lower = typeStr;
      std::transform(lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return std::tolower(c); });

      if (lower == "unknown") return SpellType::Unknown;
      if (lower == "healing") return SpellType::Healing;
      if (lower == "damage") return SpellType::Damage;
      if (lower == "defensive") return SpellType::Defensive;
      if (lower == "utility") return SpellType::Utility;
      if (lower == "summon") return SpellType::Summon;
      if (lower == "buff") return SpellType::Buff;
      if (lower == "debuff") return SpellType::Debuff;

      logger::warn("Unknown spell type: {}"sv, typeStr);
      return std::nullopt;
   }

   SpellTag SpellOverrides::ParseSpellTags(const std::string& tagsStr)
   {
      SpellTag result = SpellTag::None;

      // Split by comma
      std::istringstream stream(tagsStr);
      std::string token;

      while (std::getline(stream, token, ',')) {
      // Trim whitespace
      token.erase(0, token.find_first_not_of(" \t\n\r"));
      token.erase(token.find_last_not_of(" \t\n\r") + 1);

      if (auto tag = ParseSingleTag(token)) {
        result = static_cast<SpellTag>(static_cast<uint32_t>(result) | static_cast<uint32_t>(*tag));
      }
      }

      return result;
   }

   std::optional<SpellTag> SpellOverrides::ParseSingleTag(const std::string& tagStr)
   {
      std::string lower = tagStr;
      std::transform(lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return std::tolower(c); });

      // Damage types
      if (lower == "fire") return SpellTag::Fire;
      if (lower == "frost") return SpellTag::Frost;
      if (lower == "shock") return SpellTag::Shock;
      if (lower == "poison") return SpellTag::Poison;
      if (lower == "sun") return SpellTag::Sun;

      // Range/area
      if (lower == "ranged") return SpellTag::Ranged;
      if (lower == "melee") return SpellTag::Melee;
      if (lower == "touch") return SpellTag::Melee;  // Touch = melee range
      if (lower == "aoe") return SpellTag::AOE;
      if (lower == "concentration") return SpellTag::Concentration;

      // Special properties
      if (lower == "antiundead") return SpellTag::AntiUndead;
      if (lower == "antidaedra") return SpellTag::AntiDaedra;
      if (lower == "stealth") return SpellTag::Stealth;
      if (lower == "conjuration") return SpellTag::Conjuration;

      // Restoration specific
      if (lower == "restoration") return SpellTag::RestoreHealth;  // Generic restoration
      if (lower == "restorehealth") return SpellTag::RestoreHealth;
      if (lower == "restoremagicka") return SpellTag::RestoreMagicka;
      if (lower == "restorestamina") return SpellTag::RestoreStamina;
      if (lower == "ward") return SpellTag::Ward;
      if (lower == "turnundead") return SpellTag::TurnUndead;

      // Alteration specific
      if (lower == "alteration") return SpellTag::Armor;  // Generic alteration
      if (lower == "armor") return SpellTag::Armor;
      if (lower == "defensive") return SpellTag::Armor;  // Defensive = armor
      if (lower == "detectlife") return SpellTag::DetectLife;
      if (lower == "light") return SpellTag::Light;
      if (lower == "telekinesis") return SpellTag::Telekinesis;
      if (lower == "paralysis") return SpellTag::Paralysis;

      // Illusion specific
      if (lower == "illusion") return SpellTag::Calm;  // Generic illusion
      if (lower == "calm") return SpellTag::Calm;
      if (lower == "charm") return SpellTag::Calm;  // Charm = calm
      if (lower == "fear") return SpellTag::Fear;
      if (lower == "frenzy") return SpellTag::Frenzy;
      if (lower == "invisibility") return SpellTag::Invisibility;
      if (lower == "muffle") return SpellTag::Muffle;

      // Destruction/offense (no specific tag, use fire/frost/shock instead)
      if (lower == "destruction") return SpellTag::Fire;  // Default to fire
      if (lower == "offensive") return SpellTag::Fire;  // Default to fire

      logger::warn("Unknown spell tag: {}"sv, tagStr);
      return std::nullopt;
   }
}

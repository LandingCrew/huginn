#include "SpellOverrides.h"
#include "IniLoad.h"
#include <algorithm>
#include <cctype>

namespace Huginn::Spell
{
   bool SpellOverrides::LoadFromFile(const std::filesystem::path& iniPath)
   {
      // Clear BEFORE the early return, not after: a player who deletes or renames
      // the file to turn overrides off and runs `hg rebuild` must actually get
      // them off. Keeping last-known-good on a missing file would leave every
      // stale override live for the rest of the session with no way to clear it.
      m_nameOverrides.clear();
      m_formIDOverrides.clear();

      CSimpleIniA ini;
      if (!LoadIniFile(ini, iniPath, "SpellOverrides"sv, IniMissing::Warn)) {
      return false;
      }

      logger::info("Loading spell overrides from: {}"sv, iniPath.string());

      // Iterate through all sections (each section is a spell name or FormID)
      CSimpleIniA::TNamesDepend sections;
      ini.GetAllSections(sections);

      size_t sharedSections = 0;  // unprefixed, so also parsed as item overrides

      for (const auto& section : sections) {
      const std::string rawSection = section.pItem;

      // Skip comments and empty sections
      if (rawSection.empty() || rawSection[0] == ';' || rawSection[0] == '#') {
        continue;
      }

      // This file is shared with ItemOverrides. Skip anything explicitly
      // namespaced for the item domain, and keep the bare name for the rest.
      const auto match = MatchOverrideSection(rawSection, OverrideDomain::Spell);
      if (!match.belongs) {
        continue;
      }
      if (!match.prefixed) {
        ++sharedSections;
      }
      const std::string& sectionName = match.name;
      if (sectionName.empty()) {
        continue;
      }

      SpellOverride override;
      override.name = sectionName;

      // Read type. Keys are read from the RAW section name — the prefix is part
      // of the INI's section header, only the stored key drops it.
      const char* typeStr = ini.GetValue(rawSection.c_str(), "type", nullptr);
      if (typeStr) {
        override.type = ParseSpellType(typeStr);
        // The tag guard cannot cover `type`: a token that parses is not evidence
        // it was meant for this domain. `buff` and `unknown` parse in BOTH
        // vocabularies, so an unprefixed section carrying one silently sets the
        // type for an item of the same name too. Ambiguous rather than wrong --
        // the user may mean both -- so warn and let it stand.
        if (override.type && !match.prefixed && IsAmbiguousTypeToken(typeStr)) {
           logger::warn("[SpellOverrides] '{}': type '{}' parses in both the spell and item "
                        "vocabularies, and this section is unprefixed — it will also "
                        "set the type for an item named '{}'. Prefix with 'Spell:' or "
                        "'Item:' to scope it"sv, sectionName, typeStr, sectionName);
        }
      }

      // Read tags. Engage the optional ONLY if something actually parsed:
      // an unrecognised list (or one written in the item vocabulary) yields
      // SpellTag::None, and an ENGAGED None would beat auto-detection in
      // SpellClassifier's value_or and silently blank the spell's tags.
      const char* tagsStr = ini.GetValue(rawSection.c_str(), "tags", nullptr);
      if (tagsStr) {
        const SpellTag parsed = ParseSpellTags(tagsStr);
        if (parsed != SpellTag::None) {
           override.tags = parsed;
        } else {
           logger::warn("[SpellOverrides] '{}': no recognised spell tags in '{}' — "
                        "keeping auto-detection"sv, sectionName, tagsStr);
        }
      }

      // A section that produced neither a type nor tags contributes nothing.
      // Storing it is harmless today (the classifier tests each field) but it
      // inflates the counts and hides typos, so drop it and say so.
      if (!override.type && !override.tags) {
        if (typeStr || tagsStr) {
           logger::debug("[SpellOverrides] '{}': nothing usable for the spell domain, skipped"sv,
              sectionName);
        }
        continue;
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

      if (sharedSections > 0) {
      logger::info("[SpellOverrides] {} unprefixed section(s) are also offered to "
                   "ItemOverrides — prefix with 'Spell:' or 'Item:' to disambiguate"sv,
         sharedSections);
      }

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
        result |= *tag;  // uses SpellTag operator|= from SpellData.h
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

#include "ItemOverrides.h"
#include "IniLoad.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <system_error>

namespace Huginn::Item
{
   bool ItemOverrides::LoadFromFile(const std::filesystem::path& iniPath)
   {
      // Two different failures, two different answers. A file that is GONE means
      // the player turned overrides off -- clear, or every stale entry stays live
      // for the session with no way to drop it. A file that exists but fails to
      // PARSE (held open by an editor, caught mid-save, truncated) is a transient
      // read error, and wiping good overrides on it would silently reclassify the
      // whole registry. Keep last-known-good there, and say so.
      std::error_code existsEc;
      const bool filePresent = std::filesystem::exists(iniPath, existsEc) && !existsEc;
      if (!filePresent) {
      m_nameOverrides.clear();
      m_formIDOverrides.clear();
      }

      CSimpleIniA ini;
      if (!LoadIniFile(ini, iniPath, "ItemOverrides"sv, IniMissing::Warn)) {
      if (filePresent) {
        logger::warn("[ItemOverrides] Parse failed — KEEPING the {} override(s) already loaded. "
                     "Fix the file and re-run `hg rebuild`"sv, GetOverrideCount());
      }
      return false;
      }

      // Parsed cleanly: now replace wholesale.
      m_nameOverrides.clear();
      m_formIDOverrides.clear();

      logger::info("Loading item overrides from: {}"sv, iniPath.string());

      // Iterate through all sections (each section is an item name or FormID)
      CSimpleIniA::TNamesDepend sections;
      ini.GetAllSections(sections);

      // Keys claimed by an explicitly prefixed section, so precedence does not
      // depend on SimpleIni's section ordering.
      std::unordered_set<std::string> prefixedNames;
      std::unordered_set<RE::FormID> prefixedFormIDs;

      size_t sharedSections = 0;  // unprefixed, so also parsed as spell overrides

      for (const auto& section : sections) {
      const std::string rawSection = section.pItem;

      // Skip comments and empty sections
      if (rawSection.empty() || rawSection[0] == ';' || rawSection[0] == '#') {
        continue;
      }

      // This file is shared with SpellOverrides. Skip anything explicitly
      // namespaced for the spell domain, and keep the bare name for the rest.
      const auto match = MatchOverrideSection(rawSection, OverrideDomain::Item);
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

      ItemOverride override;
      override.name = sectionName;

      // Read type. Keys are read from the RAW section name — the prefix is part
      // of the INI's section header, only the stored key drops it.
      const char* typeStr = ini.GetValue(rawSection.c_str(), "type", nullptr);
      if (typeStr) {
        override.type = ParseItemType(typeStr);
        // The tag guard cannot cover `type`: a token that parses is not evidence
        // it was meant for this domain. `buff` and `unknown` parse in BOTH
        // vocabularies, so an unprefixed section carrying one silently sets the
        // type for a spell of the same name too. Ambiguous rather than wrong --
        // the user may mean both -- so warn and let it stand.
        if (override.type && !match.prefixed && IsAmbiguousTypeToken(typeStr)) {
           logger::warn("[ItemOverrides] '{}': type '{}' parses in both the spell and item "
                        "vocabularies, and this section is unprefixed — it will also "
                        "set the type for a spell named '{}'. Prefix with 'Spell:' or "
                        "'Item:' to scope it"sv, sectionName, typeStr, sectionName);
        }
      }

      // Read tags. Engage the optional ONLY if something actually parsed:
      // an unrecognised list (or one written in the spell vocabulary) yields
      // ItemTag::None, and an ENGAGED None would beat auto-detection in
      // ItemClassifier's value_or and silently blank the item's tags.
      const char* tagsStr = ini.GetValue(rawSection.c_str(), "tags", nullptr);
      if (tagsStr) {
        const ItemTag parsed = ParseItemTags(tagsStr);
        if (parsed != ItemTag::None) {
           override.tags = parsed;
        } else {
           logger::warn("[ItemOverrides] '{}': no recognised item tags in '{}' — "
                        "keeping auto-detection"sv, sectionName, tagsStr);
        }
        // The tag guard only neutralises a cross-domain section when NOTHING
        // parses. Seven tokens parse in BOTH vocabularies, so an unprefixed
        // section using one silently sets the tags for a spell of the same name.
        // This is the WIDER half of the overlap -- `type` shares only two.
        if (override.tags && !match.prefixed && AnyAmbiguousTagToken(tagsStr)) {
           logger::warn("[ItemOverrides] '{}': tags '{}' include token(s) that parse in both the "
                        "spell and item vocabularies, and this section is unprefixed — "
                        "it will also set the tags for a spell named '{}'. Prefix with "
                        "'Spell:' or 'Item:' to scope it"sv, sectionName, tagsStr, sectionName);
        }
      }

      // A section that produced neither a type nor tags contributes nothing.
      // Storing it is harmless today (the classifier tests each field) but it
      // inflates the counts and hides typos, so drop it and say so.
      if (!override.type && !override.tags) {
        if (typeStr || tagsStr) {
           logger::debug("[ItemOverrides] '{}': nothing usable for the item domain, skipped"sv,
              sectionName);
        }
        continue;
      }

      // FormID or name? Strict all-hex parse -- std::stoul stops at the first
      // invalid character instead of throwing, so a name like "Deadwood" used to
      // register as FormID 0x0000DEAD and match nothing, and any name starting
      // "0x" registered as FormID 0.
      if (const auto formID = TryParseFormID(sectionName)) {
        // Prefixed beats unprefixed, whatever order SimpleIni hands us the
        // sections in. Stripping makes [X] and [Item:X] the same key, and
        // last-write-wins would otherwise be decided by alphabetical order of
        // the RAW name -- so [Fireball] would lose to [Item:Fireball] but
        // [Whirlwind] would BEAT [Item:Whirlwind]. Half-migrated files are
        // exactly what backward compatibility invites, so make it deterministic
        // and say when it happens.
        if (!match.prefixed && prefixedFormIDs.contains(*formID)) {
           logger::warn("[ItemOverrides] '{}': ignored — a prefixed section already defines "
                        "FormID {:08X}. Delete the unprefixed duplicate"sv,
              rawSection, *formID);
           continue;
        }
        if (match.prefixed) {
           if (m_formIDOverrides.contains(*formID)) {
              logger::warn("[ItemOverrides] '{}': overriding an earlier unprefixed section for "
                           "FormID {:08X} — the prefixed one wins"sv, rawSection, *formID);
           }
           prefixedFormIDs.insert(*formID);
        }
        m_formIDOverrides[*formID] = override;
        logger::debug("Loaded FormID override: {:08X}"sv, *formID);
      } else {
        if (!match.prefixed && prefixedNames.contains(sectionName)) {
           logger::warn("[ItemOverrides] '{}': ignored — a prefixed section already defines "
                        "'{}'. Delete the unprefixed duplicate"sv, rawSection, sectionName);
           continue;
        }
        if (match.prefixed) {
           if (m_nameOverrides.contains(sectionName)) {
              logger::warn("[ItemOverrides] '{}': overriding an earlier unprefixed section for "
                           "'{}' — the prefixed one wins"sv, rawSection, sectionName);
           }
           prefixedNames.insert(sectionName);
        }
        m_nameOverrides[sectionName] = override;
        logger::debug("Loaded name override: {}"sv, sectionName);
      }
      }

      logger::info("Loaded {} item overrides ({} by name, {} by FormID)"sv,
      GetOverrideCount(), m_nameOverrides.size(), m_formIDOverrides.size());

      if (sharedSections > 0) {
      logger::info("[ItemOverrides] {} unprefixed section(s) are also offered to "
                   "SpellOverrides — prefix with 'Spell:' or 'Item:' to disambiguate"sv,
         sharedSections);
      }

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

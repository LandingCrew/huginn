#include "SpellOverrides.h"
#include "IniLoad.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <system_error>

namespace Huginn::Spell
{
   bool SpellOverrides::LoadFromFile(const std::filesystem::path& iniPath)
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
      if (!LoadIniFile(ini, iniPath, "SpellOverrides"sv, IniMissing::Warn)) {
      if (filePresent) {
        logger::warn("[SpellOverrides] Parse failed — KEEPING the {} override(s) already loaded. "
                     "Fix the file and re-run `hg rebuild`"sv, GetOverrideCount());
      }
      return false;
      }

      // Parsed cleanly: now replace wholesale.
      m_nameOverrides.clear();
      m_formIDOverrides.clear();

      logger::info("Loading spell overrides from: {}"sv, iniPath.string());

      // Iterate through all sections (each section is a spell name or FormID)
      CSimpleIniA::TNamesDepend sections;
      ini.GetAllSections(sections);

      // Keys claimed by an explicitly prefixed section, so precedence does not
      // depend on SimpleIni's section ordering.
      std::unordered_set<std::string> prefixedNames;
      std::unordered_set<RE::FormID> prefixedFormIDs;

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
        // The tag guard only neutralises a cross-domain section when NOTHING
        // parses. Seven tokens parse in BOTH vocabularies, so an unprefixed
        // section using one silently sets the tags for an item of the same name.
        // This is the WIDER half of the overlap -- `type` shares only two.
        if (override.tags && !match.prefixed && AnyAmbiguousTagToken(tagsStr)) {
           logger::warn("[SpellOverrides] '{}': tags '{}' include token(s) that parse in both the "
                        "spell and item vocabularies, and this section is unprefixed — "
                        "it will also set the tags for an item named '{}'. Prefix with "
                        "'Spell:' or 'Item:' to scope it"sv, sectionName, tagsStr, sectionName);
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

      // FormID or name? Strict all-hex parse -- std::stoul stops at the first
      // invalid character instead of throwing, so a name like "Deadwood" used to
      // register as FormID 0x0000DEAD and match nothing, and any name starting
      // "0x" registered as FormID 0.
      if (const auto formID = TryParseFormID(sectionName)) {
        // Prefixed beats unprefixed, whatever order SimpleIni hands us the
        // sections in. Stripping makes [X] and [Spell:X] the same key, and
        // last-write-wins would otherwise be decided by alphabetical order of
        // the RAW name -- so [Fireball] would lose to [Spell:Fireball] but
        // [Whirlwind] would BEAT [Spell:Whirlwind]. Half-migrated files are
        // exactly what backward compatibility invites, so make it deterministic
        // and say when it happens.
        if (!match.prefixed && prefixedFormIDs.contains(*formID)) {
           logger::warn("[SpellOverrides] '{}': ignored — a prefixed section already defines "
                        "FormID {:08X}. Delete the unprefixed duplicate"sv,
              rawSection, *formID);
           continue;
        }
        if (match.prefixed) {
           if (m_formIDOverrides.contains(*formID)) {
              logger::warn("[SpellOverrides] '{}': overriding an earlier unprefixed section for "
                           "FormID {:08X} — the prefixed one wins"sv, rawSection, *formID);
           }
           prefixedFormIDs.insert(*formID);
        }
        m_formIDOverrides[*formID] = override;
        logger::debug("Loaded FormID override: {:08X}"sv, *formID);
      } else {
        if (!match.prefixed && prefixedNames.contains(sectionName)) {
           logger::warn("[SpellOverrides] '{}': ignored — a prefixed section already defines "
                        "'{}'. Delete the unprefixed duplicate"sv, rawSection, sectionName);
           continue;
        }
        if (match.prefixed) {
           if (m_nameOverrides.contains(sectionName)) {
              logger::warn("[SpellOverrides] '{}': overriding an earlier unprefixed section for "
                           "'{}' — the prefixed one wins"sv, rawSection, sectionName);
           }
           prefixedNames.insert(sectionName);
        }
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

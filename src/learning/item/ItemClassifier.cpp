#include "ItemClassifier.h"
#include "apparel/ApparelData.h"   // CraftSkillForActorValue - one AV vocabulary, two callers

namespace Huginn::Item
{
   namespace
   {
      bool IsResistAV(RE::ActorValue av) noexcept
      {
      switch (av) {
      case RE::ActorValue::kResistFire:
      case RE::ActorValue::kResistFrost:
      case RE::ActorValue::kResistShock:
      case RE::ActorValue::kPoisonResist:
      case RE::ActorValue::kResistMagic:
      case RE::ActorValue::kResistDisease:
        return true;
      default:
        return false;
      }
      }

      // Tag a beneficial resist effect by the actor value it concerns.
      // Returns false when `av` is not a resistance.
      bool TagResist(RE::ActorValue av, ItemData& data) noexcept
      {
      switch (av) {
      case RE::ActorValue::kResistFire:    data.tags |= ItemTag::ResistFire;    data.element = ElementType::Fire;    return true;
      case RE::ActorValue::kResistFrost:   data.tags |= ItemTag::ResistFrost;   data.element = ElementType::Frost;   return true;
      case RE::ActorValue::kResistShock:   data.tags |= ItemTag::ResistShock;   data.element = ElementType::Shock;   return true;
      case RE::ActorValue::kPoisonResist:  data.tags |= ItemTag::ResistPoison;  data.element = ElementType::Poison;  return true;
      case RE::ActorValue::kResistMagic:   data.tags |= ItemTag::ResistMagic;   data.element = ElementType::Magic;   return true;
      case RE::ActorValue::kResistDisease: data.tags |= ItemTag::ResistDisease; data.element = ElementType::Disease; return true;
      default:                             return false;
      }
      }
   }

   void ItemClassifier::LoadOverrides(const std::filesystem::path& iniPath)
   {
      m_overrides.LoadFromFile(iniPath);
   }

   ItemData ItemClassifier::ClassifyItem(RE::AlchemyItem* item) const
   {
      if (!item) {
      // E6 (v0.7.21): Standardized to warn - null input is caller error, not system error
      logger::warn("ClassifyItem called with null item"sv);
      return ItemData{};
      }

      ItemData data{};
      data.formID = item->GetFormID();
      data.name = item->GetName();
      data.value = item->GetGoldValue();
      data.isHostile = item->IsPoison();

      // Check for overrides first (by FormID, then by name)
      std::optional<ItemOverride> override;

      if (m_overrides.HasOverride(data.formID)) {
      override = m_overrides.GetOverride(data.formID);
      logger::debug("Using FormID override for: {} ({:08X})"sv, data.name, data.formID);
      } else if (m_overrides.HasOverride(data.name)) {
      override = m_overrides.GetOverride(data.name);
      logger::debug("Using name override for: {}"sv, data.name);
      }

      // STEP 1: Compute tags FIRST (single source of matching)
      // v0.8: PopulateItemTags now populates tags, tagsExt, school, combatSkill, utilitySkill, element
      if (override && override->tags) {
      data.tags = *override->tags;
      } else {
      PopulateItemTags(item, data);
      }

      // STEP 2: Determine type - API first, then derive from tags
      if (override && override->type) {
      data.type = *override->type;
      } else {
      data.type = DetermineItemType(item);  // API-based
      if (data.type == ItemType::Unknown) {
        data.type = DeriveItemTypeFromTags(data.tags);  // Tag-based fallback
      }
      // Sub-classify food items: check if actually alcohol
      if (data.type == ItemType::Food && IsAlcohol(item, data.name)) {
        data.type = ItemType::Alcohol;
      }
      }

      // STEP 3: Get magnitude and duration from costliest effect
      data.magnitude = GetPrimaryMagnitude(item);
      data.duration = GetPrimaryDuration(item);

      // STEP 4: Soul gem capacity encoding (v0.7.8)
      //
      // FALLBACK PATH ONLY — reached by mod items that are AlchemyItem forms
      // wearing soul gem keywords, never by a real TESSoulGem (ClassifySoulGem
      // handles those and reads the soul actually held). There is no soul data
      // to read here at all, so capacity stands in for it: a rough proxy, and
      // the best available. The tagsExt bits are the durable part.
      //
      // Black is 5, not 6: SOUL_LEVEL tops out at kGrand=5, so a 6 would rank
      // these fallback items above anything the real API can produce.
      if (data.type == ItemType::SoulGem) {
      auto* keywordForm = item->As<RE::BGSKeywordForm>();
      if      (HasKeyword(keywordForm, "SoulGemBlack"))   { data.magnitude = 5.0f; data.tagsExt |= ItemTagExt::SoulGemBlack; }
      else if (HasKeyword(keywordForm, "SoulGemGrand"))   { data.magnitude = 5.0f; data.tagsExt |= ItemTagExt::SoulGemGrand; }
      else if (HasKeyword(keywordForm, "SoulGemGreater")) { data.magnitude = 4.0f; data.tagsExt |= ItemTagExt::SoulGemGreater; }
      else if (HasKeyword(keywordForm, "SoulGemCommon"))  { data.magnitude = 3.0f; data.tagsExt |= ItemTagExt::SoulGemCommon; }
      else if (HasKeyword(keywordForm, "SoulGemLesser"))  { data.magnitude = 2.0f; data.tagsExt |= ItemTagExt::SoulGemLesser; }
      else if (HasKeyword(keywordForm, "SoulGemPetty"))   { data.magnitude = 1.0f; data.tagsExt |= ItemTagExt::SoulGemPetty; }
      else data.magnitude = 0.0f;  // Unknown soul gem
      }

      return data;
   }

   ItemData ItemClassifier::ClassifySoulGem(RE::TESSoulGem* soulGem) noexcept
   {
      if (!soulGem) {
      logger::warn("ItemClassifier::ClassifySoulGem called with nullptr"sv);
      return ItemData{};
      }

      ItemData data{};
      data.formID = soulGem->GetFormID();
      data.name = soulGem->GetName();
      data.type = ItemType::SoulGem;
      data.tags = ItemTag::None;  // Tags not used for soul gems
      data.value = soulGem->GetGoldValue();
      data.isHostile = false;
      data.duration = 0.0f;

      // Magnitude is the soul the gem HOLDS, not the size of the gem.
      //
      // It used to be GetMaximumCapacity(), which ranked a Grand gem holding a
      // petty soul above a Common gem holding a common one — less charge
      // returned, and the expensive gem spent to do it. Capacity is the box;
      // this is what is in it, and only this decides the recharge.
      //
      // SOUL_LEVEL runs kNone=0 … kGrand=5. There is no 6 — the old comment
      // claiming 1-6 came from the keyword table's SoulGemBlack=6, a value the
      // API cannot return.
      //
      // Base form only, so this sees vendor/loot gems that ship with a soul.
      // Player-filled gems keep theirs in ExtraSoul, which is per-instance and
      // invisible here — ItemRegistry's scan reads that and overrides.
      data.magnitude = static_cast<float>(soulGem->GetContainedSoul());

      // Capacity moves to tagsExt rather than being dropped. It stopped being
      // the magnitude above, and nothing else in the system recorded it, so
      // "how big is this gem" became unanswerable — including for the black
      // gem query, which asked magnitude >= 6 and could no longer ever be true.
      // The AlchemyItem fallback path already sets these same bits from
      // keywords; this makes real TESSoulGem forms agree with it.
      switch (soulGem->GetMaximumCapacity()) {
         case RE::SOUL_LEVEL::kPetty:   data.tagsExt |= ItemTagExt::SoulGemPetty;   break;
         case RE::SOUL_LEVEL::kLesser:  data.tagsExt |= ItemTagExt::SoulGemLesser;  break;
         case RE::SOUL_LEVEL::kCommon:  data.tagsExt |= ItemTagExt::SoulGemCommon;  break;
         case RE::SOUL_LEVEL::kGreater: data.tagsExt |= ItemTagExt::SoulGemGreater; break;
         case RE::SOUL_LEVEL::kGrand:   data.tagsExt |= ItemTagExt::SoulGemGrand;   break;
         default: break;
      }
      // Black is a record flag, not a capacity — a black gem is a grand-capacity
      // gem that may also hold NPC souls, so it carries both bits.
      if (soulGem->CanHoldNPCSoul()) {
         data.tagsExt |= ItemTagExt::SoulGemBlack;
      }

      logger::trace("Classified soul gem: {} (soul={:.0f}, capacity={})"sv,
      data.name, data.magnitude, static_cast<int>(soulGem->GetMaximumCapacity()));

      return data;
   }

   ItemType ItemClassifier::DetermineItemType(RE::AlchemyItem* item) noexcept
   {
      // TIER 0: Soul gem detection (HIGHEST PRIORITY - v0.7.8)
      // Soul gems are AlchemyItem but need special handling before other checks
      if (HasSoulGemKeyword(item)) {
      return ItemType::SoulGem;
      }

      // TIER 1: Built-in API classification (most reliable)
      if (item->IsPoison()) {
      return ItemType::Poison;
      }
      if (item->IsFood()) {
      return ItemType::Food;
      }

      // Check if it's a raw ingredient (no "medicine" flag, has effects)
      // Ingredients have effects but aren't classified as medicine
      if (!item->IsMedicine()) {
      // Could be an ingredient if it has effects but isn't food/poison/medicine
      if (!item->effects.empty()) {
        // Check if it's actually usable as-is or needs to be crafted
        // Ingredients typically have 4 effects that need discovery
        // For now, classify non-medicine items with effects as Unknown
        // (will be derived from tags if needed)
        return ItemType::Unknown;
      }
      return ItemType::Unknown;
      }

      // TIER 2: Effect-based classification for medicines (potions)
      auto* effect = GetCostliestEffect(item);
      if (!effect || !effect->baseEffect) {
      return ItemType::BuffPotion;  // Default for medicine without effects
      }

      auto arch = effect->baseEffect->GetArchetype();
      auto primaryAV = effect->baseEffect->data.primaryAV;

      // Cure effects (check specific archetypes)
      if (arch == RE::EffectSetting::Archetype::kCureDisease ||
          arch == RE::EffectSetting::Archetype::kCurePoison) {
      return ItemType::CurePotion;
      }

      // Value modifiers (restore) - these RESTORE current value (healing potions)
      if (arch == RE::EffectSetting::Archetype::kValueModifier) {
      bool isHostile = effect->baseEffect->IsHostile();

      // ...unless its keyword says it fortifies the vital instead.
      const bool fortifies =
        ClassifyVitalEffect(effect->baseEffect, primaryAV) == VitalEffect::Fortify;
      if (!isHostile) {
        if (!fortifies) {
           switch (primaryAV) {
           case RE::ActorValue::kHealth:
            return ItemType::HealthPotion;
           case RE::ActorValue::kMagicka:
            return ItemType::MagickaPotion;
           case RE::ActorValue::kStamina:
            return ItemType::StaminaPotion;
           default:
            break;
           }
        }

        // Resist potions: the resistance is either what the effect modifies
        // (primaryAV) or what it is resisted by (resistVariable)
        if (IsResistAV(primaryAV)) {
           return ItemType::ResistPotion;
        }
        auto resistAV = effect->baseEffect->data.resistVariable;
        if (resistAV != RE::ActorValue::kNone) {
           switch (resistAV) {
           case RE::ActorValue::kResistFire:
           case RE::ActorValue::kResistFrost:
           case RE::ActorValue::kResistShock:
           case RE::ActorValue::kPoisonResist:
           case RE::ActorValue::kResistMagic:
           case RE::ActorValue::kResistDisease:
            return ItemType::ResistPotion;
           default:
            break;
           }
        }
      }
      }

      // Peak value modifiers (fortify) - these MODIFY max value (buff potions)
      // Fortify Health/Magicka/Stamina increase the max, not current value
      if (arch == RE::EffectSetting::Archetype::kPeakValueModifier) {
      bool isHostile = effect->baseEffect->IsHostile();
      if (!isHostile) {
        // A Peak modifier on a resistance IS a resist potion (Apothecary)
        if (IsResistAV(primaryAV)) {
           return ItemType::ResistPotion;
        }
        // ...and one whose keyword says it restores a vital is a heal
        // (LoreRim builds its restores as Peak modifiers)
        if (ClassifyVitalEffect(effect->baseEffect, primaryAV) == VitalEffect::Restore) {
           switch (primaryAV) {
           case RE::ActorValue::kHealth:  return ItemType::HealthPotion;
           case RE::ActorValue::kMagicka: return ItemType::MagickaPotion;
           case RE::ActorValue::kStamina: return ItemType::StaminaPotion;
           default: break;
           }
        }
        // Fortify vitals are buffs, not restore potions
        return ItemType::BuffPotion;
      }
      }

      // Dual value modifiers (fortify + regenerate, or restore + regen for mods like LORERIM)
      if (arch == RE::EffectSetting::Archetype::kDualValueModifier) {
      bool isHostile = effect->baseEffect->IsHostile();
      if (!isHostile) {
        // Check if primary effect is restore vital (LORERIM uses DualValueModifier for restore potions)
        switch (primaryAV) {
        case RE::ActorValue::kHealth:
           return ItemType::HealthPotion;
        case RE::ActorValue::kMagicka:
           return ItemType::MagickaPotion;
        case RE::ActorValue::kStamina:
           return ItemType::StaminaPotion;
        default:
           break;
        }
      }
      // Fallback: fortify effects (non-vital primary AV)
      return ItemType::BuffPotion;
      }

      // Invisibility potion
      if (arch == RE::EffectSetting::Archetype::kInvisibility) {
      return ItemType::BuffPotion;
      }

      // Default: treat as buff potion (fortify effects, etc.)
      return ItemType::BuffPotion;
   }

   ItemType ItemClassifier::DeriveItemTypeFromTags(ItemTag tags) noexcept
   {
      // Priority order matters - check most specific first

      // Cures (highest priority - specific function)
      if (HasTag(tags, ItemTag::CureDisease) || HasTag(tags, ItemTag::CurePoison)) {
      return ItemType::CurePotion;
      }

      // Restore vitals. A potion tagged BOTH restore and fortify for the same
      // vital is a fortify that the effect scan could not tell apart -- the
      // name fallback adds the Fortify tag. That was LoreRim's Fortify Health
      // (0x201), typed a health potion and offered by the CRITICAL health
      // override as a heal (2026-09-25). It falls through to the buff check.
      if (HasTag(tags, ItemTag::RestoreHealth) && !HasTag(tags, ItemTag::FortifyHealth)) return ItemType::HealthPotion;
      if (HasTag(tags, ItemTag::RestoreMagicka) && !HasTag(tags, ItemTag::FortifyMagicka)) return ItemType::MagickaPotion;
      if (HasTag(tags, ItemTag::RestoreStamina) && !HasTag(tags, ItemTag::FortifyStamina)) return ItemType::StaminaPotion;

      // Resistances
      if (HasTag(tags, ItemTag::ResistFire) || HasTag(tags, ItemTag::ResistFrost) ||
          HasTag(tags, ItemTag::ResistShock) || HasTag(tags, ItemTag::ResistMagic) ||
          HasTag(tags, ItemTag::ResistPoison) || HasTag(tags, ItemTag::ResistDisease)) {
      return ItemType::ResistPotion;
      }

      // Poison effects (hostile)
      // NOTE (v0.8): Lingering moved to ItemTagExt, checked separately if needed
      if (HasTag(tags, ItemTag::DamageHealth) || HasTag(tags, ItemTag::DamageMagicka) ||
          HasTag(tags, ItemTag::DamageStamina) || HasTag(tags, ItemTag::Paralyze) ||
          HasTag(tags, ItemTag::Slow) || HasTag(tags, ItemTag::Frenzy) ||
          HasTag(tags, ItemTag::Fear)) {
      return ItemType::Poison;
      }

      // Food (survival mode)
      if (HasTag(tags, ItemTag::SatisfiesHunger) || HasTag(tags, ItemTag::SatisfiesCold)) {
      return ItemType::Food;
      }

      // Buffs (fortify, regen, special effects)
      // v0.8: FortifySkill split into FortifyMagicSchool, FortifyCombatSkill, FortifyUtilitySkill
      if (HasTag(tags, ItemTag::FortifyHealth) || HasTag(tags, ItemTag::FortifyMagicka) ||
          HasTag(tags, ItemTag::FortifyStamina) ||
          HasTag(tags, ItemTag::FortifyMagicSchool) || HasTag(tags, ItemTag::FortifyCombatSkill) ||
          HasTag(tags, ItemTag::FortifyUtilitySkill) || HasTag(tags, ItemTag::FortifyCarryWeight) ||
          HasTag(tags, ItemTag::RegenHealth) || HasTag(tags, ItemTag::RegenMagicka) ||
          HasTag(tags, ItemTag::RegenStamina) ||
          HasTag(tags, ItemTag::Invisibility) || HasTag(tags, ItemTag::Waterbreathing)) {
      return ItemType::BuffPotion;
      }

      return ItemType::Unknown;
   }

   // =============================================================================
   // FORTIFY SKILL TYPE DETERMINATION (v0.8)
   // =============================================================================
   // Maps ActorValue → appropriate grouped tag + specific enum field
   // Called from PopulateItemTags when a fortify skill effect is detected
   // =============================================================================
   void ItemClassifier::DetermineFortifySkillType(RE::ActorValue av, ItemData& data) noexcept
   {
      // DEBUG v0.8: Log which AV we're trying to classify
      logger::debug("[DetermineFortifySkillType] {} - AV={}"sv, data.name, static_cast<int>(av));

      // Craft skills come first, through the shared vocabulary in ApparelData.h.
      // They used to be three groups in the switch below, hand-maintained
      // against a second copy in ApparelClassifier, and the two had drifted:
      // this side never learned the "Modifier" series that apparel enchantments
      // use, so a potion carrying kAlchemyModifier fell to the default and was
      // never tagged. That is the same omission that made #65 inert on its
      // first play-test, still live here.
      //
      // Each craft keeps the destination it always had — Alchemy is a utility
      // skill, Smithing a combat skill, Enchanting a magic school. Sharing the
      // question does not mean sharing the answer.
      if (const auto craft = Apparel::CraftSkillForActorValue(av);
          craft != Apparel::CraftSkill::None) {
         switch (craft) {
         case Apparel::CraftSkill::Alchemy:
            data.tags |= ItemTag::FortifyUtilitySkill;
            data.utilitySkill = UtilitySkill::Alchemy;
            return;
         case Apparel::CraftSkill::Smithing:
            data.tags |= ItemTag::FortifyCombatSkill;
            data.combatSkill = CombatSkill::Smithing;
            return;
         case Apparel::CraftSkill::Enchanting:
            data.tags |= ItemTag::FortifyMagicSchool;
            data.school = MagicSchool::Enchanting;
            return;
         case Apparel::CraftSkill::None:
            break;
         }
      }

      switch (av) {
      // Magic Schools
      // Magic schools take the PowerModifier series too, for the same reason
      // every other group below already does: LoreRim expresses fortify
      // effects that way, and StateManager_MagicEffects.cpp:251+ has handled
      // all five variants for as long as it has existed. Without them a
      // Fortify Destruction potion on that list falls to default, never gets
      // FortifyMagicSchool, and DeriveItemTypeFromTags answers Unknown rather
      // than BuffPotion -- the state layer sees the buff while the item layer
      // cannot recommend the potion that produces it. Same failure shape as
      // the craft drift above, one switch group over.
      case RE::ActorValue::kAlteration:
      case RE::ActorValue::kAlterationPowerModifier:   // LORERIM (148)
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Alteration;
      logger::debug("[DetermineFortifySkillType] {} -> FortifyMagicSchool, school=Alteration"sv, data.name);
      break;
      case RE::ActorValue::kConjuration:
      case RE::ActorValue::kConjurationPowerModifier:  // LORERIM (149)
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Conjuration;
      break;
      case RE::ActorValue::kDestruction:
      case RE::ActorValue::kDestructionPowerModifier:  // LORERIM (150)
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Destruction;
      break;
      case RE::ActorValue::kIllusion:
      case RE::ActorValue::kIllusionPowerModifier:     // LORERIM (151)
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Illusion;
      break;
      case RE::ActorValue::kRestoration:
      case RE::ActorValue::kRestorationPowerModifier:  // LORERIM (153)
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Restoration;
      break;
      // Combat Skills (base + PowerModifier variants for LORERIM compatibility)
      case RE::ActorValue::kOneHanded:
      case RE::ActorValue::kOneHandedPowerModifier:   // LORERIM (135)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::OneHanded;
      break;
      case RE::ActorValue::kTwoHanded:
      case RE::ActorValue::kTwoHandedPowerModifier:   // LORERIM (136)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::TwoHanded;
      break;
      case RE::ActorValue::kArchery:
      case RE::ActorValue::kMarksmanPowerModifier:    // LORERIM (137)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::Marksman;
      break;
      case RE::ActorValue::kBlock:
      case RE::ActorValue::kBlockPowerModifier:       // LORERIM (138)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::Block;
      break;
      case RE::ActorValue::kHeavyArmor:
      case RE::ActorValue::kHeavyArmorPowerModifier:  // LORERIM (139)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::HeavyArmor;
      break;
      case RE::ActorValue::kLightArmor:
      case RE::ActorValue::kLightArmorPowerModifier:  // LORERIM (140)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::LightArmor;
      break;
      case RE::ActorValue::kUnarmedDamage:            // LORERIM Fortify Unarmed (35)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::OneHanded;  // Map to OneHanded (closest combat skill)
      break;

      // Utility Skills (base + PowerModifier variants for LORERIM compatibility)
      case RE::ActorValue::kSneak:
      case RE::ActorValue::kSneakingPowerModifier:    // LORERIM (144)
      data.tags |= ItemTag::FortifyUtilitySkill;
      data.utilitySkill = UtilitySkill::Sneak;
      break;
      case RE::ActorValue::kLockpicking:
      case RE::ActorValue::kLockpickingPowerModifier: // LORERIM (145)
      data.tags |= ItemTag::FortifyUtilitySkill;
      data.utilitySkill = UtilitySkill::Lockpicking;
      break;
      case RE::ActorValue::kPickpocket:
      case RE::ActorValue::kPickpocketPowerModifier:  // LORERIM (146)
      data.tags |= ItemTag::FortifyUtilitySkill;
      data.utilitySkill = UtilitySkill::Pickpocket;
      break;
      case RE::ActorValue::kSpeech:
      case RE::ActorValue::kSpeechcraftPowerModifier: // LORERIM (147)
      data.tags |= ItemTag::FortifyUtilitySkill;
      data.utilitySkill = UtilitySkill::Speech;
      break;
      // Special cases
      case RE::ActorValue::kCarryWeight:
      data.tags |= ItemTag::FortifyCarryWeight;
      break;
      case RE::ActorValue::kSpeedMult:                // Fortify Speed (30)
      // Speed is a movement/utility buff - classify as utility skill
      // No specific skill enum, but tag ensures BuffPotion classification
      data.tags |= ItemTag::FortifyUtilitySkill;
      // utilitySkill stays None - this is a generic utility buff
      break;

      default:
      // Unknown fortify effect - log for debugging
      logger::trace("Unknown fortify skill ActorValue: {}"sv, static_cast<int>(av));
      break;
      }
   }

   void ItemClassifier::PopulateItemTags(RE::AlchemyItem* item, ItemData& data) const
   {
      if (!item) return;

      data.tags = ItemTag::None;
      data.tagsExt = ItemTagExt::None;
      const std::string_view name = item->GetName();

      // =============================================================================
      // EFFECT-BASED TAGS (Primary - most reliable)
      // =============================================================================

      for (auto* effect : item->effects) {
      if (!effect || !effect->baseEffect) continue;

      auto arch = effect->baseEffect->GetArchetype();
      auto primaryAV = effect->baseEffect->data.primaryAV;
      auto secondaryAV = effect->baseEffect->data.secondaryAV;
      auto resistAV = effect->baseEffect->data.resistVariable;
      bool isHostile = effect->baseEffect->IsHostile();

      // DEBUG v0.8: Log effect details to diagnose classification issues
      std::string keywords;
      for (uint32_t k = 0; k < effect->baseEffect->GetNumKeywords(); ++k) {
        if (auto kw = effect->baseEffect->GetKeywordAt(k); kw && *kw) {
           if (!keywords.empty()) keywords += ',';
           keywords += (*kw)->GetFormEditorID();
        }
      }
      logger::debug("[PopulateItemTags] {} - Effect: arch={}, primaryAV={}, secondaryAV={}, hostile={}, recover={}, kw=[{}]"sv,
        name,
        static_cast<int>(arch),
        static_cast<int>(primaryAV),
        static_cast<int>(secondaryAV),
        isHostile,
        effect->baseEffect->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kRecover),
        keywords);

      // Cure effects
      if (arch == RE::EffectSetting::Archetype::kCureDisease) {
        data.tags |= ItemTag::CureDisease;
      }
      if (arch == RE::EffectSetting::Archetype::kCurePoison) {
        data.tags |= ItemTag::CurePoison;
      }

      // Invisibility
      if (arch == RE::EffectSetting::Archetype::kInvisibility) {
        data.tags |= ItemTag::Invisibility;
      }

      // Paralysis
      if (arch == RE::EffectSetting::Archetype::kParalysis) {
        data.tags |= ItemTag::Paralyze;
      }

      // Frenzy
      if (arch == RE::EffectSetting::Archetype::kFrenzy) {
        data.tags |= ItemTag::Frenzy;
      }

      // Fear/Demoralize (v0.8: NEW - crowd control poison)
      // Note: kDemoralize is the archetype name in CommonLibSSE for fear effects
      if (arch == RE::EffectSetting::Archetype::kDemoralize) {
        data.tags |= ItemTag::Fear;
      }

      // Slow (check for slow archetype or movement speed reduction)
      if (arch == RE::EffectSetting::Archetype::kSlowTime) {
        data.tags |= ItemTag::Slow;
      }

      // Value modifiers (restore/damage/fortify)
      if (arch == RE::EffectSetting::Archetype::kValueModifier ||
          arch == RE::EffectSetting::Archetype::kPeakValueModifier) {
        if (!isHostile) {
           // Restore unless the effect's keyword says fortify -- see
           // ClassifyVitalEffect for why the archetype cannot decide this.
           const bool fortifies =
               ClassifyVitalEffect(effect->baseEffect, primaryAV) == VitalEffect::Fortify;
           switch (primaryAV) {
           case RE::ActorValue::kHealth:
            data.tags |= fortifies ? ItemTag::FortifyHealth : ItemTag::RestoreHealth;
            break;
           case RE::ActorValue::kMagicka:
            data.tags |= fortifies ? ItemTag::FortifyMagicka : ItemTag::RestoreMagicka;
            break;
           case RE::ActorValue::kStamina:
            data.tags |= fortifies ? ItemTag::FortifyStamina : ItemTag::RestoreStamina;
            break;
           case RE::ActorValue::kHealRate:
           case RE::ActorValue::kHealRateMult:
            data.tags |= ItemTag::RegenHealth;
            break;
           case RE::ActorValue::kMagickaRate:
           case RE::ActorValue::kMagickaRateMult:
            data.tags |= ItemTag::RegenMagicka;
            break;
           // v0.8: NEW - stamina regen detection
           case RE::ActorValue::kStaminaRate:
           case RE::ActorValue::kStaminaRateMult:
            data.tags |= ItemTag::RegenStamina;
            break;
           default:
            // A resist potion that modifies the resistance itself (Apothecary:
            // Peak modifier, primaryAV = kResistFrost). The resistVariable
            // check below never sees these, so they were classified by name
            // alone -- and "Potion of Resist Cold" matched no name, so it
            // came out Unknown.
            if (TagResist(primaryAV, data)) {
              break;
            }
            // v0.8 FIX: Fortify skill potions use kValueModifier, not just kDualValueModifier
            // Call DetermineFortifySkillType to handle Fortify Alteration, Fortify Marksman, etc.
            DetermineFortifySkillType(primaryAV, data);
            break;
           }
        } else {
           // Damage effects (hostile)
           switch (primaryAV) {
           case RE::ActorValue::kHealth:
            data.tags |= ItemTag::DamageHealth;
            break;
           case RE::ActorValue::kMagicka:
            data.tags |= ItemTag::DamageMagicka;
            break;
           case RE::ActorValue::kStamina:
            data.tags |= ItemTag::DamageStamina;
            break;
           case RE::ActorValue::kSpeedMult:
            data.tags |= ItemTag::Slow;
            break;
           default:
            break;
           }

           // v0.8: Weakness effects detection (hostile + resistVariable set)
           // This detects "Weakness to Fire" style poisons
           if (resistAV != RE::ActorValue::kNone) {
            data.tagsExt |= ItemTagExt::WeaknessElement;
            switch (resistAV) {
            case RE::ActorValue::kResistFire:
              data.element = ElementType::Fire;
              break;
            case RE::ActorValue::kResistFrost:
              data.element = ElementType::Frost;
              break;
            case RE::ActorValue::kResistShock:
              data.element = ElementType::Shock;
              break;
            case RE::ActorValue::kResistMagic:
              data.element = ElementType::Magic;
              break;
            case RE::ActorValue::kPoisonResist:
              data.element = ElementType::Poison;
              break;
            default:
              break;
            }
           }
        }

        // Resistance effects (from resistVariable) - non-hostile only
        if (!isHostile) {
           TagResist(resistAV, data);
        }
      }

      // Dual value modifiers (fortify + secondary effect)
      if (arch == RE::EffectSetting::Archetype::kDualValueModifier && !isHostile) {
        // Check for fortify vitals
        switch (primaryAV) {
        case RE::ActorValue::kHealth:
           data.tags |= ItemTag::FortifyHealth;
           break;
        case RE::ActorValue::kMagicka:
           data.tags |= ItemTag::FortifyMagicka;
           break;
        case RE::ActorValue::kStamina:
           data.tags |= ItemTag::FortifyStamina;
           break;
        default:
           // v0.8: Use grouped skill fortification with specific skill tracking
           DetermineFortifySkillType(primaryAV, data);
           break;
        }

        // Check secondary AV for regen
        switch (secondaryAV) {
        case RE::ActorValue::kHealRate:
        case RE::ActorValue::kHealRateMult:
           data.tags |= ItemTag::RegenHealth;
           break;
        case RE::ActorValue::kMagickaRate:
        case RE::ActorValue::kMagickaRateMult:
           data.tags |= ItemTag::RegenMagicka;
           break;
        // v0.8: NEW - stamina regen from secondary AV
        case RE::ActorValue::kStaminaRate:
        case RE::ActorValue::kStaminaRateMult:
           data.tags |= ItemTag::RegenStamina;
           break;
        default:
           break;
        }
      }

      // Waterbreathing (detected via actor value, not archetype)
      if (primaryAV == RE::ActorValue::kWaterBreathing) {
        data.tags |= ItemTag::Waterbreathing;
      }
      }

      // =============================================================================
      // KEYWORD-BASED TAGS (CC Survival Mode)
      // Direct keyword scans — allocation-free, early-exit per check
      // (items carry 1-5 keywords, so scanning beats building a hash set)
      // NOTE (v0.8): Soul gem tags moved to ItemTagExt, handled in ClassifyItem
      // =============================================================================

      auto* keywordForm = item->As<RE::BGSKeywordForm>();

      // Survival mode food keywords
      if (HasKeyword(keywordForm, "Survival_FoodRestoreHunger") ||
          HasKeyword(keywordForm, "Survival_FoodRestoreHungerSmall") ||
          HasKeyword(keywordForm, "Survival_FoodRestoreHungerMedium") ||
          HasKeyword(keywordForm, "Survival_FoodRestoreHungerLarge")) {
      data.tags |= ItemTag::SatisfiesHunger;
      }

      if (HasKeyword(keywordForm, "Survival_FoodWarm") ||
          HasKeyword(keywordForm, "Survival_FoodWarmSmall") ||
          HasKeyword(keywordForm, "Survival_FoodWarmMedium") ||
          HasKeyword(keywordForm, "Survival_FoodWarmLarge")) {
      data.tags |= ItemTag::SatisfiesCold;
      }

      // =============================================================================
      // NAME-BASED TAGS (Fallback for edge cases)
      // =============================================================================

      // v0.8: Lingering poison detection → ItemTagExt
      if (NameContains(name, "lingering")) {
      data.tagsExt |= ItemTagExt::Lingering;
      }

      // Resist potions (name fallback)
      if (NameContains(name, "resist fire") || NameContains(name, "fire resistance")) {
      data.tags |= ItemTag::ResistFire;
      data.element = ElementType::Fire;
      }
      if (NameContains(name, "resist frost") || NameContains(name, "frost resistance")) {
      data.tags |= ItemTag::ResistFrost;
      data.element = ElementType::Frost;
      }
      if (NameContains(name, "resist shock") || NameContains(name, "shock resistance")) {
      data.tags |= ItemTag::ResistShock;
      data.element = ElementType::Shock;
      }
      if (NameContains(name, "resist magic") || NameContains(name, "magic resistance")) {
      data.tags |= ItemTag::ResistMagic;
      data.element = ElementType::Magic;
      }
      if (NameContains(name, "resist poison") || NameContains(name, "poison resistance")) {
      data.tags |= ItemTag::ResistPoison;
      data.element = ElementType::Poison;
      }

      // Cure potions (name fallback)
      if (NameContains(name, "cure disease")) {
      data.tags |= ItemTag::CureDisease;
      }
      if (NameContains(name, "cure poison")) {
      data.tags |= ItemTag::CurePoison;
      }

      // Fortify potions (name fallback)
      if (NameContains(name, "fortify health") && !NameContains(name, "damage")) {
      data.tags |= ItemTag::FortifyHealth;
      }
      if (NameContains(name, "fortify magicka") && !NameContains(name, "damage")) {
      data.tags |= ItemTag::FortifyMagicka;
      }
      if (NameContains(name, "fortify stamina") && !NameContains(name, "damage")) {
      data.tags |= ItemTag::FortifyStamina;
      }

      // Invisibility and waterbreathing (name fallback)
      if (NameContains(name, "invisibility")) {
      data.tags |= ItemTag::Invisibility;
      }
      if (NameContains(name, "waterbreathing") || NameContains(name, "water breathing")) {
      data.tags |= ItemTag::Waterbreathing;
      }
   }

   RE::Effect* ItemClassifier::GetCostliestEffect(const RE::AlchemyItem* item) noexcept
   {
      if (!item || item->effects.empty()) return nullptr;

      RE::Effect* costliestEffect = nullptr;
      float highestCost = 0.0f;  // Costs are non-negative

      // Duration scaling factor for effect cost calculation
      // Longer duration effects are more valuable (10s baseline)
      constexpr float DURATION_BASELINE_SECONDS = 10.0f;

      for (auto* effect : item->effects) {
      if (!effect || !effect->baseEffect) continue;

      // Calculate effect cost: baseCost * magnitude * duration factor
      float baseCost = effect->baseEffect->data.baseCost;
      float magnitude = effect->effectItem.magnitude;
      float duration = static_cast<float>(effect->effectItem.duration);

      // Duration factor: 1 for instant, scales with duration otherwise
      float durationFactor = (duration > 0.0f) ? (duration / DURATION_BASELINE_SECONDS) : 1.0f;
      float cost = baseCost * std::max(1.0f, magnitude) * durationFactor;

      if (cost > highestCost) {
        highestCost = cost;
        costliestEffect = effect;
      }
      }

      return costliestEffect;  // nullptr if no valid effects found
   }

   float ItemClassifier::GetPrimaryMagnitude(const RE::AlchemyItem* item) noexcept
   {
      auto* effect = GetCostliestEffect(item);
      return effect ? effect->effectItem.magnitude : 0.0f;
   }

   float ItemClassifier::GetPrimaryDuration(const RE::AlchemyItem* item) noexcept
   {
      auto* effect = GetCostliestEffect(item);
      return effect ? static_cast<float>(effect->effectItem.duration) : 0.0f;
   }

   bool ItemClassifier::HasArchetype(const RE::AlchemyItem* item, RE::EffectSetting::Archetype archetype) noexcept
   {
      if (!item) return false;

      return std::any_of(item->effects.begin(), item->effects.end(),
      [archetype](const auto* effect) {
        return effect && effect->baseEffect &&
               effect->baseEffect->GetArchetype() == archetype;
      });
   }

   bool ItemClassifier::AffectsActorValue(const RE::AlchemyItem* item, RE::ActorValue av) noexcept
   {
      if (!item) return false;

      return std::any_of(item->effects.begin(), item->effects.end(),
      [av](const auto* effect) {
        return effect && effect->baseEffect &&
               (effect->baseEffect->data.primaryAV == av ||
                effect->baseEffect->data.secondaryAV == av);
      });
   }

   bool ItemClassifier::HasResistEffect(const RE::AlchemyItem* item, RE::ActorValue resistAV) noexcept
   {
      if (!item) return false;

      return std::any_of(item->effects.begin(), item->effects.end(),
      [resistAV](const auto* effect) {
        return effect && effect->baseEffect &&
               effect->baseEffect->data.resistVariable == resistAV;
      });
   }

   bool ItemClassifier::NameContains(std::string_view name, std::string_view keyword) noexcept
   {
      // Case-insensitive search using std::search - no heap allocations
      auto caseInsensitiveEqual = [](char a, char b) noexcept {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
      };

      return std::search(name.begin(), name.end(),
                         keyword.begin(), keyword.end(),
                         caseInsensitiveEqual) != name.end();
   }

   bool ItemClassifier::HasKeyword(const RE::AlchemyItem* item, std::string_view keywordEditorID) const noexcept
   {
      if (!item) return false;
      return HasKeyword(item->As<RE::BGSKeywordForm>(), keywordEditorID);
   }

   // Restore or fortify? The archetype cannot say: vanilla fortifies with a
   // Peak modifier, but LoreRim's Restore Health is a Peak modifier too --
   // arch=34, primaryAV=24 for both it and Fortify Health (2026-09-25) -- so
   // reading Peak as "fortify" turned every LoreRim heal into a buff and left
   // the health override with nothing to offer. The vanilla alchemy keywords
   // name the intent directly; when they are absent, say so and let the
   // caller keep its old behaviour rather than guess.
   ItemClassifier::VitalEffect ItemClassifier::ClassifyVitalEffect(
      const RE::EffectSetting* mgef, RE::ActorValue av) noexcept
   {
      if (!mgef) return VitalEffect::Unknown;
      std::string_view restoreKw;
      std::string_view fortifyKw;
      switch (av) {
      case RE::ActorValue::kHealth:
      restoreKw = "MagicAlchRestoreHealth"; fortifyKw = "MagicAlchFortifyHealth"; break;
      case RE::ActorValue::kMagicka:
      restoreKw = "MagicAlchRestoreMagicka"; fortifyKw = "MagicAlchFortifyMagicka"; break;
      case RE::ActorValue::kStamina:
      restoreKw = "MagicAlchRestoreStamina"; fortifyKw = "MagicAlchFortifyStamina"; break;
      default:
      return VitalEffect::Unknown;
      }
      if (HasKeyword(mgef, restoreKw)) return VitalEffect::Restore;
      if (HasKeyword(mgef, fortifyKw)) return VitalEffect::Fortify;
      return VitalEffect::Unknown;
   }

   bool ItemClassifier::HasKeyword(
      const RE::BGSKeywordForm* keywordForm, std::string_view keywordEditorID) noexcept
   {
      if (!keywordForm) return false;

      for (uint32_t i = 0; i < keywordForm->GetNumKeywords(); ++i) {
      auto keywordOpt = keywordForm->GetKeywordAt(i);
      if (keywordOpt.has_value()) {
        auto* keyword = keywordOpt.value();
        if (keyword && keyword->GetFormEditorID() == keywordEditorID) {
           return true;
        }
      }
      }

      return false;
   }

   bool ItemClassifier::IsAlcohol(const RE::AlchemyItem* item, std::string_view name) noexcept
   {
      // TIER 1: Keyword-based detection (mod support - CACO, etc.)
      auto* keywordForm = item->As<RE::BGSKeywordForm>();
      if (HasKeyword(keywordForm, "VendorItemAlcohol") ||
          HasKeyword(keywordForm, "CACO_IsAlcohol") ||
          HasKeyword(keywordForm, "VendorItemSkooma")) {
         return true;
      }

      // TIER 2: Name-based fallback for vanilla and untagged items
      // Full drink names (match anywhere in name, case-insensitive)
      if (NameContains(name, "alto wine") ||
          NameContains(name, "argonian ale") ||
          NameContains(name, "black-briar mead") ||
          NameContains(name, "honningbrew mead") ||
          NameContains(name, "nord mead") ||
          NameContains(name, "spiced wine") ||
          NameContains(name, "firebrand wine") ||
          NameContains(name, "colovian brandy") ||
          NameContains(name, "cyrodilic brandy") ||
          NameContains(name, "skooma") ||
          NameContains(name, "mazte") ||
          NameContains(name, "flin") ||
          NameContains(name, "shein") ||
          NameContains(name, "jagga") ||
          NameContains(name, "rotmeth")) {
         return true;
      }

      // Generic terms — word-boundary check to avoid false positives
      // (e.g. " ale" in "Scale Armor", "wine" in "Wineberry")
      auto endsWithWordCI = [](std::string_view str, std::string_view suffix) noexcept {
         if (str.size() < suffix.size()) return false;
         size_t start = str.size() - suffix.size();
         // Suffix must be preceded by a space (or be the entire string)
         if (start > 0 && str[start - 1] != ' ') return false;
         for (size_t i = 0; i < suffix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(str[start + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i])))
               return false;
         }
         return true;
      };

      auto startsWithWordCI = [](std::string_view str, std::string_view prefix) noexcept {
         if (str.size() < prefix.size()) return false;
         // Prefix must be followed by a space (or be the entire string)
         if (str.size() > prefix.size() && str[prefix.size()] != ' ') return false;
         for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(str[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i])))
               return false;
         }
         return true;
      };

      constexpr std::string_view genericTerms[] = { "ale", "mead", "wine", "beer", "brandy" };
      for (auto term : genericTerms) {
         if (endsWithWordCI(name, term) || startsWithWordCI(name, term)) {
            return true;
         }
      }

      return false;
   }

   bool ItemClassifier::HasSoulGemKeyword(const RE::AlchemyItem* item) noexcept
   {
      if (!item) return false;

      auto* keywordForm = item->As<RE::BGSKeywordForm>();

      // Check against known soul gem keywords (direct scans, allocation-free)
      return HasKeyword(keywordForm, "SoulGemPetty") ||
             HasKeyword(keywordForm, "SoulGemLesser") ||
             HasKeyword(keywordForm, "SoulGemCommon") ||
             HasKeyword(keywordForm, "SoulGemGreater") ||
             HasKeyword(keywordForm, "SoulGemGrand") ||
             HasKeyword(keywordForm, "SoulGemBlack");
   }
}

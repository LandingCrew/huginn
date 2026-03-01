#include "ItemClassifier.h"

namespace Huginn::Item
{
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
      // For soul gems, magnitude encodes capacity level (1.0-6.0)
      // OPTIMIZATION (v0.7.20 H2): Build keyword set once, then O(1) lookups
      if (data.type == ItemType::SoulGem) {
      auto* keywordForm = item->As<RE::BGSKeywordForm>();
      auto keywords = BuildKeywordSet(keywordForm);
      if      (keywords.contains("SoulGemBlack"))   { data.magnitude = 6.0f; data.tagsExt |= ItemTagExt::SoulGemBlack; }
      else if (keywords.contains("SoulGemGrand"))   { data.magnitude = 5.0f; data.tagsExt |= ItemTagExt::SoulGemGrand; }
      else if (keywords.contains("SoulGemGreater")) { data.magnitude = 4.0f; data.tagsExt |= ItemTagExt::SoulGemGreater; }
      else if (keywords.contains("SoulGemCommon"))  { data.magnitude = 3.0f; data.tagsExt |= ItemTagExt::SoulGemCommon; }
      else if (keywords.contains("SoulGemLesser"))  { data.magnitude = 2.0f; data.tagsExt |= ItemTagExt::SoulGemLesser; }
      else if (keywords.contains("SoulGemPetty"))   { data.magnitude = 1.0f; data.tagsExt |= ItemTagExt::SoulGemPetty; }
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

      // Encode capacity level as magnitude (1.0-6.0)
      // GetMaximumCapacity() returns 1-6 directly
      auto capacity = soulGem->GetMaximumCapacity();
      data.magnitude = static_cast<float>(capacity);

      logger::trace("Classified soul gem: {} (capacity={:.0f})"sv, data.name, data.magnitude);

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

      if (!isHostile) {
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

        // Check for resistance effects using resistVariable
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

      // Restore vitals
      if (HasTag(tags, ItemTag::RestoreHealth)) return ItemType::HealthPotion;
      if (HasTag(tags, ItemTag::RestoreMagicka)) return ItemType::MagickaPotion;
      if (HasTag(tags, ItemTag::RestoreStamina)) return ItemType::StaminaPotion;

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

      switch (av) {
      // Magic Schools
      case RE::ActorValue::kAlteration:
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Alteration;
      logger::debug("[DetermineFortifySkillType] {} -> FortifyMagicSchool, school=Alteration"sv, data.name);
      break;
      case RE::ActorValue::kConjuration:
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Conjuration;
      break;
      case RE::ActorValue::kDestruction:
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Destruction;
      break;
      case RE::ActorValue::kIllusion:
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Illusion;
      break;
      case RE::ActorValue::kRestoration:
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Restoration;
      break;
      case RE::ActorValue::kEnchanting:
      data.tags |= ItemTag::FortifyMagicSchool;
      data.school = MagicSchool::Enchanting;
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
      case RE::ActorValue::kSmithing:
      case RE::ActorValue::kSmithingPowerModifier:    // LORERIM (141)
      data.tags |= ItemTag::FortifyCombatSkill;
      data.combatSkill = CombatSkill::Smithing;
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
      case RE::ActorValue::kAlchemy:
      case RE::ActorValue::kAlchemyPowerModifier:     // LORERIM (148)
      data.tags |= ItemTag::FortifyUtilitySkill;
      data.utilitySkill = UtilitySkill::Alchemy;
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
      logger::debug("[PopulateItemTags] {} - Effect: arch={}, primaryAV={}, secondaryAV={}, hostile={}"sv,
        name,
        static_cast<int>(arch),
        static_cast<int>(primaryAV),
        static_cast<int>(secondaryAV),
        isHostile);

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
           // Restore/Fortify effects
           switch (primaryAV) {
           case RE::ActorValue::kHealth:
            data.tags |= ItemTag::RestoreHealth;
            break;
           case RE::ActorValue::kMagicka:
            data.tags |= ItemTag::RestoreMagicka;
            break;
           case RE::ActorValue::kStamina:
            data.tags |= ItemTag::RestoreStamina;
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
           switch (resistAV) {
           case RE::ActorValue::kResistFire:
            data.tags |= ItemTag::ResistFire;
            data.element = ElementType::Fire;
            break;
           case RE::ActorValue::kResistFrost:
            data.tags |= ItemTag::ResistFrost;
            data.element = ElementType::Frost;
            break;
           case RE::ActorValue::kResistShock:
            data.tags |= ItemTag::ResistShock;
            data.element = ElementType::Shock;
            break;
           case RE::ActorValue::kPoisonResist:
            data.tags |= ItemTag::ResistPoison;
            data.element = ElementType::Poison;
            break;
           case RE::ActorValue::kResistMagic:
            data.tags |= ItemTag::ResistMagic;
            data.element = ElementType::Magic;
            break;
           case RE::ActorValue::kResistDisease:
            data.tags |= ItemTag::ResistDisease;
            data.element = ElementType::Disease;
            break;
           default:
            break;
           }
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
      // OPTIMIZATION (v0.7.20 H2): Build keyword set once, then O(1) lookups
      // Reduces O(14n) keyword iterations to O(n + 14) per item
      // NOTE (v0.8): Soul gem tags moved to ItemTagExt, handled in ClassifyItem
      // =============================================================================

      auto* keywordForm = item->As<RE::BGSKeywordForm>();
      auto keywords = BuildKeywordSet(keywordForm);

      // Survival mode food keywords
      if (keywords.contains("Survival_FoodRestoreHunger") ||
          keywords.contains("Survival_FoodRestoreHungerSmall") ||
          keywords.contains("Survival_FoodRestoreHungerMedium") ||
          keywords.contains("Survival_FoodRestoreHungerLarge")) {
      data.tags |= ItemTag::SatisfiesHunger;
      }

      if (keywords.contains("Survival_FoodWarm") ||
          keywords.contains("Survival_FoodWarmSmall") ||
          keywords.contains("Survival_FoodWarmMedium") ||
          keywords.contains("Survival_FoodWarmLarge")) {
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

      // Check if item has a specific keyword
      auto* keywordForm = item->As<RE::BGSKeywordForm>();
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

   // OPTIMIZATION (v0.7.20 H2): Build keyword set once for O(1) lookups
   // Reduces O(n*k) to O(n + k) where n = keywords on form, k = keywords to check
   //
   // LIFETIME SAFETY: The string_view entries point to EditorID strings stored in
   // Skyrim's static keyword forms. These persist for the game session lifetime,
   // making the string_views safe to use within this function's scope.
   std::unordered_set<std::string_view> ItemClassifier::BuildKeywordSet(
      const RE::BGSKeywordForm* keywordForm) noexcept
   {
      std::unordered_set<std::string_view> keywords;
      if (!keywordForm) return keywords;

      keywords.reserve(keywordForm->GetNumKeywords());
      for (uint32_t i = 0; i < keywordForm->GetNumKeywords(); ++i) {
      auto keywordOpt = keywordForm->GetKeywordAt(i);
      if (keywordOpt.has_value() && keywordOpt.value()) {
        keywords.insert(keywordOpt.value()->GetFormEditorID());
      }
      }
      return keywords;
   }

   bool ItemClassifier::IsAlcohol(const RE::AlchemyItem* item, std::string_view name) const noexcept
   {
      // TIER 1: Keyword-based detection (mod support - CACO, etc.)
      auto* keywordForm = item->As<RE::BGSKeywordForm>();
      if (keywordForm) {
      auto keywords = BuildKeywordSet(keywordForm);
      if (keywords.contains("VendorItemAlcohol") ||
          keywords.contains("CACO_IsAlcohol") ||
          keywords.contains("VendorItemSkooma")) {
        return true;
      }
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

      // Generic terms — match as whole words to avoid false positives
      // (e.g. "ale" in "stale bread" handled by checking known full names first)
      if (NameContains(name, " ale") ||
          NameContains(name, " mead") ||
          NameContains(name, " wine") ||
          NameContains(name, " beer") ||
          NameContains(name, " brandy")) {
      return true;
      }

      // Check if name starts with the generic term
      auto startsWithCI = [](std::string_view str, std::string_view prefix) noexcept {
      if (str.size() < prefix.size()) return false;
      for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
           return false;
      }
      return true;
      };

      if (startsWithCI(name, "ale") ||
          startsWithCI(name, "mead") ||
          startsWithCI(name, "wine") ||
          startsWithCI(name, "beer") ||
          startsWithCI(name, "brandy")) {
      return true;
      }

      return false;
   }

   bool ItemClassifier::HasSoulGemKeyword(const RE::AlchemyItem* item) noexcept
   {
      if (!item) return false;

      auto* keywordForm = item->As<RE::BGSKeywordForm>();
      if (!keywordForm) return false;

      // OPTIMIZATION (v0.7.20 H2): Build keyword set once, check against known soul gem keywords
      auto keywords = BuildKeywordSet(keywordForm);

      // Check against known soul gem keywords
      return keywords.contains("SoulGemPetty") ||
             keywords.contains("SoulGemLesser") ||
             keywords.contains("SoulGemCommon") ||
             keywords.contains("SoulGemGreater") ||
             keywords.contains("SoulGemGrand") ||
             keywords.contains("SoulGemBlack");
   }
}

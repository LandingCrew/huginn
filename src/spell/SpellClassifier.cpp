#include "SpellClassifier.h"

namespace Huginn::Spell
{
   void SpellClassifier::LoadOverrides(const std::filesystem::path& iniPath)
   {
      m_overrides.LoadFromFile(iniPath);
   }

   SpellData SpellClassifier::ClassifySpell(RE::SpellItem* spell) const
   {
      if (!spell) {
      logger::error("ClassifySpell called with null spell"sv);
      return SpellData{};
      }

      SpellData data{};
      data.formID = spell->GetFormID();
      data.name = spell->GetName();

      // Check for overrides first (by FormID, then by name)
      std::optional<SpellOverride> override;

      if (m_overrides.HasOverride(data.formID)) {
      override = m_overrides.GetOverride(data.formID);
      logger::debug("Using FormID override for: {} ({:08X})"sv, data.name, data.formID);
      } else if (m_overrides.HasOverride(data.name)) {
      override = m_overrides.GetOverride(data.name);
      logger::debug("Using name override for: {}"sv, data.name);
      }

      // OPTIMIZATION (v0.7.19): Compute costliest effect ONCE for all helpers
      // This reduces O(4n) → O(n) effect iterations per spell at load time
      RE::Effect* costliestEffect = GetCostliestEffect(spell);
      RE::EffectSetting* primaryEffect = costliestEffect ? costliestEffect->baseEffect : nullptr;

      // STEP 1: Compute tags FIRST (single source of name-matching)
      data.tags = override ? override->tags.value_or(DetermineSpellTags(spell))
                           : DetermineSpellTags(spell);

      // STEP 2: Determine type - API first, then derive from tags
      if (override && override->type) {
      data.type = *override->type;
      } else {
      data.type = DetermineSpellType(spell, primaryEffect);  // API-based
      if (data.type == SpellType::Unknown) {
        data.type = DeriveSpellTypeFromTags(data.tags);  // Tag-based fallback
      }
      }

      // STEP 3: School - API only (no name fallback needed)
      data.school = DetermineMagicSchool(costliestEffect);

      // STEP 4: Element - API first, then derive from tags
      data.element = DetermineElementType(costliestEffect);  // API-based (resistVariable)
      if (data.element == ElementType::None) {
      data.element = DeriveElementFromTags(data.tags);  // Tag-based fallback
      }
      // Sun damage overrides Magic (Dawnguard spells use kResistMagic but are semantically Sun)
      if (data.element == ElementType::Magic && HasTag(data.tags, SpellTag::Sun)) {
      data.element = ElementType::Sun;
      }
      // Utility spells (Open Lock, Telekinesis) shouldn't have elemental damage
      // Some modded spells incorrectly use elemental resistVariable
      if (data.type == SpellType::Utility && data.element != ElementType::None) {
      data.element = ElementType::None;
      }

      data.baseCost = GetBaseCost(spell);
      data.isConcentration = IsConcentration(spell);
      data.range = GetEffectiveRange(spell);

      return data;
   }

   SpellType SpellClassifier::DetermineSpellType(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const
   {
      // ONLY API-based checks here
      // Name-based fallback handled by DeriveSpellTypeFromTags() in ClassifySpell()
      // OPTIMIZATION (v0.7.19): primaryEffect is pre-computed by caller

      if (!spell) return SpellType::Unknown;
      if (!primaryEffect) return SpellType::Unknown;

      const auto archetype = primaryEffect->GetArchetype();
      const bool isHostile = primaryEffect->data.flags.any(
      RE::EffectSetting::EffectSettingData::Flag::kHostile);
      // Derive school from primaryEffect directly to avoid redundant GetCostliestEffect call
      const auto school = [&]() -> MagicSchool {
      auto skillAV = primaryEffect->GetMagickSkill();
      switch (skillAV) {
      case RE::ActorValue::kDestruction:  return MagicSchool::Destruction;
      case RE::ActorValue::kRestoration:  return MagicSchool::Restoration;
      case RE::ActorValue::kAlteration:   return MagicSchool::Alteration;
      case RE::ActorValue::kIllusion:     return MagicSchool::Illusion;
      case RE::ActorValue::kConjuration:  return MagicSchool::Conjuration;
      default:                            return MagicSchool::Unknown;
      }
      }();

      // Healing: non-hostile health restoration
      if (!isHostile && primaryEffect->data.primaryAV == RE::ActorValue::kHealth) {
      if (archetype == RE::EffectSetting::Archetype::kValueModifier ||
          archetype == RE::EffectSetting::Archetype::kPeakValueModifier) {
        return SpellType::Healing;
      }
      }

      // Summon: SummonCreature archetype
      if (archetype == RE::EffectSetting::Archetype::kSummonCreature) {
      return SpellType::Summon;
      }

      // Damage: Hostile Destruction spells
      if (isHostile && school == MagicSchool::Destruction) {
      return SpellType::Damage;
      }

      // Defensive: Alteration armor spells (DamageResist)
      if (school == MagicSchool::Alteration && !isHostile) {
      if (primaryEffect->data.primaryAV == RE::ActorValue::kDamageResist) {
        return SpellType::Defensive;
      }
      }

      // Debuff: Hostile Illusion spells
      if (isHostile && school == MagicSchool::Illusion) {
      return SpellType::Debuff;
      }

      // Buff: Non-hostile self-targeted spells with buff archetypes
      if (!isHostile && spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
      if (archetype == RE::EffectSetting::Archetype::kInvisibility ||
          archetype == RE::EffectSetting::Archetype::kCloak) {
        return SpellType::Buff;
      }
      }

      // Utility: Light archetype
      if (archetype == RE::EffectSetting::Archetype::kLight) {
      return SpellType::Utility;
      }

      return SpellType::Unknown;  // Will fall back to tag-based in ClassifySpell
   }

   SpellType SpellClassifier::DeriveSpellTypeFromTags(SpellTag tags) noexcept
   {
      // Priority order matters - check most specific first

      // Healing
      if (HasTag(tags, SpellTag::RestoreHealth)) return SpellType::Healing;

      // Summon (bound weapons + conjuration summons)
      if (HasTag(tags, SpellTag::BoundWeapon) ||
          HasTag(tags, SpellTag::SummonDaedra) ||
          HasTag(tags, SpellTag::SummonUndead) ||
          HasTag(tags, SpellTag::SummonCreature)) {
      return SpellType::Summon;
      }

      // Defensive (wards, armor)
      if (HasTag(tags, SpellTag::Ward) || HasTag(tags, SpellTag::Armor)) {
      return SpellType::Defensive;
      }

      // Debuff (paralysis, calm, fear, frenzy, turn undead, banish)
      if (HasTag(tags, SpellTag::Paralysis) || HasTag(tags, SpellTag::Calm) ||
          HasTag(tags, SpellTag::Fear) || HasTag(tags, SpellTag::Frenzy) ||
          HasTag(tags, SpellTag::TurnUndead) || HasTag(tags, SpellTag::AntiDaedra)) {
      return SpellType::Debuff;
      }

      // Buff (stealth, invisibility, muffle, waterbreathing)
      if (HasTag(tags, SpellTag::Invisibility) || HasTag(tags, SpellTag::Muffle) ||
          HasTag(tags, SpellTag::Stealth)) {
      return SpellType::Buff;
      }

      // Utility (detect, light, telekinesis)
      if (HasTag(tags, SpellTag::DetectLife) || HasTag(tags, SpellTag::Light) ||
          HasTag(tags, SpellTag::Telekinesis)) {
      return SpellType::Utility;
      }

      // Damage (elements - check last as it's broad)
      if (HasTag(tags, SpellTag::Fire) || HasTag(tags, SpellTag::Frost) ||
          HasTag(tags, SpellTag::Shock) || HasTag(tags, SpellTag::Poison) ||
          HasTag(tags, SpellTag::Sun)) {
      return SpellType::Damage;
      }

      return SpellType::Unknown;
   }

   ElementType SpellClassifier::DeriveElementFromTags(SpellTag tags) noexcept
   {
      // Check in priority order (Sun first as it's most specific)
      if (HasTag(tags, SpellTag::Sun))    return ElementType::Sun;
      if (HasTag(tags, SpellTag::Fire))   return ElementType::Fire;
      if (HasTag(tags, SpellTag::Frost))  return ElementType::Frost;
      if (HasTag(tags, SpellTag::Shock))  return ElementType::Shock;
      if (HasTag(tags, SpellTag::Poison)) return ElementType::Poison;

      return ElementType::None;
   }

   SpellTag SpellClassifier::DetermineSpellTags(RE::SpellItem* spell) const
   {
      if (!spell) return SpellTag::None;

      SpellTag tags = SpellTag::None;
      const std::string_view name = spell->GetName();

      // M6 (v0.7.21): OPTIMIZATION - Lowercase name ONCE, then use case-sensitive find()
      // This reduces ~50+ tolower() calls per character to just one pass over the string.
      // Original: O(name_len * keyword_len) per keyword × ~50 keywords
      // Optimized: O(name_len) once + O(name_len) per keyword with fast find()
      std::string lowerName;
      lowerName.reserve(name.size());
      for (char c : name) {
      lowerName.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }

      // Fast case-sensitive search on pre-lowercased name
      auto contains = [&lowerName](std::string_view keyword) {
      return lowerName.find(keyword) != std::string::npos;
      };

      // Damage type tags
      if (contains("fire") || contains("flame") ||
          contains("incinerate") || contains("burn")) {
      tags |= SpellTag::Fire;
      }
      if (contains("frost") || contains("ice") ||
          contains("freeze") || contains("blizzard")) {
      tags |= SpellTag::Frost;
      }
      if (contains("shock") || contains("lightning") ||
          contains("thunder") || contains("spark")) {
      tags |= SpellTag::Shock;
      }
      if (contains("poison")) {
      tags |= SpellTag::Poison;
      }
      if (contains("sun") || contains("vampire's bane")) {
      tags |= SpellTag::Sun;
      tags |= SpellTag::AntiUndead;
      }

      // Anti-undead tags
      if (contains("turn undead") || contains("circle of protection") ||
          contains("repel undead")) {
      tags |= SpellTag::AntiUndead;
      tags |= SpellTag::TurnUndead;
      }

      // Anti-daedra tags
      if (contains("banish") || contains("expel daedra")) {
      tags |= SpellTag::AntiDaedra;
      }

      // Range/area tags
      if (contains("bolt") || contains("ball") ||
          contains("spear") || contains("blast")) {
      tags |= SpellTag::Ranged;
      }
      if (contains("touch") || contains("grasp")) {
      tags |= SpellTag::Melee;
      }
      if (contains("ball") || contains("cloak") ||
          contains("storm") || contains("rune") ||
          contains("wall") || contains("circle")) {
      tags |= SpellTag::AOE;
      }

      // Concentration tag
      if (IsConcentration(spell)) {
      tags |= SpellTag::Concentration;
      }

      // Restoration specific tags
      if (contains("heal") || contains("cure") ||
          contains("restore health")) {
      tags |= SpellTag::RestoreHealth;
      }
      if (contains("restore magicka")) {
      tags |= SpellTag::RestoreMagicka;
      }
      if (contains("restore stamina")) {
      tags |= SpellTag::RestoreStamina;
      }
      if (contains("ward")) {
      tags |= SpellTag::Ward;
      }

      // Alteration specific tags
      if (contains("armor") || contains("flesh")) {
      tags |= SpellTag::Armor;
      }
      if (contains("detect life")) {
      tags |= SpellTag::DetectLife;
      }
      // Light detection - exclude "lightning" which contains "light"
      if ((contains("light") && !contains("lightning")) ||
          contains("candlelight") || contains("magelight")) {
      tags |= SpellTag::Light;
      }
      if (contains("telekinesis")) {
      tags |= SpellTag::Telekinesis;
      }
      if (contains("open")) {
      tags |= SpellTag::Telekinesis;  // Open Lock spells derive to Utility like Telekinesis
      }
      if (contains("waterbreath")) {
      tags |= SpellTag::Stealth;  // Waterbreathing is an environmental protection buff
      }
      if (contains("paralyze")) {
      tags |= SpellTag::Paralysis;
      }

      // Illusion specific tags
      if (contains("calm") || contains("pacify")) {
      tags |= SpellTag::Calm;
      }
      if (contains("fear") || contains("rout")) {
      tags |= SpellTag::Fear;
      }
      if (contains("frenzy") || contains("fury") ||
          contains("mayhem")) {
      tags |= SpellTag::Frenzy;
      }
      if (contains("invisibility")) {
      tags |= SpellTag::Invisibility;
      tags |= SpellTag::Stealth;
      }
      if (contains("muffle")) {
      tags |= SpellTag::Muffle;
      tags |= SpellTag::Stealth;
      }

      // Conjuration specific tags
      if (contains("summon") || contains("conjure")) {
      tags |= SpellTag::Conjuration;

      // Determine summon type
      if (contains("atronach") || contains("dremora")) {
        tags |= SpellTag::SummonDaedra;
      } else if (contains("zombie") || contains("wraith") ||
                 contains("boneman") || contains("mistman")) {
        tags |= SpellTag::SummonUndead;
      } else {
        tags |= SpellTag::SummonCreature;
      }
      }
      if (contains("bound")) {
      tags |= SpellTag::BoundWeapon;
      tags |= SpellTag::Conjuration;
      }

      return tags;
   }

   uint32_t SpellClassifier::GetBaseCost(RE::SpellItem* spell) const
   {
      if (!spell) return 0;

      // Get the base cost from the spell
      // This is unmodified by perks or enchantments
      return static_cast<uint32_t>(spell->CalculateMagickaCost(nullptr));
   }

   bool SpellClassifier::IsConcentration(RE::SpellItem* spell) const
   {
      if (!spell) return false;

      // Check spell casting type
      return spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
   }

   float SpellClassifier::GetEffectiveRange(RE::SpellItem* spell) const
   {
      if (!spell) return 0.0f;

      // Get delivery type to determine range
      const auto delivery = spell->GetDelivery();

      switch (delivery) {
      case RE::MagicSystem::Delivery::kSelf:
      case RE::MagicSystem::Delivery::kTouch:
      return 0.0f;  // Melee range

      case RE::MagicSystem::Delivery::kAimed:
      case RE::MagicSystem::Delivery::kTargetActor:
      case RE::MagicSystem::Delivery::kTargetLocation:
      {
        // Check for projectile data to get actual range
        // Default to long range for projectile spells
        auto* primaryEffect = GetPrimaryEffect(spell);
        if (primaryEffect && primaryEffect->data.projectileBase) {
           // Use projectile range if available
           return primaryEffect->data.projectileBase->data.range;
        }
        // Default long range for aimed spells
        return 4096.0f;  // ~64 meters
      }

      default:
      return 0.0f;
      }
   }

   RE::EffectSetting* SpellClassifier::GetPrimaryEffect(RE::SpellItem* spell) const
   {
      if (!spell || spell->effects.empty()) return nullptr;

      // Use costliest effect as primary (more reliable than first effect)
      auto* costliestEffect = GetCostliestEffect(spell);
      if (costliestEffect && costliestEffect->baseEffect) {
      return costliestEffect->baseEffect;
      }

      // Fallback to first effect
      auto* effect = spell->effects[0];
      return effect ? effect->baseEffect : nullptr;
   }

   RE::Effect* SpellClassifier::GetCostliestEffect(RE::SpellItem* spell) const
   {
      if (!spell || spell->effects.empty()) return nullptr;

      RE::Effect* costliestEffect = nullptr;
      RE::Effect* firstValidEffect = nullptr;  // L3 (v0.7.21): Track during main loop
      float highestCost = -1.0f;

      for (auto* effect : spell->effects) {
      if (!effect || !effect->baseEffect) continue;

      // L3 (v0.7.21): Track first valid effect during main loop (eliminates fallback loop)
      if (!firstValidEffect) {
        firstValidEffect = effect;
      }

      // Calculate effect cost using Skyrim's actual formula (v0.7.10)
      // Formula: baseCost × (magnitude^1.1) × durationFactor × areaFactor
      float baseCost = effect->baseEffect->data.baseCost;
      float magnitude = effect->effectItem.magnitude;
      float duration = effect->effectItem.duration;
      float area = effect->effectItem.area;

      // Duration factor: 1 for instant, scales with duration otherwise
      float durationFactor = (duration > 0) ? (duration / 10.0f) : 1.0f;

      // Area factor: 1 for single-target, 0.15 × area for AoE spells
      float areaFactor = (area > 0) ? (0.15f * area) : 1.0f;

      // Magnitude uses exponential scaling (1.1 exponent)
      float magnitudeFactor = std::pow(std::max(1.0f, magnitude), 1.1f);

      float cost = baseCost * magnitudeFactor * durationFactor * areaFactor;

      if (cost > highestCost) {
        highestCost = cost;
        costliestEffect = effect;
      }
      }

      // Return costliest if found, otherwise first valid effect (safety fallback)
      return costliestEffect ? costliestEffect : firstValidEffect;
   }

   MagicSchool SpellClassifier::DetermineMagicSchool(RE::Effect* costliestEffect) const
   {
      // OPTIMIZATION (v0.7.19): costliestEffect is pre-computed by caller
      if (!costliestEffect || !costliestEffect->baseEffect) {
      return MagicSchool::Unknown;
      }

      // Use SKSE API: GetMagickSkill() returns the ActorValue for the spell's school
      auto skillAV = costliestEffect->baseEffect->GetMagickSkill();

      switch (skillAV) {
      case RE::ActorValue::kDestruction:  return MagicSchool::Destruction;
      case RE::ActorValue::kRestoration:  return MagicSchool::Restoration;
      case RE::ActorValue::kAlteration:   return MagicSchool::Alteration;
      case RE::ActorValue::kIllusion:     return MagicSchool::Illusion;
      case RE::ActorValue::kConjuration:  return MagicSchool::Conjuration;
      default:                            return MagicSchool::Unknown;
      }
   }

   ElementType SpellClassifier::DetermineElementType(RE::Effect* costliestEffect) const
   {
      // ONLY API-based checks here
      // Name-based fallback handled by DeriveElementFromTags() in ClassifySpell()
      // OPTIMIZATION (v0.7.19): costliestEffect is pre-computed by caller

      if (!costliestEffect || !costliestEffect->baseEffect) {
      return ElementType::None;
      }

      auto resistAV = costliestEffect->baseEffect->data.resistVariable;

      switch (resistAV) {
      case RE::ActorValue::kResistFire:   return ElementType::Fire;
      case RE::ActorValue::kResistFrost:  return ElementType::Frost;
      case RE::ActorValue::kResistShock:  return ElementType::Shock;
      case RE::ActorValue::kPoisonResist: return ElementType::Poison;
      case RE::ActorValue::kResistMagic:  return ElementType::Magic;
      default:                            return ElementType::None;
      }
   }
}

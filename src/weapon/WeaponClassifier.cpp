#include "WeaponClassifier.h"
#include "Config.h"

namespace Huginn::Weapon
{
   WeaponData WeaponClassifier::ClassifyWeapon(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) {
      // E6 (v0.7.21): Standardized to warn - null input is caller error, not system error
      logger::warn("ClassifyWeapon called with null weapon"sv);
      return WeaponData{};
      }

      WeaponData data{};
      data.formID = weapon->GetFormID();
      data.name = weapon->GetName();
      if (data.name.empty()) {
      // Some modded weapons lack a display name — fall back to editor ID
      const char* editorID = weapon->GetFormEditorID();
      if (editorID && editorID[0]) {
        data.name = editorID;
      }
      }

      // Reject weapons with empty names (both display name and editor ID are missing)
      if (data.name.empty()) {
      logger::warn("Rejecting weapon with empty name: FormID={:08X}"sv, data.formID);
      return WeaponData{};
      }

      // STEP 1: Determine weapon type
      data.type = DetermineWeaponType(weapon);

      // STEP 2: Determine all tags (includes enchantment analysis)
      data.tags = DetermineWeaponTags(weapon, data.type);

      // STEP 3: Get combat stats
      data.baseDamage = GetBaseDamage(weapon);
      data.speed = GetSpeed(weapon);
      data.reach = GetReach(weapon);

      // STEP 4: Get enchantment info
      // Staves are inherently enchanted but don't use formEnchanting,
      // so GetEnchantment() returns nullptr for them.
      auto* enchantment = GetEnchantment(weapon);
      data.hasEnchantment = (enchantment != nullptr) ||
                            (data.type == WeaponType::Staff);
      if (data.hasEnchantment) {
      // For now, set charge to 100% - UpdateWeaponCharge will be called with actual values
      data.currentCharge = 1.0f;
      data.maxCharge = 1.0f;
      }

      logger::trace("Classified weapon: {}"sv, data.ToString());
      return data;
   }

   AmmoData WeaponClassifier::ClassifyAmmo(RE::TESAmmo* ammo) const
   {
      if (!ammo) {
      logger::error("ClassifyAmmo called with null ammo"sv);
      return AmmoData{};
      }

      AmmoData data{};
      data.formID = ammo->GetFormID();
      data.name = ammo->GetName();

      // STEP 1: Determine ammo type
      data.type = DetermineAmmoType(ammo);

      // STEP 2: Determine tags (enchantment effects)
      data.tags = DetermineAmmoTags(ammo);

      // STEP 3: Get damage
      data.baseDamage = ammo->GetRuntimeData().data.damage;

      // STEP 4: Check for enchantment
      data.hasEnchantment = HasTag(data.tags, WeaponTag::MagicAmmo);

      logger::trace("Classified ammo: {}"sv, data.ToString());
      return data;
   }

   void WeaponClassifier::UpdateWeaponCharge(WeaponData& data, RE::TESObjectWEAP* weapon) const
   {
      if (!weapon || !data.hasEnchantment) return;

      float chargePercent = GetChargePercentage(weapon);
      data.currentCharge = chargePercent;

      // Update NeedsCharge tag based on charge level
      if (chargePercent < Config::WEAPON_CHARGE_LOW_THRESHOLD) {
      data.tags |= WeaponTag::NeedsCharge;
      } else {
      data.tags &= ~WeaponTag::NeedsCharge;
      }
   }

   // =============================================================================
   // TYPE DETERMINATION
   // =============================================================================

   WeaponType WeaponClassifier::DetermineWeaponType(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return WeaponType::Unknown;

      auto weaponType = weapon->GetWeaponType();

      switch (weaponType) {
      // One-handed melee
      case RE::WEAPON_TYPE::kOneHandSword:  return WeaponType::OneHandSword;
      case RE::WEAPON_TYPE::kOneHandAxe:    return WeaponType::OneHandAxe;
      case RE::WEAPON_TYPE::kOneHandMace:   return WeaponType::OneHandMace;
      case RE::WEAPON_TYPE::kOneHandDagger: return WeaponType::OneHandDagger;

      // Two-handed melee
      case RE::WEAPON_TYPE::kTwoHandSword:  return WeaponType::TwoHandSword;
      case RE::WEAPON_TYPE::kTwoHandAxe:    return WeaponType::TwoHandAxe;

      // Ranged
      case RE::WEAPON_TYPE::kBow:           return WeaponType::Bow;
      case RE::WEAPON_TYPE::kCrossbow:      return WeaponType::Crossbow;

      // Special
      case RE::WEAPON_TYPE::kStaff:         return WeaponType::Staff;

      default:
      logger::warn("Unknown weapon type {:d} for weapon: {}"sv,
        static_cast<int>(weaponType), weapon->GetName());
      return WeaponType::Unknown;
      }
   }

   AmmoType WeaponClassifier::DetermineAmmoType(RE::TESAmmo* ammo) const
   {
      if (!ammo) return AmmoType::Unknown;

      // Check if it's a bolt (crossbow ammo)
      if (ammo->IsBolt()) {
      return AmmoType::Bolt;
      }

      // Default to arrow
      return AmmoType::Arrow;
   }

   // =============================================================================
   // TAG DETERMINATION
   // =============================================================================

   WeaponTag WeaponClassifier::DetermineWeaponTags(RE::TESObjectWEAP* weapon, WeaponType type) const
   {
      if (!weapon) return WeaponTag::None;

      WeaponTag tags = WeaponTag::None;

      // Combat style tags based on type
      switch (type) {
      case WeaponType::OneHandSword:
      case WeaponType::OneHandAxe:
      case WeaponType::OneHandMace:
      case WeaponType::OneHandDagger:
      tags |= WeaponTag::Melee | WeaponTag::OneHanded;
      break;

      case WeaponType::TwoHandSword:
      case WeaponType::TwoHandAxe:
      case WeaponType::TwoHandMace:
      tags |= WeaponTag::Melee | WeaponTag::TwoHanded;
      break;

      case WeaponType::Bow:
      case WeaponType::Crossbow:
      tags |= WeaponTag::Ranged | WeaponTag::TwoHanded;
      break;

      case WeaponType::Staff:
      // Staves are ranged but one-handed
      tags |= WeaponTag::Ranged | WeaponTag::OneHanded;
      break;

      default:
      break;
      }

      // Material/special tags
      if (IsSilvered(weapon)) {
      tags |= WeaponTag::Silver;
      }
      if (IsDaedric(weapon)) {
      tags |= WeaponTag::Daedric;
      }
      if (IsBound(weapon)) {
      tags |= WeaponTag::Bound;
      }

      // Enchantment analysis
      auto* enchantment = GetEnchantment(weapon);
      if (enchantment) {
      tags |= WeaponTag::Enchanted;
      tags |= ClassifyEnchantmentEffects(enchantment);
      }

      return tags;
   }

   WeaponTag WeaponClassifier::DetermineAmmoTags(RE::TESAmmo* ammo) const
   {
      if (!ammo) return WeaponTag::None;

      WeaponTag tags = WeaponTag::None;

      // Check for silver arrows/bolts (v0.7.8)
      // Silver ammo has bonus damage vs undead/werewolves
      auto* keywordForm = ammo->As<RE::BGSKeywordForm>();
      if (keywordForm && HasKeyword(ammo, "WeapMaterialSilver")) {
      tags |= WeaponTag::Silver;
      } else {
      // Fallback: check name for "Silver"
      std::string_view name = ammo->GetName();
      if (NameContains(name, "silver")) {
        tags |= WeaponTag::Silver;
      }
      }

      // Check for enchantment on ammo
      // Ammo inherits from TESEnchantableForm
      auto* enchantable = ammo->As<RE::TESEnchantableForm>();
      if (enchantable && enchantable->formEnchanting) {
      tags |= WeaponTag::MagicAmmo;
      tags |= ClassifyEnchantmentEffects(enchantable->formEnchanting);
      }

      return tags;
   }

   // =============================================================================
   // SPECIAL PROPERTY DETECTION
   // =============================================================================

   bool WeaponClassifier::IsSilvered(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return false;

      // Check for silver keyword first (most reliable)
      if (HasKeyword(weapon, "WeapMaterialSilver")) {
      return true;
      }

      // Fallback: check name for "Silver"
      std::string_view name = weapon->GetName();
      if (NameContains(name, "silver")) {
      return true;
      }

      return false;
   }

   bool WeaponClassifier::IsBound(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return false;

      // Check for bound weapon keyword
      if (HasKeyword(weapon, "WeapTypeBoundWeapon")) {
      return true;
      }

      // Fallback: check name
      std::string_view name = weapon->GetName();
      if (NameContains(name, "bound")) {
      return true;
      }

      return false;
   }

   bool WeaponClassifier::IsDaedric(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return false;

      // Check for daedric material keyword
      if (HasKeyword(weapon, "WeapMaterialDaedric")) {
      return true;
      }

      // Fallback: check name
      std::string_view name = weapon->GetName();
      if (NameContains(name, "daedric")) {
      return true;
      }

      return false;
   }

   // =============================================================================
   // ENCHANTMENT ANALYSIS
   // =============================================================================

   RE::EnchantmentItem* WeaponClassifier::GetEnchantment(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return nullptr;

      // Check if weapon is enchantable and has an enchantment
      auto* enchantable = weapon->As<RE::TESEnchantableForm>();
      if (enchantable && enchantable->formEnchanting) {
      return enchantable->formEnchanting;
      }

      return nullptr;
   }

   WeaponTag WeaponClassifier::ClassifyEnchantmentEffects(RE::EnchantmentItem* enchantment) const
   {
      if (!enchantment) return WeaponTag::None;

      WeaponTag tags = WeaponTag::None;

      // Iterate through enchantment effects
      for (auto* effect : enchantment->effects) {
      if (!effect || !effect->baseEffect) continue;

      auto archetype = effect->baseEffect->GetArchetype();
      auto resistAV = effect->baseEffect->data.resistVariable;
      auto primaryAV = effect->baseEffect->data.primaryAV;

      // Element detection via resistVariable (same pattern as SpellClassifier)
      switch (resistAV) {
      case RE::ActorValue::kResistFire:   tags |= WeaponTag::EnchantFire;   break;
      case RE::ActorValue::kResistFrost:  tags |= WeaponTag::EnchantFrost;  break;
      case RE::ActorValue::kResistShock:  tags |= WeaponTag::EnchantShock;  break;
      default: break;
      }

      // Special effect detection via archetype
      switch (archetype) {
      case RE::EffectSetting::Archetype::kAbsorb:
        // Check what's being absorbed
        if (primaryAV == RE::ActorValue::kHealth) {
           tags |= WeaponTag::EnchantAbsorbHealth;
        } else if (primaryAV == RE::ActorValue::kMagicka) {
           tags |= WeaponTag::EnchantAbsorbMagicka;
        } else if (primaryAV == RE::ActorValue::kStamina) {
           tags |= WeaponTag::EnchantAbsorbStamina;
        }
        break;

      case RE::EffectSetting::Archetype::kSoulTrap:
        tags |= WeaponTag::EnchantSoulTrap;
        break;

      case RE::EffectSetting::Archetype::kParalysis:
        tags |= WeaponTag::EnchantParalyze;
        break;

      case RE::EffectSetting::Archetype::kTurnUndead:
        tags |= WeaponTag::EnchantTurnUndead;
        break;

      case RE::EffectSetting::Archetype::kBanish:
        tags |= WeaponTag::EnchantBanish;
        break;

      case RE::EffectSetting::Archetype::kDemoralize:
      case RE::EffectSetting::Archetype::kFrenzy:
        tags |= WeaponTag::EnchantFear;  // Fear-like effects
        break;


      // Staff-specific spell archetypes (v0.7.7)
      // These are less common on weapons but found on staves
      case RE::EffectSetting::Archetype::kValueModifier:
      case RE::EffectSetting::Archetype::kPeakValueModifier:
        // Healing/Damage effects (for staves of healing, etc.)
        // Check if it's a beneficial effect (for restoration staves)
        if (!effect->baseEffect->IsHostile()) {
           // Restoration staff healing effects
           // Note: These don't get specific tags since WeaponTag doesn't have
           // healing-specific flags, but the effect is still analyzed
           logger::trace("Staff restoration effect detected on enchantment"sv);
        }
        break;

      case RE::EffectSetting::Archetype::kSummonCreature:
        // Summoning staves (conjuration)
        // No specific tag, but logged for debugging
        logger::trace("Staff summon effect detected on enchantment"sv);
        break;

      default:
        break;
      }
      }

      return tags;
   }

   float WeaponClassifier::GetChargePercentage(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return 1.0f;

      // This is a placeholder - actual charge tracking requires inventory entry data
      // The registry will update this with actual charge from the inventory entry
      auto* enchantable = weapon->As<RE::TESEnchantableForm>();
      if (enchantable && enchantable->formEnchanting) {
      // Return max charge for now, actual charge comes from inventory entry
      float maxCharge = enchantable->amountofEnchantment;
      if (maxCharge > 0.0f) {
        return 1.0f;  // Will be updated by registry with actual charge
      }
      }

      return 1.0f;  // Not enchanted or no charge tracking
   }

   // =============================================================================
   // COMBAT STATS
   // =============================================================================

   float WeaponClassifier::GetBaseDamage(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return 0.0f;

      // Get attack damage from weapon
      return static_cast<float>(weapon->GetAttackDamage());
   }

   float WeaponClassifier::GetSpeed(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return 1.0f;

      // Speed is stored in weapon data - access directly
      // TESObjectWEAP inherits from TESAttackDamageForm
      return weapon->weaponData.speed;
   }

   float WeaponClassifier::GetReach(RE::TESObjectWEAP* weapon) const
   {
      if (!weapon) return 1.0f;

      // Reach is stored in weapon data - access directly
      return weapon->weaponData.reach;
   }

   // =============================================================================
   // HELPERS
   // =============================================================================

   bool WeaponClassifier::HasKeyword(RE::TESForm* form, std::string_view keywordEditorID) const
   {
      if (!form) return false;

      auto* keywordForm = form->As<RE::BGSKeywordForm>();
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

   bool WeaponClassifier::NameContains(std::string_view name, std::string_view keyword) const
   {
      // Case-insensitive search using std::search - no heap allocations
      auto caseInsensitiveEqual = [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
      };

      return std::search(name.begin(), name.end(),
                         keyword.begin(), keyword.end(),
                         caseInsensitiveEqual) != name.end();
   }
}

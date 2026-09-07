#include "ApparelClassifier.h"

namespace Huginn::Apparel
{
   // =============================================================================
   // ACTOR VALUE -> CRAFT SKILL
   // =============================================================================
   // The AV vocabulary is shared with ItemClassifier::DetermineFortifySkillType,
   // which is where the LoreRim-specific *PowerModifier values were first found.
   // Keep the two in step: a fortify effect that a potion recognises should be
   // recognised on a ring too.
   // =============================================================================
   CraftSkill ApparelClassifier::CraftSkillForActorValue(RE::ActorValue av) noexcept
   {
      switch (av) {
      case RE::ActorValue::kAlchemy:
      case RE::ActorValue::kAlchemyPowerModifier:     // LORERIM (148)
         return CraftSkill::Alchemy;

      case RE::ActorValue::kSmithing:
      case RE::ActorValue::kSmithingPowerModifier:    // LORERIM (141)
         return CraftSkill::Smithing;

      case RE::ActorValue::kEnchanting:
         return CraftSkill::Enchanting;

      default:
         return CraftSkill::None;
      }
   }

   ApparelSlot ApparelClassifier::DetermineSlot(RE::TESObjectARMO* armor) noexcept
   {
      if (!armor) return ApparelSlot::Unknown;

      using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
      const auto mask = armor->GetSlotMask();

      const auto has = [mask](Slot s) noexcept {
         return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(s)) != 0;
      };

      // Ordered most- to least-specific: a circlet also occupies the hair slot,
      // so the narrow checks must win.
      if (has(Slot::kRing))    return ApparelSlot::Ring;
      if (has(Slot::kAmulet))  return ApparelSlot::Amulet;
      if (has(Slot::kCirclet)) return ApparelSlot::Circlet;
      if (has(Slot::kHands))   return ApparelSlot::Hands;
      if (has(Slot::kFeet))    return ApparelSlot::Feet;
      if (has(Slot::kHead))    return ApparelSlot::Head;
      if (has(Slot::kBody))    return ApparelSlot::Body;

      return ApparelSlot::Other;
   }

   ApparelData ApparelClassifier::ClassifyApparel(
      RE::TESObjectARMO* armor,
      RE::EnchantmentItem* enchantment) const
   {
      ApparelData data{};
      if (!armor) return data;

      data.formID = armor->GetFormID();
      if (const char* name = armor->GetName(); name) {
         data.name = name;
      }
      data.slot = DetermineSlot(armor);

      // Unenchanted gear can never be craft-relevant — leave craftSkill None so
      // the registry rejects it without walking anything.
      if (!enchantment) return data;

      // Pick the STRONGEST craft-relevant effect. Fortify-crafting enchantments
      // are usually single-effect, but a player-made piece can carry two, and the
      // magnitude is what the scorer ranks on — so the larger one is the honest
      // representative of the item.
      float bestMagnitude = -1.0f;
      for (const auto* effect : enchantment->effects) {
         if (!effect || !effect->baseEffect) continue;

         // A hostile effect on the same AV is a Damage <skill> enchantment, not a
         // fortify — ranking it as craft gear would recommend sabotage.
         if (effect->baseEffect->IsHostile()) continue;

         const CraftSkill skill = CraftSkillForActorValue(effect->baseEffect->data.primaryAV);
         if (skill == CraftSkill::None) continue;

         const float magnitude = effect->effectItem.magnitude;
         if (magnitude > bestMagnitude) {
            bestMagnitude = magnitude;
            data.craftSkill = skill;
            data.magnitude = magnitude;
         }
      }

      return data;
   }
}

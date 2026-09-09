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
   namespace
   {
      // Skyrim carries each skill THREE times: the skill itself, a "+90" modifier
      // series, and a power-modifier series. Apparel enchantments overwhelmingly
      // use the +90 one, and the original version of this function did not map it
      // at all — it reused the vocabulary from ItemClassifier, which was written
      // for POTIONS, so every enchanted piece in a real inventory was rejected.
      //
      // Measured 2026-09-08 on LoreRim, six independent confirmations in one
      // inventory: Alchemy 106, Speechcraft 107, Alteration 108, Destruction 110,
      // Illusion 111, Restoration 112 — each exactly its skill index + 90. That
      // puts Smithing at 100 and Enchanting at 113.
      constexpr int kSkillModifierOffset = 90;

      /// The +90 modifier AV for a base skill AV.
      [[nodiscard]] constexpr RE::ActorValue SkillModifier(RE::ActorValue skill) noexcept
      {
         return static_cast<RE::ActorValue>(static_cast<int>(skill) + kSkillModifierOffset);
      }

      // Pin the derivation to the value actually observed in the log, so this
      // cannot drift on a CommonLib enum change without the build saying so.
      // 106 is 'Circlet of Minor Alchemy' as logged by ApparelRegistry.
      static_assert(static_cast<int>(SkillModifier(RE::ActorValue::kAlchemy)) == 106,
         "Fortify Alchemy apparel was measured at AV 106 — the +90 skill-modifier "
         "derivation no longer produces it");
      static_assert(static_cast<int>(SkillModifier(RE::ActorValue::kSmithing)) == 100,
         "Fortify Smithing apparel is expected at AV 100");
      static_assert(static_cast<int>(SkillModifier(RE::ActorValue::kEnchanting)) == 113,
         "Fortify Enchanting apparel is expected at AV 113");
   }

   CraftSkill ApparelClassifier::CraftSkillForActorValue(RE::ActorValue av) noexcept
   {
      // Not a switch any more: the modifier values are computed from the base
      // skill rather than written as literals, so they cannot be case labels.
      // Keeping them derived is the point — a bare `case 100:` would be a magic
      // number no one could check.

      if (av == RE::ActorValue::kAlchemy ||
          av == RE::ActorValue::kAlchemyPowerModifier ||          // LORERIM (148)
          av == SkillModifier(RE::ActorValue::kAlchemy)) {        // 106 — apparel
         return CraftSkill::Alchemy;
      }

      if (av == RE::ActorValue::kSmithing ||
          av == RE::ActorValue::kSmithingPowerModifier ||         // LORERIM (141)
          av == SkillModifier(RE::ActorValue::kSmithing)) {       // 100 — apparel
         return CraftSkill::Smithing;
      }

      if (av == RE::ActorValue::kEnchanting ||
          av == SkillModifier(RE::ActorValue::kEnchanting)) {     // 113 — apparel
         return CraftSkill::Enchanting;
      }

      return CraftSkill::None;
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

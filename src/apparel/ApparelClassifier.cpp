#include "ApparelClassifier.h"

namespace Huginn::Apparel
{
   // The mapping now lives in ApparelData.h so ItemClassifier can share it —
   // the two copies had already drifted, and it was this one being wrong that
   // made #65 inert on its first play-test. Kept as a forwarder rather than
   // deleted: it is a declared member of the class and the classifier reads
   // better asking itself the question.
   CraftSkill ApparelClassifier::CraftSkillForActorValue(RE::ActorValue av) noexcept
   {
      return Apparel::CraftSkillForActorValue(av);
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

      // Record EVERY craft-relevant effect, one magnitude per skill. Keeping only
      // the single largest across unrelated skills is what made a dual-fortify
      // piece invisible at one of its own benches — see CraftMagnitudes.
      for (const auto* effect : enchantment->effects) {
         if (!effect || !effect->baseEffect) continue;

         // A hostile effect on the same AV is a Damage <skill> enchantment, not a
         // fortify — ranking it as craft gear would recommend sabotage.
         if (effect->baseEffect->IsHostile()) continue;

         const CraftSkill skill = CraftSkillForActorValue(effect->baseEffect->data.primaryAV);
         if (skill == CraftSkill::None) continue;

         data.magnitudes.Set(skill, effect->effectItem.magnitude);
      }

      // The primary is the strongest craft overall: what the widget shows, what
      // the log names, and what PriorCalculator ranks on. The context weight does
      // NOT come from it — WeightForCandidate reads the whole triple, so a piece
      // still surfaces at the bench for its weaker craft.
      data.craftSkill = data.magnitudes.Primary();
      data.magnitude = data.magnitudes.For(data.craftSkill);

      return data;
   }
}

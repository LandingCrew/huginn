#pragma once

#include "ApparelData.h"

namespace Huginn::Apparel
{
   // =============================================================================
   // APPAREL CLASSIFIER (#65)
   // =============================================================================
   // Decides whether a wearable is craft-relevant, and if so which skill it
   // fortifies and by how much.
   //
   // CLASSIFICATION IS THE SCOPE GUARD. Anything that does not fortify Alchemy,
   // Smithing or Enchanting classifies as CraftSkill::None and is rejected by the
   // registry, so the candidate pool never grows by the size of the wardrobe.
   // Widening the feature to resist/carry-weight gear is a change to
   // CraftSkillForActorValue and nothing else.
   //
   // BOTH ENCHANTMENT SOURCES MATTER: base-form enchantments (TESEnchantableForm::
   // formEnchanting, e.g. vanilla's pre-enchanted circlets) and player-applied
   // ones (ExtraEnchantment on the inventory stack). Fortify-crafting gear is
   // usually player-made, so handling only formEnchanting would miss the common
   // case. The caller resolves which enchantment applies and passes it in.
   // =============================================================================

   class ApparelClassifier
   {
   public:
      ApparelClassifier() = default;
      ~ApparelClassifier() = default;

      /**
       * @brief Classify a wearable by its effective enchantment
       * @param armor The armor form
       * @param enchantment Effective enchantment (player-applied if present, else
       *                    the base form's). May be null for unenchanted gear.
       * @return ApparelData; craftSkill == None means "not a candidate"
       */
      [[nodiscard]] ApparelData ClassifyApparel(
         RE::TESObjectARMO* armor,
         RE::EnchantmentItem* enchantment) const;

      /**
       * @brief Map an ActorValue to the crafting skill it fortifies
       * @return CraftSkill::None for any AV that is not craft-relevant
       * @note Mirrors the AV vocabulary in ItemClassifier::DetermineFortifySkillType,
       *       including the LoreRim *PowerModifier variants.
       */
      [[nodiscard]] static CraftSkill CraftSkillForActorValue(RE::ActorValue av) noexcept;

   private:
      /// Biped slot from the armor's slot mask (display/logging only).
      [[nodiscard]] static ApparelSlot DetermineSlot(RE::TESObjectARMO* armor) noexcept;
   };
}

#include "ScrollClassifier.h"

namespace Huginn::Scroll
{
   using namespace Spell;

   ScrollData ScrollClassifier::ClassifyScroll(RE::ScrollItem* scroll) const
   {
      if (!scroll) {
      logger::warn("ScrollClassifier::ClassifyScroll called with nullptr scroll");
      return ScrollData{};
      }

      ScrollData data;
      data.formID = scroll->GetFormID();
      data.name = scroll->GetName();

      // Get linked spell from scroll
      auto* linkedSpell = GetLinkedSpell(scroll);
      if (!linkedSpell) {
      logger::warn("Scroll '{}' ({:08X}) has no linked spell, classifying as Unknown",
        data.name, data.formID);
      return data;  // Returns default Unknown type
      }

      // Delegate to SpellClassifier for effect analysis
      auto spellData = m_spellClassifier.ClassifySpell(linkedSpell);

      // Convert SpellData to ScrollData
      data = ConvertToScrollData(data.formID, data.name, spellData);

      // Override magnitude and duration with scroll-specific values
      // (scrolls may have different magnitude/duration than base spell)
      data.magnitude = GetPrimaryMagnitude(scroll);
      data.duration = GetPrimaryDuration(scroll);

      logger::trace("Classified scroll: {}", data.ToString());

      return data;
   }

   RE::SpellItem* ScrollClassifier::GetLinkedSpell(RE::ScrollItem* scroll) const
   {
      if (!scroll) {
      return nullptr;
      }

      // ScrollItem IS a SpellItem (inherits from it)
      // Just cast/return the scroll itself as a SpellItem
      return scroll;
   }

   ScrollData ScrollClassifier::ConvertToScrollData(
      RE::FormID scrollFormID,
      std::string_view scrollName,
      const SpellData& spellData) const
   {
      ScrollData data;
      data.formID = scrollFormID;
      data.name = std::string(scrollName);

      // Copy classification from spell
      data.type = static_cast<ScrollType>(spellData.type);
      data.tags = static_cast<ScrollTag>(spellData.tags);
      data.school = spellData.school;
      data.element = spellData.element;
      data.baseCost = spellData.baseCost;

      // magnitude and duration will be set by caller
      data.magnitude = 0.0f;
      data.duration = 0.0f;

      return data;
   }

   // OPTIMIZATION (v0.7.20 H1+H6): Delegate to SpellClassifier to eliminate duplicate code
   // ScrollItem inherits from SpellItem, so we can pass it directly to GetCostliestEffect()
   float ScrollClassifier::GetPrimaryMagnitude(RE::ScrollItem* scroll) const
   {
      if (!scroll) {
      return 0.0f;
      }
      // ScrollItem IS-A SpellItem - delegate to single source of truth for cost calculation
      auto* effect = m_spellClassifier.GetCostliestEffect(scroll);
      return effect ? effect->effectItem.magnitude : 0.0f;
   }

   float ScrollClassifier::GetPrimaryDuration(RE::ScrollItem* scroll) const
   {
      if (!scroll) {
      return 0.0f;
      }
      // ScrollItem IS-A SpellItem - delegate to single source of truth for cost calculation
      auto* effect = m_spellClassifier.GetCostliestEffect(scroll);
      return effect ? static_cast<float>(effect->effectItem.duration) : 0.0f;
   }
}

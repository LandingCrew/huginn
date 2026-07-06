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

      // Delegate to SpellClassifier for effect analysis
      // (ScrollItem IS-A SpellItem, so the scroll classifies directly as a spell)
      auto spellData = m_spellClassifier.ClassifySpell(scroll);

      // Convert SpellData to ScrollData
      ScrollData data = ConvertToScrollData(scroll->GetFormID(), scroll->GetName(), spellData);

      // Override magnitude and duration with scroll-specific values
      // (scrolls may have different magnitude/duration than base spell).
      // ScrollItem IS-A SpellItem, so reuse the SpellClassifier's costliest-effect
      // calc once and read both fields from it (avoids two extra list scans).
      if (auto* effect = m_spellClassifier.GetCostliestEffect(scroll)) {
      data.magnitude = effect->effectItem.magnitude;
      data.duration = static_cast<float>(effect->effectItem.duration);
      }

      logger::trace("Classified scroll: {}", data.ToString());

      return data;
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

      // magnitude and duration keep their zero defaults; ClassifyScroll overrides
      // them with scroll-specific values

      return data;
   }
}

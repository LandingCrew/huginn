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

      // Delegate to SpellClassifier for effect analysis
      // (ScrollItem IS-A SpellItem, so the scroll classifies directly as a spell)
      auto spellData = m_spellClassifier.ClassifySpell(scroll);

      // Convert SpellData to ScrollData
      data = ConvertToScrollData(data.formID, data.name, spellData);

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

      // magnitude and duration will be set by caller
      data.magnitude = 0.0f;
      data.duration = 0.0f;

      return data;
   }
}

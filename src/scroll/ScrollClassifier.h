#pragma once

#include "ScrollData.h"
#include "../spell/SpellClassifier.h"

namespace Huginn::Scroll
{
   // =============================================================================
   // SCROLL CLASSIFIER (v0.7.7)
   // =============================================================================
   // ScrollClassifier analyzes scroll FormIDs from the game and classifies them
   // by type and tags for use in the Q-learning recommendation system.
   //
   // ARCHITECTURE:
   // - Scrolls are consumable spell casts with no magicka cost
   // - Classification strategy: ScrollItem IS-A SpellItem, so the scroll is
   //   classified directly by SpellClassifier
   // - Reuses spell classification logic to avoid duplication
   // - Converts SpellData to ScrollData for scroll-specific metadata
   // =============================================================================

   class ScrollClassifier
   {
   public:
      // Constructor takes SpellClassifier dependency for effect analysis
      explicit ScrollClassifier(const Spell::SpellClassifier& spellClassifier)
      : m_spellClassifier(spellClassifier) {}

      ~ScrollClassifier() = default;

      // Classify a scroll from its SKSE scroll object
      // Returns ScrollData with detected type, tags, and metadata
      // Delegates effect analysis to SpellClassifier
      //
      // CONTRACT: for non-null input the result always carries the scroll's real
      // formID — there is no rejection path (unlike WeaponClassifier, which returns
      // a formID==0 sentinel). ScrollRegistry stores results unchecked and re-keys
      // m_formIDIndex on data.formID during swap-pop removal, so a sentinel return
      // would desync its index. If a rejection path is ever added, restore the
      // formID==0 guard in ScrollRegistry::AddScroll.
      [[nodiscard]] ScrollData ClassifyScroll(RE::ScrollItem* scroll) const;

   private:
      // Convert SpellData to ScrollData (same classification, different metadata)
      [[nodiscard]] ScrollData ConvertToScrollData(
      RE::FormID scrollFormID,
      std::string_view scrollName,
      const Spell::SpellData& spellData) const;

      // Reference to SpellClassifier for effect analysis
      const Spell::SpellClassifier& m_spellClassifier;
   };
}

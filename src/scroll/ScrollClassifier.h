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
   // - Classification strategy: Extract linked spell, delegate to SpellClassifier
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
      [[nodiscard]] ScrollData ClassifyScroll(RE::ScrollItem* scroll) const;

   private:
      // Extract linked spell from scroll
      // Returns nullptr if scroll has no linked spell
      [[nodiscard]] RE::SpellItem* GetLinkedSpell(RE::ScrollItem* scroll) const;

      // Convert SpellData to ScrollData (same classification, different metadata)
      [[nodiscard]] ScrollData ConvertToScrollData(
      RE::FormID scrollFormID,
      std::string_view scrollName,
      const Spell::SpellData& spellData) const;

      // Get magnitude of the scroll's primary effect
      [[nodiscard]] float GetPrimaryMagnitude(RE::ScrollItem* scroll) const;

      // Get duration of the scroll's primary effect
      [[nodiscard]] float GetPrimaryDuration(RE::ScrollItem* scroll) const;

      // Reference to SpellClassifier for effect analysis
      const Spell::SpellClassifier& m_spellClassifier;
   };
}

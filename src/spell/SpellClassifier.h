#pragma once

#include "SpellData.h"
#include "SpellOverrides.h"

namespace Huginn::Spell
{
   // SpellClassifier analyzes spell FormIDs from the game and classifies them
   // by type and tags for use in the Q-learning recommendation system
   class SpellClassifier
   {
   public:
      SpellClassifier() = default;
      ~SpellClassifier() = default;

      // Load spell classification overrides from INI file
      // Should be called once during initialization
      void LoadOverrides(const std::filesystem::path& iniPath);

      // Classify a spell from its SKSE spell object
      // Returns SpellData with detected type, tags, and metadata
      // Will use override data if available, otherwise auto-classify
      [[nodiscard]] SpellData ClassifySpell(RE::SpellItem* spell) const;

      // OPTIMIZATION (v0.7.20 H1+H6): Made public for delegation from ScrollClassifier
      // Get the effect with the highest magicka cost (primary/defining effect)
      // Uses Skyrim's actual cost formula: baseCost × magnitude^1.1 × durationFactor × areaFactor
      [[nodiscard]] RE::Effect* GetCostliestEffect(RE::SpellItem* spell) const;

   private:
      // OPTIMIZATION (v0.7.19): Methods now accept pre-computed effect to avoid
      // redundant GetCostliestEffect() calls (O(4n) → O(n) per spell at load time)

      // Determine primary spell type using API-first approach (no name fallback)
      // @param spell The spell to classify
      // @param primaryEffect Pre-computed costliest effect's base setting (may be null)
      [[nodiscard]] SpellType DetermineSpellType(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const;

      // Derive SpellType from computed SpellTag bitflags (fallback when API fails)
      [[nodiscard]] static SpellType DeriveSpellTypeFromTags(SpellTag tags) noexcept;

      // Derive ElementType from computed SpellTag bitflags (fallback when API fails)
      [[nodiscard]] static ElementType DeriveElementFromTags(SpellTag tags) noexcept;

      // Determine magic school from effect's GetMagickSkill() API
      // @param costliestEffect Pre-computed costliest effect (may be null)
      [[nodiscard]] MagicSchool DetermineMagicSchool(RE::Effect* costliestEffect) const;

      // Determine element type from effect's resistVariable API
      // @param costliestEffect Pre-computed costliest effect (may be null)
      [[nodiscard]] ElementType DetermineElementType(RE::Effect* costliestEffect) const;

      // Generate contextual tags based on effects and keywords
      [[nodiscard]] SpellTag DetermineSpellTags(RE::SpellItem* spell) const;

      // Extract base magicka cost (unmodified by perks/enchantments)
      [[nodiscard]] uint32_t GetBaseCost(RE::SpellItem* spell) const;

      // Check if spell is concentration (continuous) vs one-shot
      [[nodiscard]] bool IsConcentration(RE::SpellItem* spell) const;

      // Estimate effective range from delivery type and projectile
      [[nodiscard]] float GetEffectiveRange(RE::SpellItem* spell) const;

      // Helper: Get primary magic effect (typically first effect)
      [[nodiscard]] RE::EffectSetting* GetPrimaryEffect(RE::SpellItem* spell) const;

      // Spell overrides loaded from INI file
      SpellOverrides m_overrides;
   };
}

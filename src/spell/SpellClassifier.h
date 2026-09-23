#pragma once

#include "SpellData.h"
#include "SpellOverrides.h"

namespace Huginn::Spell
{
   // SpellClassifier analyzes spell FormIDs from the game and classifies them
   // by type and tags for use in the contextual bandit recommendation system
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

      /// Did the non-script retry decide this spell's type?
      ///
      /// For the dump, and exact by construction: it asks the same two functions
      /// in the same order ClassifySpell does, rather than restating the
      /// condition. The dump used to set its own flag on "the costliest effect
      /// is a script and another exists", which is NOT the same thing -- a
      /// harmful Destruction or Illusion script is answered by the school
      /// fallback and never retries, and 22 of 52 flagged rows were that case.
      /// An audit that groups by the archetype column believes those rows
      /// describe an effect the classifier never looked at.
      [[nodiscard]] bool RetryDecidedType(RE::SpellItem* spell) const;

      // Reconcile the override file against reality: how many entries matched a
      // spell, and which never did. Call after a pass that classifies every
      // spell, which is what makes the answer meaningful.
      void ReportOverrideUsage(std::string_view context) const {
      m_overrides.ReportUsage(context);
      }

      // OPTIMIZATION (v0.7.20 H1+H6): Made public for delegation from ScrollClassifier
      // Get the effect with the highest magicka cost (primary/defining effect)
      // Uses Skyrim's actual cost formula: baseCost × magnitude^1.1 × durationFactor × areaFactor
      [[nodiscard]] RE::Effect* GetCostliestEffect(RE::SpellItem* spell) const;

      // Same, but skipping kScript effects.
      //
      // A script effect tells the API nothing -- no archetype, no actor value,
      // no hostility worth reading -- so when it happens to be the costliest one
      // it hides whatever else the spell does. Of the 157 script-primary spells
      // a LoreRim player can learn, 66 carry another effect that IS readable
      // (2026-09-21 dump). Returns nullptr when every effect is a script, which
      // is the honest answer for the other 89.
      [[nodiscard]] RE::Effect* GetCostliestNonScriptEffect(RE::SpellItem* spell) const;

   private:
      // OPTIMIZATION (v0.7.19): Methods now accept pre-computed effect to avoid
      // redundant GetCostliestEffect() calls (O(4n) → O(n) per spell at load time)

      // Determine primary spell type using API-first approach (no name fallback)
      // @param spell The spell to classify
      // @param primaryEffect Pre-computed costliest effect's base setting (may be null)
      // `evidence`, when given, is set to HOW the answer was reached, so the
      // registry can report the guesses. Defaulted to Archetype on entry and
      // overwritten only on the paths that read less than the effect data --
      // there are four of those and forty that are not, so writing it at the
      // exceptions keeps the forty honest by construction.
      [[nodiscard]] SpellType DetermineSpellType(RE::SpellItem* spell,
      RE::EffectSetting* primaryEffect,
      TypeEvidence* evidence = nullptr) const;

      // Last-resort typing by NAME, for spells whose behaviour lives entirely in
      // a Papyrus script.
      //
      // Only reached when the effect data has already said nothing: 121 of the
      // 124 spells a LoreRim player can learn and Huginn cannot type are script
      // effects (2026-09-21), and among them are Soul Trap and the whole "Open
      // <rank> Lock" line. Names are a bad signal and this deliberately reads
      // only the handful of words that name a spell's PURPOSE rather than its
      // flavour, so a miss stays Unknown rather than becoming a wrong answer.
      [[nodiscard]] static SpellType DeriveSpellTypeFromName(std::string_view name) noexcept;

      // Derive SpellType from computed tag bitflags (fallback when API fails).
      // Takes BOTH sets: an Open Lock or Waterbreathing spell carries no
      // primary tag at all now that the extended ones exist, so passing only
      // `tags` would send it back out as Unknown (#79).
      [[nodiscard]] static SpellType DeriveSpellTypeFromTags(SpellTag tags, SpellTagExt tagsExt) noexcept;

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

      // Generate extended tags (#79). API-first — walks every effect rather
      // than only the costliest, because these are frequently the cheap half
      // of a multi-effect spell (a waterbreathing rider on an armour spell).
      [[nodiscard]] SpellTagExt DetermineSpellTagsExt(RE::SpellItem* spell) const;

      // Extract base magicka cost (unmodified by perks/enchantments)
      [[nodiscard]] uint32_t GetBaseCost(RE::SpellItem* spell) const;

      // Check if spell is concentration (continuous) vs one-shot
      [[nodiscard]] bool IsConcentration(RE::SpellItem* spell) const;

      // Estimate effective range from delivery type and projectile.
      // @param primaryEffect Pre-computed costliest effect's base setting (may be null)
      [[nodiscard]] float GetEffectiveRange(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const;

      // Spell overrides loaded from INI file
      SpellOverrides m_overrides;
   };
}

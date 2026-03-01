#pragma once

#include "ItemData.h"
#include "ItemOverrides.h"
#include <unordered_set>

namespace Huginn::Item
{
   // ItemClassifier analyzes alchemy items (potions, poisons, food) and classifies them
   // by type and tags for use in the Q-learning recommendation system.
   // Mirrors SpellClassifier API-first, tag-fallback pattern.
   class ItemClassifier
   {
   public:
      ItemClassifier() = default;
      ~ItemClassifier() = default;

      // Load item classification overrides from INI file
      // Should be called once during initialization
      void LoadOverrides(const std::filesystem::path& iniPath);

      // Classify an alchemy item from its SKSE object
      // Returns ItemData with detected type, tags, and metadata
      // Will use override data if available, otherwise auto-classify
      [[nodiscard]] ItemData ClassifyItem(RE::AlchemyItem* item) const;

      // Classify a soul gem from its SKSE object (v0.7.10)
      // Note: Soul gems are NOT cached (classification is trivial, cache overhead not worth it)
      [[nodiscard]] static ItemData ClassifySoulGem(RE::TESSoulGem* soulGem) noexcept;

   private:
      // Determine primary item type using API-first approach
      [[nodiscard]] static ItemType DetermineItemType(RE::AlchemyItem* item) noexcept;

      // Derive ItemType from computed ItemTag bitflags (fallback when API fails)
      [[nodiscard]] static ItemType DeriveItemTypeFromTags(ItemTag tags) noexcept;

      // v0.8: Populate all tag fields based on effects and keywords
      // Populates: data.tags, data.tagsExt, data.school, data.combatSkill, data.utilitySkill, data.element
      void PopulateItemTags(RE::AlchemyItem* item, ItemData& data) const;

      // v0.8: Map ActorValue to appropriate grouped fortify tag + specific skill enum
      // Called from PopulateItemTags when a fortify skill effect is detected
      static void DetermineFortifySkillType(RE::ActorValue av, ItemData& data) noexcept;

      // Get the effect with the highest magicka cost (primary/defining effect)
      [[nodiscard]] static RE::Effect* GetCostliestEffect(const RE::AlchemyItem* item) noexcept;

      // Get magnitude of the costliest effect
      [[nodiscard]] static float GetPrimaryMagnitude(const RE::AlchemyItem* item) noexcept;

      // Get duration of the costliest effect
      [[nodiscard]] static float GetPrimaryDuration(const RE::AlchemyItem* item) noexcept;

      // Check if item has any effect with the specified archetype
      [[nodiscard]] static bool HasArchetype(const RE::AlchemyItem* item, RE::EffectSetting::Archetype archetype) noexcept;

      // Check if item affects the specified actor value
      [[nodiscard]] static bool AffectsActorValue(const RE::AlchemyItem* item, RE::ActorValue av) noexcept;

      // Check if item has a resistance effect for the specified actor value
      [[nodiscard]] static bool HasResistEffect(const RE::AlchemyItem* item, RE::ActorValue resistAV) noexcept;

      // Check if a food item is actually an alcoholic beverage
      // Two-tier detection: keyword check (mod support), then name fallback
      [[nodiscard]] bool IsAlcohol(const RE::AlchemyItem* item, std::string_view name) const noexcept;

      // Helper: Check if item name contains keyword (case-insensitive)
      [[nodiscard]] static bool NameContains(std::string_view name, std::string_view keyword) noexcept;

      // Helper: Check if item has a specific keyword by EditorID
      [[nodiscard]] bool HasKeyword(const RE::AlchemyItem* item, std::string_view keywordEditorID) const noexcept;

      // OPTIMIZATION (v0.7.20 H2): Build keyword set once for O(1) lookups
      // @param keywordForm The keyword form to extract keywords from
      // @return Set of keyword EditorIDs for fast contains() checks
      [[nodiscard]] static std::unordered_set<std::string_view> BuildKeywordSet(
      const RE::BGSKeywordForm* keywordForm) noexcept;

      // Helper: Check if item is a soul gem (v0.7.8)
      [[nodiscard]] static bool HasSoulGemKeyword(const RE::AlchemyItem* item) noexcept;

      // Item overrides loaded from INI file
      ItemOverrides m_overrides;
   };
}

#pragma once

#include "SpellData.h"
#include "SpellClassifier.h"
#include <shared_mutex>  // v0.7.12 - thread safety
#include <atomic>        // M3 v0.7.21 - atomic m_isLoading
#include <type_traits>   // For ForEach visitor pattern

namespace Huginn::Spell
{
   // SpellRegistry maintains a database of all player-known spells with metadata
   // Provides fast lookup by FormID and filtering by type/tags
   // Also listens to TESEquipEvent for immediate spell equip/unequip detection (v0.7.8)
   class SpellRegistry : public RE::BSTEventSink<RE::TESEquipEvent>
   {
   public:
      SpellRegistry();
      ~SpellRegistry() = default;

      // Event handler for spell equip/unequip (v0.7.8)
      RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* event, RE::BSTEventSource<RE::TESEquipEvent>* source) override;

      // Load spell classification overrides from INI file
      // Should be called before RebuildRegistry()
      void LoadOverrides(const std::filesystem::path& iniPath);

      // Scan and classify all player-known spells
      // Called on game load and when player learns new spells
      void RebuildRegistry();

      // Add a newly learned spell dynamically (returns true if added, false if duplicate)
      bool AddNewSpell(RE::SpellItem* spell);

      // Reconcile registry with actual player spells (adds new, keeps existing)
      // Returns number of newly added spells
      size_t ReconcileSpells();

      /**
       * @brief Reconcile with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      size_t ReconcileSpells(RE::PlayerCharacter* player);

      // Get spell data by FormID (returns nullptr if not found)
      [[nodiscard]] const SpellData* GetSpellData(RE::FormID formID) const;

      // Get all spells of a specific type
      [[nodiscard]] std::vector<const SpellData*> GetSpellsByType(SpellType type) const;

      // Get count of spells of a specific type (efficient - no allocation)
      [[nodiscard]] size_t GetSpellCountByType(SpellType type) const;

      // Get all spells with a specific tag
      [[nodiscard]] std::vector<const SpellData*> GetSpellsWithTag(SpellTag tag) const;

      // Get all spells castable with current magicka
      [[nodiscard]] std::vector<const SpellData*> GetCastableSpells(float currentMagicka) const;

      // Get total number of registered spells
      [[nodiscard]] size_t GetSpellCount() const;

      // Get all registered spells (returns copy for thread safety - v0.7.12)
      [[nodiscard]] std::vector<SpellData> GetAllSpells() const;

      /**
       * @brief Iterate over all spells without copying (zero-allocation visitor pattern)
       * @tparam Func Callable with signature void(const SpellData&) or bool(const SpellData&)
       * @param func Function to call for each spell. If returns bool, iteration stops on false.
       * @note Thread-safe: Holds shared_lock during iteration
       * @note PERFORMANCE: Use this instead of GetAllSpells() in hot paths
       */
      template<typename Func>
      void ForEachSpell(Func&& func) const
      {
      std::shared_lock lock(m_mutex);
      for (const auto& spell : m_spells) {
        if constexpr (std::is_same_v<std::invoke_result_t<Func, const SpellData&>, bool>) {
           if (!func(spell)) return;  // Early exit if callback returns false
        } else {
           func(spell);
        }
      }
      }

      // Check if registry is currently loading/building
      // M3 (v0.7.21): Use atomic load with acquire semantics
      [[nodiscard]] bool IsLoading() const noexcept { return m_isLoading.load(std::memory_order_acquire); }

      // =============================================================================
      // SPELL FAVORITES (v0.7.8)
      // =============================================================================

      // Refresh favorites status for tracked spells (call at 500ms intervals)
      size_t RefreshFavorites();

      /**
       * @brief Refresh favorites with pre-fetched player pointer (v0.7.19 S2 optimization)
       * @param player Pre-fetched player pointer (avoids redundant GetSingleton)
       */
      size_t RefreshFavorites(RE::PlayerCharacter* player);

      // Get favorited spells only
      [[nodiscard]] std::vector<const SpellData*> GetFavoritedSpells() const;

      // Check if specific spell is favorited
      [[nodiscard]] bool IsFavorited(RE::FormID formID) const;

      // Get the spell classifier (v0.7.7 - for ScrollRegistry dependency injection)
      [[nodiscard]] const SpellClassifier& GetClassifier() const { return m_classifier; }

      // Log all registered spells (debug)
      void LogAllSpells() const;

   private:
      // Scan player's known spells from SKSE
      [[nodiscard]] std::vector<RE::SpellItem*> ScanPlayerSpells() const;
      [[nodiscard]] std::vector<RE::SpellItem*> ScanPlayerSpells(RE::PlayerCharacter* player) const;

      // Scan inventory for spell favorites status (returns set of FormIDs)
      [[nodiscard]] std::unordered_set<RE::FormID> ScanSpellFavorites() const;

      // Classify a spell and add to registry
      void AddSpell(RE::SpellItem* spell);

      // Remove a spell from registry by FormID (returns true if removed, false if not found)
      bool RemoveSpell(RE::FormID formID);

      // Storage
      std::vector<SpellData> m_spells;                       // All registered spells
      std::unordered_map<RE::FormID, size_t> m_formIDIndex;  // FormID -> index lookup

      // Classifier instance
      SpellClassifier m_classifier;

      // Thread safety (v0.7.12)
      // Protects m_spells and m_formIDIndex from concurrent access
      // Readers (render thread) use shared_lock, writers (update thread) use unique_lock
      mutable std::shared_mutex m_mutex;

      // Loading state flag (M3 v0.7.21: atomic for thread-safe access)
      std::atomic<bool> m_isLoading{false};
   };
}

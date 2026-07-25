#pragma once

#include "SpellData.h"
#include "SpellClassifier.h"
#include "registry/FormRegistry.h"  // Huginn::Registry::FormRegistry core (finding #8)

namespace Huginn::Spell
{
   // SpellRegistry maintains a database of all player-known spells with metadata
   // Provides fast lookup by FormID and filtering by type/tags
   // Also listens to TESEquipEvent for immediate spell equip/unequip detection (v0.7.8)
   class SpellRegistry : public RE::BSTEventSink<RE::TESEquipEvent>,
                         public Registry::FormRegistry<SpellRegistry, SpellData>
   {
   public:
      SpellRegistry();
      ~SpellRegistry() = default;

      // CRTP contract for FormRegistry: SpellData IS the entry; FormID lives on it.
      [[nodiscard]] static RE::FormID FormIDOf(const SpellData& spell) noexcept
      {
         return spell.formID;
      }

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
      // LIFETIME: The returned pointer is only valid synchronously, on the same
      // thread, until the next registry mutation (AddNewSpell/ReconcileSpells/
      // RebuildRegistry may reallocate the entry store and dangle it). Copy immediately;
      // never store. For cross-thread or retained access use GetAllSpells/ForEachSpell.
      [[nodiscard]] const SpellData* GetSpellData(RE::FormID formID) const;

      // Get count of spells of a specific type (efficient - no allocation)
      [[nodiscard]] size_t GetSpellCountByType(SpellType type) const;

      // Thin forwarders keeping the historical public names (zero call-site churn).
      // Count / copy-out / loading-flag / iteration all live in the shared core.
      [[nodiscard]] size_t GetSpellCount() const { return EntryCount(); }
      [[nodiscard]] std::vector<SpellData> GetAllSpells() const { return AllEntriesCopy(); }

      /** @brief Zero-allocation visitor over all spells (holds shared_lock). */
      template<typename Func>
      void ForEachSpell(Func&& func) const { ForEachEntry(std::forward<Func>(func)); }

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

      // Storage (vector/index), mutex, and loading flag live in the FormRegistry base.

      // Classifier instance
      SpellClassifier m_classifier;

      // Path the overrides INI was loaded from (re-loaded on each RebuildRegistry
      // so `hg rebuild` / `hg reset all` pick up edits without a game restart).
      std::filesystem::path m_overridesPath;
   };
}

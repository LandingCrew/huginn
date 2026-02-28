#pragma once

#include "InputHandler.h"
#include "../ui/SlotTypes.h"
#include "../slot/SlotSettings.h"
// Note: std headers (functional) come from PCH via RE/Skyrim.h
#include <array>
#include <shared_mutex>

namespace Huginn::Input
{
   /**
    * @brief Callback when a spell is equipped
    * @param formID The FormID of the equipped spell
    * @param wasRecommended true if the spell was in the widget slots
    */
   using EquipCallback = std::function<void(RE::FormID formID, bool wasRecommended)>;

   /**
    * @brief Manages spell equipping from widget slot selections
    *
    * Handles:
    * - Equipping spells to left/right/both hands
    * - Playing equip sounds
    * - Triggering learning callbacks (for Q-value updates)
    */
   class EquipManager
   {
   public:
      static EquipManager& GetSingleton();

      /**
       * @brief Equip the item in a specific slot
       * @param slotIndex 0-based slot index from widget
       * @param hand Which hand(s) to equip to
       * @return true if equip succeeded
       */
      bool EquipSlot(size_t slotIndex, EquipHand hand);

      /**
       * @brief Update cached slot content (called from update loop)
       * Replaces the old dependency on SpellRecommendationWidget::GetSlotContent()
       */
      void SetSlotContent(size_t index, const UI::SlotContent& content);

      /**
       * @brief Get cached slot content for a given index
       * Returns by value (copy-out pattern) for thread safety.
       */
      [[nodiscard]] UI::SlotContent GetSlotContent(size_t index) const;

      /**
       * @brief Set callback for when a spell is equipped
       * Used to trigger Q-learning updates
       * @param callback Function to call after successful equip
       */
      void SetEquipCallback(EquipCallback callback) { m_equipCallback = std::move(callback); }

      /**
       * @brief Enable or disable equip sounds
       * @param enabled true to play sounds
       */
      void SetSoundsEnabled(bool enabled) { m_soundsEnabled = enabled; }

   private:
      EquipManager() = default;
      ~EquipManager() = default;
      EquipManager(const EquipManager&) = delete;
      EquipManager& operator=(const EquipManager&) = delete;

      /// Get the equip slot for a hand
      RE::BGSEquipSlot* GetEquipSlot(EquipHand hand, bool isLeftHand);

      /// Equip a spell to the specified hand
      bool EquipSpellToHand(RE::SpellItem* spell, bool leftHand);

      /// Use a potion by FormID
      bool UsePotion(RE::FormID formID);

      /// Equip a weapon by FormID
      bool EquipWeapon(RE::FormID formID, bool leftHand = false);

      /// Use a soul gem to recharge the equipped enchanted weapon
      bool UseSoulGem(RE::FormID formID);

      /// Callback for learning system
      EquipCallback m_equipCallback;

      /// Sound settings
      bool m_soundsEnabled = true;

      /// Cached slot contents (written by update loop, read by EquipSlot)
      static constexpr size_t MAX_SLOTS = Slot::MAX_SLOTS_PER_PAGE;
      std::array<UI::SlotContent, MAX_SLOTS> m_slotContents{};

      /// Protects m_slotContents: shared_lock for reads, unique_lock for writes
      mutable std::shared_mutex m_slotMutex;
   };
}

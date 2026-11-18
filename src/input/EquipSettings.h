#pragma once

#include <atomic>

namespace Huginn::Input
{
   /**
    * @brief [Equip]: how a Huginn key puts things in the player's hands (0.24.26)
    *
    * Read from the main Huginn.ini over the shipped one (Settings::LoadMainIni),
    * at kDataLoaded and on every settings reload (SettingsReloader::ApplyIni),
    * so a change applies to the next key press. Atomics: written by the loader
    * (the message thread, or the reload's caller), read by the hand equip gate
    * on the main thread.
    */
   class EquipSettings
   {
   public:
      static EquipSettings& GetSingleton();

      /// Load [Equip] from the parsed main INI.
      void LoadFromIni(const CSimpleIniA& ini);

      /// bRedrawAfterHandEquip: after a key equips a spell, scroll, weapon or
      /// staff while the weapon is drawn, call DrawWeaponMagicHands(true), as
      /// Wheeler-Refined does after every equip from its wheel.
      [[nodiscard]] bool RedrawAfterHandEquip() const noexcept { return m_redraw.load(std::memory_order_relaxed); }

   private:
      EquipSettings() = default;

      std::atomic<bool> m_redraw{ true };
   };
}

#pragma once

// Only compile in debug mode
#ifdef _DEBUG

#include <imgui.h>
#include <chrono>

// Forward declarations
namespace Huginn::Spell { class SpellRegistry; }
namespace Huginn::Item { class ItemRegistry; }
namespace Huginn::Weapon { class WeaponRegistry; }
namespace Huginn::Scroll { class ScrollRegistry; }

namespace Huginn::UI
{
   /**
    * @brief Debug widget for combined registry display (v0.7.7)
    *
    * Shows all registry stats in a single collapsible window:
    *   - SpellRegistry: spell counts by type
    *   - ItemRegistry: item counts by type, best potions
    *   - WeaponRegistry: weapon/ammo counts, best weapons
    *   - ScrollRegistry: scroll counts by type/element (v0.7.7)
    *
    * USAGE:
    *   - Toggle visibility with RegistryDebugWidget::GetSingleton().ToggleVisible()
    *   - Auto-updates each frame when visible
    *   - Position persists across frames (draggable)
    */
   class RegistryDebugWidget
   {
   public:
      static RegistryDebugWidget& GetSingleton() noexcept;

      // Disable copy/move
      RegistryDebugWidget(const RegistryDebugWidget&) = delete;
      RegistryDebugWidget(RegistryDebugWidget&&) = delete;
      RegistryDebugWidget& operator=(const RegistryDebugWidget&) = delete;
      RegistryDebugWidget& operator=(RegistryDebugWidget&&) = delete;

      /**
       * @brief Draw the debug widget (called each frame)
       */
      void Draw();

      /**
       * @brief Show or hide the widget
       */
      void SetVisible(bool visible) { m_isVisible = visible; }

      /**
       * @brief Check if widget is visible
       */
      [[nodiscard]] bool IsVisible() const { return m_isVisible; }

      /**
       * @brief Toggle visibility
       */
      void ToggleVisible() { m_isVisible = !m_isVisible; }

   private:
      RegistryDebugWidget() = default;
      ~RegistryDebugWidget() = default;

      // Section renderers
      void DrawSpellRegistrySection(const Spell::SpellRegistry* registry);
      void DrawItemRegistrySection(const Item::ItemRegistry* registry);
      void DrawWeaponRegistrySection(const Weapon::WeaponRegistry* registry);
      void DrawScrollRegistrySection(const Scroll::ScrollRegistry* registry);

      // Cache update helpers (v0.7.18 - throttle expensive operations)
      void UpdateSpellCache(const Spell::SpellRegistry* registry);
      void UpdateWeaponCache(const Weapon::WeaponRegistry* registry);
      bool ShouldUpdateCache();

      bool m_isVisible = true;  // Visible by default in Debug builds
      float m_posX = 10.0f;     // Top-left corner (v0.7.7)
      float m_posY = 10.0f;     // Top-left corner (v0.7.7)

      // FIX (v0.7.18): Cache expensive computations to eliminate per-frame stutter
      // Update interval: 250ms is fast enough for debug display, avoids frame drops
      static constexpr float CACHE_UPDATE_INTERVAL_MS = 250.0f;
      std::chrono::steady_clock::time_point m_lastCacheUpdate{};

      // Cached spell registry data
      struct SpellCacheData {
      size_t totalSpells = 0;
      size_t damageCount = 0;
      size_t healingCount = 0;
      size_t defensiveCount = 0;
      size_t utilityCount = 0;
      size_t summonCount = 0;
      size_t buffCount = 0;
      size_t debuffCount = 0;
      size_t favoritedCount = 0;
      } m_spellCache;

      // Cached weapon registry data
      struct WeaponCacheData {
      size_t weaponCount = 0;
      size_t ammoCount = 0;
      size_t favoritedCount = 0;
      } m_weaponCache;
   };
}

#endif  // _DEBUG

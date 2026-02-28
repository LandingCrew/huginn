#pragma once

#include <string_view>
#include <string>
#include <array>
#include <atomic>
#include "IntuitionSettings.h"
#include "SlotTypes.h"  // SlotContentType
#include "slot/SlotAssignment.h"  // SlotVisualState enum

namespace Huginn::Slot { struct SlotAssignment; }
namespace Huginn::State { struct PlayerActorState; }

namespace Huginn::UI
{
    /**
     * @brief Slot type enum matching Intuition.as TYPE_* constants
     * These values are sent to the SWF via setSlot() and must stay in sync.
     */
    enum class IntuitionSlotType : int
    {
        kEmpty          = 0,
        kNoMatch        = 1,
        kSpell          = 2,
        kWildcard       = 3,
        kHealthPotion   = 4,
        kMagickaPotion  = 5,
        kStaminaPotion  = 6,
        kMeleeWeapon    = 7,
        kRangedWeapon   = 8
    };

    /**
     * @brief Scaleform-based HUD menu for Huginn's Intuition widget
     *
     * Loads Huginn/Intuition.swf and exposes the AS2 public API as C++ methods.
     * Registered as a Skyrim IMenu so the game manages its lifecycle.
     * Primary HUD display for Huginn slot recommendations.
     */
    class IntuitionMenu : public RE::IMenu
    {
    public:
        static constexpr std::string_view MENU_NAME{ "IntuitionMenu" };
        static constexpr std::string_view FILE_NAME{ "Huginn/Intuition" };

        // -- Lifecycle --
        static void Register();
        static RE::IMenu* CreateInstance();
        static void Show();
        static void Hide();

        // -- Singleton access (valid after Show() creates the instance) --
        static IntuitionMenu* GetSingleton();

        IntuitionMenu();
        ~IntuitionMenu() override
        {
            // Stage 3a: Thread-safe instance cleanup
            IntuitionMenu* expected = this;
            bool cleared = s_instance.compare_exchange_strong(expected, nullptr);
            logger::info("IntuitionMenu::~IntuitionMenu() - destroyed (singleton cleared={})"sv, cleared);
        }

        // -- Public API (mirrors AS2 methods) --
        void SetSlot(int index, std::string_view name, IntuitionSlotType type,
                     double confidence, std::string_view detail = "",
                     Slot::SlotVisualState visualState = Slot::SlotVisualState::Normal);
        void ClearSlot(int index);
        void SetSlotCount(int count);
        void SetPage(int current, int total, std::string_view name);
        void SetUrgent(int index, bool active);
        void SetWidgetAlpha(double alpha);
        void SetPosition(float x, float y);
        void SetVisible(bool a_visible);

        /// Re-read IntuitionSettings from INI and push all values to the live widget.
        /// Called by `hg reload` console command for hot-tuning.
        void ReapplySettings();

        /// Apply a pre-built config snapshot (avoids re-reading INI).
        /// Called by SettingsReloader::ApplySideEffects() after INI is already loaded.
        void ReapplySettings(const IntuitionConfig& config);

        // -- IMenu overrides --
        void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;

        // -- Type mapping --
        static IntuitionSlotType MapSlotContentType(SlotContentType type);

        // -- Detail string builder --
        static std::string BuildSlotDetail(
            const Slot::SlotAssignment& assignment,
            DisplayMode mode,
            const State::PlayerActorState& playerState);

    private:
        RE::GFxValue m_widget;  // Reference to _root.widget in SWF

        // Stage 3a: Thread-safe singleton instance
        // Atomic pointer prevents torn reads/writes during menu creation/destruction
        static inline std::atomic<IntuitionMenu*> s_instance{ nullptr };

    };
}

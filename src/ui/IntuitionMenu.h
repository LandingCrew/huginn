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
        kRangedWeapon   = 8,
        // #65 craft gear. Adding a value here is only half the change — the AS2
        // TYPE_* constants in src/swf/Intuition.as must match, and Intuition.swf
        // must be REBUILT and committed (docs/reference/intuition-scaleform-build.md).
        // The C++ build only copies the committed artifact.
        kApparel        = 9
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
        /// RE::UI opened or closed this menu: any kShow Show() queued has
        /// been consumed (HudVisibilityManager calls this).
        static void OnMenuOpenClose(bool a_opening) noexcept;
        /// Update tick: a kShow RE::UI has not answered within
        /// SHOW_PENDING_MAX_MS is warned about once, with a dump of what
        /// RE::UI holds for this menu. If the menu's menuMap entry is gone,
        /// the dump re-registers it and shows the widget again (2026-11-01).
        static void CheckPendingShow();

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
        void SetSlotCount(int count);
        void SetPage(int current, int total, std::string_view name);
        void SetWidgetAlpha(double alpha);
        void SetPosition(float x, float y);
        void SetVisible(bool a_visible);

        /// The reset key (`iResetWidgetKey`, default `x`) and dMenu's Reset
        /// Widget: reopen the menu if RE::UI closed it, recompute visibility,
        /// re-apply the HUD's alpha and re-send the whole bar. It never hides.
        ///
        /// 0.24.28: this was a hide toggle with its own latch. The player's
        /// HUD-hiding mod (Ultimate Immersion Toggle, on the same key) keeps its
        /// own flag, and any press one counted and the other did not left them
        /// inverted for the rest of the session (2026-11-02 19:33:43: the HUD up
        /// with the widget hidden, then the reverse). Hiding now follows the HUD
        /// instead (FollowHudAlpha), so there is one state and nothing to drift.
        static void ResetWidget();

        /// Ask the next push to re-send every slot, bypassing the backend's
        /// identical-frame cache. Set whenever the widget is (re)shown.
        static void RequestResync() noexcept;
        /// True once per request; IntuitionBackend::Push consumes it.
        [[nodiscard]] static bool ConsumeResync() noexcept;

        /// Apply a pre-built config snapshot (avoids re-reading INI).
        /// Called by SettingsReloader::ApplySideEffects() after INI is already loaded.
        void ReapplySettings(const IntuitionConfig& config);

        // -- IMenu overrides --
        void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;

        // -- Type mapping --
        static IntuitionSlotType MapSlotContentType(SlotContentType type);

        // -- Detail string builder --
        // minimalCounts applies only when mode == Minimal; the caller reads it
        // from IntuitionSettings so this stays a pure function of its arguments.
        static std::string BuildSlotDetail(
            const Slot::SlotAssignment& assignment,
            DisplayMode mode,
            const State::PlayerActorState& playerState,
            bool minimalCounts);

    private:
        RE::GFxValue m_widget;  // Reference to _root.widget in SWF

        /// UI thread only (AdvanceMovie). Copies the HUD Menu's `_root._alpha`
        /// onto ours, so whatever hides or fades the HUD (a HUD-toggle mod sets
        /// that alpha to 0) hides the widget too. Polled, not per frame: the
        /// menu lookup builds a BSFixedString.
        void FollowHudAlpha(float a_interval);
        float m_hudPollAccumSec{ 0.0f };
        double m_hudAlphaApplied{ -1.0 };  // -1: nothing applied yet

        // Stage 3a: Thread-safe singleton instance
        // Atomic pointer prevents torn reads/writes during menu creation/destruction
        static inline std::atomic<IntuitionMenu*> s_instance{ nullptr };

    };
}

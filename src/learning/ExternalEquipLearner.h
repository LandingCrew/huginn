#pragma once

#include "LearningSettings.h"  // For LearningConfig
#include <chrono>
#include <functional>
#include <string>

namespace Huginn::Learning
{
    // =========================================================================
    // EXTERNAL EQUIP LEARNER
    // =========================================================================
    // Turns an equip Huginn did not trigger -- the player's inventory,
    // favorites or magic menu, a vanilla hotkey, one of their own Wheeler
    // wheels -- into a player selection (SelectionTracker), provided there was
    // player input behind it (PlayerInputGate). A script's equip has none and
    // teaches nothing.
    //
    // Attribution uses PipelineStateCache to label how well Huginn was already
    // surfacing the item. The label feeds the soak telemetry (accept%, goal 1)
    // and the selection log. It is NOT a reward weight any more: a selection
    // teaches the same whatever device made it (one selection path,
    // 2026-10-02). The multipliers that used to scale these cases are gone.
    //
    // Cases:
    //   A: Not a candidate
    //   B-low: Low-rank candidate
    //   B-med: Mid-rank candidate
    //   C: High-rank, not displayed (near-miss)
    //   D: Displayed, page changed since snap
    //   E: Displayed, current page (Huginn already surfaced it)
    //
    // CASE D IS NARROWER THAN IT READS. PipelineStateCache only ever records
    // assignments for the page that was current when it snapshotted, so an item
    // displayed on some *other* page is not "displayed" as far as attribution is
    // concerned — it never enters the displayed set. What actually separates D
    // from E is comparing the snapshot's page against the LIVE page, so D fires
    // only when the player changed pages between the last pipeline run and the
    // equip. Reading the page from the cache instead of live would collapse D
    // into E permanently.
    // =========================================================================

    class ExternalEquipLearner
    {
    public:
        static ExternalEquipLearner& GetSingleton()
        {
            static ExternalEquipLearner instance;
            return instance;
        }

        // Called by ExternalEquipListener and SpellRegistry on external equip
        void OnExternalEquip(RE::FormID formID, const char* formType);

        /// Replace the stored config snapshot (e.g., after INI hot-reload).
        void SetConfig(const LearningConfig& config) { m_config = config; }

        // Live queries this class needs but must not reach *up* for (critique
        // #10). Both are read at equip time, not snapshot time, and that is
        // load-bearing — see the note on Case D above. Wired once by the
        // composition root in Main.cpp, which is the only place that already
        // knows about SlotAllocator and WheelerClient.
        //
        // Left unwired, OnExternalEquip bails before doing anything — including
        // before recording soak telemetry, so a wiring fault can't be mistaken
        // for a recommendation hit. A misconfigured learner that stops learning
        // is recoverable; one silently attributing rewards against wrong page
        // state is not.
        struct Environment
        {
            std::function<size_t()> currentDisplayPage;
        };

        // Validates immediately rather than waiting for an equip event that may
        // never arrive: a player who only uses Huginn's keys and wheel never
        // reaches this path, so it can stay silent for a whole session.
        void SetEnvironment(Environment env)
        {
            if (!env.currentDisplayPage) {
                logger::error("[ExternalEquipLearner] SetEnvironment incomplete "
                    "(currentDisplayPage unset) — external-equip learning will be suppressed"sv);
            }
            m_env = std::move(env);
        }

    private:
        ExternalEquipLearner() = default;
        ~ExternalEquipLearner() = default;
        ExternalEquipLearner(const ExternalEquipLearner&) = delete;
        ExternalEquipLearner& operator=(const ExternalEquipLearner&) = delete;

        // Config snapshot (updated via SetConfig on hot-reload)
        LearningConfig m_config;

        // Injected live queries — see SetEnvironment. Read unsynchronized from
        // the equip path, like m_config above: both writers are main-thread
        // (Main.cpp at init, SettingsReloader on hot-reload, which replaces
        // m_config only and leaves m_env intact).
        Environment m_env;

        // Slot-relative thresholds: how many ranks past the display cutoff
        // determines the attribution case. E.g., with 5 display slots:
        //   rank 5-7 (overshoot 0-2) → Case C "near-miss" (high reward)
        //   rank 8-10 (overshoot 3-5) → Case B-med (medium reward)
        //   rank 11+ (overshoot 6+) → Case B-low (low reward)
        static constexpr size_t NEAR_MISS_SLOTS = 2;   // Within 2 ranks of display = near-miss
        static constexpr size_t FAR_MISS_SLOTS = 5;     // Within 5 ranks = mid, beyond = low

        // True once both live queries are wired. Warns once when they aren't;
        // checked before anything else so an unwired learner records no
        // telemetry rather than a misleading one.
        bool EnvironmentReady() const;

        // Skip reason codes for ShouldSkip / SoakMetrics::RecordEquipSkip.
        // Deliberately lowercase: the 'A'..'E' space belongs to attribution
        // case labels, and the two are recorded through different counters.
        //
        // The wheel-open and anti-spam skips went with the one selection path.
        // Huginn's own wheel picks DO reach here first: Wheeler equips before it
        // calls back, so the equip event arrives before WheelerClient marks it
        // (measured on LoreRim 2026-10-02: event at .096, callback at .102).
        // The pick is recorded as an outside selection, then relabelled as a
        // Wheeler pick -- or withdrawn, for a Remembrance swap-back -- when the
        // callback lands (SelectionTracker::Select / Withdraw). The player's own
        // wheel picks are outside selections, and a repeat event for one pick
        // merges into its pending selection.
        static constexpr char SKIP_NONE     = '\0';  // do not skip
        static constexpr char SKIP_NO_INPUT = 'n';    // a CONSUMABLE with no player input behind it (a script)
        static constexpr char SKIP_ENGINE   = 'e';    // a non-consumable with no input: the engine refilling
                                                      // the quiver, a bound weapon... NOT counted in skipped=
        static constexpr char SKIP_STALE    = 's';    // pipeline snapshot too old to attribute
        static constexpr char SKIP_DISABLED = 'x';    // learnFromExternalEquips off

        /// Which filter (if any) rejects this equip. SKIP_NONE = proceed to
        /// attribution. Returns the reason rather than a bool so the caller can
        /// record WHY nothing was attributed — see SoakMetrics::RecordEquipSkip.
        /// `via` receives how the player made it when there was player input.
        char ShouldSkip(RE::FormID formID, std::string& via) const;

        /// The A-E case label for this equip, from pipeline state.
        const char* ComputeAttribution(RE::FormID formID) const;
    };

}  // namespace Huginn::Learning

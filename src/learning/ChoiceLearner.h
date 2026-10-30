#pragma once

// =============================================================================
// CHOICE LEARNER (R8.2, live from R8.3) -- doc 9's learner, seeded from the R6 bootstrap
// =============================================================================
// The new model (core/ChoiceScore.h) and its online update
// (core/ChoiceUpdate.h), held for the plugin: the bootstrap file
// (Data/SKSE/Plugins/Huginn_Bootstrap.json, core/Bootstrap.h), the per-item b
// with its battery on the play clock, recent_<family>'s pick timeline, the
// Plackett-Luce memory of which items each context already gave up, and the
// opportunity counter.
//
// R8.3 (0.24.0): LIVE. PipelineCoordinator ranks every candidate on mu
// computed from a snapshot of this learner (core/ChoiceLive.h) and logs mu, sd
// and the explanation in the v3 rows; picks update it. The cosave carries it
// (THTA, BIAS: persist/ChoiceSerializer.h) beside the old learner's BNDW:
// a load restores the save's theta per key over the bootstrap and its b; a
// save without them (older, new game) starts from the bootstrap with b under
// sBootstrapBias. `hg reset weights` / `hg reset all` / the dMenu reset return
// theta to the bootstrap and clear b.
//
// State.
//   theta, strength, the recent rows and theta_covered: one {mu, sigma^2}
//   entry per weight, each with a stable key (core/Bootstrap.h StableKey: a
//   hash of the need and column NAMES, the recent family and tau, "covered"),
//   never a position (the lesson of the old learner's features, where a positional
//   cosave key broke once). The entries sit in a dense array in the build's
//   index order for the scorer's speed; ExportEntries / ImportEntries (the
//   cosave's THTA, R8.3) go by key only.
//   b per item: {mu, sigma^2, n, play seconds at the last pick}, prior N(0, 0.25),
//   seeded from the file's items under [Learning] sBootstrapBias.
//   kappa and the feature weights: the file's, fixed (format v1 gives them no
//   precision, so there is no belief to update).
//   The discovery boost (0.24.14, core/DiscoveryBoost.h): the forms this
//   character has held or known, and the newly gained ones with the play
//   second they came; the snapshot hands the boosts to the wildcard draw
//   (b's variance x m(age) there only). Cosave record DISC.
//
// Threading: a shared_mutex. The update thread writes (OnPick, the onset
// counter, the battery sweep); a scoring pass takes one copy-out Snapshot
// (shared_ptr to an immutable copy, rebuilt only after a write).
// =============================================================================

#include "core/Bootstrap.h"
#include "core/Battery.h"
#include "core/ChoiceScore.h"
#include "core/ChoiceUpdate.h"
#include "core/DecisionLog.h"
#include "core/DiscoveryBoost.h"
#include "core/NeedEvaluator.h"
#include "core/ThetaDrift.h"

#include <SimpleIni.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Huginn::Learning
{
    /// The INI keys the new model reads (R8.2).
    struct ChoiceSettings
    {
        Core::Bootstrap::BiasMode biasMode = Core::Bootstrap::BiasMode::Match;   // [Learning] sBootstrapBias
        double thetaProcessNoise = 1.0e-4;   // [Learning] fThetaProcessNoise (per onset, variance units)
        double coveredPrior = -1.0;          // [Scoring] fCoveredPrior (precision 1)
        double favoriteLifeMult = 2.0;       // [Favorites] fFavoriteLifeMult (Boost mode)
        bool legacyLearnFeature = false;     // [Scoring] bLegacyLearnFeature (R8.3: live, hot-reloaded; 0.24.17: off, B live)
        bool shadowLegacy = true;            // [Scoring] bShadowLegacy (0.24.17: D2 in shadow, hot-reloaded)
        Core::Discovery::Params discovery;   // [Learning] bDiscoveryBoost, fDiscoveryBoostVarMult,
                                             // fDiscoveryBoostHalfLifeSec (0.24.14, hot-reloaded)

        static ChoiceSettings FromIni(const CSimpleIniA& ini);
    };

    class ChoiceLearner
    {
    public:
        static constexpr const char* kFileName = "Huginn_Bootstrap.json";

        /// One immutable copy for a scoring pass.
        struct Snapshot
        {
            Core::Choice::Model model;
            std::unordered_map<std::uint32_t, Core::Choice::BBelief> b;
            std::vector<Core::Bootstrap::BuildCurve> curves;
            Core::Battery::MemoryLife life;
            double favoriteLifeMult = 2.0;
            bool favoritesBoost = true;
            std::unordered_map<std::uint32_t, double> discovered;   // boosted form -> play seconds when gained
            Core::Discovery::Params discovery;
            std::uint64_t version = 0;

            /// The need vector the model reads: the live curve outputs, with each
            /// need the file carries a curve for re-read through that curve from
            /// its logged input (format doc, curves; curve.with_curve).
            [[nodiscard]] std::array<double, Core::Choice::K> Needs(const Core::Needs::NeedArray& value,
                                                                    const Core::Needs::NeedArray& input) const;
            /// b as the score reads it now (the battery applied).
            [[nodiscard]] Core::Choice::BRead B(std::uint32_t form, bool favorite, double playNow) const;
            /// The discovery boost's multiplier on b's variance for the
            /// wildcard draw (core/DiscoveryBoost.h); 1 when not boosted or off.
            [[nodiscard]] double DiscoveryMult(std::uint32_t form, double playNow) const;
        };

        struct Stats
        {
            std::size_t thetaEntries = 0;     // keyed weights (theta, strength, recent, covered)
            std::size_t thetaMoved = 0;       // ... whose {mu, var} differ from the bootstrap's
            std::size_t picksLearned = 0;
            std::size_t onsets = 0;           // need onsets seen
            std::size_t onsetsCounted = 0;    // ... with an answering item on the page (variance grown)
            std::size_t bEntries = 0;
        };

        ChoiceLearner() = default;

        /// Read, check and map the bootstrap file (once, at kDataLoaded or the
        /// first game load). A missing or invalid file logs ONE error and leaves
        /// the new model off (Ready() false). Logs the parse time.
        bool LoadBootstrap(const std::filesystem::path& path);
        [[nodiscard]] bool Ready() const noexcept { return m_ready.load(std::memory_order_acquire); }

        void SetSettings(const ChoiceSettings& s);
        [[nodiscard]] ChoiceSettings GetSettings() const;
        void SetMemoryLife(const Core::Battery::MemoryLife& life);
        void SetFavoritesBoost(bool boost);

        /// Back to the bootstrap: theta etc. from the file; b from the file's
        /// items when `seedB` and sBootstrapBias allows it for `characterID`
        /// (the save's HCID), else empty; the timeline, the Plackett-Luce
        /// memory and the onset counter cleared. A load without THTA (seedB),
        /// `hg reset weights` / `hg reset all` / the dMenu reset (no seed: b
        /// cleared).
        void ResetToBootstrap(std::uint64_t characterID, bool seedB = true);

        /// A load that keeps the in-memory learning (a reload of the same
        /// character): the timeline, the Plackett-Luce memory and the onset
        /// counter belong to the session left behind.
        void ClearTransient();

        /// The latch PipelineCoordinator's skip gates read (with the old
        /// learner's): a pick moved theta or b, an import or a reset replaced
        /// them, or b entries were forgotten -- the ranking moved although no
        /// sensor did. Cleared at the gate's commit. The opportunity counter
        /// grows variances only (mu, and so the ranking, unchanged): no latch.
        [[nodiscard]] bool WeightsChanged() const noexcept { return m_weightsChanged.load(std::memory_order_acquire); }
        void ClearWeightsChanged() noexcept { m_weightsChanged.store(false, std::memory_order_release); }

        [[nodiscard]] std::shared_ptr<const Snapshot> GetSnapshot();

        /// recent_<family> at steady time `t` in load generation `gen`.
        [[nodiscard]] std::vector<double> RecentValues(const Snapshot& s, double t, std::uint32_t gen) const;

        /// One confirmed pick, as the selection log v3 recorded it (the same
        /// context and rows, so the choice set is fitlib's). `pickTime` is the
        /// decision moment on the steady clock.
        void OnPick(const Core::DecisionLog::Decision& d, double pickTime);

        /// The opportunity counter: the shadow need vector of a pipeline run
        /// and the attributes of what the page shows.
        void OnPipelineRun(const std::array<double, Core::Choice::K>& n, const std::vector<Core::Choice::Attr>& page);

        void AdvancePlayTime(float seconds) noexcept;
        [[nodiscard]] double GetPlaySeconds() const noexcept { return m_playSeconds.load(std::memory_order_relaxed); }
        /// At most once a minute of play: delete the b entries past their
        /// life, and end the discovery boosts that have run out.
        std::size_t MaybeForgetFaded();

        // ---- the discovery boost (core/DiscoveryBoost.h, 0.24.14) -------------
        // Runs whether or not the model is ready (its record is the
        // character's, not the model's); only the snapshot reads the boosts.
        /// A form the player just gained (`via`: "inventory" or "spell", for
        /// the log). Any thread (the container event's, the update thread).
        Core::Discovery::Tracker::Gain OnGained(std::uint32_t form, const char* via);
        /// A load starts: gains are dropped until PrimeDiscovery.
        void SuspendDiscovery();
        /// What the load found, and every form b holds an entry for (the
        /// character held it): seen, not boosted; dynamic forms that no longer
        /// exist dropped first; gains count from here. Game thread. Logs the
        /// per-load summary. Returns how many were new to the seen set.
        std::size_t PrimeDiscovery(const std::vector<std::uint32_t>& present);
        [[nodiscard]] Core::Discovery::Saved ExportDiscovery() const;
        /// Replaces the state with a save's (another character, a later save).
        void ImportDiscovery(const Core::Discovery::Saved& saved);
        /// A reload of the same character: in-memory kept, dynamic forms from
        /// the save. Returns {removed, added}.
        std::pair<std::size_t, std::size_t> ReplaceDynamicDiscovery(const Core::Discovery::Saved& saved);
        /// A new character.
        void ClearDiscovery();
        struct DiscoveryStats
        {
            std::size_t seen = 0;
            std::size_t boosted = 0;
            bool live = false;
        };
        [[nodiscard]] DiscoveryStats GetDiscoveryStats() const;
        /// The multiplier `form` reads now and its play seconds since the gain
        /// (`hg theta`); {1, -1} when not boosted.
        [[nodiscard]] std::pair<double, double> DiscoveryOf(std::uint32_t form) const;

        // ---- the cosave (persist/ChoiceSerializer.h, THTA / BIAS) ------------
        struct Entry
        {
            std::uint64_t key = 0;
            double mu = 0.0;
            double var = 0.0;
        };
        struct BEntry
        {
            std::uint32_t form = 0;
            double mu = 0.0;
            double var = 0.0;
            float n = 0.0f;
            std::uint32_t minutesSinceChosen = 0;
            bool favorite = false;
        };
        /// Every keyed weight, or (movedOnly, what THTA writes) only those
        /// whose {mu, var} differ from the bootstrap's: the load's merge gives
        /// a key the save lacks the file's value, so the result is the same.
        /// The saving is early only: every answered onset grows the variance
        /// of all of its need's theta and strength columns (GrowNeed, 2 x J =
        /// 486 entries), so a fight's worth of needs is thousands of entries
        /// and long play nears the full set (~36k, 0.87 MB).
        [[nodiscard]] std::vector<Entry> ExportEntries(bool movedOnly = false) const;
        /// Every key the build holds, in slot order (the load's merge).
        [[nodiscard]] std::vector<std::uint64_t> Keys() const;
        /// Applies every entry whose key the build knows; returns how many.
        std::size_t ImportEntries(const std::vector<Entry>& entries);
        [[nodiscard]] std::vector<BEntry> ExportB() const;
        /// Replaces b with these entries.
        void ImportB(const std::vector<BEntry>& entries);
        /// A reload of the same character keeps the in-memory b except for
        /// dynamic forms (0xFF......), which the engine can reuse: those come
        /// from the save (as BNDW's ReplaceDynamicEntries). Returns {removed,
        /// added}.
        std::pair<std::size_t, std::size_t> ReplaceDynamicB(const std::vector<BEntry>& saved);

        [[nodiscard]] Stats GetStats() const;

        /// R11: how far the live weights moved from the bootstrap
        /// (core/ThetaDrift.h): the theta block (need x column), the strength
        /// block when the file has one, b's spread, and theta_covered. For the
        /// [Soak] heartbeat's drift fields and `hg theta`.
        struct DriftReport
        {
            bool ready = false;
            Core::Choice::Drift theta;      // maxSlot = need * J + column
            Core::Choice::Drift strength;   // empty without a strength block
            Core::Choice::BDrift b;
            Core::Choice::Weight covered;
            Core::Choice::Weight coveredBoot;
        };
        [[nodiscard]] DriftReport GetDrift() const;

        /// R11 (`hg dump theta`, `hg theta`): one theta weight, live and as the
        /// bootstrap has it.
        struct ThetaRow
        {
            std::uint16_t need = 0;
            std::uint16_t col = 0;
            Core::Choice::Weight live;
            Core::Choice::Weight boot;
        };
        /// The theta block, need-major; `movedOnly` keeps the weights whose
        /// mean or variance differ from the bootstrap's.
        [[nodiscard]] std::vector<ThetaRow> ThetaRows(bool movedOnly) const;
        /// The file's created_utc and SHA-256 (the v3 head's "bootstrap").
        [[nodiscard]] std::pair<std::string, std::string> FileIdentity() const;
        [[nodiscard]] std::vector<Core::Bootstrap::BuildCurve> Curves() const;

    private:
        void BuildKeys();
        /// Slot `slot`'s weight differs from the bootstrap's (lock held).
        [[nodiscard]] bool MovedLocked(std::size_t slot) const;
        void Touch() noexcept { ++m_version; }
        /// A change to mu (theta or b): Touch and raise the latch.
        void Moved() noexcept
        {
            ++m_version;
            m_weightsChanged.store(true, std::memory_order_release);
        }

        mutable std::shared_mutex m_mutex;
        std::atomic<bool> m_ready{ false };
        ChoiceSettings m_settings;
        Core::Battery::MemoryLife m_life;
        bool m_favoritesBoost = true;

        // the file, mapped once
        std::optional<Core::Bootstrap::Document> m_doc;
        Core::Choice::Model m_bootModel;
        std::vector<Core::Bootstrap::BuildCurve> m_curves;
        std::vector<Core::Bootstrap::ResolvedItem> m_bootItems;
        std::string m_createdUtc, m_sha256;

        // the learner
        Core::Choice::Model m_model;
        std::unordered_map<std::uint32_t, Core::Choice::BBelief> m_b;
        std::vector<std::uint64_t> m_keys;                       // per slot (theta, strength, recent, covered)
        std::unordered_map<std::uint64_t, std::size_t> m_slotOf;  // key -> slot
        Core::Choice::RecentTracker m_recent;
        Core::Choice::OnsetCounter m_onsetCounter;
        std::deque<std::pair<std::uint64_t, std::set<std::uint64_t>>> m_taken;   // context id -> stacks taken
        std::uint64_t m_nextPickId = 1;
        Stats m_stats;
        Core::Discovery::Tracker m_discovery;

        std::uint64_t m_version = 1;
        std::atomic<bool> m_weightsChanged{ false };
        std::shared_ptr<const Snapshot> m_snapshot;
        std::atomic<double> m_playSeconds{ 0.0 };
        std::atomic<double> m_lastForgetSweep{ 0.0 };
    };
}

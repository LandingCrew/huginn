#pragma once

#include "TelemetrySettings.h"
#include "learning/EquipEvent.h"
#include "learning/ScoredCandidate.h"
#include "learning/StateFeatures.h"
#include "slot/SlotAssignment.h"
#include "context/ContextReason.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Huginn::Telemetry
{
    namespace Json { class Object; }

    // =========================================================================
    // DECISION LOG (opt-in offline telemetry)
    // =========================================================================
    // Writes a JSON Lines file of what Huginn showed the player and what the
    // player then did, so a player can VOLUNTARILY send it to the developers
    // for aggregation, offline training and replay evaluation. Default OFF
    // ([Telemetry] bEnabled = 0). Nothing is ever uploaded.
    //
    // Record types (schema v2; full field list in docs/architecture/9-telemetry.md):
    //   session  file header: schema, plugin version, random session id,
    //            feature names, reward constants. Written on every file open.
    //   cfg      scorer / fit / wildcard / equivalence-cap parameters that shape the policy.
    //   load     a save was loaded or a new game started (no names).
    //   imp      an impression: phi, the top-N scored candidates with full
    //            breakdowns and fit-relevant properties, and what each slot of
    //            the displayed page showed. Written only when the display
    //            CHANGES or right after a reward (CLAUDE.md "log transitions").
    //   rew      a reward event (equip via hotkey/Wheeler/external, consume,
    //            misclick penalty), carrying the id of the preceding impression.
    //   drop     N records were lost because the queue was full.
    //
    // THREADING
    //   Producers (the game main thread for impressions and hotkey/external
    //   equips, the Wheeler callback thread, the poll thread for consumption)
    //   build each record as a string -- they are the only side that touches
    //   game forms -- and push it onto a bounded queue. One background writer
    //   thread does ALL file IO and makes NO RE:: calls. A full queue drops the
    //   new record and counts it; the writer reports the count as a drop record.
    //   Stopping never joins on the caller's thread: it closes the queue (late
    //   records are dropped), and the writer abandons its batch after the
    //   current line; the next start joins it.
    //
    // LIFETIME
    //   Heap-allocated and intentionally never destroyed: joining a thread from
    //   a static destructor runs under the loader lock at process exit. The OS
    //   reclaims the writer at exit; each drained batch is flushed, so at most
    //   the in-flight batch is lost.
    //
    // PRIVACY: no item/character/save names, no paths, no OS or user info, no
    // wall-clock time. Only the inputs the scorer already uses (Core Principle).
    // =========================================================================

    inline constexpr int SCHEMA_VERSION = 3;   // v2: fit fields, eqk, delivery/skill props, fit+cap cfg
                                               // v3: ContextReason gains Cold, Hungry (#154)
    inline constexpr size_t QUEUE_CAPACITY = 4096;

    /// One pipeline tick's decision, as the pipeline hands it over. All
    /// references are only read during RecordImpression.
    struct ImpressionInput
    {
        const std::array<float, Learning::StateFeatures::NUM_FEATURES>& phi;
        const Scoring::ScoredCandidateList& scored;   // ranked prefix + unsorted tail + remembered-only extras
        const Slot::SlotAssignments& assignments;     // ctx.assignments (post-lock), the display page
        size_t pageIndex = 0;
        size_t pageCount = 0;
        Context::ContextReason reason = Context::ContextReason::None;
        bool overrideTookSlot = false;
    };

    class DecisionLog
    {
    public:
        static DecisionLog& GetSingleton();

        /// Start / stop the writer, reopen on a file-name change. Called at game
        /// load and from SettingsReloader::ApplySideEffects (hot reload / reset).
        void ApplyConfig(const TelemetryConfig& config);

        /// Cheap gate for every producer.
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }

        /// A save was loaded / a new game started. Resets the game-time base
        /// (always, so a later enable measures from the right place) and, when
        /// enabled, writes a "load" record.
        void OnGameLoaded(bool isNewGame);

        /// Transition-gated: writes only when the displayed page/slot contents
        /// changed since the last impression, or a reward happened since.
        void RecordImpression(const ImpressionInput& input);

        /// An equip/consume event reached the learner's subscriber list.
        /// @param appliedReward what the bandit was updated with (0 if not trained)
        void RecordReward(const Learning::EquipEvent& event, float appliedReward, bool trained);

        /// The previous pick was penalised as a misclick.
        void RecordMisclick(RE::FormID penalized, const Learning::StateFeatures& features, float penalty);

        struct Status
        {
            bool enabled = false;
            std::uint64_t written = 0;
            std::uint64_t dropped = 0;
            std::uint64_t fileBytes = 0;
            size_t queued = 0;
            std::string fileName;
        };
        [[nodiscard]] Status GetStatus() const;

    private:
        DecisionLog() = default;
        ~DecisionLog() = default;   // never runs: see LIFETIME above
        DecisionLog(const DecisionLog&) = delete;
        DecisionLog& operator=(const DecisionLog&) = delete;

        // ---- queue / writer --------------------------------------------------
        /// Bounded: when full the record is dropped and counted. Assigns "n".
        void Enqueue(std::string&& line);
        /// File IO ONLY here. No RE:: calls.
        void WriterLoop(std::stop_token stop);
        void StartWriterLocked();   // m_lifecycleMutex held
        void StopWriterLocked();    // m_lifecycleMutex held; closes the queue, does not join
        void ReapRetiredWriterLocked();  // m_lifecycleMutex held; joins a writer StopWriterLocked retired
        [[nodiscard]] std::string BuildSessionHeader();

        // ---- producer helpers (may touch forms) ------------------------------
        [[nodiscard]] std::string ItemKeyFor(RE::FormID formID);
        [[nodiscard]] std::int64_t RealMsSinceSession() const;
        /// Game seconds since the last load record; nullopt without a calendar.
        [[nodiscard]] std::optional<float> GameSecondsSinceLoad() const;
        [[nodiscard]] std::string BuildCfgRecord();
        void EnqueueReward(RE::FormID formID, std::string_view source, float multiplier,
                           bool wasRecommended, float reward, bool trained,
                           const Learning::StateFeatures& features);
        static void AppendCandidateProps(std::string& out, const Candidate::CandidateVariant& c);

        /// Item-context fit (Scoring::FitScorer) from the breakdown: "fit" and
        /// "fitOn" always; "fitCasts", "fitAfford", "fitRange" only when fit
        /// was computed for a spell (fitCastsLeft != -1).
        static void AppendFitFields(Json::Object& c, const Scoring::ScoreBreakdown& bd);
        /// Equivalence key (slot/EquivalenceKey.h) as "eqk": its string form for
        /// spells and scrolls, null otherwise. Written whether or not
        /// [SlotLocker] bCapEquivalents is on, using the current cost bands.
        static void AppendCapFields(Json::Object& c, const Candidate::CandidateVariant& candidate);

        // ---- lifecycle (m_lifecycleMutex) ------------------------------------
        mutable std::mutex m_lifecycleMutex;
        TelemetryConfig m_config{};
        std::jthread m_writer;
        /// A writer StopWriterLocked stopped but did not join (it may still be
        /// finishing its current line). Joined before the next start.
        std::jthread m_retiredWriter;
        std::atomic<bool> m_enabled{ false };

        // Read by the writer; written under m_lifecycleMutex while it is stopped
        // or via the atomics below.
        std::filesystem::path m_filePath;
        std::atomic<std::uint64_t> m_maxBytes{ 16ull << 20 };
        std::atomic<std::uint32_t> m_maxRotated{ 3 };
        std::atomic<std::uint32_t> m_topCandidates{ 20 };

        // ---- queue (m_queueMutex) --------------------------------------------
        mutable std::mutex m_queueMutex;
        std::condition_variable_any m_queueCv;
        std::deque<std::string> m_queue;
        std::uint64_t m_seq = 0;                        // next "n"
        /// True only while a writer is live. Enqueue rejects (and counts as
        /// dropped) every record while false, so none outlives its session.
        bool m_queueOpen = false;

        // ---- counters (status) -----------------------------------------------
        std::atomic<std::uint64_t> m_written{ 0 };
        std::atomic<std::uint64_t> m_dropped{ 0 };
        std::atomic<std::uint64_t> m_droppedUnreported{ 0 };
        std::atomic<std::uint64_t> m_fileBytes{ 0 };

        // ---- session identity (set once per process, at first enable) ----------
        std::string m_sessionId;
        std::chrono::steady_clock::time_point m_sessionStart{};
        std::atomic<bool> m_sessionStarted{ false };

        // Game-time base in days (Calendar::GetCurrentGameTime at the last load).
        // Atomic: written on the main thread, read by every producer. <0 = unset.
        std::atomic<double> m_gameTimeBaseDays{ -1.0 };

        // ---- producer state (m_producerMutex) --------------------------------
        struct SlotSig
        {
            size_t slot = 0;
            RE::FormID formID = 0;
            std::uint16_t uniqueID = 0;
            Slot::AssignmentType type = Slot::AssignmentType::Empty;
            bool operator==(const SlotSig&) const = default;
        };
        mutable std::mutex m_producerMutex;
        std::vector<SlotSig> m_lastSignature;
        size_t m_lastPage = SIZE_MAX;
        bool m_forceNextImpression = true;
        bool m_cfgPending = true;
        std::uint64_t m_impCounter = 0;
        std::uint64_t m_lastImpId = 0;                  // 0 = none yet

        // ---- item key cache (m_keyMutex) -------------------------------------
        std::mutex m_keyMutex;
        std::unordered_map<RE::FormID, std::string> m_keyCache;
    };

}  // namespace Huginn::Telemetry

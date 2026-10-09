#include "SlotAllocator.h"
#include "Remembrance.h"
#include "SlotLocker.h"
#include "override/OverrideConditions.h"
#include "override/OverrideConfig.h"
#include "telemetry/SoakMetrics.h"
#include <algorithm>
#include <format>
#include <set>

namespace Huginn::Slot
{
    // File-local helper: Should this override's log lines skip dedup entirely?
    // Unstamped (Unknown) conditions log un-deduped in DEBUG builds so the
    // missing stamp is loud right next to its tripwire warn; in RELEASE they
    // dedup normally through the Unknown row the log arrays already reserve
    // (an unexplained per-tick spam would be worse than a quiet alias).
    [[nodiscard]] static constexpr bool BypassDedup([[maybe_unused]] Override::OverrideCondition condition) noexcept
    {
#ifdef NDEBUG
        return false;
#else
        return condition == Override::OverrideCondition::Unknown;
#endif
    }

    SlotAllocator& SlotAllocator::GetSingleton()
    {
        static SlotAllocator instance;
        return instance;
    }

    void SlotAllocator::Initialize()
    {
        // v0.12.0: Allocation is now stateless - we just log initialization
        auto& settings = SlotSettings::GetSingleton();
        SKSE::log::info("[SlotAllocator] Initialized with {} page(s)", settings.GetPageCount());

        // Log each page's configuration
        for (size_t p = 0; p < settings.GetPageCount(); ++p) {
            const auto page = settings.GetPage(p);
            std::string slotSummary;
            for (size_t s = 0; s < page.slots.size(); ++s) {
                if (s > 0) slotSummary += "|";
                slotSummary += std::format("{}:p{}",
                    SlotClassificationToString(page.slots[s].classification),
                    page.slots[s].priority);
            }
            SKSE::log::info("[SlotAllocator] Page {} '{}': [{}]", p, page.name, slotSummary);
        }

        // Keep the player where they are. Initialize() runs at startup, where
        // m_currentPage is already 0, and again on every `hg reload` — so zeroing
        // it unconditionally moved a player off page 3 with no notice and no log
        // line. Only the reloaded layout having FEWER pages forces a move, and
        // that one is announced. SlotAllocator::Reset() remains the deliberate
        // "go to page 0" path, and still runs on game load.
        const size_t pageCount = settings.GetPageCount();
        const size_t currentPage = m_currentPage.load();
        if (pageCount == 0) {
            m_currentPage = 0;
        } else if (currentPage >= pageCount) {
            SKSE::log::info("[SlotAllocator] Page {} no longer exists ({} page(s) configured) — moving to page {}",
                currentPage, pageCount, pageCount - 1);
            m_currentPage = pageCount - 1;
            // A forced move IS a page change. Raising the flag keeps anything
            // caching "the page on screen" (WildcardManager) from going stale
            // while the pipeline is hash-skipped.
            m_pageChanged = true;
        }

        // Config changed (startup or reload): re-arm the unplaced-override warn
        // latch, then re-validate placeability against the new page layout.
        {
            std::lock_guard<std::mutex> logLock(m_logMutex);
            m_warnedUnplacedConditions.clear();
        }
        ValidateOverridePlaceability();
    }

    void SlotAllocator::Reset()
    {
        m_currentPage = 0;
        {
            std::lock_guard<std::mutex> logLock(m_logMutex);
            m_loggedMissingClassifications.clear();
            m_warnedUnplacedConditions.clear();
        }
        {
            // Seats are a memory of a layout that may not be the one coming
            // back: Reset runs on a game load, where the next save's inventory
            // and pages have nothing to do with this one's.
            //
            // It is NOT the reload path -- SettingsReloader calls Initialize(),
            // not this -- so the generation check in ApplySeating is what covers
            // an INI reload, and is not redundant with this.
            std::lock_guard<std::mutex> seatLock(m_seatingMutex);
            for (auto& page : m_seating) {
                page.fill(0);
            }
            for (auto& page : m_lastPlaced) {
                page.fill(0);
            }
            m_departed = {};
            m_homeClaims = {};
            m_seatingGeneration = UINT32_MAX;
        }
        SKSE::log::info("[SlotAllocator] Reset to page 0");
    }

    // =========================================================================
    // PAGE MANAGEMENT
    // =========================================================================

    void SlotAllocator::SetCurrentPage(size_t pageIndex)
    {
        size_t pageCount = GetPageCount();
        if (pageIndex >= pageCount) {
            SKSE::log::warn("[SlotAllocator] SetCurrentPage: {} out of range (max {}), clamping",
                pageIndex, pageCount - 1);
            pageIndex = pageCount > 0 ? pageCount - 1 : 0;
        }

        if (m_currentPage != pageIndex) {
            m_currentPage = pageIndex;
            m_pageChanged = true;

            // Clear all slot locks — stale locks from the previous page's context
            // would prevent the new page's allocations from appearing.
            SlotLocker::GetSingleton().UnlockAll();
            Telemetry::SoakMetrics::GetSingleton().RecordPageFlip();

            SKSE::log::info("[SlotAllocator] Switched to page {} '{}'",
                pageIndex, GetCurrentPageName());
        }
    }

    void SlotAllocator::NextPage()
    {
        size_t pageCount = GetPageCount();
        if (pageCount == 0) return;

        size_t nextPage = (m_currentPage + 1) % pageCount;
        SetCurrentPage(nextPage);
    }

    void SlotAllocator::PreviousPage()
    {
        size_t pageCount = GetPageCount();
        if (pageCount == 0) return;

        size_t prevPage = (m_currentPage == 0) ? (pageCount - 1) : (m_currentPage - 1);
        SetCurrentPage(prevPage);
    }

    size_t SlotAllocator::GetPageCount() const
    {
        return GetConfigSnapshot()->size();
    }

    // =========================================================================
    // CONFIGURATION ACCESS
    // =========================================================================

    std::shared_ptr<const std::vector<PageConfig>> SlotAllocator::GetConfigSnapshot(
        uint32_t* outGeneration) const
    {
        auto& settings = SlotSettings::GetSingleton();

        std::lock_guard<std::mutex> lock(m_cacheMutex);
        // GetGeneration() and GetAllPages() take SlotSettings' lock separately,
        // so a reload landing between them can leave m_cacheGeneration trailing
        // the copied data by one. That only ever forces a redundant rebuild on
        // the next call — never serves stale data — because the writer bumps the
        // generation only after committing pages, so a stored gen is always <=
        // the data's true generation and any newer reload re-trips the mismatch.
        const uint32_t gen = settings.GetGeneration();
        if (!m_configCache || m_cacheGeneration != gen) {
            m_configCache = std::make_shared<const std::vector<PageConfig>>(settings.GetAllPages());
            m_cacheGeneration = gen;
        }
        if (outGeneration) {
            *outGeneration = m_cacheGeneration;
        }
        return m_configCache;
    }

    size_t SlotAllocator::GetSlotCount() const
    {
        return GetSlotCount(m_currentPage);
    }

    size_t SlotAllocator::GetSlotCount(size_t pageIndex) const
    {
        auto snap = GetConfigSnapshot();
        if (pageIndex >= snap->size()) {
            return 0;
        }
        return (*snap)[pageIndex].slots.size();
    }

    size_t SlotAllocator::GetWildcardSlotCount(size_t pageIndex) const
    {
        auto snap = GetConfigSnapshot();
        if (pageIndex >= snap->size()) {
            return 0;
        }
        const auto& slots = (*snap)[pageIndex].slots;
        return static_cast<size_t>(std::count_if(slots.begin(), slots.end(),
            [](const SlotConfig& c) { return c.wildcardsEnabled; }));
    }

    bool SlotAllocator::IsRegularSlot(size_t pageIndex, size_t slotIndex) const
    {
        auto snap = GetConfigSnapshot();
        if (pageIndex >= snap->size()) return true;
        const auto& slots = (*snap)[pageIndex].slots;
        return slotIndex >= slots.size() ||
               slots[slotIndex].classification == SlotClassification::Regular;
    }

    SlotConfig SlotAllocator::GetSlotConfig(size_t slotIndex) const
    {
        const auto configs = GetSlotConfigs();
        if (slotIndex >= configs.size()) {
            SKSE::log::warn("[SlotAllocator] GetSlotConfig: slot {} out of range", slotIndex);
            return SlotConfig{};
        }
        return configs[slotIndex];
    }

    std::vector<SlotConfig> SlotAllocator::GetSlotConfigs() const
    {
        auto snap = GetConfigSnapshot();
        if (snap->empty()) {
            return {};
        }
        const size_t page = m_currentPage < snap->size() ? m_currentPage.load() : 0;
        return (*snap)[page].slots;
    }

    std::string SlotAllocator::GetPageName(size_t pageIndex) const
    {
        auto snap = GetConfigSnapshot();
        if (pageIndex >= snap->size()) {
            return "Page";
        }
        return (*snap)[pageIndex].name;
    }

    std::string SlotAllocator::GetCurrentPageName() const
    {
        return GetPageName(m_currentPage.load());
    }

    // =========================================================================
    // MAIN ALLOCATION API
    // =========================================================================

    SlotAssignments SlotAllocator::AllocateSlots(
        const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides,
        const State::PlayerActorState& player,
        const State::WorldState& world) const
    {
        // Delegate to page-specific allocation with current page
        return AllocateSlotsForPage(m_currentPage, candidates, overrides, player, world);
    }

    SlotAssignments SlotAllocator::AllocateSlotsForPage(
        size_t pageIndex,
        const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides,
        const State::PlayerActorState& player,
        const State::WorldState& world) const
    {
        // Hold the snapshot for the duration of the call so the config refs
        // passed to AllocateSlotsInternal stay valid even across a concurrent reload.
        uint32_t generation = UINT32_MAX;
        auto snap = GetConfigSnapshot(&generation);
        if (snap->empty()) {
            return {};
        }
        const size_t page = pageIndex < snap->size() ? pageIndex : 0;
        return AllocateSlotsInternal(page, generation, (*snap)[page].slots,
            candidates, overrides, player, world);
    }

    SlotAssignments SlotAllocator::AllocateSlots(
        const Scoring::ScoredCandidateList& candidates) const
    {
        // Call full version with empty overrides
        Override::OverrideCollection emptyOverrides;
        State::PlayerActorState dummyPlayer;
        State::WorldState dummyWorld;
        return AllocateSlots(candidates, emptyOverrides, dummyPlayer, dummyWorld);
    }

    // =========================================================================
    // INTERNAL IMPLEMENTATION
    // =========================================================================

    const char* SlotAllocator::NoteOverridePlaced(size_t page,
        Override::OverrideCondition condition, RE::FormID formID, size_t slot) const
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        auto& entry = m_overrideLogs[std::min(page, MAX_PAGES - 1)][static_cast<size_t>(condition)];
        const char* why = nullptr;
        if (BypassDedup(condition))        why = "unstamped condition (debug builds log every placement)";
        else if (entry.Empty())            why = "new placement";
        else if (entry.displaced)          why = "placed again after being displaced";
        else if (entry.formID != formID)   why = "different item";
        else if (entry.slot != slot)       why = "different slot";
        entry = { formID, slot, false };
        return why;
    }

    bool SlotAllocator::NoteOverrideDisplaced(size_t page, Override::OverrideCondition condition) const
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        auto& entry = m_overrideLogs[std::min(page, MAX_PAGES - 1)][static_cast<size_t>(condition)];
        const bool transition = BypassDedup(condition) || !entry.displaced;
        entry = { 0, SIZE_MAX, true };
        return transition;
    }

    void SlotAllocator::ResetOverrideLogs(size_t page) const
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        bool hadAny = false;
        for (auto& pageEntries : m_overrideLogs) {
            for (auto& entry : pageEntries) {
                hadAny |= !entry.Empty();
                entry = {};
            }
        }
        // Transition only: logs the moment the last override goes, not every
        // quiet tick. If a placement re-logs with "new placement" while an
        // override is plainly still up, this line just before it is the why.
        if (hadAny) {
            SKSE::log::debug("[SlotAllocator] Override log state reset (no override active, page {})", page);
        }
    }

    SlotAssignments SlotAllocator::AllocateSlotsInternal(
        size_t pageIndex,
        uint32_t configGeneration,
        const std::vector<SlotConfig>& slotConfigs,
        const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides,
        const State::PlayerActorState& player,
        [[maybe_unused]] const State::WorldState& world,
        bool forTest) const
    {
        namespace SA = Core::SlotAlloc;

        // One clock reading for the whole pass: home keys measure how long an
        // item was away against it, and departures are stamped with it.
        const auto now = std::chrono::steady_clock::now();

        SlotAssignments assignments;
        if (slotConfigs.empty()) {
            return assignments;
        }
        assignments.reserve(slotConfigs.size());

        if (slotConfigs.size() > MAX_SLOTS_PER_PAGE) {
            // Not silent: a page configured with more slots than the fixed cap
            // would otherwise drop the overflow with no trace.
            thread_local size_t s_lastWarnedCount = 0;
            if (slotConfigs.size() != s_lastWarnedCount) {
                SKSE::log::warn("[SlotAllocator] Page has {} slots, exceeding MAX_SLOTS_PER_PAGE ({}); "
                    "extra slots ignored", slotConfigs.size(), MAX_SLOTS_PER_PAGE);
                s_lastWarnedCount = slotConfigs.size();
            }
        }

#ifndef NDEBUG
        // Diagnostic: candidates by classification (logs only on change).
        size_t damageCount = 0, weaponCount = 0, buffCount = 0;
        for (const auto& c : candidates) {
            if (SlotClassifier::Matches(c, SlotClassification::DamageAny)) damageCount++;
            if (SlotClassifier::Matches(c, SlotClassification::WeaponsAny)) weaponCount++;
            if (SlotClassifier::Matches(c, SlotClassification::BuffsAny)) buffCount++;
        }
        thread_local size_t lastDamage = 0, lastWeapon = 0, lastBuff = 0, lastTotal = 0;
        if (damageCount != lastDamage || weaponCount != lastWeapon || buffCount != lastBuff || candidates.size() != lastTotal) {
            SKSE::log::debug("[SlotAllocator] Candidates: {} damage, {} weapon, {} buff (of {} total)",
                damageCount, weaponCount, buffCount, candidates.size());
            lastDamage = damageCount; lastWeapon = weaponCount; lastBuff = buffCount; lastTotal = candidates.size();
        }

        for (const auto& override : overrides.activeOverrides) {
            if (override.candidate && override.condition == Override::OverrideCondition::Unknown) {
                thread_local bool s_warnedUnstamped = false;
                if (!s_warnedUnstamped) {
                    SKSE::log::warn("[SlotAllocator] Override '{}' carries no OverrideCondition "
                        "stamp - its evaluator must set result.condition",
                        override.reason);
                    s_warnedUnstamped = true;
                }
            }
        }
#endif

        // =======================================================================
        // The decision: Core::SlotAlloc (src/core/SlotAllocCore.h) on the slot
        // scores (ScoredCandidate::SlotScore), which may have any sign. Its
        // passes, in order:
        //   1, 1b  overrides: marked where the page already shows the item,
        //          else the first override-enabled slot of their type, else
        //          the first slot that accepts their category;
        //   1a     Remembrance: what pressing a slot took off, held there;
        //   1c     the slot hold: seated items against near-tied challengers;
        //   2      the rank-ordered fill, with the class cap on Regular keys;
        //   3-5    seating (home keys, chains, swaps), the optional job-key
        //          pull, the refill of what seating vacated, and the record of
        //          where everything sat.
        // Why each rule exists is in the core's comments and
        // docs/architecture/5-slots.md. What happens here is the game's side:
        // the seating memory in and out, the assignments built from the
        // game's candidates, the logs, telemetry and Remembrance notes.
        // =======================================================================
        const SA::Settings settings = ReadAllocSettings();
        std::vector<size_t> overrideIndex;
        SA::Input in = BuildAllocInput(pageIndex, now, slotConfigs, candidates, overrides, &player, settings,
            &overrideIndex);
        in.memoryAvailable = pageIndex < MAX_PAGES;

#ifndef NDEBUG
        // Slot capture (SlotSnapshot.h): this allocation's input and result.
        std::optional<SA::Snapshot> capture;
        if (Capture::Enabled()) {
            capture.emplace();
            capture->tag = Capture::ThreadTag();
            Capture::NoteRealList(pageIndex, candidates, slotConfigs);
        }
#endif

        SA::Output out;
        {
            // Read, decided on and written back under one lock. The pipeline
            // (and Wheeler's pages in the same pass) runs under UpdateHandler's
            // mutex on the update loop's thread: a game job thread in gameplay,
            // the main thread in menus (UpdateLoop.cpp, THREADS above OnUpdate).
            // In Debug builds SlotCapture.cpp's campaign calls AllocateForTest
            // from an SKSE task, outside that mutex, while the update loop keeps
            // ticking, and nothing orders the two (in gameplay SKSE tasks ran
            // on job threads in an earlier verifier round, not in a Tracy
            // trace; see SlotCapture.cpp). So this lock is needed, not future-proofing. Tests.cpp's suites allocate
            // from the SKSE message handler (TestHarness.cpp), not under that
            // mutex either. Free when uncontended.
            std::lock_guard<std::mutex> lock(m_seatingMutex);
            in.generationMatches = m_seatingGeneration == configGeneration;
            if (in.memoryAvailable) {
                in.memory = MemoryOfLocked(pageIndex);
            }
#ifndef NDEBUG
            if (capture) {
                capture->in = in;
            }
#endif
            out = SA::Allocate(in);
            if (out.clearedAllPages) {
                // A layout reload: every page's seats describe slots that may
                // not mean the same thing now.
                for (auto& page : m_seating) page.fill(0);
                for (auto& page : m_lastPlaced) page.fill(0);
                m_departed = {};
                m_homeClaims = {};
            }
            if (in.memoryAvailable) {
                SetMemoryLocked(pageIndex, out.memory);
            }
            if (out.generationMatches) {
                m_seatingGeneration = configGeneration;
            }
        }

        // The page, from the game's candidates.
        for (size_t i = 0; i < out.slots.size(); ++i) {
            const auto& s = out.slots[i];
            const auto cls = slotConfigs[i].classification;
            switch (s.kind) {
            case SA::Kind::Empty:
                assignments.push_back(SlotAssignment::Empty(i, cls));
                break;
            case SA::Kind::Override: {
                // Told apart by its type, never by its score: the score is the
                // +inf sentinel (the old 1000.0 utility would overflow exp()).
                Scoring::ScoredCandidate sc;
                sc.candidate = *overrides.activeOverrides[overrideIndex[s.src]].candidate;
                sc.isWildcard = false;
                auto a = SlotAssignment::FromCandidate(i, cls, sc, AssignmentType::Override);
                a.score = Core::kPinnedScore;
                assignments.push_back(std::move(a));
                break;
            }
            case SA::Kind::Remembered: {
                Scoring::ScoredCandidate sc = candidates[s.src];
                sc.isWildcard = false;
                assignments.push_back(SlotAssignment::FromCandidate(i, cls, sc, AssignmentType::Remembered));
                break;
            }
            default:
                assignments.push_back(SlotAssignment::FromCandidate(i, cls, candidates[s.src],
                    s.kind == SA::Kind::Wildcard ? AssignmentType::Wildcard : AssignmentType::Normal));
                break;
            }
            assignments.back().seatMoved = s.seatMoved;
        }

        ReportEvents(pageIndex, slotConfigs, candidates, overrides, overrideIndex, out.events, forTest);

#ifndef NDEBUG
        // [Seating]: the seat map when it CHANGES -- a steady map is the
        // feature working. Read next to the [VisualState] line of the same
        // tick: seats are recorded from the allocator's own output, while
        // [VisualState] is what SlotLocker let through, so if the two disagree
        // the memory is chasing an arrangement the player never saw. A '*'
        // marks a seat whose owner sits somewhere else this tick (displaced by
        // an override, or standing in for one).
        if (out.seatingRecorded && out.memory.seats != out.seatsBeforeRecord) {
            auto keyOf = [](const SlotAssignment& a) -> uint64_t {
                if (a.IsEmpty() || !a.candidate) return 0;
                return Candidate::GetBase(a.candidate->candidate).GetDeduplicationKey();
            };
            const size_t slotCount = std::min(assignments.size(), MAX_SLOTS_PER_PAGE);
            std::string summary;
            for (size_t i = 0; i < slotCount; ++i) {
                const uint64_t seat = out.memory.seats[i];
                if (seat == 0) continue;
                std::string_view who = "?";
                bool elsewhere = true;
                for (size_t j = 0; j < slotCount; ++j) {
                    if (keyOf(assignments[j]) == seat) {
                        who = assignments[j].name;
                        elsewhere = (j != i);
                        break;
                    }
                }
                if (!summary.empty()) summary += " ";
                summary += std::format("{}={}{}", i, who, elsewhere ? "*" : "");
            }
            SKSE::log::debug("[Seating] page {} | {}", pageIndex, summary.empty() ? "(no seats)" : summary);
        }
#endif

        // What the class cap kept off the page, on change only.
        if (Core::ClassCapActive(settings.classDiscount) && pageIndex < MAX_PAGES) {
            std::vector<std::string> keptNames;
            for (const uint32_t i : out.skipped) {
                const RE::FormID id = in.candidates[i].formID;
                const bool shown = std::any_of(assignments.begin(), assignments.end(),
                    [id](const SlotAssignment& a) { return !a.IsEmpty() && a.formID == id; });
                if (!shown) {
                    keptNames.push_back(std::format("'{}' ({})", in.candidates[i].name,
                        SlotClassificationToString(static_cast<SlotClassification>(in.candidates[i].capClass))));
                }
            }
            std::sort(keptNames.begin(), keptNames.end());
            std::string kept;
            for (const auto& k : keptNames) {
                if (!kept.empty()) kept += ", ";
                kept += k;
            }
            std::lock_guard<std::mutex> logLock(m_logMutex);
            if (kept != m_classCapLog[pageIndex]) {
                if (kept.empty()) {
                    SKSE::log::debug("[SlotClassCap] Page {}: nothing kept off", pageIndex);
                } else {
                    SKSE::log::debug("[SlotClassCap] Page {}: kept off, {} or more of their class already shown: {}",
                        pageIndex, settings.classFree, kept);
                }
                m_classCapLog[pageIndex] = std::move(kept);
            }
        }

#ifndef NDEBUG
        if (capture) {
            SA::SetResult(*capture, out);
            Capture::Write(*capture);
        }
#endif

        return assignments;
    }

    void SlotAllocator::ReportEvents(
        size_t pageIndex,
        const std::vector<SlotConfig>& slotConfigs,
        const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides,
        const std::vector<size_t>& overrideIndex,
        const std::vector<Core::SlotAlloc::Event>& events,
        bool forTest) const
    {
        namespace SA = Core::SlotAlloc;
        auto overrideOf = [&](uint32_t o) -> const Override::OverrideResult& {
            return overrides.activeOverrides[overrideIndex[o]];
        };
        auto nameOf = [&](uint32_t src, bool isOverride) -> std::string_view {
            if (isOverride) {
                return Candidate::GetName(*overrideOf(src).candidate);
            }
            return candidates[src].GetName();
        };

        for (const auto& e : events) {
            switch (e.kind) {
            case SA::EventKind::OverrideMarked:
            case SA::EventKind::OverridePlaced:
            case SA::EventKind::OverrideFallback: {
                // Only log if the override changed (different item or slot, or
                // placed again after a displacement). The page index is not
                // decoration: allocation runs once per page (display page,
                // Wheeler pages), so one override legitimately logs several
                // lines a tick with different slots.
                const auto& ov = overrideOf(e.a);
                const RE::FormID formID = Candidate::GetFormID(*ov.candidate);
                if (const char* why = NoteOverridePlaced(pageIndex, ov.condition, formID, e.b)) {
                    if (e.kind == SA::EventKind::OverrideMarked) {
                        SKSE::log::info("[SlotAllocator] Override '{}' → Page {} Slot {} (marks the slot already showing it)",
                            ov.reason, pageIndex, e.b);
                    } else {
                        SKSE::log::info("[SlotAllocator] Override '{}' → Page {} Slot {} ({}{})",
                            ov.reason, pageIndex, e.b, e.kind == SA::EventKind::OverrideFallback ? "fallback, " : "",
                            SlotClassificationToString(slotConfigs[e.b].classification));
                    }
                    SKSE::log::debug("[SlotAllocator]   logged: {}", why);
                }
                break;
            }
            case SA::EventKind::OverrideUnplaced: {
                const auto& ov = overrideOf(e.a);
                const bool unstamped = BypassDedup(ov.condition);
                if (e.b != 0) {
                    // All accepting slots on THIS page are occupied (typically
                    // by higher-priority overrides) -- not a config problem.
                    // If the same contention holds on every page the override
                    // is starved config-wide, so the transition logs at info.
                    if (NoteOverrideDisplaced(pageIndex, ov.condition)) {
                        SKSE::log::info("[SlotAllocator] Override '{}' displaced on page {} "
                            "(accepting slots occupied by higher-priority overrides)",
                            ov.reason, pageIndex);
                    }
                } else if (!AnyPageAcceptsCategory(ov.category)) {
                    // No slot on ANY page accepts this category -- a genuine
                    // config gap. Warn once per condition: the reason string
                    // can embed live values (ammo counts), so it must not be
                    // the dedup key.
                    std::lock_guard<std::mutex> logLock(m_logMutex);
                    if (unstamped || m_warnedUnplacedConditions.insert(ov.condition).second) {
                        SKSE::log::warn("[SlotAllocator] Override '{}' could not be placed - "
                            "no slot on any page accepts category '{}'",
                            ov.reason, Override::OverrideCategoryToString(ov.category));
                    }
                }
                // else: another page accepts this category -- expected with
                // multi-page layouts; the override is visible there.
                break;
            }
            case SA::EventKind::OverridesInactive:
                // Truly inactive, not just unplaced on this page. Not for a test
                // allocation: its made-up overrides say nothing about the real
                // pages' log state.
                if (!forTest) {
                    ResetOverrideLogs(pageIndex);
                }
                break;
            case SA::EventKind::HoldNotCandidate: {
                // Not a candidate at all -- a shield, a torch, an unaffordable
                // spell. Logged once per item.
                thread_local RE::FormID s_lastMissing = 0;
                if (s_lastMissing != e.b) {
                    SKSE::log::debug("[Remembrance] Page {} Slot {}: {:08X} is not a candidate; slot fills normally",
                        pageIndex, e.a, e.b);
                    s_lastMissing = e.b;
                }
                break;
            }
            case SA::EventKind::HoldShown:
                // On a key whose class it does not fit, it shows briefly.
                Remembrance::GetSingleton().NoteShownSlot(pageIndex, e.a, e.b, e.c != 0);
                break;
            case SA::EventKind::NoCandidate: {
                // Rate-limited per classification.
                const auto cls = slotConfigs[e.a].classification;
                std::lock_guard<std::mutex> logLock(m_logMutex);
                if (m_loggedMissingClassifications.insert(cls).second) {
                    SKSE::log::info("[SlotAllocator] Slot {}: No {} candidate found",
                        e.a, SlotClassificationToString(cls));
                }
                break;
            }
            case SA::EventKind::HoldGaveWay: {
                // Scores, not utilities: the margin is a difference of scores
                // (ln m), which a negative score cannot turn upside down.
                const double holder = e.x + e.z;
                const double challenger = e.y + e.w;
                const double logMargin = Core::LogHoldMargin(SlotSettings::GetSingleton().ChallengerMargin());
                if (e.capped) {
                    SKSE::log::debug("[Hold] Page {} slot {}: '{}' gives way to '{}' (s={:+.3f} vs {:+.3f}; "
                        "class cap {:+.3f} vs {:+.3f}: {:+.3f} > ln m {:.3f})",
                        pageIndex, e.a, candidates[e.b].GetName(), candidates[e.c].GetName(), e.y, e.x,
                        e.w, e.z, challenger - holder, logMargin);
                } else {
                    SKSE::log::debug("[Hold] Page {} slot {}: '{}' gives way to '{}' (s={:+.3f} vs {:+.3f}, {:+.3f} > ln m {:.3f})",
                        pageIndex, e.a, candidates[e.b].GetName(), candidates[e.c].GetName(), e.y, e.x,
                        challenger - holder, logMargin);
                }
                break;
            }
            case SA::EventKind::PulledToJobKey:
                SKSE::log::debug("[SlotAllocator] Slot {} ({}) was empty: took '{}' from Regular slot {}",
                    e.a, SlotClassificationToString(slotConfigs[e.a].classification), candidates[e.c].GetName(), e.b);
                break;
            case SA::EventKind::Returner: {
                // Where each returner ended up: the heartbeat's
                // returns(home= wait= away=) for the displayed page, and one
                // line per return.
                const bool home = e.c == e.b;
                using Outcome = Telemetry::SoakMetrics::ReturnOutcome;
                if (pageIndex == GetCurrentPage()) {
                    Telemetry::SoakMetrics::GetSingleton().RecordReturn(
                        home ? Outcome::Home : e.why == SA::Away::Waits ? Outcome::Waiting : Outcome::Away);
                }
                const auto name = nameOf(e.a, e.x != 0.0);
                if (!home && e.why == SA::Away::Waits) {
                    SKSE::log::debug("[HomeKey] Page {}: '{}' back after {:.1f}s, waits on slot {} for slot {} "
                        "(an override or Remembrance holds it)",
                        pageIndex, name, e.f, e.c, e.b);
                } else if (home) {
                    // The gap-filler named at the claim, wherever it ended up.
                    const std::string moved = e.d != SA::kNone
                        ? std::format(" ('{}' moved to slot {})", nameOf(e.d, e.y != 0.0), e.e)
                        : std::string{};
                    SKSE::log::debug("[HomeKey] Page {}: '{}' back on slot {} after {:.1f}s{}",
                        pageIndex, name, e.c, e.f, moved);
                } else {
                    static constexpr std::array<std::string_view, 6> kWhy = {
                        "seating could not reach it", "home keys off", "a later leaver took it",
                        "it waits for it", "its class does not fit", "an override marks that key's own item" };
                    SKSE::log::debug("[HomeKey] Page {}: '{}' left slot {} {:.1f}s ago, now on slot {} ({})",
                        pageIndex, name, e.b, e.f, e.c, kWhy[static_cast<size_t>(e.why)]);
                }
                break;
            }
            }
        }
    }

#ifndef NDEBUG
    SlotAssignments SlotAllocator::AllocateForTest(size_t pageIndex, uint32_t generation,
        const std::vector<SlotConfig>& slotConfigs, const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides) const
    {
        const State::PlayerActorState noPlayer{};
        const State::WorldState noWorld{};
        return AllocateSlotsInternal(pageIndex, generation, slotConfigs, candidates, overrides, noPlayer, noWorld,
            /*forTest=*/true);
    }

    uint32_t SlotAllocator::CurrentGenerationForTest() const
    {
        uint32_t generation = UINT32_MAX;
        (void)GetConfigSnapshot(&generation);
        return generation;
    }
#endif

    // =========================================================================
    // SEATING MEMORY <-> the core's record
    // =========================================================================

    Core::SlotAlloc::PageMemory SlotAllocator::MemoryOfLocked(size_t pageIndex) const
    {
        Core::SlotAlloc::PageMemory m;
        if (pageIndex >= MAX_PAGES) {
            return m;
        }
        m.seats = m_seating[pageIndex];
        m.lastPlaced = m_lastPlaced[pageIndex];
        m.homeClaims = m_homeClaims[pageIndex];
        for (size_t j = 0; j < MAX_SLOTS_PER_PAGE; ++j) {
            for (size_t k = 0; k < HOME_MEMORY_PER_SLOT; ++k) {
                const auto& d = m_departed[pageIndex][j][k];
                m.departed[j][k] = { d.key, ToCoreNs(d.leftAt) };
            }
        }
        return m;
    }

    void SlotAllocator::SetMemoryLocked(size_t pageIndex, const Core::SlotAlloc::PageMemory& m) const
    {
        if (pageIndex >= MAX_PAGES) {
            return;
        }
        m_seating[pageIndex] = m.seats;
        m_lastPlaced[pageIndex] = m.lastPlaced;
        m_homeClaims[pageIndex] = m.homeClaims;
        for (size_t j = 0; j < MAX_SLOTS_PER_PAGE; ++j) {
            for (size_t k = 0; k < HOME_MEMORY_PER_SLOT; ++k) {
                const auto& d = m.departed[j][k];
                m_departed[pageIndex][j][k] = { d.key, FromCoreNs(d.leftAtNs) };
            }
        }
    }

    void SlotAllocator::PullIntoEmptyJobKeys(
        const std::vector<SlotConfig>& slotConfigs,
        SlotAssignments& assignments,
        const State::PlayerActorState* player,
        const std::array<size_t, MAX_SLOTS_PER_PAGE>& priorityOrder,
        size_t priorityCount) const
    {
        namespace SA = Core::SlotAlloc;
        // The page's own items are the candidates; an override's goes in as
        // an override (the pull never moves a pinned item, but its record has
        // to be there).
        Scoring::ScoredCandidateList items;
        Override::OverrideCollection pinned;
        std::vector<SA::SlotState> state(assignments.size());
        for (size_t i = 0; i < assignments.size(); ++i) {
            const auto& a = assignments[i];
            if (a.IsEmpty() || !a.candidate) {
                continue;
            }
            if (a.IsOverride()) {
                Override::OverrideResult r;
                r.candidate = a.candidate->candidate;
                state[i] = { SA::Kind::Override, static_cast<uint32_t>(pinned.activeOverrides.size()), a.seatMoved };
                pinned.activeOverrides.push_back(std::move(r));
                continue;
            }
            state[i] = { static_cast<SA::Kind>(a.type), static_cast<uint32_t>(items.size()), a.seatMoved };
            items.push_back(*a.candidate);
        }
        const SA::Settings settings = ReadAllocSettings();
        const SA::Input in = BuildAllocInput(0, std::chrono::steady_clock::now(), slotConfigs, items, pinned, player,
            settings, nullptr);

        std::array<size_t, SA::kMaxSlots> order{};
        for (size_t k = 0; k < std::min(priorityCount, SA::kMaxSlots); ++k) {
            order[k] = priorityOrder[k];
        }
        std::vector<SA::Event> events;
        (void)SA::PullIntoEmptyJobKeys(in, std::move(state), order, priorityCount,
            SA::LogScorePolicy::From(settings), &events);

        // Replay the moves on the real assignments, in order.
        for (const auto& e : events) {
            if (e.kind != SA::EventKind::PulledToJobKey) continue;
            const size_t to = e.a, from = e.b;
            SKSE::log::debug("[SlotAllocator] Slot {} ({}) was empty: took '{}' from Regular slot {}",
                to, SlotClassificationToString(slotConfigs[to].classification), assignments[from].name, from);
            assignments[to] = std::move(assignments[from]);
            assignments[to].slotIndex = to;
            assignments[to].classification = slotConfigs[to].classification;
            assignments[from] = SlotAssignment::Empty(from, slotConfigs[from].classification);
        }
    }

    // =========================================================================
    // OVERRIDE PLACEABILITY
    // =========================================================================

    bool SlotAllocator::AnyPageAcceptsCategory(Override::OverrideCategory category) const
    {
        auto snap = GetConfigSnapshot();
        for (const auto& page : *snap) {
            for (const auto& slot : page.slots) {
                if (AcceptsOverride(slot.overrideFilter, category)) {
                    return true;
                }
            }
        }
        return false;
    }

    void SlotAllocator::ValidateOverridePlaceability() const
    {
        using Override::OverrideCategory;

        struct ConditionCheck
        {
            bool enabled;
            std::string_view name;
            OverrideCategory category;
        };
        const ConditionCheck checks[] = {
            { Override::Config::ENABLE_CRITICAL_HEALTH(),  "CriticalHealth",  OverrideCategory::HP },
            { Override::Config::ENABLE_CRITICAL_MAGICKA(), "CriticalMagicka", OverrideCategory::MP },
            { Override::Config::ENABLE_CRITICAL_STAMINA(), "CriticalStamina", OverrideCategory::SP },
            { Override::Config::ENABLE_DROWNING(),         "Drowning",        OverrideCategory::Other },
            { Override::Config::ENABLE_WEAPON_CHARGE(),    "WeaponCharge",    OverrideCategory::Other },
            { Override::Config::ENABLE_LOW_AMMO(),         "LowAmmo",         OverrideCategory::Other },
        };

        std::string unplaceable;
        for (const auto& check : checks) {
            if (!check.enabled) continue;
            if (AnyPageAcceptsCategory(check.category)) continue;
            if (!unplaceable.empty()) unplaceable += ", ";
            unplaceable += check.name;
        }

        if (!unplaceable.empty()) {
            SKSE::log::warn("[SlotAllocator] Override condition(s) [{}] are enabled but no slot on "
                "any page accepts their category - they will never be shown. "
                "Check bOverridesEnabled under [PageN.SlotM] in Huginn.ini.",
                unplaceable);
            RE::DebugNotification(
                std::format("Huginn: override(s) [{}] have no accepting slot (see log)", unplaceable).c_str());
        }

        // Capacity heuristic: overrides compete per page, and the same
        // higher-priority conditions win on EVERY page — so the binding
        // constraint is the accepting-slot count of the single best page, not
        // the config-wide total. More enabled conditions than that means the
        // lowest-priority ones are starved config-wide whenever all fire at
        // once. The heuristic is one-sided: a warn means real risk, but
        // silence does not prove safety — Any slots are credited to every
        // category here yet hold only one override at runtime, so cross-
        // category contention can still starve. The runtime "displaced" log
        // is the backstop that confirms starvation if it actually happens.
        constexpr OverrideCategory kCategories[] = {
            OverrideCategory::HP, OverrideCategory::MP,
            OverrideCategory::SP, OverrideCategory::Other,
        };
        auto snap = GetConfigSnapshot();
        for (const auto category : kCategories) {
            size_t enabledCount = 0;
            for (const auto& check : checks) {
                if (check.enabled && check.category == category) ++enabledCount;
            }
            if (enabledCount == 0) continue;

            size_t maxAcceptingPerPage = 0;
            for (const auto& page : *snap) {
                size_t accepting = 0;
                for (const auto& slot : page.slots) {
                    if (AcceptsOverride(slot.overrideFilter, category)) ++accepting;
                }
                maxAcceptingPerPage = std::max(maxAcceptingPerPage, accepting);
            }

            // maxAcceptingPerPage == 0 is already covered by the unplaceable warn.
            if (maxAcceptingPerPage > 0 && enabledCount > maxAcceptingPerPage) {
                SKSE::log::warn("[SlotAllocator] {} enabled override condition(s) map to category "
                    "'{}' but at most {} slot(s) on any single page accept it - if several fire "
                    "at once, lower-priority overrides will not be shown",
                    enabledCount, Override::OverrideCategoryToString(category), maxAcceptingPerPage);
            }
        }
    }

}  // namespace Huginn::Slot

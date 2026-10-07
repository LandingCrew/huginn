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
    // Utility stamped on override slot assignments. The magnitude is cosmetic:
    // consumers identify overrides via AssignmentType::Override, never by
    // comparing utility — this value only makes them read as clearly
    // non-organic in debug output.
    static constexpr float kOverrideUtility = 1000.0f;

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

    // File-local helper: Does this slot's filter accept the given override category?
    [[nodiscard]] static constexpr bool AcceptsOverride(OverrideFilter filter, Override::OverrideCategory category) noexcept
    {
        switch (filter) {
            case OverrideFilter::None: return false;
            case OverrideFilter::Any:  return true;
            case OverrideFilter::HP:   return category == Override::OverrideCategory::HP;
            case OverrideFilter::MP:   return category == Override::OverrideCategory::MP;
            case OverrideFilter::SP:   return category == Override::OverrideCategory::SP;
            case OverrideFilter::Other: return category == Override::OverrideCategory::Other;
            default:                   return false;
        }
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
        [[maybe_unused]] const State::WorldState& world) const
    {
        SlotAssignments assignments;
        assignments.reserve(slotConfigs.size());

        // Initialize all slots as empty
        for (size_t i = 0; i < slotConfigs.size(); ++i) {
            assignments.push_back(SlotAssignment::Empty(i, slotConfigs[i].classification));
        }

        if (slotConfigs.empty()) {
            return assignments;
        }

        // Compute priority order for this config (fixed-size, no heap allocation)
        std::array<size_t, MAX_SLOTS_PER_PAGE> priorityOrder;
        const size_t priorityCount = ComputePriorityOrder(slotConfigs, priorityOrder);

        // Diagnostic: Count candidates by classification (logs only on change, debug level)
        // Use thread_local for thread safety in case of multi-threaded access
#ifndef NDEBUG
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
#endif

        // Track which candidates have been assigned (by FormID and name)
        // Name tracking prevents duplicate enchanted items (different FormIDs, same name)
        std::set<RE::FormID> assignedFormIDs;
        std::set<std::string_view> assignedNames;

        // =======================================================================
        // PASS 1: Assign overrides to MATCHING slots first
        // =======================================================================
        // Track last override assignment to avoid log spam (only log on change).
        // Keyed per page × condition: the same override lands on different slot
        // indices on different pages, and two overrides active on ONE page
        // (e.g. Drowning + CriticalHealth) must not alternate-overwrite a shared
        // entry — either way a coarser key ping-pongs and logs every tick.
        // `displaced` records that the condition lost its accepting slots to
        // higher-priority overrides, so starvation logs on transition only.
        // Unknown (unstamped) conditions bypass dedup in DEBUG builds only
        // (see BypassDedup) — loud next to the tripwire warn there, deduped
        // normally through the reserved Unknown row in release.
        // State lives in m_overrideLogs (NoteOverridePlaced and friends).
        bool overrideAssignedThisFrame = false;

        if (overrides.HasActiveOverride()) {
            for (const auto& override : overrides.activeOverrides) {
                if (!override.candidate) continue;

#ifndef NDEBUG
                if (override.condition == Override::OverrideCondition::Unknown) {
                    thread_local bool s_warnedUnstamped = false;
                    if (!s_warnedUnstamped) {
                        SKSE::log::warn("[SlotAllocator] Override '{}' carries no OverrideCondition "
                            "stamp - its evaluator must set result.condition",
                            override.reason);
                        s_warnedUnstamped = true;
                    }
                }
#endif

                RE::FormID formID = Candidate::GetFormID(*override.candidate);
                if (assignedFormIDs.contains(formID)) continue;

                // If the item is ALREADY on this page, the override marks that
                // slot -- label and pulse on the key the player already knows --
                // instead of putting a second copy in its configured slot. The
                // copy was the worst churn left: dedup bounced the potion
                // between the two slots four times a second, and the item the
                // configured slot held was pushed out and evicted another
                // (2026-09-27 19:03:07, 19:08:56; Minor Healing in slot 7 moved
                // to slot 0 at 19:14:16 when slot 7 could have just been marked).
                //
                // Except the VITALS, which by default keep their configured
                // slots: in an emergency the player should not have to find the
                // potion -- the key is muscle memory. Health first (2026-09-27
                // 19:26, user's call: marking it where it stood put it on key 7);
                // magicka and stamina joined when the flagship page gave every
                // key a job and the magicka emergency pulsed the Potion key, not
                // key 2 (2026-09-28 18:00:27). Each is an INI switch
                // ([Overrides] bPin{Health,Magicka,Stamina}ToSlot). The quieter
                // prompts -- ammo, soul gem, drowning -- always mark in place,
                // where a still bar is worth more.
                using OC = Override::OverrideCondition;
                const bool pinnedToSlot =
                    (override.condition == OC::CriticalHealth && Override::Config::PIN_HEALTH_TO_SLOT()) ||
                    (override.condition == OC::CriticalMagicka && Override::Config::PIN_MAGICKA_TO_SLOT()) ||
                    (override.condition == OC::CriticalStamina && Override::Config::PIN_STAMINA_TO_SLOT());
                if (const size_t home = pinnedToSlot ? SIZE_MAX : FindItemSlot(pageIndex, configGeneration,
                        Candidate::GetBase(*override.candidate).GetDeduplicationKey(),
                        std::min(slotConfigs.size(), MAX_SLOTS_PER_PAGE));
                    home != SIZE_MAX && assignments[home].IsEmpty()) {
                    Scoring::ScoredCandidate sc;
                    sc.candidate = *override.candidate;
                    sc.utility = kOverrideUtility;
                    sc.isWildcard = false;
                    auto marked = SlotAssignment::FromCandidate(
                        home, slotConfigs[home].classification, sc, AssignmentType::Override);
                    if (SlotAccepts(slotConfigs[home], marked, &player)) {
                        assignments[home] = std::move(marked);
                        assignedFormIDs.insert(formID);
                        assignedNames.insert(Candidate::GetName(*override.candidate));
                        overrideAssignedThisFrame = true;

                        if (const char* why = NoteOverridePlaced(pageIndex, override.condition, formID, home)) {
                            SKSE::log::info("[SlotAllocator] Override '{}' → Page {} Slot {} (marks the slot already showing it)",
                                override.reason, pageIndex, home);
                            SKSE::log::debug("[SlotAllocator]   logged: {}", why);
                        }
                        continue;
                    }
                }

                // Find first override-enabled slot that MATCHES this override's type
                for (size_t k = 0; k < priorityCount; ++k) {
                    const size_t priorityIdx = priorityOrder[k];
                    const auto& config = slotConfigs[priorityIdx];
                    if (!AcceptsOverride(config.overrideFilter, override.category)) continue;
                    if (!assignments[priorityIdx].IsEmpty()) continue;  // Already filled

                    if (SlotClassifier::Matches(*override.candidate, config.classification)) {
                        Scoring::ScoredCandidate sc;
                        sc.candidate = *override.candidate;
                        sc.utility = kOverrideUtility;
                        sc.isWildcard = false;

                        assignments[priorityIdx] = SlotAssignment::FromCandidate(
                            priorityIdx, config.classification, sc, AssignmentType::Override);
                        assignedFormIDs.insert(formID);
                        assignedNames.insert(Candidate::GetName(*override.candidate));
                        overrideAssignedThisFrame = true;

                        // Only log if override changed (different formID or slot,
                        // or re-placed after a displacement)
                        if (const char* why = NoteOverridePlaced(pageIndex, override.condition, formID, priorityIdx)) {
                            // Page index is not decoration: allocation runs once
                            // per page (display page here, Wheeler pages in
                            // WheelerBackend), so one override legitimately logs
                            // several lines a tick with different slots. Without
                            // the page that reads as the same override being
                            // placed twice, which cost a wrong diagnosis once.
                            SKSE::log::info("[SlotAllocator] Override '{}' → Page {} Slot {} ({})",
                                override.reason, pageIndex, priorityIdx,
                                SlotClassificationToString(config.classification));
                            SKSE::log::debug("[SlotAllocator]   logged: {}", why);
                        }
                        break;
                    }
                }
            }

            // PASS 1b: Fallback - unassigned overrides go to first empty slot that accepts their category
            for (const auto& override : overrides.activeOverrides) {
                if (!override.candidate) continue;

                RE::FormID formID = Candidate::GetFormID(*override.candidate);
                if (assignedFormIDs.contains(formID)) continue;  // Already placed

                // Find first empty slot that accepts this override category
                bool placed = false;
                bool sawAcceptingSlot = false;  // page accepts the category; slots were just occupied
                for (size_t k = 0; k < priorityCount; ++k) {
                    const size_t priorityIdx = priorityOrder[k];
                    const auto& config = slotConfigs[priorityIdx];
                    if (!AcceptsOverride(config.overrideFilter, override.category)) continue;
                    sawAcceptingSlot = true;
                    if (!assignments[priorityIdx].IsEmpty()) continue;

                    Scoring::ScoredCandidate sc;
                    sc.candidate = *override.candidate;
                    sc.utility = kOverrideUtility;
                    sc.isWildcard = false;

                    assignments[priorityIdx] = SlotAssignment::FromCandidate(
                        priorityIdx, config.classification, sc, AssignmentType::Override);
                    assignedFormIDs.insert(formID);
                    assignedNames.insert(Candidate::GetName(*override.candidate));
                    overrideAssignedThisFrame = true;
                    placed = true;

                    // Only log if override changed (different formID or slot,
                    // or re-placed after a displacement)
                    if (const char* why = NoteOverridePlaced(pageIndex, override.condition, formID, priorityIdx)) {
                        SKSE::log::info("[SlotAllocator] Override '{}' → Page {} Slot {} (fallback, {})",
                            override.reason, pageIndex, priorityIdx,
                            SlotClassificationToString(config.classification));
                        SKSE::log::debug("[SlotAllocator]   logged: {}", why);
                    }
                    break;
                }

                if (!placed) {
                    const bool unstamped = BypassDedup(override.condition);
                    if (sawAcceptingSlot) {
                        // All accepting slots on THIS page are occupied (typically
                        // by higher-priority overrides) — not a config problem.
                        // But if the same contention holds on every page, the
                        // override is starved config-wide, so log the transition
                        // at info: it must be diagnosable from release logs.
                        if (NoteOverrideDisplaced(pageIndex, override.condition)) {
                            SKSE::log::info("[SlotAllocator] Override '{}' displaced on page {} "
                                "(accepting slots occupied by higher-priority overrides)",
                                override.reason, pageIndex);
                        }
                    } else if (!AnyPageAcceptsCategory(override.category)) {
                        // No slot on ANY page accepts this category — a genuine
                        // config gap. Warn once per condition: the reason string
                        // can embed live values (e.g. ammo counts), so it must
                        // not be the dedup key.
                        std::lock_guard<std::mutex> logLock(m_logMutex);
                        if (unstamped || m_warnedUnplacedConditions.insert(override.condition).second) {
                            SKSE::log::warn("[SlotAllocator] Override '{}' could not be placed - "
                                "no slot on any page accepts category '{}'",
                                override.reason,
                                Override::OverrideCategoryToString(override.category));
                        }
                    }
                    // else: another page accepts this category — expected with
                    // multi-page layouts; the override is visible there (the
                    // player reaches it by paging / opening that wheel).
                }
            }
        }

        // Reset tracking only when overrides are truly inactive (not just unassigned
        // on this particular page). With multi-page support, Page 1 may have no
        // override-eligible slots, but the override is still active on Page 0.
        if (!overrides.HasActiveOverride()) {
            ResetOverrideLogs(pageIndex);
        }

        // =======================================================================
        // PASS 1a: Remembrance -- what pressing a slot took off, held there
        // =======================================================================
        // A rule, not a ranking: it takes the slot the player pressed whatever
        // that slot's classification or bSkipEquipped says, and only an
        // override (placed above) outranks it. A hold whose item goes back in
        // a hand some other way is ended by Remembrance itself.
        //
        // sRemembranceTarget = Job sends an item that does not fit the
        // pressed key to the first empty swap-back key whose class fits it
        // (a dagger taken off by the attack-magic key goes to the Weapon key).
        // Two sweeps so a routed item cannot take a key another hold was
        // pressed on: the ones staying put are placed first.
        {
            auto& remembrance = Remembrance::GetSingleton();
            const auto& slotSettings = SlotSettings::GetSingleton();
            const bool toJob = slotSettings.RemembranceToJobKey();
            const auto held = remembrance.GetPage(pageIndex);
            const size_t n = std::min(slotConfigs.size(), MAX_SLOTS_PER_PAGE);
            for (int sweep = 0; sweep < 2; ++sweep)
            for (size_t j = 0; j < n; ++j) {
                const auto& entry = held[j];
                if (!entry.Active() || !slotConfigs[j].remembrance) continue;
                if (assignedFormIDs.contains(entry.formID)) continue;  // shown already (an override, or sweep 0)

                // The best-scoring stack of a weapon owned twice. Only a prefix
                // of the list is sorted, so compare rather than take the first.
                const Scoring::ScoredCandidate* found = nullptr;
                for (const auto& c : candidates) {
                    if (c.GetFormID() == entry.formID && (!found || c.utility > found->utility)) {
                        found = &c;
                    }
                }
                if (!found) {
                    // Not a candidate at all -- a shield, a torch, an
                    // unaffordable spell. Logged once per item.
                    thread_local RE::FormID s_lastMissing = 0;
                    if (s_lastMissing != entry.formID) {
                        SKSE::log::debug("[Remembrance] Page {} Slot {}: {:08X} is not a candidate; slot fills normally",
                            pageIndex, j, entry.formID);
                        s_lastMissing = entry.formID;
                    }
                    continue;
                }
                auto fits = [&](size_t k) {
                    return slotConfigs[k].classification == SlotClassification::Regular ||
                           SlotClassifier::Matches(*found, slotConfigs[k].classification);
                };
                size_t target = j;
                if (toJob && !fits(j)) {
                    if (sweep == 0) continue;
                    for (size_t k = 0; k < priorityCount; ++k) {
                        const size_t t = priorityOrder[k];
                        if (t >= n || t == j || !slotConfigs[t].remembrance) continue;
                        if (slotConfigs[t].classification == SlotClassification::Regular) continue;
                        // Another hold was pressed there, and that key is its
                        // fallback: taking it could leave that item nowhere
                        // (/code-review #151).
                        if (held[t].Active()) continue;
                        if (!assignments[t].IsEmpty() || !fits(t)) continue;
                        target = t;
                        break;
                    }
                } else if (sweep == 1) {
                    continue;
                }
                if (!assignments[target].IsEmpty()) continue;  // an override has the slot

                // On a key whose class it does not fit -- the pressed key under
                // Pressed, or Job with no key that fits -- it shows briefly.
                remembrance.NoteShownSlot(pageIndex, j, target, !fits(target));
                Scoring::ScoredCandidate sc = *found;
                sc.isWildcard = false;
                assignments[target] = SlotAssignment::FromCandidate(
                    target, slotConfigs[target].classification, sc, AssignmentType::Remembered);
                assignedFormIDs.insert(entry.formID);
                assignedNames.insert(found->GetName());
            }
        }

        // The per-need soft cap on Regular keys (NeedCap.h), from here on:
        // what overrides and Remembrance placed above counts against it, but
        // they are never moved for it.
        NeedCap needCap(SlotSettings::GetSingleton().NeedRepeatDiscount(),
            SlotSettings::GetSingleton().NeedFreeSlots(), &candidates);
        needCap.Recount(assignments);

        // =======================================================================
        // PASS 1c: Hold seated items against near-tied challengers
        // =======================================================================
        {
            const auto& settings = SlotSettings::GetSingleton();
            if (settings.KeepSlotPositions() && settings.HoldSeatedItems()) {
                HoldIncumbents(pageIndex, configGeneration, slotConfigs, candidates, assignments,
                    assignedFormIDs, assignedNames, &player, settings.ChallengerMargin(),
                    priorityOrder, priorityCount, needCap);
            }
        }

        // =======================================================================
        // PASS 2: Fill remaining slots with candidates by classification
        // =======================================================================
        for (size_t k = 0; k < priorityCount; ++k) {
            const size_t priorityIdx = priorityOrder[k];
            const auto& config = slotConfigs[priorityIdx];
            auto& assignment = assignments[priorityIdx];

            // Skip if already filled (by override)
            if (!assignment.IsEmpty()) continue;

            // Find best matching candidate
            auto bestCandidate = FindBestCandidate(
                candidates, config.classification, assignedFormIDs, assignedNames, config.skipEquipped, &player,
                /*skipWildcards=*/false, &needCap);

            if (!bestCandidate) {
                // Rate-limit "no candidate found" logs per classification type
                std::lock_guard<std::mutex> logLock(m_logMutex);
                if (m_loggedMissingClassifications.find(config.classification) == m_loggedMissingClassifications.end()) {
                    SKSE::log::info("[SlotAllocator] Slot {}: No {} candidate found",
                        priorityIdx, SlotClassificationToString(config.classification));
                    m_loggedMissingClassifications.insert(config.classification);
                }
            }

            if (bestCandidate) {
                // Determine assignment type
                AssignmentType assignType = bestCandidate->isWildcard
                    ? AssignmentType::Wildcard
                    : AssignmentType::Normal;

                // Honor wildcard setting: if this slot forbids wildcards, re-run
                // the search restricted to non-wildcard candidates. Reusing
                // FindBestCandidate (rather than an inline scan) means the fallback
                // also honors skipEquipped, and returns nullopt cleanly when no
                // alternative exists — so the wildcard is NOT left assigned to a
                // wildcard-disabled slot.
                if (assignType == AssignmentType::Wildcard && !config.wildcardsEnabled) {
                    bestCandidate = FindBestCandidate(
                        candidates, config.classification, assignedFormIDs,
                        assignedNames, config.skipEquipped, &player,
                        /*skipWildcards=*/true, &needCap);
                    assignType = AssignmentType::Normal;
                }

                if (bestCandidate) {
                    assignment = SlotAssignment::FromCandidate(
                        priorityIdx,
                        config.classification,
                        *bestCandidate,
                        assignType);
                    assignedFormIDs.insert(bestCandidate->GetFormID());
                    assignedNames.insert(bestCandidate->GetName());
                    needCap.Add(*bestCandidate);
                }
            }
        }

        if (!SlotSettings::GetSingleton().KeepSlotPositions() &&
            SlotSettings::GetSingleton().FillJobKeysFromRegular()) {
            PullIntoEmptyJobKeys(slotConfigs, assignments, &player, priorityOrder, priorityCount);

            // Without seating there is no pass 4, so refill the Regular key the
            // pull just emptied here, or it stays blank (/code-review #151).
            needCap.Recount(assignments);
            for (size_t k = 0; k < priorityCount; ++k) {
                const size_t idx = priorityOrder[k];
                if (!assignments[idx].IsEmpty()) continue;
                const auto& config = slotConfigs[idx];
                auto refill = FindBestCandidate(
                    candidates, config.classification, assignedFormIDs, assignedNames,
                    config.skipEquipped, &player, /*skipWildcards=*/!config.wildcardsEnabled, &needCap);
                if (!refill) continue;
                assignments[idx] = SlotAssignment::FromCandidate(
                    idx, config.classification, *refill,
                    refill->isWildcard ? AssignmentType::Wildcard : AssignmentType::Normal);
                assignedFormIDs.insert(refill->GetFormID());
                assignedNames.insert(refill->GetName());
                needCap.Add(*refill);
            }
        }

        // =======================================================================
        // PASS 3: Seat returning items where they were (anti-juggling)
        // =======================================================================
        // Passes 1 and 2 decided WHICH items the player sees; this decides WHERE.
        // Without it, one item arriving or leaving shifts every item below it by
        // a slot -- the recommendations stay right and every key under the
        // player's fingers changes meaning.
        if (SlotSettings::GetSingleton().KeepSlotPositions()) {
            // The generation of the snapshot slotConfigs came from, not whatever
            // the cache holds now: a reload on another thread between the two
            // would stamp seats taken under this layout with the number of the
            // next one, and the mismatch check below would then miss it.
            const uint32_t generation = configGeneration;

            ApplySeating(pageIndex, generation, slotConfigs, assignments, &player);

            // Optional: a key with a job that would be blank takes a matching
            // item off a Regular key. After seating, so seating does not undo
            // it; before the refill, which then fills the Regular key.
            if (SlotSettings::GetSingleton().FillJobKeysFromRegular()) {
                PullIntoEmptyJobKeys(slotConfigs, assignments, &player, priorityOrder, priorityCount);
            }

            // PASS 4: refill whatever pass 3 vacated. An item moving back to its
            // own seat can leave the slot it was sitting in empty, and a gap in
            // the middle of the widget is a worse trade than the shuffle this
            // whole thing exists to prevent. The dedup sets are still the ones
            // pass 2 built, so this cannot re-place an item already on screen.
            needCap.Recount(assignments);
            for (size_t k = 0; k < priorityCount; ++k) {
                const size_t priorityIdx = priorityOrder[k];
                auto& assignment = assignments[priorityIdx];
                if (!assignment.IsEmpty()) continue;

                const auto& config = slotConfigs[priorityIdx];
                auto refill = FindBestCandidate(
                    candidates, config.classification, assignedFormIDs, assignedNames,
                    config.skipEquipped, &player, /*skipWildcards=*/!config.wildcardsEnabled, &needCap);
                if (!refill) continue;

                assignment = SlotAssignment::FromCandidate(
                    priorityIdx, config.classification, *refill,
                    refill->isWildcard ? AssignmentType::Wildcard : AssignmentType::Normal);
                assignedFormIDs.insert(refill->GetFormID());
                assignedNames.insert(refill->GetName());
                needCap.Add(*refill);
            }

            // PASS 5: remember the result for next time.
            RecordSeating(pageIndex, generation, assignments);
        }

        // What the need cap kept off the page, on change only.
        if (needCap.Active() && pageIndex < MAX_PAGES) {
            std::string kept = needCap.Summary(assignments);
            std::lock_guard<std::mutex> logLock(m_logMutex);
            if (kept != m_needCapLog[pageIndex]) {
                if (kept.empty()) {
                    SKSE::log::debug("[NeedCap] Page {}: nothing kept off", pageIndex);
                } else {
                    SKSE::log::debug("[NeedCap] Page {}: kept off, {} or more of their need already shown: {}",
                        pageIndex, SlotSettings::GetSingleton().NeedFreeSlots(), kept);
                }
                m_needCapLog[pageIndex] = std::move(kept);
            }
        }

        return assignments;
    }

#ifndef NDEBUG
    SlotAssignments SlotAllocator::AllocateForTest(size_t pageIndex, uint32_t generation,
        const std::vector<SlotConfig>& slotConfigs, const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides) const
    {
        const State::PlayerActorState noPlayer{};
        const State::WorldState noWorld{};
        return AllocateSlotsInternal(pageIndex, generation, slotConfigs, candidates, overrides, noPlayer, noWorld);
    }
#endif

    // =========================================================================
    // SEATING (anti-juggling)
    // =========================================================================

    bool SlotAllocator::SlotAccepts(
        const SlotConfig& config,
        const SlotAssignment& assignment,
        const State::PlayerActorState* player)
    {
        if (assignment.IsEmpty() || !assignment.candidate) {
            return true;  // nothing to place; any slot will hold a gap
        }
        const auto& sc = *assignment.candidate;

        if (!SlotClassifier::Matches(sc, config.classification)) {
            return false;
        }
        if (sc.isWildcard && !config.wildcardsEnabled) {
            return false;
        }
        if (config.skipEquipped) {
            const auto& base = Candidate::GetBase(sc.candidate);
            if (base.isEquipped) {
                return false;
            }
            if (player && player->IsItemEquipped(base.formID)) {
                return false;
            }
        }
        return true;
    }

    size_t SlotAllocator::FindItemSlot(
        size_t pageIndex, uint32_t generation, uint64_t key, size_t slotCount) const
    {
        if (key == 0 || pageIndex >= MAX_PAGES || !SlotSettings::GetSingleton().KeepSlotPositions()) {
            return SIZE_MAX;
        }
        std::lock_guard<std::mutex> lock(m_seatingMutex);
        if (m_seatingGeneration != generation) {
            return SIZE_MAX;
        }
        const size_t n = std::min(slotCount, MAX_SLOTS_PER_PAGE);
        // Its own seat first; failing that, where it stood last pass (a guest).
        for (size_t j = 0; j < n; ++j) {
            if (m_seating[pageIndex][j] == key) return j;
        }
        for (size_t j = 0; j < n; ++j) {
            if (m_lastPlaced[pageIndex][j] == key) return j;
        }
        return SIZE_MAX;
    }

    void SlotAllocator::HoldIncumbents(
        size_t pageIndex,
        uint32_t generation,
        const std::vector<SlotConfig>& slotConfigs,
        const Scoring::ScoredCandidateList& candidates,
        SlotAssignments& assignments,
        std::set<RE::FormID>& assignedFormIDs,
        std::set<std::string_view>& assignedNames,
        const State::PlayerActorState* player,
        float margin,
        const std::array<size_t, MAX_SLOTS_PER_PAGE>& priorityOrder,
        size_t priorityCount,
        NeedCap& needCap) const
    {
        if (pageIndex >= MAX_PAGES) {
            return;
        }
        const size_t slotCount = std::min(assignments.size(),
            std::min(slotConfigs.size(), MAX_SLOTS_PER_PAGE));

        std::array<uint64_t, MAX_SLOTS_PER_PAGE> seats{};
        std::array<uint64_t, MAX_SLOTS_PER_PAGE> placed{};
        std::array<uint64_t, MAX_SLOTS_PER_PAGE> claims{};
        {
            std::lock_guard<std::mutex> lock(m_seatingMutex);
            // A stale generation means a layout reload; ApplySeating clears
            // the map for this pass, and there is nothing to hold.
            if (m_seatingGeneration != generation) {
                return;
            }
            seats = m_seating[pageIndex];
            placed = m_lastPlaced[pageIndex];
            claims = m_homeClaims[pageIndex];
        }

        // Phase A: which seat owners are still candidates, and still allowed
        // in their seat? An item the slot has given up -- now equipped under
        // skip-equipped, a wildcard in a slot that refuses them -- is not held,
        // however well it scores: holding it would keep something the slot
        // itself just rejected.
        struct Tentative { size_t slot; const Scoring::ScoredCandidate* item; };
        std::array<Tentative, MAX_SLOTS_PER_PAGE> tentative{};
        size_t tentativeCount = 0;
        std::set<RE::FormID> excludedIDs = assignedFormIDs;
        std::set<std::string_view> excludedNames = assignedNames;

        // Priority order, the order the fill uses: when two held slots want
        // the same challenger, the one the fill would have served first gets it.
        for (size_t k = 0; k < priorityCount; ++k) {
            const size_t j = priorityOrder[k];
            if (j >= slotCount) continue;
            if (!assignments[j].IsEmpty() || seats[j] == 0) {
                continue;  // an override took the slot, or nobody owns it
            }
            const Scoring::ScoredCandidate* owner = nullptr;
            for (const auto& c : candidates) {
                if (!c.isRememberedOnly &&
                    Candidate::GetBase(c.candidate).GetDeduplicationKey() == seats[j]) {
                    owner = &c;
                    break;
                }
            }
            if (!owner || assignedFormIDs.contains(owner->GetFormID()) ||
                assignedNames.contains(owner->GetName())) {
                continue;  // left the candidates, or an override is showing it
            }
            const auto probe = SlotAssignment::FromCandidate(j, slotConfigs[j].classification, *owner);
            if (!SlotAccepts(slotConfigs[j], probe, player)) {
                continue;
            }
            tentative[tentativeCount++] = { j, owner };
            excludedIDs.insert(owner->GetFormID());
            excludedNames.insert(owner->GetName());
        }

        // Owners whose seat an override is sitting in stay out of the
        // challengers. Free to challenge, the Iron Dagger an override pushed
        // out of slot 1 beat Sparks in slot 4, Sparks landed in slot 3, and
        // it all ran backwards when the override ended -- one override, six
        // slot changes (2026-09-27 19:08:30-37). It is still held where it
        // stands (the guests below), and goes home when the override ends.
        std::set<RE::FormID> displacedOwners;
        for (size_t j = 0; j < slotCount; ++j) {
            // A remembered item sitting in someone's seat displaces its owner
            // the same way an override does.
            if (!assignments[j].IsPinned() || seats[j] == 0 || !assignments[j].candidate) continue;
            if (Candidate::GetBase(assignments[j].candidate->candidate).GetDeduplicationKey() == seats[j]) {
                continue;  // the pinned item IS the owner: it is in its own slot
            }
            for (const auto& c : candidates) {
                if (Candidate::GetBase(c.candidate).GetDeduplicationKey() == seats[j]) {
                    excludedIDs.insert(c.GetFormID());
                    excludedNames.insert(c.GetName());
                    displacedOwners.insert(c.GetFormID());
                    break;
                }
            }
        }

        // Guests: an item standing in a slot that is not its seat, because an
        // override occupies the seat. Without this it was a free challenger
        // every pass -- the mace whose seat the health potion held beat Wine
        // in slot 5, then the sword in slot 1, then Wine again, on every run
        // for half a minute (2026-09-26 14:28:49-14:29:20). Held where it
        // stands, under the same test, until the override ends and seating
        // takes it home.
        //
        // The displaced owners above are exactly these items, and the loop
        // above put them in the excluded sets -- which this loop read as
        // "already held", so a guest was never held: it went to whichever
        // empty slot the fill reached first, and a lock left on its old slot
        // cleared and refilled that slot twice in 0.4 s (Raw Crab Meat,
        // waiting for an override on its home key, LoreRim 2026-10-06
        // 21:07:58). Home keys make such guests common.
        for (size_t k = 0; k < priorityCount; ++k) {
            const size_t j = priorityOrder[k];
            if (j >= slotCount || !assignments[j].IsEmpty() || placed[j] == 0) continue;
            bool taken = false;
            for (size_t t = 0; t < tentativeCount; ++t) {
                taken = taken || tentative[t].slot == j;
            }
            if (taken) continue;

            const Scoring::ScoredCandidate* guest = nullptr;
            for (const auto& c : candidates) {
                if (!c.isRememberedOnly &&
                    Candidate::GetBase(c.candidate).GetDeduplicationKey() == placed[j]) {
                    guest = &c;
                    break;
                }
            }
            if (!guest) {
                continue;  // gone
            }
            if (displacedOwners.contains(guest->GetFormID())) {
                if (assignedFormIDs.contains(guest->GetFormID()) || assignedNames.contains(guest->GetName())) {
                    continue;  // an override or Remembrance hold is showing it
                }
            } else if (excludedIDs.contains(guest->GetFormID()) || excludedNames.contains(guest->GetName())) {
                continue;  // shown by an override, or already held in its own seat
            }
            const auto probe = SlotAssignment::FromCandidate(j, slotConfigs[j].classification, *guest);
            if (!SlotAccepts(slotConfigs[j], probe, player)) {
                continue;
            }
            tentative[tentativeCount++] = { j, guest };
            excludedIDs.insert(guest->GetFormID());
            excludedNames.insert(guest->GetName());
        }

        // Owners and guests were gathered in two sweeps; judge them in one
        // slot-priority order.
        std::array<size_t, MAX_SLOTS_PER_PAGE> rank{};
        for (size_t k = 0; k < priorityCount; ++k) {
            if (priorityOrder[k] < MAX_SLOTS_PER_PAGE) rank[priorityOrder[k]] = k;
        }
        std::sort(tentative.begin(), tentative.begin() + tentativeCount,
            [&rank](const Tentative& a, const Tentative& b) { return rank[a.slot] < rank[b.slot]; });

        // The need cap over the holders TOGETHER, by utility, before any is
        // judged. Counted slot by slot instead, a challenger for an early slot
        // could not see the holders of its need further down: the axe took
        // slot 0 while the sword and mace still held slots 1 and 4, and the
        // page showed four weapons (vanilla, 2026-10-06 17:13:24) -- or the
        // axe came in at slot 0 only for the sword to be capped out of slot 7,
        // a swap of one weapon for another (17:14:11). Each holder's factor is
        // its rank in its need; a holder being judged is taken out of the
        // count, so a challenger of the same need is weighed as it would be.
        std::array<float, MAX_SLOTS_PER_PAGE> holderCap{};
        holderCap.fill(1.0f);
        {
            std::array<size_t, MAX_SLOTS_PER_PAGE> byUtility{};
            for (size_t t = 0; t < tentativeCount; ++t) byUtility[t] = t;
            std::stable_sort(byUtility.begin(), byUtility.begin() + tentativeCount,
                [&tentative](size_t a, size_t b) { return tentative[a].item->utility > tentative[b].item->utility; });
            for (size_t i = 0; i < tentativeCount; ++i) {
                const auto& [slot, item] = tentative[byUtility[i]];
                if (slotConfigs[slot].classification == SlotClassification::Regular) {
                    holderCap[byUtility[i]] = needCap.Factor(*item);
                }
                needCap.Add(*item);
            }
        }

        // Phase B: each holder against the best challenger for its own slot.
        // Challengers exclude every other holder -- an item staying put in
        // slot 3 is not about to move into slot 5 -- and every challenger
        // already given a slot this pass.
        //
        // A winner is placed on the spot and reserved. Deciding the release
        // and leaving the placement to the fill let two held slots lose to
        // the SAME challenger: one got it, the other gave up its item for
        // nothing (2026-09-26 14:04:35, slots 3 and 6 both yielding to one
        // Resist Cold). And the loser gives up its seat, or the rule "you keep
        // your seat while you are on screen" hands it straight back next pass
        // and the same release repeats every run -- 939 [Hold] lines in ten
        // minutes on the first build.
        const float factor = 1.0f + margin;
        for (size_t t = 0; t < tentativeCount; ++t) {
            const auto [j, item] = tentative[t];
            const auto& config = slotConfigs[j];

            // A wildcard is not challenged on score. WildcardManager swaps it
            // into a rank POSITION and leaves its own low utility in place,
            // so the fill (which walks the list in order) treats it as that
            // rank while a utility comparison always finds it beaten. Held on
            // score, it was released on every pass and re-placed by position
            // somewhere else, evicting that slot's item -- the Woodcutter's
            // Axe went slot 7 -> 3 and Flames lost its slot mid-fight, six
            // wildcard moves in twelve seconds (2026-09-27 19:02:40-46). It
            // stays until WildcardManager ends it (the flag clears) or the
            // slot stops accepting it (Phase A).
            if (item->isWildcard) {
                assignments[j] = SlotAssignment::FromCandidate(j, config.classification, *item,
                    AssignmentType::Wildcard);
                assignedFormIDs.insert(item->GetFormID());
                assignedNames.insert(item->GetName());
                continue;
            }

            // On a Regular key both sides are weighed after the need cap: a
            // holder past its need's free count holds at its capped utility,
            // or a crowd that formed before the cap applied would be held
            // there for good.
            needCap.Remove(*item);
            const size_t skipMark = needCap.SkipMark();
            const auto challenger = FindBestCandidate(candidates, config.classification,
                excludedIDs, excludedNames, config.skipEquipped, player,
                /*skipWildcards=*/!config.wildcardsEnabled, &needCap);
            const bool capped = config.classification == SlotClassification::Regular;
            const float itemCap = capped ? holderCap[t] : 1.0f;
            const float challengerCap = capped && challenger ? needCap.Factor(*challenger) : 1.0f;
            const float itemScore = item->utility * itemCap;
            const float challengerScore = challenger ? challenger->utility * challengerCap : 0.0f;

            if (challenger && challengerScore > itemScore * factor) {
                needCap.Add(*challenger);
                if (itemCap < 1.0f || challengerCap < 1.0f) {
                    SKSE::log::debug("[Hold] Page {} slot {}: '{}' gives way to '{}' (u={:.3f} vs {:.3f}; "
                        "need cap x{:.2f} vs x{:.2f}: x{:.2f} > x{:.2f})",
                        pageIndex, j, item->GetName(), challenger->GetName(), challenger->utility, item->utility,
                        challengerCap, itemCap, itemScore > 0.0f ? challengerScore / itemScore : 0.0f, factor);
                } else {
                    SKSE::log::debug("[Hold] Page {} slot {}: '{}' gives way to '{}' (u={:.3f} vs {:.3f}, x{:.2f} > x{:.2f})",
                        pageIndex, j, item->GetName(), challenger->GetName(), challenger->utility, item->utility,
                        item->utility > 0.0f ? challenger->utility / item->utility : 0.0f, factor);
                }

                assignments[j] = SlotAssignment::FromCandidate(j, config.classification, *challenger,
                    challenger->isWildcard ? AssignmentType::Wildcard : AssignmentType::Normal);
                assignedFormIDs.insert(challenger->GetFormID());
                assignedNames.insert(challenger->GetName());
                excludedIDs.insert(challenger->GetFormID());
                excludedNames.insert(challenger->GetName());

                // The loser is free again -- the fill may show it elsewhere --
                // but not as this seat's owner. A guest owns a seat somewhere
                // else, and the seat here is not its to give up. An owner an
                // override displaced stays out of the challengers even so
                // (code review of #179): freed, it could take another held
                // slot -- the one-override, six-changes cascade.
                if (!displacedOwners.contains(item->GetFormID())) {
                    excludedIDs.erase(item->GetFormID());
                    excludedNames.erase(item->GetName());
                }
                const uint64_t loserKey = Candidate::GetBase(item->candidate).GetDeduplicationKey();
                if (seats[j] == loserKey) {
                    std::lock_guard<std::mutex> lock(m_seatingMutex);
                    if (m_seatingGeneration == generation && m_seating[pageIndex][j] == loserKey) {
                        m_seating[pageIndex][j] = 0;
                    }
                }
                continue;
            }

            assignments[j] = SlotAssignment::FromCandidate(j, config.classification, *item,
                item->isWildcard ? AssignmentType::Wildcard : AssignmentType::Normal);
            // A home claimant arriving at its key from where it waited: the
            // lock still showing it there may let go (SlotLocker).
            {
                const uint64_t key = Candidate::GetBase(item->candidate).GetDeduplicationKey();
                assignments[j].seatMoved = claims[j] == key && placed[j] != key;
            }
            assignedFormIDs.insert(item->GetFormID());
            assignedNames.insert(item->GetName());
            needCap.Add(*item);   // back in the count it left to be judged
            // The search was hypothetical: an item it passed for the cap was
            // kept off by the cap only if, uncapped, it would have taken the
            // slot from this holder.
            needCap.DropSkipsSince(skipMark, itemScore * factor);
        }
    }

    void SlotAllocator::ApplySeating(
        size_t pageIndex,
        uint32_t generation,
        const std::vector<SlotConfig>& slotConfigs,
        SlotAssignments& assignments,
        const State::PlayerActorState* player) const
    {
        if (pageIndex >= MAX_PAGES) {
            return;
        }
        const size_t slotCount = std::min(assignments.size(),
            std::min(slotConfigs.size(), MAX_SLOTS_PER_PAGE));
        if (slotCount == 0) {
            return;
        }

        // Work on a copy: the pass reads one consistent snapshot of where things
        // sat, and the lock is not held across the moves.
        std::array<uint64_t, MAX_SLOTS_PER_PAGE> seats{};
        std::array<std::array<Departure, HOME_MEMORY_PER_SLOT>, MAX_SLOTS_PER_PAGE> departed{};
        std::array<uint64_t, MAX_SLOTS_PER_PAGE> claims{};
        {
            std::lock_guard<std::mutex> lock(m_seatingMutex);
            if (m_seatingGeneration != generation) {
                // A layout reload happened; last pass's seats describe slots that
                // may not mean the same thing now. Start over from this pass.
                for (auto& page : m_seating) {
                    page.fill(0);
                }
                for (auto& page : m_lastPlaced) {
                    page.fill(0);
                }
                m_departed = {};
                m_homeClaims = {};
                m_seatingGeneration = generation;
                return;
            }
            seats = m_seating[pageIndex];
            departed = m_departed[pageIndex];
            claims = m_homeClaims[pageIndex];
        }

        auto keyOf = [](const SlotAssignment& a) -> uint64_t {
            if (a.IsEmpty() || !a.candidate) return 0;
            return Candidate::GetBase(a.candidate->candidate).GetDeduplicationKey();
        };

        // Which slot does this item want? At most one seat per key and one key
        // per seat, so no two items can want the same slot -- the mapping is
        // injective by construction, and nothing has to arbitrate.
        auto seatWantedBy = [&](uint64_t key) -> size_t {
            if (key == 0) return SIZE_MAX;
            for (size_t j = 0; j < slotCount; ++j) {
                if (seats[j] == key) return j;
            }
            return SIZE_MAX;
        };

        // An override sits where the override pass put it, which was a decision
        // about that slot. Nothing moves it, and nothing moves into it.
        // A remembered item likewise: it is under the key the player pressed.
        auto movable = [&](size_t idx) {
            return !assignments[idx].IsPinned();
        };

        // Only a home claimant's move is marked (code review of #179): every
        // other seating move waits out the lock on its old slot, as before.
        auto moveTo = [&](size_t from, size_t to) {
            const bool claimant = claims[to] != 0 && claims[to] == keyOf(assignments[from]);
            assignments[to] = std::move(assignments[from]);
            assignments[to].seatMoved = claimant;
            assignments[to].slotIndex = to;
            assignments[to].classification = slotConfigs[to].classification;
            assignments[from] = SlotAssignment::Empty(from, slotConfigs[from].classification);
        };

        // Phase 0: home keys. A seat is freed the moment its owner leaves the
        // screen, so an item that drops off for a few seconds -- outranked
        // through a burst of refreshes -- came back to whatever key was open:
        // 1,073 of 1,365 returns within ten minutes landed on a different key
        // (21 logs, 2026-09-30 to 10-06), most of them within a minute.
        //
        // A RETURNER is on screen with no seat and left a slot of this page
        // within fHomeKeyMemorySec. It takes that slot's seat back from the
        // item that filled the gap (the user's rule, 2026-10-06: whoever
        // arrived after the returner left only filled the gap), and the
        // phases below move it there -- into the slot if empty, else by the
        // phase-2 swap, which puts the gap-filler where the returner landed.
        // Only where the move can happen: the home slot takes the returner,
        // and its occupant fits the returner's slot. Otherwise the returner
        // keeps the key it landed on, which becomes its home -- a
        // recommendation is never hidden to wait for a key. Two returners for
        // one slot: the later leaver wins.
        //
        // Blocked only by an override or a Remembrance hold -- both pass --
        // the returner gets the right of first refusal (the user, 2026-10-06):
        // it claims the seat and waits, shown where it landed, as an item an
        // override displaced already does. When the slot frees, the slot hold
        // seats it there unless a challenger beats it by the margin; if it
        // leaves the page meanwhile, the claim lapses with it. 17 of 22 misses
        // on LoreRim's first run were this case (2026-10-06 20:14-20:25).
        //
        // Not when the pinned item IS that slot's seat owner -- an override
        // marking its own seat (code review of #179): taking the seat would
        // make the override jump to its configured slot mid-emergency.
        enum class Away : uint8_t { None, Off, Taken, Waits, Class, Owned };
        struct Returner
        {
            uint64_t key;
            size_t at;
            size_t home;
            float awaySec;
            Away why;
            uint64_t displaced = 0;   // the gap-filler on the home slot, for the log
        };
        std::array<Returner, MAX_SLOTS_PER_PAGE> returners{};
        size_t returnerCount = 0;
        const auto& slotSettings = SlotSettings::GetSingleton();
        const float memorySec = slotSettings.HomeKeyMemorySec();
        if (memorySec > 0.0f) {
            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < slotCount; ++i) {
                const uint64_t key = keyOf(assignments[i]);
                if (key == 0 || !movable(i) || seatWantedBy(key) != SIZE_MAX) continue;
                size_t home = SIZE_MAX;
                std::chrono::steady_clock::time_point leftAt{};
                for (size_t j = 0; j < slotCount; ++j) {
                    for (const auto& d : departed[j]) {
                        if (d.key == key && (home == SIZE_MAX || d.leftAt > leftAt)) {
                            home = j;
                            leftAt = d.leftAt;
                        }
                    }
                }
                const float awaySec = std::chrono::duration<float>(now - leftAt).count();
                if (home == SIZE_MAX || awaySec > memorySec) continue;
                returners[returnerCount++] = { key, i, home, awaySec, Away::None, 0 };
            }
            std::sort(returners.begin(), returners.begin() + returnerCount,
                [](const Returner& a, const Returner& b) { return a.awaySec < b.awaySec; });
        }

        bool seatsReclaimed = false;
        {
            std::array<bool, MAX_SLOTS_PER_PAGE> claimed{};
            const bool returnHome = slotSettings.ReturnToHomeKey();
            for (size_t r = 0; r < returnerCount; ++r) {
                auto& ret = returners[r];
                const size_t h = ret.home;
                if (!returnHome) { ret.why = Away::Off; continue; }
                if (claimed[h]) { ret.why = Away::Taken; continue; }
                if (ret.at != h) {
                    if (!SlotAccepts(slotConfigs[h], assignments[ret.at], player)) {
                        ret.why = Away::Class;
                        continue;
                    }
                    if (!movable(h)) {
                        if (keyOf(assignments[h]) == seats[h]) {
                            ret.why = Away::Owned;
                            continue;
                        }
                        ret.why = Away::Waits;   // claims the seat; phases 1-2 leave a pinned slot alone
                    } else if (!assignments[h].IsEmpty() &&
                               !SlotAccepts(slotConfigs[ret.at], assignments[h], player)) {
                        ret.why = Away::Class;
                        continue;
                    }
                    if (movable(h)) {
                        ret.displaced = keyOf(assignments[h]);
                    }
                }
                claimed[h] = true;
                seats[h] = ret.key;   // the gap-filler's claim ends; it takes a seat where it lands
                claims[h] = ret.key;
                seatsReclaimed = true;
            }
        }
        if (returnerCount > 0) {
            // Written back now, not left to RecordSeating: that keeps "you keep
            // your seat while on screen", and the gap-filler -- still on screen
            // -- would otherwise keep the seat it just lost.
            //
            // Every returner's departure is spent, whatever the outcome (code
            // review of #179): one kept would make the item a returner again
            // on every pass while it stays seatless -- a [HomeKey] line and a
            // counted return ten times a second -- and could later send it to
            // that stale key instead of its latest one.
            std::lock_guard<std::mutex> lock(m_seatingMutex);
            if (m_seatingGeneration == generation) {
                if (seatsReclaimed) {
                    m_seating[pageIndex] = seats;
                    m_homeClaims[pageIndex] = claims;
                }
                for (size_t r = 0; r < returnerCount; ++r) {
                    for (auto& slot : m_departed[pageIndex]) {
                        for (auto& d : slot) {
                            if (d.key == returners[r].key) d = {};
                        }
                    }
                }
            }
        }

        // Phase 1: moves into empty seats, repeated.
        //
        // Repeated because the common case is a chain, not a swap: one item
        // leaves, everything below it moves up a slot, and putting them back
        // starts with the last one dropping into the hole at the bottom, which
        // frees the seat the one above it wants, and so on up. One round per
        // slot is enough for any chain, and the loop stops as soon as nothing
        // moved.
        for (size_t round = 0; round < slotCount; ++round) {
            bool moved = false;
            for (size_t i = 0; i < slotCount; ++i) {
                if (assignments[i].IsEmpty() || !movable(i)) continue;

                const size_t want = seatWantedBy(keyOf(assignments[i]));
                if (want == SIZE_MAX || want == i || want >= slotCount) continue;
                if (!assignments[want].IsEmpty() || !movable(want)) continue;
                if (!SlotAccepts(slotConfigs[want], assignments[i], player)) continue;

                moveTo(i, want);
                moved = true;
            }
            if (!moved) break;
        }

        // Phase 2: two items holding each other's seats. A chain cannot resolve
        // that -- neither seat is ever empty -- and it is what a pure reorder of
        // an unchanged set looks like.
        //
        // Repeated, for the same reason phase 1 is. The sweep runs top to bottom
        // and a swap can throw an item BELOW the index it has already passed,
        // where nothing comes back for it: seen 2026-09-19 23:53:38, where the
        // potion swapping home to slot 7 put the Iron Sword in slot 4, the
        // dagger swapping home to 4 then put the sword in 6, and the sword --
        // whose seat was 5, free of anything but a newcomer -- sat in the wrong
        // slot for 3.5 s until an unrelated pipeline run picked it up.
        //
        // It terminates: a swap only ever fires when the item at `i` can reach
        // its OWN seat, and the seat map is injective, so the item it displaces
        // was not in its own seat to begin with (that slot belongs to the mover)
        // and is not put into one by being moved. Every swap therefore increases
        // the number of items sitting in their own seat by at least one, and
        // that number cannot exceed the slot count.
        for (size_t round = 0; round < slotCount; ++round) {
            bool swapped = false;
            for (size_t i = 0; i < slotCount; ++i) {
                if (assignments[i].IsEmpty() || !movable(i)) continue;

                const size_t want = seatWantedBy(keyOf(assignments[i]));
                if (want == SIZE_MAX || want == i || want >= slotCount) continue;
                if (assignments[want].IsEmpty() || !movable(want)) continue;

                // Only when BOTH are legal in the other's slot. Half a swap would
                // put something in a slot its classification forbids.
                if (!SlotAccepts(slotConfigs[want], assignments[i], player)) continue;
                if (!SlotAccepts(slotConfigs[i], assignments[want], player)) continue;

                const bool claimant = claims[want] != 0 && claims[want] == keyOf(assignments[i]);
                std::swap(assignments[i], assignments[want]);
                assignments[i].seatMoved = claimant;      // the gap-filler, swapped out
                assignments[want].seatMoved = claimant;   // the claimant, home
                assignments[i].slotIndex = i;
                assignments[i].classification = slotConfigs[i].classification;
                assignments[want].slotIndex = want;
                assignments[want].classification = slotConfigs[want].classification;
                swapped = true;
            }
            if (!swapped) break;
        }

        // Where each returner ended up: the heartbeat's returns(home= wait= away=)
        // for the displayed page, and one line per return.
        for (size_t r = 0; r < returnerCount; ++r) {
            const auto& ret = returners[r];
            size_t now = SIZE_MAX;
            for (size_t i = 0; i < slotCount; ++i) {
                if (keyOf(assignments[i]) == ret.key) { now = i; break; }
            }
            if (now == SIZE_MAX) continue;
            const bool home = now == ret.home;
            using Outcome = Telemetry::SoakMetrics::ReturnOutcome;
            if (pageIndex == GetCurrentPage()) {
                Telemetry::SoakMetrics::GetSingleton().RecordReturn(
                    home ? Outcome::Home : ret.why == Away::Waits ? Outcome::Waiting : Outcome::Away);
            }
            if (!home && ret.why == Away::Waits) {
                SKSE::log::debug("[HomeKey] Page {}: '{}' back after {:.1f}s, waits on slot {} for slot {} "
                    "(an override or Remembrance holds it)",
                    pageIndex, assignments[now].name, ret.awaySec, now, ret.home);
            } else if (home) {
                // The gap-filler named at the claim, wherever it ended up.
                std::string moved;
                for (size_t i = 0; ret.displaced != 0 && i < slotCount; ++i) {
                    if (keyOf(assignments[i]) == ret.displaced) {
                        moved = std::format(" ('{}' moved to slot {})", assignments[i].name, i);
                        break;
                    }
                }
                SKSE::log::debug("[HomeKey] Page {}: '{}' back on slot {} after {:.1f}s{}",
                    pageIndex, assignments[now].name, now, ret.awaySec, moved);
            } else {
                static constexpr std::array<std::string_view, 6> kWhy = {
                    "seating could not reach it", "home keys off", "a later leaver took it",
                    "it waits for it", "its class does not fit", "an override marks that key's own item" };
                SKSE::log::debug("[HomeKey] Page {}: '{}' left slot {} {:.1f}s ago, now on slot {} ({})",
                    pageIndex, assignments[now].name, ret.home, ret.awaySec, now,
                    kWhy[static_cast<size_t>(ret.why)]);
            }
        }
    }

    void SlotAllocator::RecordSeating(
        size_t pageIndex,
        uint32_t generation,
        const SlotAssignments& assignments) const
    {
        if (pageIndex >= MAX_PAGES) {
            return;
        }
        const size_t slotCount = std::min(assignments.size(), MAX_SLOTS_PER_PAGE);

        std::lock_guard<std::mutex> lock(m_seatingMutex);
        const auto& previous = m_seating[pageIndex];

        auto keyOf = [](const SlotAssignment& a) -> uint64_t {
            if (a.IsEmpty() || !a.candidate) return 0;
            return Candidate::GetBase(a.candidate->candidate).GetDeduplicationKey();
        };
        auto previousSeatOf = [&](uint64_t key) -> size_t {
            if (key == 0) return SIZE_MAX;
            for (size_t j = 0; j < slotCount; ++j) {
                if (previous[j] == key) return j;
            }
            return SIZE_MAX;
        };

        std::array<uint64_t, MAX_SLOTS_PER_PAGE> seats{};

        // The rule is one sentence: you keep your seat for as long as you are on
        // screen, and you only get a new one if you do not have one.
        //
        // Stated the other way round -- "record where everything ended up" --
        // every reason an item could not reach its seat this pass became a
        // permanent move. An override pins a slot, the item that lives there is
        // placed elsewhere, and recording that spot as its new home means the
        // override clears and seating then holds it in the wrong place for good.
        // The cascade is worse than the direct case and was visible in the same
        // play-test: the mace an override displaced took the bow's slot, and the
        // BOW -- which no override ever touched -- was the one that lost its
        // seat (2026-09-19, 23:31:23 to 23:31:26).

        // Anyone on screen but not in their own seat keeps the claim they had.
        // Override guests included: the potion is still on screen, so the slot it
        // normally lives in is still its slot, waiting for the override to end.
        for (size_t i = 0; i < slotCount; ++i) {
            const uint64_t key = keyOf(assignments[i]);
            if (key == 0) {
                continue;
            }
            const size_t home = previousSeatOf(key);
            // An override that marked its item's OWN seat keeps it: the second
            // loop below skips overrides, and without this the seat would
            // lapse, the next pass would find no slot showing the item, and
            // the override would jump to its configured slot.
            if (home != SIZE_MAX && (home != i || assignments[i].IsPinned())) {
                seats[home] = key;
            }
        }

        // Anyone sitting in their own seat keeps it, and an item with no seat at
        // all takes the one it is sitting in -- unless someone displaced still
        // owns it. An override guest claims nothing: it is passing through, and
        // its own seat was preserved above.
        //
        // Both loops write at most one slot per key and one key per slot, so the
        // map stays injective, which is what lets ApplySeating assume no two
        // items can want the same slot.
        for (size_t i = 0; i < slotCount; ++i) {
            if (assignments[i].IsPinned()) {  // a remembered item is passing through too
                continue;
            }
            const uint64_t key = keyOf(assignments[i]);
            if (key == 0) {
                continue;
            }
            const size_t home = previousSeatOf(key);
            if (home == i) {
                seats[i] = key;
            } else if (home == SIZE_MAX && seats[i] == 0) {
                seats[i] = key;
            }
        }

        // An item that left the screen entirely is in neither loop, so its seat
        // is simply not carried over -- which is how a seat is ever freed.

#ifndef NDEBUG
        // Log the map when it CHANGES, which is the only interesting moment: a
        // steady map is the feature working and says nothing. Reading it next to
        // the [VisualState] line for the same tick answers the one question this
        // instrumentation exists for -- seats are recorded from the allocator's
        // own output, while [VisualState] is what SlotLocker let through, so if
        // the two disagree the memory is chasing an arrangement the player never
        // saw. A '*' marks a seat whose owner is sitting somewhere else this
        // tick (displaced by an override, or standing in for one).
        if (seats != previous) {
            std::string summary;
            for (size_t i = 0; i < slotCount; ++i) {
                if (seats[i] == 0) continue;

                std::string_view who = "?";
                bool elsewhere = true;
                for (size_t j = 0; j < slotCount; ++j) {
                    if (keyOf(assignments[j]) == seats[i]) {
                        who = assignments[j].name;
                        elsewhere = (j != i);
                        break;
                    }
                }
                if (!summary.empty()) summary += " ";
                summary += std::format("{}={}{}", i, who, elsewhere ? "*" : "");
            }
            SKSE::log::debug("[Seating] page {} | {}", pageIndex,
                summary.empty() ? "(no seats)" : summary);
        }
#endif

        std::array<uint64_t, MAX_SLOTS_PER_PAGE> placedNow{};
        for (size_t i = 0; i < slotCount; ++i) {
            if (!assignments[i].IsPinned()) {
                placedNow[i] = keyOf(assignments[i]);
            }
        }

        // An item that left the screen entirely is remembered against the
        // slot it owned, for home keys (ApplySeating, phase 0) -- or, with no
        // seat, the slot it stood in last pass. The second is not a corner
        // case: an item the slot hold outranked loses its seat on the spot
        // (HoldIncumbents), so its seat reads empty here, and without the
        // fallback the item the feature exists for -- outranked off the page
        // for a few seconds -- was never remembered (code review of #179).
        const auto now = std::chrono::steady_clock::now();
        auto onScreen = [&](uint64_t key) {
            return std::any_of(assignments.begin(), assignments.end(),
                [&](const SlotAssignment& a) { return keyOf(a) == key; });
        };
        // Not from a slot a home claimant has claimed: an item that lost the
        // seat to the claim only filled the gap, and remembering it there let
        // it come back and claim the seat from the claimant still waiting for
        // it (RunHomeKeyTest, 2026-10-06 21:56:10).
        auto& claims = m_homeClaims[pageIndex];
        auto remember = [&](size_t j, uint64_t key) {
            if (claims[j] != 0 && claims[j] != key) return;
            auto& ring = m_departed[pageIndex][j];
            std::move_backward(ring.begin(), ring.end() - 1, ring.end());
            ring[0] = { key, now };
        };
        for (size_t j = 0; j < slotCount; ++j) {
            if (previous[j] != 0 && !onScreen(previous[j])) remember(j, previous[j]);
        }
        const auto& stood = m_lastPlaced[pageIndex];
        for (size_t j = 0; j < slotCount; ++j) {
            const uint64_t key = stood[j];
            if (key == 0 || onScreen(key)) continue;
            if (std::find(previous.begin(), previous.begin() + slotCount, key) != previous.begin() + slotCount) {
                continue;  // remembered by its seat above
            }
            remember(j, key);
        }

        // A home claim ends when its item sits in the slot, or loses the seat.
        for (size_t j = 0; j < slotCount; ++j) {
            if (claims[j] != 0 && (seats[j] != claims[j] || keyOf(assignments[j]) == claims[j])) {
                claims[j] = 0;
            }
        }

        m_seatingGeneration = generation;
        m_seating[pageIndex] = seats;
        m_lastPlaced[pageIndex] = placedNow;
    }

    void SlotAllocator::PullIntoEmptyJobKeys(
        const std::vector<SlotConfig>& slotConfigs,
        SlotAssignments& assignments,
        const State::PlayerActorState* player,
        const std::array<size_t, MAX_SLOTS_PER_PAGE>& priorityOrder,
        size_t priorityCount) const
    {
        const size_t slotCount = std::min(assignments.size(), std::min(slotConfigs.size(), MAX_SLOTS_PER_PAGE));
        for (size_t k = 0; k < priorityCount; ++k) {
            const size_t i = priorityOrder[k];
            if (i >= slotCount || !assignments[i].IsEmpty()) continue;
            if (slotConfigs[i].classification == SlotClassification::Regular) continue;

            // Best-scoring movable match standing on a Regular key.
            size_t from = SIZE_MAX;
            for (size_t j = 0; j < slotCount; ++j) {
                const auto& a = assignments[j];
                if (j == i || a.IsEmpty() || a.IsPinned() || a.IsWildcard() || !a.candidate) continue;
                if (slotConfigs[j].classification != SlotClassification::Regular) continue;
                if (!SlotAccepts(slotConfigs[i], a, player)) continue;
                if (from == SIZE_MAX || a.utility > assignments[from].utility) from = j;
            }
            if (from == SIZE_MAX) continue;

            SKSE::log::debug("[SlotAllocator] Slot {} ({}) was empty: took '{}' from Regular slot {}",
                i, SlotClassificationToString(slotConfigs[i].classification), assignments[from].name, from);
            assignments[i] = std::move(assignments[from]);
            assignments[i].slotIndex = i;
            assignments[i].classification = slotConfigs[i].classification;
            assignments[from] = SlotAssignment::Empty(from, slotConfigs[from].classification);
        }
    }

    size_t SlotAllocator::ComputePriorityOrder(
        const std::vector<SlotConfig>& configs,
        std::array<size_t, MAX_SLOTS_PER_PAGE>& outOrder) const
    {
        const size_t n = std::min(configs.size(), MAX_SLOTS_PER_PAGE);
        if (configs.size() > MAX_SLOTS_PER_PAGE) {
            // Not silent: a page configured with more slots than the fixed cap
            // would otherwise drop the overflow with no trace.
            thread_local size_t s_lastWarnedCount = 0;
            if (configs.size() != s_lastWarnedCount) {
                SKSE::log::warn("[SlotAllocator] Page has {} slots, exceeding MAX_SLOTS_PER_PAGE ({}); "
                    "extra slots ignored", configs.size(), MAX_SLOTS_PER_PAGE);
                s_lastWarnedCount = configs.size();
            }
        }

        // Build index list
        for (size_t i = 0; i < n; ++i) {
            outOrder[i] = i;
        }

        // Sort by priority (highest first). Stable, so tied priorities keep
        // slot-index order -- the same order SlotSettings assumes when it
        // finds the slot a health override lands in (/code-review #147).
        std::stable_sort(outOrder.begin(), outOrder.begin() + n,
            [&configs](size_t a, size_t b) {
                return configs[a].priority > configs[b].priority;
            });

        return n;
    }

    std::optional<Scoring::ScoredCandidate> SlotAllocator::FindBestCandidate(
        const Scoring::ScoredCandidateList& candidates,
        SlotClassification classification,
        const std::set<RE::FormID>& assignedFormIDs,
        const std::set<std::string_view>& assignedNames,
        bool skipEquipped,
        const State::PlayerActorState* player,
        bool skipWildcards,
        NeedCap* needCap) const
    {
        const bool capped = needCap && needCap->Active() && classification == SlotClassification::Regular;
        const Scoring::ScoredCandidate* first = nullptr;  // the pick without the cap
        const Scoring::ScoredCandidate* best = nullptr;
        float bestUtility = 0.0f;

        // Candidates are already sorted by utility (highest first)
        // Find the first candidate that matches and isn't already assigned
        for (const auto& candidate : candidates) {
            // Skip wildcards when the target slot forbids them
            if (skipWildcards && candidate.isWildcard) {
                continue;
            }

            // Only a Remembrance hold may place these; they were never ranked.
            if (candidate.isRememberedOnly) {
                continue;
            }

            // Skip already assigned candidates (by FormID or name)
            // Name check catches duplicate enchanted items with different FormIDs
            if (assignedFormIDs.contains(candidate.GetFormID())) {
                continue;
            }
            if (assignedNames.contains(candidate.GetName())) {
                continue;
            }

            // Check classification match
            if (!SlotClassifier::Matches(candidate, classification)) {
                continue;
            }

            // Skip equipped items if this slot wants alternatives only.
            // Belt-and-suspenders: check both the candidate's isEquipped flag
            // (from weapon registry scan) AND the player state's equipment poll
            // (faster 100ms refresh) to cover timing gaps between the two.
            if (skipEquipped) {
                const auto& base = Candidate::GetBase(candidate.candidate);
                if (base.isEquipped) {
                    continue;
                }
                if (player && player->IsItemEquipped(base.formID)) {
                    continue;
                }
            }

            if (!capped) {
                return candidate;
            }

            // Under the need cap: the first match wins outright unless its
            // need is already full on the page; then every later match is
            // weighed by its own factor. Past the sorted prefix the list is
            // in no order, so the scan does not stop early. A wildcard keeps
            // its POSITION semantics (see HoldIncumbents): it wins only as the
            // first match, and is never weighed against capped items.
            if (!best) {
                const float factor = needCap->Factor(candidate);
                if (candidate.isWildcard || factor >= 1.0f) {
                    return candidate;
                }
                first = best = &candidate;
                bestUtility = candidate.utility * factor;
                continue;
            }
            if (candidate.isWildcard) {
                continue;
            }
            const float effective = candidate.utility * needCap->Factor(candidate);
            if (effective > bestUtility) {
                best = &candidate;
                bestUtility = effective;
            }
        }

        if (!best) {
            return std::nullopt;
        }
        if (best != first) {
            needCap->NoteSkipped(*first);
        }
        return *best;
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

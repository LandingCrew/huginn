#include "SlotAllocator.h"
#include "SlotLocker.h"
#include "override/OverrideConditions.h"
#include "override/OverrideConfig.h"
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

        m_currentPage = 0;

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

    std::shared_ptr<const std::vector<PageConfig>> SlotAllocator::GetConfigSnapshot() const
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

    std::string SlotAllocator::GetCurrentPageName() const
    {
        auto snap = GetConfigSnapshot();
        const size_t page = m_currentPage.load();
        if (page >= snap->size()) {
            return "Page";
        }
        return (*snap)[page].name;
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
        auto snap = GetConfigSnapshot();
        if (snap->empty()) {
            return {};
        }
        const size_t page = pageIndex < snap->size() ? pageIndex : 0;
        return AllocateSlotsInternal(page, (*snap)[page].slots, candidates, overrides, player, world);
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

    SlotAssignments SlotAllocator::AllocateSlotsInternal(
        size_t pageIndex,
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
        // Keyed per page: the same override lands on different slot indices on
        // different pages within a single tick, so a single-key dedup would
        // ping-pong and log every tick while an override is latched.
        struct LastOverrideLog
        {
            RE::FormID formID = 0;
            size_t slot = SIZE_MAX;
        };
        thread_local std::array<LastOverrideLog, MAX_PAGES> lastOverrideLogs{};
        auto& lastOverrideLog = lastOverrideLogs[std::min(pageIndex, MAX_PAGES - 1)];
        bool overrideAssignedThisFrame = false;

        if (overrides.HasActiveOverride()) {
            for (const auto& override : overrides.activeOverrides) {
                if (!override.candidate) continue;

                RE::FormID formID = Candidate::GetFormID(*override.candidate);
                if (assignedFormIDs.contains(formID)) continue;

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

                        // Only log if override changed (different formID or slot)
                        if (formID != lastOverrideLog.formID || priorityIdx != lastOverrideLog.slot) {
                            SKSE::log::info("[SlotAllocator] Override '{}' → Slot {} ({})",
                                override.reason, priorityIdx,
                                SlotClassificationToString(config.classification));
                            lastOverrideLog.formID = formID;
                            lastOverrideLog.slot = priorityIdx;
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

                    // Only log if override changed (different formID or slot)
                    if (formID != lastOverrideLog.formID || priorityIdx != lastOverrideLog.slot) {
                        SKSE::log::info("[SlotAllocator] Override '{}' → Slot {} (fallback, {})",
                            override.reason, priorityIdx,
                            SlotClassificationToString(config.classification));
                        lastOverrideLog.formID = formID;
                        lastOverrideLog.slot = priorityIdx;
                    }
                    break;
                }

                if (!placed) {
                    if (sawAcceptingSlot) {
                        // All accepting slots on THIS page are occupied (typically
                        // by higher-priority overrides). Allocation worked as
                        // designed — not a config problem, so no warn.
                        SKSE::log::trace("[SlotAllocator] Override '{}' displaced on page {} "
                            "(accepting slots occupied)",
                            override.reason, pageIndex);
                    } else if (!AnyPageAcceptsCategory(override.category)) {
                        // No slot on ANY page accepts this category — a genuine
                        // config gap. Warn once per condition: the reason string
                        // can embed live values (e.g. ammo counts), so it must
                        // not be the dedup key.
                        std::lock_guard<std::mutex> logLock(m_logMutex);
                        if (m_warnedUnplacedConditions.insert(override.condition).second) {
                            SKSE::log::warn("[SlotAllocator] Override '{}' could not be placed - "
                                "no slot on any page accepts category '{}'",
                                override.reason,
                                Override::OverrideCategoryToString(override.category));
                        }
                    }
                    // else: another page accepts this category — expected with
                    // multi-page layouts; the override is visible there.
                }
            }
        }

        // Reset tracking only when overrides are truly inactive (not just unassigned
        // on this particular page). With multi-page support, Page 1 may have no
        // override-eligible slots, but the override is still active on Page 0.
        if (!overrides.HasActiveOverride()) {
            for (auto& entry : lastOverrideLogs) {
                entry = LastOverrideLog{};
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
                candidates, config.classification, assignedFormIDs, assignedNames, config.skipEquipped, &player);

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
                        /*skipWildcards=*/true);
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
                }
            }
        }

        return assignments;
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

        // Sort by priority (highest first)
        std::sort(outOrder.begin(), outOrder.begin() + n,
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
        bool skipWildcards) const
    {
        // Candidates are already sorted by utility (highest first)
        // Find the first candidate that matches and isn't already assigned
        for (const auto& candidate : candidates) {
            // Skip wildcards when the target slot forbids them
            if (skipWildcards && candidate.isWildcard) {
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

            return candidate;
        }

        return std::nullopt;
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
    }

}  // namespace Huginn::Slot

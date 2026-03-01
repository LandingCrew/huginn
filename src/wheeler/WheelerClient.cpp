#include "WheelerClient.h"
#include "WheelerSettings.h"
#include <Windows.h>
#include <optional>
#include <spdlog/spdlog.h>

#include "../Config.h"
#include "../learning/EquipSourceTracker.h"
#include "../learning/EquipEventBus.h"
#include "../slot/SlotAllocator.h"
#include "../slot/SlotLocker.h"
#include "../slot/SlotSettings.h"
#include "../candidate/CandidateGenerator.h"
#include "../candidate/CandidateTypes.h"
#include "../ui/IntuitionMenu.h"

namespace Huginn::Wheeler
{
    WheelerClient& WheelerClient::GetSingleton()
    {
        static WheelerClient instance;
        return instance;
    }

    const char* WheelerClient::GetItemTypeName(RE::FormID formID)
    {
        if (formID == 0) {
            return "Unknown";
        }

        auto* form = RE::TESForm::LookupByID(formID);
        if (!form) {
            return "Unknown";
        }

        // Check form type and return descriptive string
        if (form->As<RE::SpellItem>()) {
            return "Spell";
        }
        if (form->As<RE::ScrollItem>()) {
            return "Scroll";
        }
        if (auto* alchemyItem = form->As<RE::AlchemyItem>()) {
            // Distinguish potions from poisons
            return alchemyItem->IsPoison() ? "Poison" : "Potion";
        }
        if (auto* weapon = form->As<RE::TESObjectWEAP>()) {
            // Distinguish melee from ranged
            return weapon->IsBow() || weapon->IsCrossbow() ? "Ranged Weapon" : "Melee Weapon";
        }
        if (form->As<RE::TESObjectARMO>()) {
            return "Armor";
        }
        if (form->As<RE::TESAmmo>()) {
            return "Ammo";
        }

        return "Item";
    }

    bool WheelerClient::TryConnect()
    {
        if (m_api) {
            return true;
        }

        // Try to get Wheeler.dll handle
        HMODULE hWheeler = GetModuleHandleA("Wheeler.dll");
        if (!hWheeler) {
            spdlog::debug("[WheelerClient] Wheeler.dll not loaded");
            return false;
        }

        spdlog::debug("[WheelerClient] Found Wheeler.dll at {:p}", static_cast<void*>(hWheeler));

        // Get the API interface
        using GetWheelerAPIFn = WheelerAPI::IWheelerAPI* (*)();
        auto GetWheelerAPI = reinterpret_cast<GetWheelerAPIFn>(
            GetProcAddress(hWheeler, "GetWheelerAPI"));

        if (!GetWheelerAPI) {
            spdlog::warn("[WheelerClient] GetWheelerAPI export not found - old Wheeler version?");
            return false;
        }

        m_api = GetWheelerAPI();
        if (!m_api) {
            spdlog::warn("[WheelerClient] GetWheelerAPI() returned nullptr");
            return false;
        }

        if (m_api->version < WheelerAPI::API_VERSION_MIN) {
            spdlog::warn("[WheelerClient] API version too old: got {}, need >= {}",
                m_api->version, WheelerAPI::API_VERSION_MIN);
            m_api = nullptr;
            return false;
        }

        spdlog::info("[WheelerClient] Connected to Wheeler API v{} (v2={})",
            m_api->version, m_api->version >= 2 ? "yes" : "no");

        // Register callbacks
        RegisterCallbacks();

        return true;
    }

    // ============================================================================
    // Callback Registration
    // ============================================================================

    void WheelerClient::RegisterCallbacks()
    {
        if (!m_api) {
            return;
        }

        auto itemCb = &WheelerClient::OnItemActivated;
        auto wheelCb = &WheelerClient::OnWheelStateChanged;
        auto editCb = &WheelerClient::OnEditModeChanged;

        m_api->RegisterItemActivatedCallback(itemCb);
        m_api->RegisterWheelStateCallback(wheelCb);
        m_api->RegisterEditModeCallback(editCb);

        spdlog::debug("[WheelerClient] Callbacks registered: ItemActivated={:p}, WheelState={:p}, EditMode={:p}",
            reinterpret_cast<void*>(itemCb),
            reinterpret_cast<void*>(wheelCb),
            reinterpret_cast<void*>(editCb));
    }

    void WheelerClient::UnregisterCallbacks()
    {
        if (!m_api) {
            return;
        }

        spdlog::info("[WheelerClient] Unregistering callbacks...");

        m_api->UnregisterItemActivatedCallback();
        m_api->UnregisterWheelStateCallback();
        m_api->UnregisterEditModeCallback();

        spdlog::info("[WheelerClient] Callbacks unregistered");
    }

    // ============================================================================
    // THREAD SAFETY: Wheeler callbacks may fire synchronously from Wheeler API
    // calls (e.g., SetActiveWheelIndex triggers OnWheelStateChanged). To avoid
    // deadlock: NEVER call Wheeler API functions while holding m_callbackMutex.
    // Defer API calls outside the lock (see OnWheelStateChanged pattern).
    // ============================================================================

    // ============================================================================
    // Static Callback Trampolines
    // ============================================================================

    void WheelerClient::OnItemActivated(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex, uint32_t formID, bool isPrimary)
    {
        spdlog::info("[WheelerClient] Callback from API - wheel selection made - wheel={}, entry={}, item={}, formID={:08X}, primary={}",
            wheelIndex, entryIndex, itemIndex, formID, isPrimary);

        auto& client = GetSingleton();

        // Deferred Wheeler API calls — populated inside mutex, executed outside.
        // THREAD SAFETY: Wheeler API calls can trigger synchronous callbacks,
        // so they must NEVER run while holding m_callbackMutex (see line 118-122).
        struct DeferredEmptyAction {
            int32_t targetWheelIndex = -1;
            int32_t targetEntryIndex = -1;
        };
        std::optional<DeferredEmptyAction> deferredEmpty;

        int pageIndex = -1;
        auto policy = PostActivationPolicy::Backfill;

        {
            std::lock_guard<std::mutex> lock(client.m_callbackMutex);

            // Check if this is one of our managed wheels
            pageIndex = client.FindPageForWheel(wheelIndex);
            if (pageIndex < 0) {
                spdlog::debug("[WheelerClient] ItemActivated on non-Huginn wheel {}, ignoring", wheelIndex);
                return;
            }

            spdlog::info("[WheelerClient] Item activated on page {} wheel: entry={}, item={}, formID={:08X}, primary={}",
                pageIndex, entryIndex, itemIndex, formID, isPrimary);

            // Mark that an item was activated while wheel was open (prevents skip penalty)
            client.m_itemActivatedWhileOpen = true;
            spdlog::debug("[WheelerClient] Set m_itemActivatedWhileOpen=true for wheel {} (page {})", wheelIndex, pageIndex);

            // Track which slot was activated (for post-activation policy)
            client.m_lastActivatedSlot = entryIndex;

            // Post-activation policy determines how slot behaves after use
            policy = WheelerSettings::GetSingleton().GetPostActivationPolicy();

            if (policy == PostActivationPolicy::Sticky) {
                // Sticky: Keep the activated item visible — apply long lock, skip cooldown
                Slot::SlotLocker::GetSingleton().LockSlotForActivation(static_cast<size_t>(entryIndex));
                spdlog::info("[WheelerClient] Sticky policy: slot {} locked for activation", entryIndex);
            } else if (policy == PostActivationPolicy::Empty) {
                // Empty: Mark as activation-emptied + clear cached state (inside mutex)
                // Defer the actual Wheeler API calls (ClearEntry, SetEntrySubtext) to outside the mutex
                Slot::SlotLocker::GetSingleton().OnItemUsed(static_cast<RE::FormID>(formID));
                if (pageIndex >= 0 && static_cast<size_t>(pageIndex) < client.m_pageWheels.size()) {
                    std::lock_guard<std::mutex> dataLock(client.m_pageDataMutex);
                    auto& pw = client.m_pageWheels[pageIndex];
                    size_t slotIdx = static_cast<size_t>(entryIndex);
                    if (slotIdx < pw.slotActivationEmptied.size() &&
                        slotIdx < pw.slotFormIDs.size() &&
                        slotIdx < pw.slotUniqueIDs.size()) {
                        pw.slotActivationEmptied[slotIdx] = true;
                        pw.slotFormIDs[slotIdx] = 0;
                        pw.slotUniqueIDs[slotIdx] = 0;
                    }
                    // Capture deferred action for API calls outside mutex
                    deferredEmpty = DeferredEmptyAction{pw.wheelIndex, entryIndex};
                }
                spdlog::info("[WheelerClient] Empty policy: slot {} marked activation-emptied (API calls deferred)", entryIndex);
            } else {
                // Backfill (default): Break lock so slot repopulates on next update cycle
                Slot::SlotLocker::GetSingleton().OnItemUsed(static_cast<RE::FormID>(formID));
            }

            // Mark as Huginn-mediated equip (for external equip detection)
            Learning::EquipSourceTracker::GetSingleton().MarkHuginnEquip();

            // Start cooldown so the consumed item is filtered out on the next update cycle (~100ms)
            // Without this, the item stays recommended until RefreshCounts detects count=0 (up to 500ms)
            // Skip cooldown for Sticky policy — the item should remain visible
            if (policy != PostActivationPolicy::Sticky) {
                auto& candidateGen = Candidate::CandidateGenerator::GetSingleton();
                if (candidateGen.IsInitialized()) {
                    Candidate::SourceType sourceType = Candidate::SourceType::Spell;
                    if (auto* form = RE::TESForm::LookupByID(formID)) {
                        if (form->Is(RE::FormType::AlchemyItem)) sourceType = Candidate::SourceType::Potion;
                        else if (form->Is(RE::FormType::Weapon))  sourceType = Candidate::SourceType::Weapon;
                        else if (form->Is(RE::FormType::Scroll)) sourceType = Candidate::SourceType::Scroll;
                    }
                    candidateGen.StartCooldown(formID, sourceType);
                    spdlog::info("[WheelerClient] Started cooldown for {:08X} (type {})", formID, static_cast<int>(sourceType));
                }
            } else {
                spdlog::debug("[WheelerClient] Skipping cooldown for Sticky policy");
            }
        }  // m_callbackMutex released here

        // Execute deferred Wheeler API calls OUTSIDE the mutex (safe from deadlock).
        // Validate wheel is still managed — another thread (e.g. save/load) may have
        // destroyed wheels between mutex release and here.
        if (deferredEmpty && client.m_api) {
            if (client.m_api->IsManagedWheel(deferredEmpty->targetWheelIndex)) {
                client.m_api->ClearEntry(deferredEmpty->targetWheelIndex, deferredEmpty->targetEntryIndex);
                client.SetEntrySubtext(deferredEmpty->targetWheelIndex, deferredEmpty->targetEntryIndex, "Equipped");
                spdlog::debug("[WheelerClient] Empty policy: deferred ClearEntry + SetEntrySubtext executed for wheel {} entry {}",
                    deferredEmpty->targetWheelIndex, deferredEmpty->targetEntryIndex);
            } else {
                spdlog::warn("[WheelerClient] Deferred empty action skipped — wheel {} no longer managed",
                    deferredEmpty->targetWheelIndex);
            }
        }

        // Publish to EquipEventBus OUTSIDE the mutex (subscribers handle FQL + UsageMemory).
        // Lock ordering: bus acquires StateManager shared locks in BuildEvent, then bus m_mutex,
        // then subscriber internal locks — all outside m_callbackMutex.
        if (pageIndex >= 0) {
            Learning::EquipEventBus::GetSingleton().Publish(
                formID, Learning::EquipSource::Wheeler, 1.0f, /*wasRecommended=*/true);
        }
    }

    void WheelerClient::OnWheelStateChanged(int32_t wheelIndex, bool isOpen)
    {
        auto& client = GetSingleton();
        int32_t autoFocusTarget = -1;

        {
            std::lock_guard<std::mutex> lock(client.m_callbackMutex);

            spdlog::info("[WheelerClient] WheelStateChanged: wheel={}, isOpen={}", wheelIndex, isOpen);

            if (isOpen) {
                autoFocusTarget = client.HandleWheelOpened(wheelIndex);
            } else {
                client.HandleWheelClosed(wheelIndex);
            }
        }

        // Apply auto-focus OUTSIDE the mutex to avoid re-entrant callback deadlock.
        // SetActiveWheelIndex may fire OnWheelStateChanged synchronously.
        if (autoFocusTarget >= 0 && client.m_api) {
            client.m_api->SetActiveWheelIndex(autoFocusTarget);
        }
    }

    void WheelerClient::OnEditModeChanged(bool entered, const WheelerAPI::WheelChange* changes, size_t changeCount)
    {
        // Log but don't act - Huginn doesn't need to respond to edit mode
        spdlog::debug("[WheelerClient] EditModeChanged: entered={}, changeCount={}", entered, changeCount);
    }

    void WheelerClient::LogAPIInfo()
    {
        if (!m_api) {
            spdlog::info("[WheelerClient] Not connected to Wheeler");
            return;
        }

        spdlog::info("[WheelerClient] API v{}, wheels={}, active={}",
            m_api->version, m_api->GetWheelCount(), m_api->GetActiveWheelIndex());
        spdlog::debug("[WheelerClient] Initialized={}, Open={}, EditMode={}, v2={}",
            m_api->IsInitialized(), m_api->IsWheelOpen(), m_api->IsInEditMode(),
            m_api->version >= 2);
        spdlog::debug("[WheelerClient] API fn ptrs: ItemCb={:p}, WheelStateCb={:p}, EditModeCb={:p}",
            reinterpret_cast<void*>(m_api->RegisterItemActivatedCallback),
            reinterpret_cast<void*>(m_api->RegisterWheelStateCallback),
            reinterpret_cast<void*>(m_api->RegisterEditModeCallback));
    }

    // ============================================================================
    // Spell Recommendation Wheel Management (v0.12.0: Multi-page support)
    // ============================================================================

    void WheelerClient::DestroyRecommendationWheels()
    {
        spdlog::info("[WheelerClient] Destroying {} recommendation wheels", m_pageWheels.size());

        // Clear all subtexts BEFORE deleting wheels. Wheeler may hold const char*
        // pointers into slotSubtexts — clearing tells Wheeler to drop its references
        // so the backing strings can be safely destroyed.
        for (auto& pw : m_pageWheels) {
            if (pw.wheelIndex >= 0 && m_api) {
                for (size_t i = 0; i < pw.slotCount; ++i) {
                    ClearEntrySubtext(pw.wheelIndex, static_cast<int32_t>(i));
                }
            }
        }

        for (auto& pw : m_pageWheels) {
            if (pw.wheelIndex >= 0 && m_api) {
                auto result = m_api->DeleteManagedWheel(pw.wheelIndex);
                spdlog::debug("[WheelerClient] Deleted wheel {} for page '{}': {}",
                    pw.wheelIndex, pw.pageName, static_cast<int>(result));
            }
        }
        m_pageWheels.clear();
    }

    bool WheelerClient::CreateRecommendationWheels()
    {
        if (!m_api) {
            spdlog::error("[WheelerClient] Cannot create recommendation wheels - not connected");
            return false;
        }

        if (!m_api->IsInitialized()) {
            spdlog::warn("[WheelerClient] Cannot create recommendation wheels - Wheeler not initialized");
            return false;
        }

        // Thread safety: Protect entire function - reads and modifies m_pageWheels extensively
        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        // Get page configuration from SlotSettings
        auto& slotSettings = Slot::SlotSettings::GetSingleton();
        size_t pageCount = slotSettings.GetPageCount();

        if (pageCount == 0) {
            spdlog::error("[WheelerClient] No pages configured in SlotSettings");
            return false;
        }

        // Check if wheels already exist and are valid
        if (!m_pageWheels.empty() && m_pageWheels[0].wheelIndex >= 0) {
            if (m_api->IsManagedWheel(m_pageWheels[0].wheelIndex)) {
                spdlog::debug("[WheelerClient] Recommendation wheels already exist ({} pages)", m_pageWheels.size());
                return true;
            } else {
                // Wheels were invalidated (save/load cycle) - recreate
                spdlog::info("[WheelerClient] Managed wheels invalidated after save/load, recreating...");
                DestroyRecommendationWheels();
            }
        } else if (!m_pageWheels.empty()) {
            // Stale entries with invalid wheel indices — clean up
            DestroyRecommendationWheels();
        }

        // Create one wheel per page
        m_pageWheels.reserve(pageCount);

        for (size_t p = 0; p < pageCount; ++p) {
            const auto pageConfig = slotSettings.GetPage(p);
            size_t slotCount = pageConfig.slots.size();

            if (slotCount == 0) {
                spdlog::warn("[WheelerClient] Page {} has no slots, inserting placeholder", p);
                PageWheel placeholder;
                placeholder.wheelIndex = -1;
                placeholder.slotCount = 0;
                placeholder.pageName = pageConfig.name;
                m_pageWheels.push_back(std::move(placeholder));
                continue;
            }

            PageWheel pageWheel;
            pageWheel.slotCount = slotCount;
            pageWheel.pageName = pageConfig.name;
            pageWheel.slotFormIDs.resize(slotCount, 0);
            pageWheel.slotWildcard.resize(slotCount, false);
            pageWheel.slotUniqueIDs.resize(slotCount, 0);
            pageWheel.slotSubtexts.resize(slotCount);
            pageWheel.slotRetries.resize(slotCount, 0);
            pageWheel.slotActivationEmptied.resize(slotCount, false);

            // Build wheel label - MUST be stored in struct so it stays alive for Wheeler API
            // Wheeler may hold onto the const char* pointer
            if (pageCount > 1) {
                pageWheel.wheelLabel = std::format("Huginn: {}", pageConfig.name);
            } else {
                pageWheel.wheelLabel = "Huginn";
            }

            // Create wheel with appropriate slot count
            // NOTE: clientName points to pageWheel.wheelLabel.c_str() which stays valid
            //
            // Position offset: When inserting at a fixed position (e.g. 0 = First),
            // each subsequent wheel must be placed at basePosition + pageIndex to avoid
            // shifting earlier wheels. For append (-1), all pages use -1 since
            // appending preserves creation order.
            auto& wheelerSettings = WheelerSettings::GetSingleton();
            int32_t basePosition = wheelerSettings.GetAPIPosition();
            int32_t pagePosition = (basePosition >= 0)
                ? basePosition + static_cast<int32_t>(p)
                : basePosition;

            if (m_api->version >= 2) {
                WheelerAPI::WheelConfig config = {
                    .numEntries = static_cast<int32_t>(slotCount),
                    .position = pagePosition,
                    .managed = true,
                    .clientName = pageWheel.wheelLabel.c_str(),
                    .showLabel = true,
                    .labelFontSize = 0,
                    .labelColor = 0,
                    .labelOffsetY = 0,
                    .indicatorText = "O",  // "O" for Huginn
                    .indicatorActiveColor = 0,
                    .indicatorInactiveColor = 0
                };
                pageWheel.wheelIndex = m_api->CreateManagedWheel(&config);
            } else {
                WheelerAPI::WheelConfigV1 config = {
                    .numEntries = static_cast<int32_t>(slotCount),
                    .position = pagePosition,
                    .managed = true,
                    .clientName = pageWheel.wheelLabel.c_str(),
                    .showLabel = true
                };
                pageWheel.wheelIndex = m_api->CreateManagedWheel(
                    reinterpret_cast<const WheelerAPI::WheelConfig*>(&config));
            }

            if (pageWheel.wheelIndex < 0) {
                spdlog::error("[WheelerClient] Failed to create wheel for page {}: {}",
                    p, pageWheel.wheelIndex);
                continue;
            }

            // Post-creation entry count repair: Wheeler may create fewer entries than
            // requested (observed during new game). Add missing entries individually.
            int32_t actualEntries = m_api->GetEntryCount(pageWheel.wheelIndex);
            int32_t targetEntries = static_cast<int32_t>(slotCount);
            if (actualEntries < targetEntries) {
                spdlog::warn("[WheelerClient] Page {} wheel {} created with {}/{} entries, adding missing entries",
                    p, pageWheel.wheelIndex, actualEntries, targetEntries);
                for (int32_t e = actualEntries; e < targetEntries; ++e) {
                    int32_t result = m_api->AddEntry(pageWheel.wheelIndex);
                    if (result < 0) {
                        spdlog::error("[WheelerClient] Page {} AddEntry failed at entry {} (result={})",
                            p, e, result);
                        break;
                    }
                }
                // Re-check and adjust slot count to match reality
                actualEntries = m_api->GetEntryCount(pageWheel.wheelIndex);
                if (actualEntries < targetEntries) {
                    spdlog::warn("[WheelerClient] Page {} could only create {}/{} entries, adjusting slot count",
                        p, actualEntries, targetEntries);
                    pageWheel.slotCount = static_cast<size_t>(std::max(actualEntries, 0));
                    pageWheel.slotFormIDs.resize(pageWheel.slotCount, 0);
                    pageWheel.slotWildcard.resize(pageWheel.slotCount, false);
                    pageWheel.slotUniqueIDs.resize(pageWheel.slotCount, 0);
                    pageWheel.slotSubtexts.resize(pageWheel.slotCount);
                    pageWheel.slotRetries.resize(pageWheel.slotCount, 0);
                    pageWheel.slotActivationEmptied.resize(pageWheel.slotCount, false);
                }
            }

            spdlog::info("[WheelerClient] Created wheel for page {} '{}': index={}, {} slots{}",
                p, pageConfig.name, pageWheel.wheelIndex, pageWheel.slotCount,
                (pageWheel.slotCount != slotCount) ? std::format(" (requested {})", slotCount) : "");

            m_pageWheels.push_back(std::move(pageWheel));
        }

        if (m_pageWheels.empty()) {
            spdlog::error("[WheelerClient] Failed to create any wheels");
            return false;
        }

        spdlog::info("[WheelerClient] === Multi-Page Wheels Created ===");
        for (size_t i = 0; i < m_pageWheels.size(); ++i) {
            spdlog::info("[WheelerClient] Page {}: '{}' -> wheel {}, {} slots",
                i, m_pageWheels[i].pageName, m_pageWheels[i].wheelIndex, m_pageWheels[i].slotCount);
        }
        spdlog::info("[WheelerClient] Total: {} wheels", m_pageWheels.size());
        spdlog::info("[WheelerClient] ==================================");

        return true;
    }

    int32_t WheelerClient::GetWheelIndexForPage(size_t pageIndex) const
    {
        // Thread safety: Protect size check and [] access (TOCTOU race prevention)
        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        if (pageIndex < m_pageWheels.size()) {
            return m_pageWheels[pageIndex].wheelIndex;
        }
        return -1;
    }

    int32_t WheelerClient::GetCurrentWheelIndex() const
    {
        auto& allocator = Slot::SlotAllocator::GetSingleton();
        return GetWheelIndexForPage(allocator.GetCurrentPage());
    }

    bool WheelerClient::SetActivePage(size_t pageIndex)
    {
        if (!m_api) return false;
        int32_t wheelIndex = GetWheelIndexForPage(pageIndex);
        if (wheelIndex < 0) return false;
        auto result = m_api->SetActiveWheelIndex(wheelIndex);
        if (result == WheelerAPI::Result::OK) {
            logger::debug("[WheelerClient] SetActivePage({}) -> wheel {}"sv, pageIndex, wheelIndex);
            return true;
        }
        return false;
    }

    int WheelerClient::FindPageForWheel(int32_t wheelIndex) const
    {
        // Thread safety: Protect iteration over m_pageWheels (called from callbacks)
        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        for (size_t i = 0; i < m_pageWheels.size(); ++i) {
            if (m_pageWheels[i].wheelIndex == wheelIndex) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool WheelerClient::IsOurWheel(int32_t wheelIndex) const
    {
        return FindPageForWheel(wheelIndex) >= 0;
    }

    int WheelerClient::GetActiveManagedPage() const
    {
        if (!m_api || !m_api->IsWheelOpen()) return -1;
        int32_t activeWheel = m_api->GetActiveWheelIndex();
        return FindPageForWheel(activeWheel);
    }

    void WheelerClient::UpdateRecommendations(const std::vector<RE::FormID>& spellFormIDs)
    {
        // Update current page's wheel (no wildcard info)
        std::vector<bool> emptyWildcard;
        std::vector<uint16_t> emptyUniqueIDs;
        std::vector<std::string> emptySubtexts;
        auto& allocator = Slot::SlotAllocator::GetSingleton();
        UpdatePageWheel(allocator.GetCurrentPage(), spellFormIDs, emptyWildcard, emptyUniqueIDs, emptySubtexts);
    }

    void WheelerClient::UpdateRecommendationsForPage(size_t pageIndex,
                                                      const std::vector<RE::FormID>& spellFormIDs,
                                                      const std::vector<bool>& isWildcard,
                                                      const std::vector<uint16_t>& uniqueIDs,
                                                      const std::vector<std::string>& subtexts)
    {
        UpdatePageWheel(pageIndex, spellFormIDs, isWildcard, uniqueIDs, subtexts);
    }

    void WheelerClient::UpdatePageWheel(size_t pageIndex,
                                         const std::vector<RE::FormID>& spellFormIDs,
                                         const std::vector<bool>& isWildcard,
                                         const std::vector<uint16_t>& uniqueIDs,
                                         const std::vector<std::string>& subtexts)
    {
        // Early validation (before lock - no shared state access)
        if (!m_api) {
            return;
        }

        // Thread safety: Lock BEFORE size check to prevent TOCTOU race
        // Safe to hold while calling Wheeler content APIs (RemoveItem, AddItemByFormID,
        // SetManagedWheelEntrySubtext) — these do NOT trigger synchronous callbacks.
        std::lock_guard<std::mutex> dataLock(m_pageDataMutex);

        if (pageIndex >= m_pageWheels.size()) {
            return;
        }

        auto& pageWheel = m_pageWheels[pageIndex];
        if (pageWheel.wheelIndex < 0) {
            return;
        }

        // Validate wheel ownership - another mod may have deleted it or indices shifted
        if (!m_api->IsManagedWheel(pageWheel.wheelIndex)) {
            spdlog::warn("[WheelerClient] Page {} wheel {} no longer managed, marking invalid",
                pageIndex, pageWheel.wheelIndex);
            pageWheel.wheelIndex = -1;
            return;
        }

        // A3: Safety pre-check — verify wheel has entries (catches unexpected state)
        if (m_api->IsWheelEmpty(pageWheel.wheelIndex)) {
            spdlog::warn("[WheelerClient] Page {} wheel {} is unexpectedly empty after creation",
                pageIndex, pageWheel.wheelIndex);
            return;
        }

        int32_t entryCount = m_api->GetEntryCount(pageWheel.wheelIndex);
        if (entryCount <= 0) {
            spdlog::warn("[WheelerClient] Page {} wheel has no entries", pageIndex);
            return;
        }

        // Lock now held for the per-slot update loop below

        static constexpr uint8_t MAX_SLOT_RETRIES = 3;

        // Update each slot (bounds-check all vectors to prevent out-of-range access)
        int32_t maxSlots = std::min(static_cast<int32_t>(pageWheel.slotCount), entryCount);
        maxSlots = std::min(maxSlots, static_cast<int32_t>(pageWheel.slotFormIDs.size()));
        for (int32_t i = 0; i < maxSlots; ++i) {
            size_t idx = static_cast<size_t>(i);
            RE::FormID newFormID = (idx < spellFormIDs.size()) ? spellFormIDs[idx] : 0;
            RE::FormID cachedFormID = pageWheel.slotFormIDs[idx];
            bool newWildcard = (idx < isWildcard.size()) ? isWildcard[idx] : false;
            bool cachedWildcard = pageWheel.slotWildcard[idx];

            // Update item if FormID or uniqueID changed
            uint16_t newUniqueID = (idx < uniqueIDs.size()) ? uniqueIDs[idx] : 0;
            uint16_t cachedUniqueID = pageWheel.slotUniqueIDs[idx];

            // Clear activation-emptied flag when a new candidate arrives for this slot.
            // This allows Empty policy slots to repopulate when context changes.
            if (newFormID != 0 && idx < pageWheel.slotActivationEmptied.size()
                && pageWheel.slotActivationEmptied[idx]) {
                pageWheel.slotActivationEmptied[idx] = false;
                spdlog::trace("[WheelerClient] Cleared activation-emptied flag for page {} slot {} (new candidate {:08X})",
                    pageIndex, idx, newFormID);
            }

            if (newFormID != cachedFormID || newUniqueID != cachedUniqueID) {
                // Reset retry counter when a genuinely different item is recommended.
                // Don't reset when cachedFormID is 0 — that means we never successfully
                // populated this slot, so the retry counter should keep accumulating.
                if (newFormID != cachedFormID && cachedFormID != 0) {
                    pageWheel.slotRetries[idx] = 0;
                }

                // Remove previous item (A4: use RemoveItem for precision, ClearEntry as fallback)
                if (cachedFormID != 0) {
                    if (newFormID != 0) {
                        // Replacing one item with another — use surgical RemoveItem
                        auto removeResult = m_api->RemoveItem(pageWheel.wheelIndex, i, 0);
                        if (removeResult != WheelerAPI::Result::OK) {
                            // Fallback to ClearEntry if RemoveItem fails
                            spdlog::debug("[WheelerClient] RemoveItem failed ({}), falling back to ClearEntry",
                                static_cast<int>(removeResult));
                            m_api->ClearEntry(pageWheel.wheelIndex, i);
                        }
                    } else {
                        // Clearing to empty — ClearEntry is appropriate
                        m_api->ClearEntry(pageWheel.wheelIndex, i);
                    }
                }
                if (newFormID != 0) {
                    int32_t result = m_api->AddItemByFormID(pageWheel.wheelIndex, i, newFormID, newUniqueID);
                    if (result < 0) {
                        // A3: Use IsEntryEmpty to decide recovery strategy
                        bool entryEmpty = m_api->IsEntryEmpty(pageWheel.wheelIndex, i);
                        if (entryEmpty && cachedFormID != 0) {
                            // Entry is empty after removal — restore previous item
                            m_api->AddItemByFormID(pageWheel.wheelIndex, i, cachedFormID, cachedUniqueID);
                        } else if (!entryEmpty) {
                            spdlog::debug("[WheelerClient] Entry {} not empty after AddItem failure, skipping restore", i);
                        }

                        ++pageWheel.slotRetries[idx];
                        if (pageWheel.slotRetries[idx] >= MAX_SLOT_RETRIES) {
                            spdlog::debug("[WheelerClient] AddItemByFormID {:08X} uid={} slot {} failed {} times (result={}), keeping previous item",
                                newFormID, newUniqueID, i, pageWheel.slotRetries[idx], result);
                            // Cache to stop retrying this FormID+UniqueID combination
                            pageWheel.slotFormIDs[idx] = newFormID;
                            pageWheel.slotUniqueIDs[idx] = newUniqueID;
                        } else {
                            spdlog::debug("[WheelerClient] AddItemByFormID {:08X} uid={} slot {} failed (attempt {}/{}, result={})",
                                newFormID, newUniqueID, i, pageWheel.slotRetries[idx], MAX_SLOT_RETRIES, result);
                        }
                        continue;
                    }
                }
                pageWheel.slotRetries[idx] = 0;  // Reset on success
                pageWheel.slotFormIDs[idx] = newFormID;
                pageWheel.slotUniqueIDs[idx] = newUniqueID;
            }

            // Determine subtext for this slot (priority: override > lock > wildcard > explanation)
            // Pre-computed labels (override/lock/explanation) arrive via subtexts vector.
            // Wildcard label is applied here using configurable text from WheelerSettings.
            const auto& stConfig = WheelerSettings::GetSingleton().GetSubtextLabels();
            std::string newSubtext;

            // 1. Check for pre-computed label from pipeline (override/lock/explanation)
            if (idx < subtexts.size() && !subtexts[idx].empty()) {
                newSubtext = subtexts[idx];
            }

            // 2. Wildcard label (only if no higher-priority label)
            if (newSubtext.empty() && newWildcard && newFormID != 0 && stConfig.showWildcardLabel) {
                newSubtext = stConfig.wildcardLabelText;
            }

            // 3. Apply subtext change if different from cached
            std::string& cachedSubtext = pageWheel.slotSubtexts[idx];
            if (newSubtext != cachedSubtext || newWildcard != cachedWildcard || newFormID != cachedFormID) {
                // Update cache BEFORE passing pointer to Wheeler — Wheeler stores
                // the const char* and reads it later during rendering, so the backing
                // string must outlive the API call.
                cachedSubtext = newSubtext;
                pageWheel.slotWildcard[idx] = newWildcard;

                if (!cachedSubtext.empty()) {
                    SetEntrySubtext(pageWheel.wheelIndex, i, cachedSubtext.c_str());
                } else {
                    ClearEntrySubtext(pageWheel.wheelIndex, i);
                }
            }
        }
    }

    // ============================================================================
    // v2 API: Entry Subtext Support
    // ============================================================================

    void WheelerClient::SetEntrySubtext(int32_t wheelIndex, int32_t entryIndex, const char* text)
    {
        if (!m_api || m_api->version < 2) {
            return;  // v2 API required
        }

        const auto& stConfig = WheelerSettings::GetSingleton().GetSubtextLabels();
        WheelerAPI::SubtextConfig config = {
            .text = text,
            .offsetX = stConfig.offsetX,
            .offsetY = stConfig.offsetY,
            .fontSize = 0.0f,  // Use default
            .color = 0          // Use default (70% white)
        };

        m_api->SetManagedWheelEntrySubtext(wheelIndex, entryIndex, &config);
    }

    void WheelerClient::ClearEntrySubtext(int32_t wheelIndex, int32_t entryIndex)
    {
        if (!m_api || m_api->version < 2) {
            return;
        }

        m_api->SetManagedWheelEntrySubtext(wheelIndex, entryIndex, nullptr);
    }

    void WheelerClient::UpdateRecommendations(const std::vector<RE::FormID>& spellFormIDs,
                                              const std::vector<bool>& isWildcard,
                                              const std::vector<uint16_t>& uniqueIDs)
    {
        // Update current page's wheel with wildcard info and uniqueIDs
        std::vector<std::string> emptySubtexts;
        auto& allocator = Slot::SlotAllocator::GetSingleton();
        UpdatePageWheel(allocator.GetCurrentPage(), spellFormIDs, isWildcard, uniqueIDs, emptySubtexts);
    }

    // ============================================================================
    // Wheel State Change Helpers (Extracted from OnWheelStateChanged)
    // ============================================================================

    int32_t WheelerClient::HandleWheelOpened(int32_t wheelIndex)
    {
        // Detect if this is a fresh open vs. scrolling between wheels.
        // Wheeler fires OnWheelStateChanged(newWheel, true) for both cases.
        // m_wheelVisible is true if we already had a wheel open (= scrolling).
        bool isFreshOpen = !m_wheelVisible;

        m_wheelVisible = true;
        if (isFreshOpen) {
            m_itemActivatedWhileOpen = false;  // Reset activation tracking
            spdlog::debug("[WheelerClient] Fresh open: reset m_itemActivatedWhileOpen=false");

            // Observer notification: hide IntuitionMenu when Wheeler opens
            // (SetVisible defers to UI thread via AddUITask — safe from callback thread)
            if (auto* intuition = UI::IntuitionMenu::GetSingleton()) {
                intuition->SetVisible(false);
                spdlog::debug("[WheelerClient] Notified IntuitionMenu: SetVisible(false)");
            }
        } else {
            spdlog::debug("[WheelerClient] Scroll to wheel {} (not a fresh open)", wheelIndex);
        }

        // Thread safety: Lock protects m_pageWheels access
        // Note: Called from OnWheelStateChanged with m_callbackMutex held
        // Lock ordering: m_callbackMutex (outer) → m_pageDataMutex (inner) ✓
        std::lock_guard<std::mutex> dataLock(m_pageDataMutex);

        // Find which page this wheel belongs to (inlined to avoid recursive lock)
        int pageIndex = -1;
        for (size_t i = 0; i < m_pageWheels.size(); ++i) {
            if (m_pageWheels[i].wheelIndex == wheelIndex) {
                pageIndex = static_cast<int>(i);
                break;
            }
        }

        // Auto-focus: only on FRESH open (not when scrolling between wheels).
        // If the player opened Wheeler on a non-Huginn wheel, return target for deferred focus.
        // The caller applies SetActiveWheelIndex OUTSIDE the mutex to avoid re-entrant deadlock.
        int32_t autoFocusTarget = -1;
        auto& wheelerSettings = WheelerSettings::GetSingleton();
        if (isFreshOpen && wheelerSettings.GetAutoFocusOnOpen() && pageIndex < 0 && !m_pageWheels.empty()) {
            int32_t ourWheel = m_pageWheels[0].wheelIndex;
            if (ourWheel >= 0) {
                autoFocusTarget = ourWheel;
                spdlog::info("[WheelerClient] Auto-focus requested: Huginn wheel {} (opened on non-Huginn wheel {})",
                    ourWheel, wheelIndex);
            }
        }

        if (pageIndex < 0) {
            spdlog::debug("[WheelerClient] Wheel {} is not an Huginn wheel", wheelIndex);
            return autoFocusTarget;
        }

        spdlog::info("[WheelerClient] Page {} '{}' wheel opened", pageIndex, m_pageWheels[pageIndex].pageName);

        // Observer notification: sync SlotAllocator page when Wheeler scrolls to our wheel.
        // SetCurrentPage sets m_pageChanged=true, which makes the pipeline run on the next
        // tick and update IntuitionMenu with the correct page's slot assignments.
        Slot::SlotAllocator::GetSingleton().SetCurrentPage(static_cast<size_t>(pageIndex));

        return -1;  // Already on our wheel, no auto-focus needed
    }

    void WheelerClient::HandleWheelClosed(int32_t wheelIndex)
    {
        // Don't set m_wheelVisible = false here — defer to CheckPendingWheelClose().
        // Keeping it true lets HandleWheelOpened correctly detect scroll-vs-fresh-open
        // when Wheeler fires close+open in rapid succession during page scrolling.
        //
        // Don't call IsWheelOpen() here — Wheeler fires this callback BEFORE updating
        // its own state, so IsWheelOpen() still returns true at this point.
        // CheckPendingWheelClose() will query it on the update thread where it's accurate.

        m_pendingWheelClose = true;
        m_pendingCloseWheelIndex = wheelIndex;

        int pageIndex = FindPageForWheel(wheelIndex);
        spdlog::info("[WheelerClient] Wheel {} close callback (page={}, activated={}) — deferred to update tick",
            wheelIndex, pageIndex, m_itemActivatedWhileOpen.load());
    }

    bool WheelerClient::CheckPendingWheelClose()
    {
        if (!m_pendingWheelClose.load(std::memory_order_relaxed)) {
            return false;
        }

        // Now running on the update thread — IsWheelOpen() is accurate here.
        bool stillOpen = m_api && m_api->IsWheelOpen();
        if (stillOpen) {
            // Wheeler scrolled to another wheel (close A → open B).
            // The pending flag was set by close-A, but open-B already fired,
            // so the wheel is still visible. Just clear the flag.
            m_pendingWheelClose = false;
            spdlog::debug("[WheelerClient] CheckPendingWheelClose: wheel still open (scroll), clearing flag");
            return false;
        }

        // Wheel is truly closed — consume the flag and process the close.
        m_pendingWheelClose = false;
        m_wheelVisible = false;

        // Sync SlotAllocator page to match the wheel that was last closed.
        // This handles the case where the m_pageChanged flag from SetCurrentPage()
        // was consumed while the wheel was still open (pipeline skips UI updates
        // when wheel is open), and also the case where Wheeler doesn't fire
        // separate close+open callbacks during scrolling.
        int32_t closedWheelIndex = m_pendingCloseWheelIndex.exchange(-1);
        if (closedWheelIndex >= 0) {
            int pageIndex = FindPageForWheel(closedWheelIndex);
            if (pageIndex >= 0) {
                Slot::SlotAllocator::GetSingleton().SetCurrentPage(static_cast<size_t>(pageIndex));
                spdlog::debug("[WheelerClient] CheckPendingWheelClose: synced page to {} (wheel {})",
                    pageIndex, closedWheelIndex);
            }
        }

        // Clear activation-emptied flags (allows Empty policy slots to repopulate)
        {
            std::lock_guard<std::mutex> dataLock(m_pageDataMutex);
            for (size_t p = 0; p < m_pageWheels.size(); ++p) {
                auto& pw = m_pageWheels[p];
                bool hadEmptied = false;
                for (bool flag : pw.slotActivationEmptied) {
                    if (flag) { hadEmptied = true; break; }
                }
                if (hadEmptied) {
                    std::fill(pw.slotActivationEmptied.begin(), pw.slotActivationEmptied.end(), false);
                    spdlog::debug("[WheelerClient] Cleared activation-emptied flags for page {} on wheel close", p);
                }
            }
        }

        // Show IntuitionMenu now that Wheeler is fully closed.
        // MarkPageDirty forces the pipeline to run on this tick, pushing
        // the current page's slot assignments to IntuitionMenu.
        if (auto* intuition = UI::IntuitionMenu::GetSingleton()) {
            intuition->SetVisible(true);
            spdlog::debug("[WheelerClient] CheckPendingWheelClose: SetVisible(true)");
        }
        Slot::SlotAllocator::GetSingleton().MarkPageDirty();
        spdlog::debug("[WheelerClient] CheckPendingWheelClose: MarkPageDirty for IntuitionMenu refresh");

        return true;
    }

    // ============================================================================
    // A1: Urgent Auto-Focus on Override
    // ============================================================================

    bool WheelerClient::TryUrgentAutoFocus(int overridePriority)
    {
        if (!m_api || !m_api->IsWheelOpen()) {
            return false;
        }

        auto& settings = WheelerSettings::GetSingleton();
        if (!settings.GetAutoFocusOnOverride()) {
            return false;
        }

        if (overridePriority < settings.GetAutoFocusMinPriority()) {
            return false;
        }

        if (m_pageWheels.empty() || m_pageWheels[0].wheelIndex < 0) {
            return false;
        }

        // Check if Wheeler is already on one of our wheels
        int32_t activeWheel = m_api->GetActiveWheelIndex();
        if (IsOurWheel(activeWheel)) {
            return false;  // Already on Huginn wheel, no need to focus
        }

        // Auto-focus to our first wheel (page 0)
        int32_t targetWheel = m_pageWheels[0].wheelIndex;
        spdlog::info("[WheelerClient] Urgent auto-focus: override priority {} >= {}, "
                     "focusing from wheel {} to Huginn wheel {}",
            overridePriority, settings.GetAutoFocusMinPriority(), activeWheel, targetWheel);

        // Call SetActiveWheelIndex — safe because we're not holding m_callbackMutex
        // (this is called from the update loop, not from a callback)
        auto result = m_api->SetActiveWheelIndex(targetWheel);
        if (result != WheelerAPI::Result::OK) {
            spdlog::warn("[WheelerClient] SetActiveWheelIndex({}) failed: {}",
                targetWheel, static_cast<int>(result));
            return false;
        }

        return true;
    }

    // ============================================================================
    // A2: State Validation (Debug Only)
    // ============================================================================

    void WheelerClient::ValidateWheelState() const
    {
#ifndef NDEBUG
        if (!m_api) {
            return;
        }

        for (size_t p = 0; p < m_pageWheels.size(); ++p) {
            const auto& pw = m_pageWheels[p];
            if (pw.wheelIndex < 0) {
                continue;
            }

            // Verify wheel still exists and is managed
            if (!m_api->IsManagedWheel(pw.wheelIndex)) {
                spdlog::warn("[WheelerClient] ValidateWheelState: Page {} wheel {} is no longer managed!",
                    p, pw.wheelIndex);
                continue;
            }

            int32_t entryCount = m_api->GetEntryCount(pw.wheelIndex);
            int32_t expectedCount = static_cast<int32_t>(pw.slotCount);
            if (entryCount != expectedCount) {
                spdlog::warn("[WheelerClient] ValidateWheelState: Page {} wheel {} entry count mismatch: "
                             "expected {}, got {}",
                    p, pw.wheelIndex, expectedCount, entryCount);
            }

            // Check each slot's FormID against Wheeler's actual state
            int32_t checkCount = std::min(entryCount, expectedCount);
            for (int32_t i = 0; i < checkCount; ++i) {
                size_t idx = static_cast<size_t>(i);
                if (idx >= pw.slotFormIDs.size()) break;

                uint32_t actualFormID = m_api->GetItemFormID(pw.wheelIndex, i, 0);
                RE::FormID cachedFormID = pw.slotFormIDs[idx];

                // Empty entries: GetItemFormID returns 0 for empty slots
                if (actualFormID != cachedFormID) {
                    spdlog::warn("[WheelerClient] ValidateWheelState: Page {} slot {} desync: "
                                 "cached={:08X}, actual={:08X}",
                        p, i, cachedFormID, actualFormID);
                }
            }
        }

        spdlog::debug("[WheelerClient] ValidateWheelState: checked {} pages", m_pageWheels.size());
#endif
    }

    // ============================================================================
    // Post-Activation Policy Helpers (Part B)
    // ============================================================================

    bool WheelerClient::IsSlotActivationEmptied(size_t pageIndex, size_t slotIndex) const
    {
        std::lock_guard<std::mutex> dataLock(m_pageDataMutex);
        if (pageIndex >= m_pageWheels.size()) {
            return false;
        }
        const auto& pw = m_pageWheels[pageIndex];
        if (slotIndex >= pw.slotActivationEmptied.size()) {
            return false;
        }
        return pw.slotActivationEmptied[slotIndex];
    }

    void WheelerClient::ClearActivationEmptied(size_t pageIndex)
    {
        std::lock_guard<std::mutex> dataLock(m_pageDataMutex);
        if (pageIndex >= m_pageWheels.size()) {
            return;
        }
        auto& pw = m_pageWheels[pageIndex];
        std::fill(pw.slotActivationEmptied.begin(), pw.slotActivationEmptied.end(), false);
    }
}

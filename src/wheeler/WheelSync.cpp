#include "WheelSync.h"
#include "WheelerConnection.h"
#include "WheelerSettings.h"

#include <algorithm>
#include <spdlog/spdlog.h>

#include "../slot/SlotSettings.h"

namespace Huginn::Wheeler
{
    // Log tag stays "[WheelerClient]" throughout, as in WheelerConnection. This
    // extraction is meant to be provably behaviour-preserving and identical log
    // output is how that gets checked: a Debug session before and after should
    // diff clean. Renaming would also break greps in the soak tooling.

    WheelSync& WheelSync::GetSingleton()
    {
        static WheelSync instance;
        return instance;
    }

    WheelerAPI::IWheelerAPI* WheelSync::Api() noexcept
    {
        return WheelerConnection::GetSingleton().Api();
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void WheelSync::DestroyWheels()
    {
        // External entry point (kPostLoadGame, settings reload). Detach the wheel
        // records under m_pageDataMutex, then issue the cross-DLL deletes with NO
        // lock held: a delete can fire a synchronous Wheeler callback, and the
        // close handler re-enters FindPageForWheel, which re-acquires
        // m_pageDataMutex — doing the API work under the lock would self-deadlock
        // on this thread and invert the documented callbackMutex → pageDataMutex
        // order for concurrent callbacks.
        std::vector<PageWheel> stale;
        {
            std::lock_guard<std::mutex> lock(m_pageDataMutex);
            stale = DetachWheelsLocked();
        }
        IssueWheelDeletes(std::move(stale));
    }

    std::vector<WheelSync::PageWheel> WheelSync::DetachWheelsLocked()
    {
        spdlog::info("[WheelerClient] Destroying {} recommendation wheels", m_pageWheels.size());

        // Vector move steals the heap buffer, so element (and subtext string)
        // addresses stay stable for the deferred API calls in IssueWheelDeletes.
        std::vector<PageWheel> stale = std::move(m_pageWheels);
        m_pageWheels.clear();  // moved-from state is unspecified; make it definitively empty
        // Wheels are recreated after save load / reload — the world state that made
        // an add fail may be gone, so failed combos get a fresh retry budget.
        m_addFailCooldowns.clear();
        return stale;
    }

    void WheelSync::IssueWheelDeletes(std::vector<PageWheel> staleWheels)
    {
        auto* api = Api();

        // Clear all subtexts BEFORE deleting wheels. Wheeler may hold const char*
        // pointers into slotSubtexts — clearing tells Wheeler to drop its references
        // so the backing strings (kept alive by staleWheels) can be safely
        // destroyed. Invalidated records (wheelIndex == -1) can't be cleared by
        // index; the label-keyed delete below removes the whole wheel, which
        // drops its entry pointers.
        for (auto& pw : staleWheels) {
            if (pw.wheelIndex >= 0 && api) {
                for (size_t i = 0; i < pw.slotCount; ++i) {
                    ClearEntrySubtext(pw.wheelIndex, static_cast<int32_t>(i));
                }
            }
        }

        // Delete the wheels. Prefer the v3 batch delete-by-client: it matches wheels
        // by their stable client label instead of a stored index, so it can't be
        // tripped by index shifting — every single-index DeleteManagedWheel reshuffles
        // the remaining indices, which used to leave our other stored indices stale
        // and orphan wheels (5 wheels instead of 3). Fall back to descending-index
        // deletes on older (v2) servers, where highest-first keeps the rest valid.
        if (api && api->version >= 3 && api->DeleteManagedWheelsForClient) {
            for (auto& pw : staleWheels) {
                // Keyed on the label, NOT wheelIndex: UpdatePage resets wheelIndex
                // to -1 when IsManagedWheel fails (e.g. after an index shift), but
                // the wheel may still exist under our name and hold pointers into
                // this record. The by-name delete is exactly the shift-safe
                // cleanup, and deleting the wheel drops those pointers.
                if (!pw.wheelLabel) {
                    continue;  // placeholder — no wheel was ever created
                }
                int32_t n = api->DeleteManagedWheelsForClient(pw.wheelLabel->c_str());
                if (n < 0) {
                    spdlog::warn("[WheelerClient] DeleteManagedWheelsForClient('{}') failed: {}",
                        *pw.wheelLabel, n);
                } else {
                    spdlog::debug("[WheelerClient] Deleted {} managed wheel(s) for '{}'", n, *pw.wheelLabel);
                }
            }
        } else if (api) {
            std::vector<int32_t> indices;
            for (auto& pw : staleWheels) {
                if (pw.wheelIndex >= 0) {
                    indices.push_back(pw.wheelIndex);
                }
            }
            std::sort(indices.rbegin(), indices.rend());  // highest index first
            for (int32_t idx : indices) {
                auto result = api->DeleteManagedWheel(idx);
                spdlog::debug("[WheelerClient] Deleted wheel {}: {}", idx, static_cast<int>(result));
            }
        }
    }

    bool WheelSync::CreateWheels()
    {
        auto* api = Api();
        if (!api) {
            spdlog::error("[WheelerClient] Cannot create recommendation wheels - not connected");
            return false;
        }

        if (!api->IsInitialized()) {
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

        // Check if wheels already exist and are valid. Probe the FIRST VALID wheel
        // rather than page 0: page 0 may be a wheelIndex=-1 placeholder (zero-slot or
        // a transient creation failure) while later pages hold real wheels. Keying on
        // page 0 alone would tear down and recreate every valid wheel on each call.
        if (!m_pageWheels.empty()) {
            int32_t probeWheel = -1;
            for (const auto& pw : m_pageWheels) {
                if (pw.wheelIndex >= 0) { probeWheel = pw.wheelIndex; break; }
            }
            if (probeWheel >= 0) {
                if (api->IsManagedWheel(probeWheel)) {
                    spdlog::debug("[WheelerClient] Recommendation wheels already exist ({} pages)", m_pageWheels.size());
                    return true;
                }
                // Wheels were invalidated (save/load cycle) - recreate
                spdlog::info("[WheelerClient] Managed wheels invalidated after save/load, recreating...");
                // ASSUMPTION: deletes for already-invalidated wheels do not fire
                // synchronous callbacks, so issuing them under m_pageDataMutex is
                // safe here — mirrors the CreateManagedWheel calls below, which
                // run under the same lock for the same reason.
                IssueWheelDeletes(DetachWheelsLocked());
            } else {
                // Only stale placeholder/invalid indices — clean up before recreating
                IssueWheelDeletes(DetachWheelsLocked());
            }
        }

        // Create one wheel per page
        m_pageWheels.reserve(pageCount);

        // On any per-page failure, still push a wheelIndex=-1 placeholder so
        // m_pageWheels stays 1:1 with page indices. Without it, a later page's
        // wheel would take the failed page's slot in the vector and
        // FindPageForWheel (which returns the vector index AS the page index)
        // would map wheels to the wrong page. Mirrors the zero-slot path below.
        auto pushPlaceholder = [&](size_t page, const std::string& name) {
            PageWheel placeholder;
            placeholder.wheelIndex = -1;
            placeholder.slotCount = 0;
            placeholder.pageName = name;
            m_pageWheels.push_back(std::move(placeholder));
            spdlog::warn("[WheelerClient] Page {} wheel unavailable — inserting placeholder to preserve page mapping", page);
        };

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
            pageWheel.slotRawSubtexts.resize(slotCount);
            pageWheel.slotRetries.resize(slotCount, 0);
            pageWheel.slotActivationEmptied.resize(slotCount, false);

            // Build wheel label. Wheeler holds the exported const char* indefinitely,
            // so it lives in heap storage whose address survives the push_back move
            // below AND any later m_pageWheels reallocation (a plain std::string
            // member would relocate its SSO bytes on move, dangling Wheeler's copy).
            if (pageCount > 1) {
                pageWheel.wheelLabel = std::make_unique<std::string>(std::format("Huginn: {}", pageConfig.name));
            } else {
                pageWheel.wheelLabel = std::make_unique<std::string>("Huginn");
            }

            // Create wheel with appropriate slot count
            // NOTE: clientName points into heap storage owned by pageWheel.wheelLabel,
            // which must be populated BEFORE this export (see PageWheel declaration).
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

            if (api->version >= 2) {
                WheelerAPI::WheelConfig config = {
                    .numEntries = static_cast<int32_t>(slotCount),
                    .position = pagePosition,
                    .managed = true,
                    .clientName = pageWheel.wheelLabel->c_str(),
                    .showLabel = true,
                    .labelFontSize = 0,
                    .labelColor = 0,
                    .labelOffsetY = 0,
                    .indicatorText = "O",  // "O" for Huginn
                    .indicatorActiveColor = 0,
                    .indicatorInactiveColor = 0
                };
                pageWheel.wheelIndex = api->CreateManagedWheel(&config);
            } else {
                WheelerAPI::WheelConfigV1 config = {
                    .numEntries = static_cast<int32_t>(slotCount),
                    .position = pagePosition,
                    .managed = true,
                    .clientName = pageWheel.wheelLabel->c_str(),
                    .showLabel = true
                };
                pageWheel.wheelIndex = api->CreateManagedWheel(
                    reinterpret_cast<const WheelerAPI::WheelConfig*>(&config));
            }

            if (pageWheel.wheelIndex < 0) {
                spdlog::error("[WheelerClient] Failed to create wheel for page {}: {}",
                    p, pageWheel.wheelIndex);
                pushPlaceholder(p, pageConfig.name);
                continue;
            }

            // Post-creation entry count repair: Wheeler may create fewer entries than
            // requested (observed during new game). Add missing entries individually.
            int32_t actualEntries = api->GetEntryCount(pageWheel.wheelIndex);
            int32_t targetEntries = static_cast<int32_t>(slotCount);
            if (actualEntries < 0) {
                spdlog::error("[WheelerClient] Page {} GetEntryCount returned negative ({}), skipping wheel",
                    p, actualEntries);
                api->DeleteManagedWheel(pageWheel.wheelIndex);  // wheel was created; don't orphan it
                pushPlaceholder(p, pageConfig.name);
                continue;
            } else if (actualEntries > targetEntries) {
                spdlog::warn("[WheelerClient] Page {} wheel {} has more entries ({}) than requested ({}), discarding excess",
                    p, pageWheel.wheelIndex, actualEntries, targetEntries);
                for (int32_t e = actualEntries - 1; e >= targetEntries; --e) {
                    api->DeleteEntry(pageWheel.wheelIndex, e);
                }
            } else if (actualEntries < targetEntries) {
                spdlog::warn("[WheelerClient] Page {} wheel {} created with {}/{} entries, adding missing entries",
                    p, pageWheel.wheelIndex, actualEntries, targetEntries);
                for (int32_t e = actualEntries; e < targetEntries; ++e) {
                    int32_t result = api->AddEntry(pageWheel.wheelIndex);
                    if (result < 0) {
                        spdlog::error("[WheelerClient] Page {} AddEntry failed at entry {} (result={})",
                            p, e, result);
                        break;
                    }
                }
                // Re-check and adjust slot count to match reality
                actualEntries = api->GetEntryCount(pageWheel.wheelIndex);
                if (actualEntries < 0) {
                    spdlog::error("[WheelerClient] Page {} GetEntryCount returned negative ({}) after repair, skipping wheel",
                        p, actualEntries);
                    api->DeleteManagedWheel(pageWheel.wheelIndex);  // wheel was created; don't orphan it
                    pushPlaceholder(p, pageConfig.name);
                    continue;
                }
                if (actualEntries < targetEntries) {
                    spdlog::warn("[WheelerClient] Page {} could only create {}/{} entries, adjusting slot count",
                        p, actualEntries, targetEntries);
                    pageWheel.slotCount = static_cast<size_t>(actualEntries);
                    pageWheel.slotFormIDs.resize(pageWheel.slotCount, 0);
                    pageWheel.slotWildcard.resize(pageWheel.slotCount, false);
                    pageWheel.slotUniqueIDs.resize(pageWheel.slotCount, 0);
                    pageWheel.slotSubtexts.resize(pageWheel.slotCount);
                    pageWheel.slotRawSubtexts.resize(pageWheel.slotCount);
                    pageWheel.slotRetries.resize(pageWheel.slotCount, 0);
                    pageWheel.slotActivationEmptied.resize(pageWheel.slotCount, false);
                }
            }

            if (pageWheel.slotCount != slotCount)
                spdlog::info("[WheelerClient] Created wheel for page {} '{}': index={}, {} slots (requested {})",
                    p, pageConfig.name, pageWheel.wheelIndex, pageWheel.slotCount, slotCount);
            else
                spdlog::info("[WheelerClient] Created wheel for page {} '{}': index={}, {} slots",
                    p, pageConfig.name, pageWheel.wheelIndex, pageWheel.slotCount);

            m_pageWheels.push_back(std::move(pageWheel));
        }

        // Placeholders keep m_pageWheels 1:1 with pages, so "not empty" no longer
        // implies real wheels exist — check for at least one valid wheel index.
        const bool anyValidWheel = std::any_of(m_pageWheels.begin(), m_pageWheels.end(),
            [](const PageWheel& pw) { return pw.wheelIndex >= 0; });
        if (!anyValidWheel) {
            spdlog::error("[WheelerClient] Failed to create any wheels");
            m_pageWheels.clear();  // drop placeholder-only state
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

    bool WheelSync::HasAnyWheel() const
    {
        // Thread safety: Protect iteration over m_pageWheels — Create/Destroy
        // can reallocate or clear it from another thread (SKSE message, settings reload).
        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        for (const auto& pw : m_pageWheels) {
            if (pw.wheelIndex >= 0) {
                return true;
            }
        }
        return false;
    }

    // ============================================================================
    // Lookup
    // ============================================================================

    int32_t WheelSync::WheelIndexForPage(size_t pageIndex) const
    {
        // Thread safety: Protect size check and [] access (TOCTOU race prevention)
        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        if (pageIndex < m_pageWheels.size()) {
            return m_pageWheels[pageIndex].wheelIndex;
        }
        return -1;
    }

    int WheelSync::FindPageForWheel(int32_t wheelIndex) const
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

    bool WheelSync::IsOurWheel(int32_t wheelIndex) const
    {
        return FindPageForWheel(wheelIndex) >= 0;
    }

    int WheelSync::ActiveManagedPage() const
    {
        auto* api = Api();
        if (!api || !api->IsWheelOpen()) return -1;
        int32_t activeWheel = api->GetActiveWheelIndex();
        return FindPageForWheel(activeWheel);
    }

    WheelSync::OpenedWheelInfo WheelSync::DescribeOpenedWheel(int32_t wheelIndex) const
    {
        // Everything the open handler needs, resolved under ONE lock. The old
        // code hand-inlined this loop inside a function that already held the
        // mutex, purely to avoid recursing into FindPageForWheel. Answering in
        // one call removes that reason to inline AND removes the alternative
        // hazard — three separate locked lookups could interleave with a
        // teardown and disagree with each other.
        OpenedWheelInfo info;

        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        for (size_t i = 0; i < m_pageWheels.size(); ++i) {
            if (m_pageWheels[i].wheelIndex == wheelIndex) {
                info.pageIndex = static_cast<int>(i);
                info.pageName = m_pageWheels[i].pageName;
                break;
            }
        }

        for (const auto& pw : m_pageWheels) {
            if (pw.wheelIndex >= 0) { info.firstValidWheel = pw.wheelIndex; break; }
        }

        return info;
    }

    // ============================================================================
    // Navigation
    // ============================================================================

    bool WheelSync::SetActivePage(size_t pageIndex)
    {
        auto* api = Api();
        if (!api) return false;
        int32_t wheelIndex = WheelIndexForPage(pageIndex);
        if (wheelIndex < 0) return false;
        auto result = api->SetActiveWheelIndex(wheelIndex);
        if (result == WheelerAPI::Result::OK) {
            logger::debug("[WheelerClient] SetActivePage({}) -> wheel {}"sv, pageIndex, wheelIndex);
            return true;
        }
        return false;
    }

    bool WheelSync::TryUrgentAutoFocus(int overridePriority)
    {
        auto* api = Api();
        if (!api || !api->IsWheelOpen()) {
            return false;
        }

        auto& settings = WheelerSettings::GetSingleton();
        if (!settings.GetAutoFocusOnOverride()) {
            return false;
        }

        if (overridePriority < settings.GetAutoFocusMinPriority()) {
            return false;
        }

        // Snapshot the first page with a real wheel under the lock. Page 0 may be a
        // wheelIndex=-1 placeholder while later pages hold valid wheels, so we can't
        // key on [0]. Capture the target here and release the lock before calling any
        // Wheeler API (SetActiveWheelIndex can fire OnWheelStateChanged synchronously,
        // which locks m_pageDataMutex — holding it here would deadlock).
        int32_t targetWheel = -1;
        {
            std::lock_guard<std::mutex> lock(m_pageDataMutex);
            for (const auto& pw : m_pageWheels) {
                if (pw.wheelIndex >= 0) { targetWheel = pw.wheelIndex; break; }
            }
        }
        if (targetWheel < 0) {
            return false;
        }

        // Check if Wheeler is already on one of our wheels
        int32_t activeWheel = api->GetActiveWheelIndex();
        if (IsOurWheel(activeWheel)) {
            return false;  // Already on Huginn wheel, no need to focus
        }
        spdlog::info("[WheelerClient] Urgent auto-focus: override priority {} >= {}, "
                     "focusing from wheel {} to Huginn wheel {}",
            overridePriority, settings.GetAutoFocusMinPriority(), activeWheel, targetWheel);

        // Call SetActiveWheelIndex — safe because we're not holding any lock
        // (this is called from the update loop, not from a callback)
        auto result = api->SetActiveWheelIndex(targetWheel);
        if (result != WheelerAPI::Result::OK) {
            spdlog::warn("[WheelerClient] SetActiveWheelIndex({}) failed: {}",
                targetWheel, static_cast<int>(result));
            return false;
        }
        return true;
    }

    // ============================================================================
    // Content
    // ============================================================================

    void WheelSync::UpdatePage(size_t pageIndex,
                               const std::vector<RE::FormID>& formIDs,
                               const std::vector<bool>& isWildcard,
                               const std::vector<uint16_t>& uniqueIDs,
                               const std::vector<std::string>& subtexts)
    {
        // Early validation (before lock - no shared state access)
        auto* api = Api();
        if (!api) {
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

        // Content-unchanged early-out: when nothing this page displays has changed
        // since the last update, skip the per-tick cross-DLL pre-flight validation
        // (IsManagedWheel / IsWheelEmpty / GetEntryCount) AND the per-slot diff loop.
        // Steady state (stable recommendations) is the common case, so this removes
        // the recurring per-page Wheeler API traffic. slotRawSubtexts caches the
        // incoming subtexts — slotSubtexts holds the final wildcard-applied label, so
        // it can't be compared against the incoming vector directly.
        //
        // Two things are intentionally deferred while a page's content is static:
        //   (a) a settings reload that changes only the wildcard label text (no item
        //       change) won't re-apply the label until the next content change; and
        //   (b) a wheel invalidated externally (another mod deleting it, or an index
        //       shift) won't be re-detected here — the IsManagedWheel check is skipped
        //       — until content changes or the wheels are recreated on reload/postload.
        // Both are rare and self-correcting, and the early-out writes nothing in this
        // state, so it can never corrupt another mod's wheel — detection is only delayed.
        if (formIDs   == pageWheel.slotFormIDs &&
            isWildcard == pageWheel.slotWildcard &&
            uniqueIDs  == pageWheel.slotUniqueIDs &&
            subtexts   == pageWheel.slotRawSubtexts) {
            return;
        }

        // Validate wheel ownership - another mod may have deleted it or indices shifted
        if (!api->IsManagedWheel(pageWheel.wheelIndex)) {
            spdlog::warn("[WheelerClient] Page {} wheel {} no longer managed, marking invalid",
                pageIndex, pageWheel.wheelIndex);
            pageWheel.wheelIndex = -1;
            return;
        }

        // A3: Safety pre-check — verify wheel has entries (catches unexpected state)
        if (api->IsWheelEmpty(pageWheel.wheelIndex)) {
            spdlog::warn("[WheelerClient] Page {} wheel {} is unexpectedly empty after creation",
                pageIndex, pageWheel.wheelIndex);
            return;
        }

        int32_t entryCount = api->GetEntryCount(pageWheel.wheelIndex);
        if (entryCount <= 0) {
            spdlog::warn("[WheelerClient] Page {} wheel has no entries", pageIndex);
            return;
        }

        // Lock now held for the per-slot update loop below

        static constexpr uint8_t MAX_SLOT_RETRIES = 3;
        const auto nowTime = std::chrono::steady_clock::now();

        // Update each slot (bounds-check all vectors to prevent out-of-range access)
        int32_t maxSlots = std::min(static_cast<int32_t>(pageWheel.slotCount), entryCount);
        maxSlots = std::min(maxSlots, static_cast<int32_t>(pageWheel.slotFormIDs.size()));
        for (int32_t i = 0; i < maxSlots; ++i) {
            size_t idx = static_cast<size_t>(i);
            RE::FormID newFormID = (idx < formIDs.size()) ? formIDs[idx] : 0;
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
                // Negative-cache check FIRST: a (formID, uniqueID) Wheeler already
                // rejected MAX_SLOT_RETRIES times must not clear the current entry
                // or hit the API again until its cooldown expires. Adopt the cache
                // so the diff goes quiet; the combo retries naturally afterwards.
                if (newFormID != 0) {
                    if (auto it = m_addFailCooldowns.find(AddFailKey(newFormID, newUniqueID));
                        it != m_addFailCooldowns.end()) {
                        if (nowTime - it->second < ADD_FAIL_COOLDOWN) {
                            pageWheel.slotFormIDs[idx] = newFormID;
                            pageWheel.slotUniqueIDs[idx] = newUniqueID;
                            pageWheel.slotRetries[idx] = 0;
                            continue;
                        }
                        m_addFailCooldowns.erase(it);
                    }
                }

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
                        auto removeResult = api->RemoveItem(pageWheel.wheelIndex, i, 0);
                        if (removeResult != WheelerAPI::Result::OK) {
                            // Fallback to ClearEntry if RemoveItem fails
                            spdlog::debug("[WheelerClient] RemoveItem failed ({}), falling back to ClearEntry",
                                static_cast<int>(removeResult));
                            api->ClearEntry(pageWheel.wheelIndex, i);
                        }
                    } else {
                        // Clearing to empty — ClearEntry is appropriate
                        api->ClearEntry(pageWheel.wheelIndex, i);
                    }
                }
                if (newFormID != 0) {
                    int32_t result = api->AddItemByFormID(pageWheel.wheelIndex, i, newFormID, newUniqueID);
                    if (result < 0) {
                        // A3: Use IsEntryEmpty to decide recovery strategy
                        bool entryEmpty = api->IsEntryEmpty(pageWheel.wheelIndex, i);
                        if (entryEmpty && cachedFormID != 0) {
                            // Entry is empty after removal — restore previous item
                            api->AddItemByFormID(pageWheel.wheelIndex, i, cachedFormID, cachedUniqueID);
                        } else if (!entryEmpty) {
                            spdlog::debug("[WheelerClient] Entry {} not empty after AddItem failure, skipping restore", i);
                        }

                        ++pageWheel.slotRetries[idx];
                        if (pageWheel.slotRetries[idx] >= MAX_SLOT_RETRIES) {
                            spdlog::warn("[WheelerClient] AddItemByFormID {:08X} uid={} slot {} failed {} times (result={}) — suppressing retries for {}s",
                                newFormID, newUniqueID, i, pageWheel.slotRetries[idx], result,
                                ADD_FAIL_COOLDOWN.count());
                            // Start the cooldown and cache to stop retrying this
                            // FormID+UniqueID combination (see m_addFailCooldowns)
                            if (m_addFailCooldowns.size() > 32) {
                                std::erase_if(m_addFailCooldowns, [&](const auto& e) {
                                    return nowTime - e.second >= ADD_FAIL_COOLDOWN;
                                });
                            }
                            m_addFailCooldowns[AddFailKey(newFormID, newUniqueID)] = nowTime;
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

            // 3. Apply subtext change if different from cached (null cache ≙ empty)
            auto& cachedSubtext = pageWheel.slotSubtexts[idx];
            const bool subtextChanged =
                cachedSubtext ? (newSubtext != *cachedSubtext) : !newSubtext.empty();
            if (subtextChanged || newWildcard != cachedWildcard || newFormID != cachedFormID) {
                // Wheeler stores the exported const char* and reads it while
                // rendering, so the OLD buffer must stay alive and untouched until
                // the API call below has handed Wheeler the new pointer. The new
                // string sits in its own heap allocation, so its address stays
                // stable for as long as Wheeler holds it.
                // Allocate the replacement FIRST: if make_unique throws, the old
                // (still-exported) buffer stays in place in the cache.
                auto fresh = std::make_unique<std::string>(std::move(newSubtext));
                auto retiring = std::move(cachedSubtext);
                cachedSubtext = std::move(fresh);
                pageWheel.slotWildcard[idx] = newWildcard;

                if (!cachedSubtext->empty()) {
                    SetEntrySubtext(pageWheel.wheelIndex, i, cachedSubtext->c_str());
                } else {
                    ClearEntrySubtext(pageWheel.wheelIndex, i);
                }
                // `retiring` is destroyed here — only after Wheeler swapped pointers.
            }
        }

        // Remember the raw inputs so the next update can early-out when unchanged.
        pageWheel.slotRawSubtexts = subtexts;
    }

    // ============================================================================
    // v2 API: Entry Subtext Support
    // ============================================================================

    void WheelSync::SetEntrySubtext(int32_t wheelIndex, int32_t entryIndex, const char* text)
    {
        auto* api = Api();
        if (!api || api->version < 2) {
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

        api->SetManagedWheelEntrySubtext(wheelIndex, entryIndex, &config);
    }

    void WheelSync::ClearEntrySubtext(int32_t wheelIndex, int32_t entryIndex)
    {
        auto* api = Api();
        if (!api || api->version < 2) {
            return;
        }

        api->SetManagedWheelEntrySubtext(wheelIndex, entryIndex, nullptr);
    }

    // ============================================================================
    // Post-Activation Policy (Empty)
    // ============================================================================

    std::optional<WheelSync::DeferredEntryClear> WheelSync::MarkActivationEmptied(
        size_t pageIndex, size_t slotIndex)
    {
        // Size check must sit INSIDE the lock: DestroyWheels can clear
        // m_pageWheels between an unlocked check and the index.
        std::lock_guard<std::mutex> dataLock(m_pageDataMutex);

        if (pageIndex >= m_pageWheels.size()) {
            return std::nullopt;
        }

        auto& pw = m_pageWheels[pageIndex];
        if (slotIndex < pw.slotActivationEmptied.size() &&
            slotIndex < pw.slotFormIDs.size() &&
            slotIndex < pw.slotUniqueIDs.size()) {
            pw.slotActivationEmptied[slotIndex] = true;
            pw.slotFormIDs[slotIndex] = 0;
            pw.slotUniqueIDs[slotIndex] = 0;
        }

        // Reported even when the slot index was out of range, matching the
        // previous behaviour: the entry still exists on the wheel and the
        // caller still wants it cleared.
        return DeferredEntryClear{pw.wheelIndex, static_cast<int32_t>(slotIndex)};
    }

    bool WheelSync::IsSlotActivationEmptied(size_t pageIndex, size_t slotIndex) const
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

    void WheelSync::ClearAllActivationEmptied()
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

    // ============================================================================
    // Diagnostics
    // ============================================================================

    void WheelSync::ValidateWheelState() const
    {
#ifndef NDEBUG
        auto* api = Api();
        if (!api) {
            return;
        }

        // MarkActivationEmptied writes slotFormIDs/slotUniqueIDs under
        // m_pageDataMutex; this diagnostic iteration needs the same lock.
        std::lock_guard<std::mutex> lock(m_pageDataMutex);

        for (size_t p = 0; p < m_pageWheels.size(); ++p) {
            const auto& pw = m_pageWheels[p];
            if (pw.wheelIndex < 0) {
                continue;
            }

            // Verify wheel still exists and is managed
            if (!api->IsManagedWheel(pw.wheelIndex)) {
                spdlog::warn("[WheelerClient] ValidateWheelState: Page {} wheel {} is no longer managed!",
                    p, pw.wheelIndex);
                continue;
            }

            int32_t entryCount = api->GetEntryCount(pw.wheelIndex);
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

                uint32_t actualFormID = api->GetItemFormID(pw.wheelIndex, i, 0);
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
}

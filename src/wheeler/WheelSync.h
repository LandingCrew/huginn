#pragma once

#include "WheelerAPI.h"

#include <chrono>
#include <cstddef>  // size_t
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Huginn::Wheeler
{
    /// Owns Huginn's managed wheels: creating them, tearing them down, and
    /// pushing per-slot content into them.
    ///
    /// Step 2 of the WheelerClient split. The state-ownership pass found this to
    /// be the genuinely contested group — m_pageWheels was read and written by
    /// both the update loop and Wheeler's callback thread. It is now exclusive
    /// to this class, and nothing outside ever receives a PageWheel reference:
    /// callers ask questions and get answers by value, so a record cannot be
    /// examined after the lock that protected it was released.
    ///
    /// LOCK ORDERING: WheelerClient::m_callbackMutex (outer) → m_pageDataMutex
    /// (inner). Expressed structurally as "WheelerClient may call WheelSync,
    /// WheelSync never calls back into WheelerClient." Keep it that way — the
    /// order is no longer enforced by both mutexes living in one class.
    ///
    /// Every public method that touches page data takes m_pageDataMutex itself,
    /// and none may be called with it already held. SetEntrySubtext is the one
    /// exception: it touches no page state and deliberately takes no lock, which
    /// is why UpdatePage may call it from inside the critical section. Do not
    /// "fix" that by adding a lock — the mutex is not recursive.
    ///
    /// That rule is why DescribeOpenedWheel and MarkActivationEmptied exist:
    /// their callers used to hold the lock and hand-inline the lookups to avoid
    /// recursing into it. A single locked call that returns everything the
    /// caller needs is both safer and more consistent than several, which could
    /// otherwise interleave with a teardown between them.
    class WheelSync
    {
    public:
        static WheelSync& GetSingleton();

        // ====================================================================
        // Lifecycle
        // ====================================================================

        /// Create one managed wheel per configured page. Idempotent: returns
        /// true immediately if the existing wheels are still valid.
        bool CreateWheels();

        /// Tear down every wheel. Detaches the records under the lock, then
        /// issues the Wheeler deletes with NO lock held — a delete can fire a
        /// synchronous callback that re-enters this class.
        void DestroyWheels();

        /// True if at least one page holds a real wheel. Scans all pages rather
        /// than only page 0: a failed or zero-slot page 0 is stored as a
        /// wheelIndex=-1 placeholder while later pages may hold real wheels.
        [[nodiscard]] bool HasAnyWheel() const;

        /// #76 recovery. UpdatePage sets wheelIndex = -1 when IsManagedWheel
        /// fails and nothing retries, so once every page is invalidated the
        /// wheel is dead for the rest of the session — HasAnyWheel() then
        /// returns false, the display push returns early, and UpdatePage is
        /// never reached again to notice anything changed. But "every page
        /// invalid" is indistinguishable from "wheels were never created",
        /// which is a state CreateWheels already knows how to fix.
        ///
        /// Rebuilds rather than latching, bounded (attempts + cooldown) so a
        /// genuinely absent or broken Wheeler cannot spin. No-op unless EVERY
        /// page that once held a wheel has lost it: a single invalidated page
        /// among healthy ones is a different problem and not this one's to fix.
        /// Takes m_pageDataMutex, releases it, then calls CreateWheels — never
        /// holds it across that call.
        bool RecoverInvalidatedWheels();

        /// Re-derive every page's wheel index from its client label, which is the
        /// only key that survives Wheeler reindexing. Returns true if any page's
        /// index actually moved.
        ///
        /// WHY THIS IS NEEDED AT ALL: a stored wheel index is identity by
        /// POSITION, and position is not stable. Wheeler shifts indices whenever
        /// a wheel is inserted or removed — including by the player, in edit
        /// mode, with no notification Huginn can act on. Observed 2026-08-21:
        /// wheels created as Smart=0/Inventory=1/Regulars=2, a player wheel later
        /// displacing 0, leaving ours at 1/2/3 while Huginn kept writing subtexts
        /// to 0/1/2. Those writes were ACCEPTED — IsManagedWheel() only answers
        /// "is this wheel managed", not "is it MINE", so the ownership pre-check
        /// in UpdatePage passes on another client's wheel and cannot catch this.
        ///
        /// Requires API v4 (GetManagedWheelsForClient). On v3 and below there is
        /// no lookup that distinguishes our managed wheels from another mod's, so
        /// this is a no-op that warns once per generation.
        bool ReResolveWheelIndices();

        // ====================================================================
        // Content
        // ====================================================================

        /// Push a page's slot contents to its wheel. No-op if the page has no
        /// wheel, or if nothing displayed has changed since the last call.
        void UpdatePage(size_t pageIndex,
                        const std::vector<RE::FormID>& formIDs,
                        const std::vector<bool>& isWildcard,
                        const std::vector<uint16_t>& uniqueIDs,
                        const std::vector<std::string>& subtexts);

        /// Set (text != nullptr) or clear an entry's subtext. Public because the
        /// Empty post-activation policy applies an "Equipped" label from the
        /// callback path, outside any lock. v2 API required; no-op otherwise.
        ///
        /// Takes the handle rather than re-loading it, so a caller that hoisted
        /// `api` keeps one consistent view across its whole sequence — see the
        /// load-once rule in WheelerConnection.h. Without this, an add could
        /// succeed and its label silently vanish on a mid-sequence disconnect.
        void SetEntrySubtext(WheelerAPI::IWheelerAPI* api,
                             int32_t wheelIndex, int32_t entryIndex, const char* text);

        // ====================================================================
        // Lookup
        // ====================================================================

        /// Wheel index for a page, or -1 if the page has none.
        [[nodiscard]] int32_t WheelIndexForPage(size_t pageIndex) const;

        /// Page index owning a wheel, or -1 if the wheel is not ours.
        [[nodiscard]] int FindPageForWheel(int32_t wheelIndex) const;

        [[nodiscard]] bool IsOurWheel(int32_t wheelIndex) const;

        /// Page index of the wheel Wheeler is currently showing, or -1 if
        /// Wheeler is closed, disconnected, or showing someone else's wheel.
        [[nodiscard]] int ActiveManagedPage() const;

        /// Everything the wheel-opened callback needs, resolved under ONE lock.
        struct OpenedWheelInfo
        {
            int pageIndex = -1;             ///< -1 if the wheel is not ours
            std::string pageName;           ///< empty unless pageIndex >= 0
            int32_t firstValidWheel = -1;   ///< auto-focus fallback; -1 if we have none
        };
        [[nodiscard]] OpenedWheelInfo DescribeOpenedWheel(int32_t wheelIndex) const;

        // ====================================================================
        // Navigation
        // ====================================================================

        /// Point Wheeler at a page's wheel. False if disconnected, the page has
        /// no wheel, or Wheeler refused.
        bool SetActivePage(size_t pageIndex);

        /// Pull Wheeler onto our first real wheel when a sufficiently urgent
        /// override fires. Returns true if focus was actually moved.
        /// THREAD SAFETY: calls SetActiveWheelIndex with no lock held.
        bool TryUrgentAutoFocus(int overridePriority);

        // ====================================================================
        // Post-activation policy (Empty)
        // ====================================================================

        /// The entry the caller must clear through the Wheeler API once it has
        /// dropped its locks. Returned rather than acted on because clearing an
        /// entry is a cross-DLL call, and those must not run under a lock.
        struct DeferredEntryClear
        {
            int32_t wheelIndex = -1;
            int32_t entryIndex = -1;
        };

        /// Mark a slot activation-emptied and drop its cached item, so the next
        /// update does not treat the entry as already populated. Returns the
        /// entry to clear, or nullopt if the page no longer exists.
        std::optional<DeferredEntryClear> MarkActivationEmptied(size_t pageIndex, size_t slotIndex);

        [[nodiscard]] bool IsSlotActivationEmptied(size_t pageIndex, size_t slotIndex) const;

        /// Clear the activation-emptied flags on every page, letting Empty-policy
        /// slots repopulate. Called when the wheel closes.
        void ClearAllActivationEmptied();

        // ====================================================================
        // Diagnostics
        // ====================================================================

        /// Verify cached FormIDs still match Wheeler's actual state.
        /// Debug builds only — logs mismatches at warn. Zero cost in Release.
        void ValidateWheelState() const;

    private:
        WheelSync() = default;

        /// Shorthand for the handle owned by WheelerConnection. Load it once per
        /// function into a local; see WheelerConnection.h for why.
        [[nodiscard]] static WheelerAPI::IWheelerAPI* Api() noexcept;

        /// True if Wheeler needs a non-zero uniqueID to accept this form —
        /// weapons and armour, whose instances differ by tempering and
        /// enchantment. See the certain-reject guard in UpdatePage (#74).
        [[nodiscard]] static bool RequiresUniqueID(RE::FormID formID);

        struct PageWheel
        {
            int32_t wheelIndex = -1;                    // Wheeler wheel index
            size_t slotCount = 0;                       // Number of slots for this page
            std::string pageName;                       // Page name (e.g., "Combat")
            // Strings whose c_str() is exported to Wheeler are indefinite borrows:
            // Wheeler stores the pointer and reads it while rendering, so the
            // buffer must stay alive AND address-stable until replaced/cleared
            // through the API. Heap-owned (unique_ptr) storage survives PageWheel
            // moves and m_pageWheels reallocation; a plain std::string does not
            // (MSVC SSO relocates short-string bytes on every move).
            std::unique_ptr<std::string> wheelLabel;    // Full wheel label (e.g., "Huginn: Combat"); non-null iff a wheel was ever created for this record (survives index invalidation — Wheeler may still hold the pointer)
            std::vector<RE::FormID> slotFormIDs;        // Cached FormIDs per slot
            std::vector<bool> slotWildcard;             // Wildcard flags per slot
            std::vector<uint16_t> slotUniqueIDs;        // Cached UniqueIDs per slot (for weapons)
            std::vector<std::unique_ptr<std::string>> slotSubtexts;  // Cached FINAL subtext labels (wildcard-applied); exported to Wheeler (see above); null ≙ empty
            std::vector<std::string> slotRawSubtexts;    // Cached RAW incoming subtexts (for the content-unchanged early-out)
            std::vector<uint8_t> slotRetries;            // Retry counter per slot (max MAX_SLOT_RETRIES)
            std::vector<uint8_t> slotUniqueIDDefers;     // Consecutive uniqueID defers per slot (max MAX_UNIQUEID_DEFERS; see #74 guard)
            std::vector<bool> slotActivationEmptied;     // Activation-emptied flags (Empty policy)
        };

        std::vector<PageWheel> m_pageWheels;            // One wheel per page. GUARDED_BY(m_pageDataMutex).

        // Negative cache for AddItemByFormID rejects. After MAX_SLOT_RETRIES
        // consecutive failures of the same (formID, uniqueID), further attempts
        // are suppressed for ADD_FAIL_COOLDOWN — slot churn (lock expiry,
        // re-allocation) otherwise restarts the retry cycle every few seconds
        // (observed: 569 API rejects in 11 min from one bad save entry that
        // Wheeler answers with UnsupportedFormType). Keyed globally, not
        // per-page: the same combo fails identically on every wheel.
        // GUARDED_BY(m_pageDataMutex).
        std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_addFailCooldowns;

        /// #76 diagnostics/recovery state. Poll thread only in practice, but they
        /// live under m_pageDataMutex with everything else they are read beside.
        /// All three are cleared by a successful CreateWheels, so each generation
        /// of wheels gets its own census and its own retry budget.
        bool m_censusLogged = false;                                  // one census per generation
        bool m_reResolveUnsupportedLogged = false;                    // one v3-server warning per generation
        int  m_recoveryAttempts = 0;
        std::chrono::steady_clock::time_point m_lastRecoveryAttempt{};
        static constexpr std::chrono::seconds ADD_FAIL_COOLDOWN{30};
        [[nodiscard]] static constexpr uint64_t AddFailKey(RE::FormID formID, uint16_t uniqueID) noexcept
        {
            return (static_cast<uint64_t>(formID) << 16) | uniqueID;
        }

        /// Protects m_pageWheels and m_addFailCooldowns. Mutable so const
        /// accessors can lock. See the lock-ordering note on the class.
        mutable std::mutex m_pageDataMutex;

        void ClearEntrySubtext(WheelerAPI::IWheelerAPI* api, int32_t wheelIndex, int32_t entryIndex);

        /// Detach all page-wheel records for teardown. REQUIRES: m_pageDataMutex
        /// held. The returned vector keeps the subtext strings alive (addresses
        /// stable — the vector move steals the buffer) until IssueWheelDeletes
        /// has told Wheeler to drop its references.
        [[nodiscard]] std::vector<PageWheel> DetachWheelsLocked();

        /// Issue the cross-DLL subtext-clear + wheel-delete calls for detached
        /// wheels. External teardown paths call this WITHOUT m_pageDataMutex held
        /// (a delete may fire a synchronous callback that re-acquires it);
        /// CreateWheels calls it under the lock on the documented assumption that
        /// already-invalidated wheels fire no callbacks.
        void IssueWheelDeletes(std::vector<PageWheel> staleWheels);

        /// #76 census: dump what Wheeler actually holds at the moment a page is
        /// invalidated. Call with m_pageDataMutex HELD (it reads m_pageWheels);
        /// only touches the Wheeler API and this object. Once per generation —
        /// the answer cannot change between pages of the same failure.
        void LogWheelCensusLocked(size_t pageIndex);

        enum class ResolveOutcome
        {
            Unchanged,  ///< the lookup agrees with the stored index
            Moved,      ///< the index changed; the page was reset and re-adopted
            Gone,       ///< no wheel carries our label; the index is now -1
            Unknown     ///< the lookup could not answer; the stored index is untouched
        };

        /// Re-derive ONE page's wheel index from its label. REQUIRES:
        /// m_pageDataMutex held.
        ///
        /// Unknown is deliberately distinct from Gone. A negative Result means
        /// Wheeler could not answer (not initialized, mid-reload); that is not
        /// evidence the wheel moved OR vanished, and treating it as either would
        /// re-create the #76 latch this exists to remove.
        ///
        /// Safe to call under the lock: GetManagedWheelsForClient is a pure read
        /// that takes Wheeler's wheel-data lock in shared mode and dispatches no
        /// notifications, so it cannot re-enter us. That is the same premise
        /// CreateWheels and UpdatePage already rely on for IsManagedWheel and
        /// GetEntryCount, and upstream states it explicitly ("Wheeler never
        /// invokes a callback while holding its wheel-data lock").
        ResolveOutcome ReResolvePageLocked(WheelerAPI::IWheelerAPI* api, size_t pageIndex);

        /// Return a moved page to the state CreateWheels leaves it in: Wheeler
        /// holds no pointers into it, its wheel is empty, and its cache says so.
        /// REQUIRES: m_pageDataMutex held, and pw.wheelIndex still the OLD index.
        ///
        /// A move breaks an invariant the whole update path rests on — that the
        /// cache describes the wheel at pw.wheelIndex. It stops being true the
        /// moment the index changes, because pushes made before the move landed
        /// somewhere else. Resetting is what makes it true again, and it is
        /// cheaper to reason about than any partial resync: from empty, the
        /// ordinary diff loop repopulates correctly with no special cases.
        void ResetPageForMoveLocked(WheelerAPI::IWheelerAPI* api, PageWheel& pw, int32_t newIndex);
    };
}

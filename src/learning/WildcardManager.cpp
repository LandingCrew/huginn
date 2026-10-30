#include "WildcardManager.h"
#include "core/CombatLock.h"     // StepWildcardsForCombat
#include "core/WheelUidGate.h"   // KeptOffWheelPage, FateOfCachedWildcard
#include "telemetry/SoakMetrics.h"   // R10: the heartbeat's wildcard rolls
#include <algorithm>

namespace Huginn::Scoring
{
    WildcardManager::WildcardManager()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto& cache : m_pages) {
            cache.rolledAt = now;
            cache.endedAt = now;
        }
    }

    Slot::WildcardRequest WildcardManager::Plan(const WildcardPage& page, const ScoredCandidateList& ranked)
    {
        Slot::WildcardRequest req;
        req.params = m_params;
        if (!m_enabled || page.index >= MAX_WILDCARD_PAGES) {
            return req;
        }
        auto& cache = m_pages[page.index];
        const auto now = std::chrono::steady_clock::now();

        // A reshape (an INI reload resizing the page or moving its wildcard
        // key) drops the page's wildcard: it was rolled for a key that may not
        // mean the same thing now. No refractory is owed -- it was never shown
        // at the new shape -- so a roll may follow on this same run.
        if (cache.slotCount != page.slotCount || cache.seat != page.seat) {
            if (cache.Active()) {
                logger::debug("[Wildcard] Page {} reshaped ({} keys, wildcard key {}; was {} / {}) - dropped {:08X}"sv,
                    page.index, page.slotCount, page.seat, cache.slotCount, cache.seat, cache.formID);
                cache.Clear();
            }
            cache.slotCount = page.slotCount;
            cache.seat = page.seat;
        }

        // In combat no wildcard is placed: none rolled, none pinned, and every
        // page's cached one dropped, so the fight-start re-score fills the key
        // with a ranked item (the locker releases that key at onset). A
        // wildcard is exploration, and under the combat freeze it held a key
        // for the whole fight: a mage's Iron Dagger, 2026-10-12 08:56-08:58.
        // When combat ends the refractory restarts on every page, so the first
        // run after the fight does not roll one onto keys that just settled.
        // Stepped on the displayed page's Plan only: the off-screen Wheeler
        // pages are allocated on the same tick, after it.
        if (page.display) {
            const auto step = Core::StepWildcardsForCombat(m_wasInCombat, page.inCombat);
            m_wasInCombat = page.inCombat;
            if (step.dropCached) {
                for (size_t p = 0; p < m_pages.size(); ++p) {
                    if (m_pages[p].Active()) {
                        logger::debug("[Wildcard] Page {}: combat - dropped {:08X}, none placed until it ends"sv,
                            p, m_pages[p].formID);
                        m_pages[p].Clear();
                        m_pages[p].endedAt = now;
                    }
                }
            }
            if (step.restartRefractory) {
                for (auto& pageCache : m_pages) {
                    pageCache.endedAt = now;
                }
                logger::debug("[Wildcard] Combat over - refractory ({:.1f}s) restarted on every page"sv,
                    m_refractorySeconds);
            }
            if (!step.mayPlace) {
                return req;
            }
        } else if (page.inCombat) {
            return req;
        }

        if (page.seat == SIZE_MAX) {
            return req;   // no wildcard key on this page: nothing to place or roll
        }

        ExpireIfDue(cache, now);

        if (cache.Active()) {
            // A wildcard whose rows all are uid-less gear on a page that hides
            // them (rolled in the second after a load, before the registry
            // knew) is dropped rather than kept: kept, it would block a roll
            // for its whole cooldown while the key shows something else.
            bool anyRow = false;
            bool showable = false;
            for (const auto& s : ranked) {
                if (s.GetFormID() != cache.formID || s.isRememberedOnly) continue;
                anyRow = true;
                showable = showable || !Core::KeptOffWheelPage(page.hideUidlessGear,
                    Candidate::IsInstanceItem(s.candidate), s.GetUniqueID());
            }
            if (Core::FateOfCachedWildcard(anyRow, showable) == Core::CachedWildcardFate::Drop) {
                logger::debug("[Wildcard] Page {} wildcard {:08X} dropped: the page's wheel cannot show it "
                    "(no uniqueID)"sv, page.index, cache.formID);
                cache.Clear();
            } else {
                req.pinForm = cache.formID;
                req.pinUniqueID = cache.uniqueID;
                return req;   // Absent: kept, and nothing is pinned this run
            }
        }

        // Nothing cached: a roll, on the displayed page, once the refractory
        // since this page's last wildcard has passed.
        const float sinceEnd = std::chrono::duration<float>(now - cache.endedAt).count();
        if (page.display && sinceEnd >= m_refractorySeconds) {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            req.roll = true;
            req.uRoll = unit(GetRNG());
            req.uDraw = unit(GetRNG());
        }
        return req;
    }

    void WildcardManager::Commit(const WildcardPage& page, const Slot::WildcardResult& result)
    {
        if (page.index >= MAX_WILDCARD_PAGES) {
            return;
        }
        auto& cache = m_pages[page.index];
        const auto& w = result.core;

        if (cache.Active() && result.pinFound && w.pin == Core::SlotAlloc::PinFate::Refused) {
            // The key no longer takes it: equipped under skip-equipped (the
            // player has it in hand, the exploration is done) or of a class the
            // key does not take. Dropped with a refractory, as an expiry.
            logger::debug("[Wildcard] Page {} wildcard {:08X} refused by key {} - dropped"sv,
                page.index, cache.formID, w.seat == Core::SlotAlloc::npos ? 0 : w.seat + 1);
            cache.Clear();
            cache.endedAt = std::chrono::steady_clock::now();
            return;
        }

        if (!result.drawn) {
            return;
        }
        const auto& d = *result.drawn;
        cache.formID = d.GetFormID();
        cache.uniqueID = d.GetUniqueID();
        cache.propensity = static_cast<float>(w.Propensity());
        cache.rolledAt = std::chrono::steady_clock::now();

        // "Relevant": a need the player has now values the item's effects (a
        // positive theta n cap term, the row's explanation). Doc 9's open item
        // asks the wildcard target to be read against these, not against
        // wildcards placed when nothing called for them.
        const bool relevant = d.choice.top[0].need >= 0;
        Telemetry::SoakMetrics::GetSingleton().RecordWildcardRolled(relevant);

        // A transition (at most one per cooldown + refractory per page): info.
        logger::info("[Wildcard] Page {} key {}: '{}' (p={:.3f}: roll {:.2f} x draw {:.3f} of {}; "
            "rel={:.3f} sd={:.3f} w={:.3f}, top w={:.3f}{}), {:.0f}s"sv,
            page.index, w.seat + 1, d.GetName(), w.Propensity(), w.pRoll, w.pDraw, w.pool,
            w.rel, w.sd, w.weight, w.topWeight, relevant ? ", relevant" : "", m_cooldownSeconds);
    }

    bool WildcardManager::UpdateExpiry()
    {
        const auto now = std::chrono::steady_clock::now();
        bool anyLapsed = false;
        for (auto& cache : m_pages) {
            const bool wasActive = cache.Active();
            ExpireIfDue(cache, now);
            anyLapsed = anyLapsed || (wasActive && !cache.Active());
        }
        return anyLapsed;
    }

    bool WildcardManager::HasActiveWildcard(size_t pageIndex) const
    {
        return pageIndex < MAX_WILDCARD_PAGES && m_pages[pageIndex].Active();
    }

    RE::FormID WildcardManager::GetWildcard(size_t pageIndex) const
    {
        return pageIndex < MAX_WILDCARD_PAGES ? m_pages[pageIndex].formID : 0;
    }

    uint16_t WildcardManager::GetWildcardUniqueID(size_t pageIndex) const
    {
        return pageIndex < MAX_WILDCARD_PAGES ? m_pages[pageIndex].uniqueID : 0;
    }

    float WildcardManager::GetWildcardPropensity(size_t pageIndex, RE::FormID formID) const
    {
        if (pageIndex >= MAX_WILDCARD_PAGES || formID == 0 || m_pages[pageIndex].formID != formID) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        return m_pages[pageIndex].propensity;
    }

    void WildcardManager::Reset()
    {
        const auto now = std::chrono::steady_clock::now();
        m_wasInCombat = false;
        for (auto& cache : m_pages) {
            cache.Clear();
            // Forget the recorded shape too: a reset is a fresh start.
            cache.slotCount = SIZE_MAX;
            cache.seat = SIZE_MAX;
            cache.rolledAt = now;
            cache.endedAt = now;
        }
    }

    void WildcardManager::ExpireIfDue(PageWildcard& cache, std::chrono::steady_clock::time_point now)
    {
        if (!cache.Active()) {
            return;
        }
        const float elapsed = std::chrono::duration<float>(now - cache.rolledAt).count();
        if (elapsed >= m_cooldownSeconds) {
            // Per page: ageing out a page the player is not looking at cannot
            // impose a refractory on the one they are.
            cache.Clear();
            cache.endedAt = now;
#ifndef NDEBUG
            logger::debug("[Wildcard] Wildcard expired - refractory {:.1f}s"sv, m_refractorySeconds);
#endif
        }
    }

    std::mt19937& WildcardManager::GetRNG()
    {
        static thread_local std::mt19937 rng(std::random_device{}());
        return rng;
    }

}  // namespace Huginn::Scoring

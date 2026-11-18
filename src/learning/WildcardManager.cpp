#include "WildcardManager.h"
#include "core/WheelUidGate.h"   // KeptOffWheelPage, FateOfCachedWildcard
#include "telemetry/SoakMetrics.h"   // R10: the heartbeat's wildcard rolls
#include <chrono>
#include <format>
#include <limits>

namespace Huginn::Scoring
{
    // The decisions are Core::Wildcard::Gate's (core/WildcardGate.h, host-tested
    // in tests/core/WildcardTests.cpp); this adds the clock, the uniforms, the
    // uid check on the ranked list, the logs and the heartbeat.

    WildcardManager::WildcardManager()
    {
        // The clocks start at GateSettings' defaults (the shipped Huginn.ini's
        // values) until LoadWildcardConfigFromINI sets them.
        m_gate.Reset(NowNs());
    }

    std::int64_t WildcardManager::NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    Slot::WildcardRequest WildcardManager::Plan(const WildcardPage& page, const ScoredCandidateList& ranked)
    {
        Slot::WildcardRequest req;
        req.params = m_params;

        const Core::Wildcard::GatePage gatePage{
            .index = page.index,
            .slotCount = page.slotCount,
            .seat = page.seat == SIZE_MAX ? Core::Wildcard::kNoSeat : page.seat,
            .inCombat = page.inCombat,
            .display = page.display,
        };
        // A wildcard whose rows all are uid-less gear on a page that hides them
        // (rolled in the second after a load, before the registry knew) cannot
        // be shown there.
        const auto fate = [&](uint32_t formID) {
            bool anyRow = false;
            bool showable = false;
            for (const auto& s : ranked) {
                if (s.GetFormID() != formID || s.isRememberedOnly) continue;
                anyRow = true;
                showable = showable || !Core::KeptOffWheelPage(page.hideUidlessGear,
                    Candidate::IsInstanceItem(s.candidate), s.GetUniqueID());
            }
            return Core::FateOfCachedWildcard(anyRow, showable);
        };
        const auto plan = m_gate.Plan(gatePage, NowNs(), fate);

        if (plan.reshapeDropped != 0) {
            logger::debug("[Wildcard] Page {} reshaped ({} keys, wildcard key {}) - dropped {:08X}"sv,
                page.index, page.slotCount, page.seat, plan.reshapeDropped);
        }
        // In combat no wildcard is placed: a wildcard is exploration, and under
        // the combat freeze it held a key for the whole fight (a mage's Iron
        // Dagger, 2026-10-12 08:56-08:58).
        for (const auto& [p, form] : plan.combatDropped) {
            logger::debug("[Wildcard] Page {}: combat - dropped {:08X}, none placed until it ends"sv, p, form);
        }
        if (plan.combatOver) {
            logger::debug("[Wildcard] Combat over - refractory ({:.1f}s) restarted on every page"sv,
                m_gate.settings.refractorySec);
        }
#ifndef NDEBUG
        if (plan.expired != 0) {
            logger::debug("[Wildcard] Page {} wildcard {:08X} expired - refractory {:.1f}s"sv,
                page.index, plan.expired, m_gate.settings.refractorySec);
        }
#endif
        if (plan.uidDropped != 0) {
            logger::debug("[Wildcard] Page {} wildcard {:08X} dropped: the page's wheel cannot show it "
                "(no uniqueID)"sv, page.index, plan.uidDropped);
        }

        req.pinForm = plan.pinForm;
        req.pinUniqueID = plan.pinUniqueID;
        if (plan.roll) {
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            req.roll = true;
            req.uRoll = unit(GetRNG());
            req.uDraw = unit(GetRNG());
        }
        return req;
    }

    void WildcardManager::Commit(const WildcardPage& page, const Slot::WildcardResult& result, bool drawnShown)
    {
        const auto& w = result.core;
        const RE::FormID before = GetWildcard(page.index);
        Core::Wildcard::CommitIn in{
            .pinFound = result.pinFound,
            .pin = w.pin,
            .rollAsked = w.rollAsked,
            .propensity = w.Propensity(),
            .drawnShown = drawnShown,
        };
        if (result.drawn) {
            in.drawnForm = result.drawn->GetFormID();
            in.drawnUniqueID = result.drawn->GetUniqueID();
        }
        const auto fate = m_gate.Commit(page.index, in, NowNs());

        std::uint64_t* notShown = page.index < m_notShownLogged.size() ? &m_notShownLogged[page.index] : nullptr;
        if (notShown && fate != Core::Wildcard::CommitFate::NotShown) *notShown = 0;

        switch (fate) {
        case Core::Wildcard::CommitFate::Refused:
            logger::debug("[Wildcard] Page {} wildcard {:08X} refused by key {} - dropped"sv,
                page.index, before, w.seat == Core::SlotAlloc::npos ? 0 : w.seat + 1);
            return;
        case Core::Wildcard::CommitFate::RollSpent:
#ifndef NDEBUG
            logger::debug("[Wildcard] Page {} roll spent (p={:.3f}, pool {}) - next in {:.0f}s"sv,
                page.index, w.pRoll, w.pool, m_gate.settings.refractorySec);
#endif
            return;
        case Core::Wildcard::CommitFate::NotShown:
        {
            // Rare: the draw already leaves out what a lock holds (by form and
            // by name), so only a lock taken or kept in ApplyLocks itself gets
            // here. The roll is not spent, so while that lock stands every run
            // in the window can draw the same row again: logged once per page
            // and draw (a transition), not per run.
            const std::uint64_t drew = (static_cast<std::uint64_t>(in.drawnForm) << 16) | in.drawnUniqueID;
            if (notShown) {
                if (*notShown == drew) return;
                *notShown = drew;
            }
            logger::debug("[Wildcard] Page {} key {}: drew '{}' but the slot locks did not show it - not kept, "
                "the roll waits"sv, page.index, w.seat + 1, result.drawn ? result.drawn->GetName() : std::string_view{});
            return;
        }
        case Core::Wildcard::CommitFate::Cached:
            break;
        default:
            return;
        }

        const auto& d = *result.drawn;
        // "Relevant": a need the player has now values the item's effects (a
        // positive theta n cap term, the row's explanation). Doc 9's open item
        // asks the wildcard target to be read against these, not against
        // wildcards placed when nothing called for them.
        const bool relevant = d.choice.top[0].need >= 0;
        Telemetry::SoakMetrics::GetSingleton().RecordWildcardRolled(relevant);

        // A transition (at most one per cooldown + refractory per page): info.
        // The key's own item, when it outranked the page's weakest row, moved
        // to that row's key (core RehomeFor): name the key it went to.
        const std::string moved = w.movedTo == Core::SlotAlloc::npos
            ? std::string()
            : std::format("; its item moved to key {}", w.movedTo + 1);
        logger::info("[Wildcard] Page {} key {}: '{}' (p={:.3f}: roll {:.2f} x draw {:.3f} of {}; "
            "rel={:.3f} sd={:.3f} w={:.3f}, top w={:.3f}{}{}), {:.0f}s"sv,
            page.index, w.seat + 1, d.GetName(), w.Propensity(), w.pRoll, w.pDraw, w.pool,
            w.rel, w.sd, w.weight, w.topWeight, relevant ? ", relevant" : "", moved, m_gate.settings.cooldownSec);
    }

    bool WildcardManager::UpdateExpiry()
    {
        const bool lapsed = m_gate.UpdateExpiry(NowNs());
#ifndef NDEBUG
        if (lapsed) {
            logger::debug("[Wildcard] Wildcard expired - refractory {:.1f}s"sv, m_gate.settings.refractorySec);
        }
#endif
        return lapsed;
    }

    RE::FormID WildcardManager::GetWildcard(size_t pageIndex) const
    {
        const auto* c = m_gate.Page(pageIndex);
        return c ? c->formID : 0;
    }

    uint16_t WildcardManager::GetWildcardUniqueID(size_t pageIndex) const
    {
        const auto* c = m_gate.Page(pageIndex);
        return c ? c->uniqueID : 0;
    }

    float WildcardManager::GetWildcardPropensity(size_t pageIndex, RE::FormID formID) const
    {
        const auto* c = m_gate.Page(pageIndex);
        if (!c || formID == 0 || c->formID != formID) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        return static_cast<float>(c->propensity);
    }

    void WildcardManager::Reset()
    {
        m_gate.Reset(NowNs());
        m_notShownLogged.fill(0);
    }

    std::mt19937& WildcardManager::GetRNG()
    {
        static thread_local std::mt19937 rng(std::random_device{}());
        return rng;
    }

}  // namespace Huginn::Scoring

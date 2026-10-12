#include "../PCH.h"
#include "WheelUidFilter.h"

#include <array>
#include <mutex>

#include "Globals.h"
#include "core/WheelUidGate.h"
#include "slot/SlotSettings.h"     // MAX_PAGES
#include "weapon/WeaponRegistry.h"
#include "wheeler/WheelerClient.h"

namespace Huginn::Display
{
    namespace
    {
        std::mutex s_logMutex;
        std::array<Core::KeptOffLog, Slot::MAX_PAGES> s_keptOff{};

        /// The name of any row of `formID` in `rows`, for the log.
        std::string_view NameIn(const Scoring::ScoredCandidateList& rows, RE::FormID formID) noexcept
        {
            for (const auto& sc : rows) {
                if (sc.GetFormID() == formID) {
                    return sc.GetName();
                }
            }
            return {};
        }

        void NoteKeptOff(size_t page, std::vector<std::uint32_t> forms, const Scoring::ScoredCandidateList& all)
        {
            if (page >= Slot::MAX_PAGES) {
                return;
            }
            std::lock_guard lock(s_logMutex);
            const auto change = s_keptOff[page].Update(std::move(forms));
            for (const auto id : change.added) {
                logger::debug("[WheelerBackend] kept off page {}: '{}' {:08X} has no uniqueID "
                              "(wheel cannot show it)"sv, page, NameIn(all, id), id);
            }
            for (const auto id : change.removed) {
                // Equipped once (the registry has its uniqueID now), gone from
                // the inventory, or the page no longer feeds a wheel.
                const auto name = NameIn(all, id);
                logger::debug("[WheelerBackend] page {}: {:08X} '{}' no longer kept off ({})"sv,
                    page, id, name, name.empty() ? "not a candidate now" : "it can be shown");
            }
        }
    }

    bool HidesUidlessGear(size_t page)
    {
        auto& client = Wheeler::WheelerClient::GetSingleton();
        Core::WheelUidGate gate;
        gate.wheelerConnected = client.IsConnected();
        gate.pageHasWheel = gate.wheelerConnected && client.GetWheelIndexForPage(page) >= 0;
        gate.instanceDetailKnown = g_weaponRegistry && g_weaponRegistry->HasInstanceDetail();
        return Core::HidesUidlessGear(gate);
    }

    bool IsUidlessGear(const Candidate::CandidateVariant& candidate) noexcept
    {
        return Core::KeptOffWheelPage(true, Candidate::IsInstanceItem(candidate), Candidate::GetUniqueID(candidate));
    }

    const Scoring::ScoredCandidateList& CandidatesForWheelPage(size_t page, bool hides,
        const Scoring::ScoredCandidateList& all, Scoring::ScoredCandidateList& scratch)
    {
        std::vector<std::uint32_t> keptOff;
        if (hides) {
            for (const auto& sc : all) {
                if (IsUidlessGear(sc.candidate)) {
                    keptOff.push_back(sc.GetFormID());
                }
            }
        }
        const bool any = !keptOff.empty();
        NoteKeptOff(page, std::move(keptOff), all);
        if (!any) {
            return all;
        }
        scratch.clear();
        scratch.reserve(all.size());
        for (const auto& sc : all) {
            if (!IsUidlessGear(sc.candidate)) {
                scratch.push_back(sc);
            }
        }
        return scratch;
    }
}

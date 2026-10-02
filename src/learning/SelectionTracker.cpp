#include "SelectionTracker.h"
#include "EquipEventBus.h"
#include "SelectionLog.h"
#include "Config.h"
#include "Globals.h"
#include "telemetry/SoakMetrics.h"

namespace Huginn::Learning
{
    namespace
    {
        std::string FormName(RE::FormID formID)
        {
            const auto* form = RE::TESForm::LookupByID(formID);
            const char* name = form ? form->GetName() : nullptr;
            return (name && *name) ? name : "?";
        }

        float MsSince(std::chrono::steady_clock::time_point then,
                      std::chrono::steady_clock::time_point now)
        {
            return std::chrono::duration<float, std::milli>(now - then).count();
        }
    }

    SelectionKind SelectionTracker::KindOf(RE::FormID formID)
    {
        // Used up rather than worn. A scroll is NOT here: selecting one puts it
        // in a hand, and the equip is the selection -- casting it later is not
        // a second one. (A cast inside the window still confirms it; see
        // OnConsumed.)
        if (const auto* form = RE::TESForm::LookupByID(formID)) {
            switch (form->GetFormType()) {
            case RE::FormType::AlchemyItem:
            case RE::FormType::SoulGem:
            case RE::FormType::Ingredient:
                return SelectionKind::Consumable;
            default:
                break;
            }
        }
        return SelectionKind::Equip;
    }

    bool SelectionTracker::IsStillEquipped(RE::FormID formID)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || formID == 0) return false;

        // Hands: weapons, spells, scrolls, the torch.
        for (const bool left : { false, true }) {
            if (const auto* obj = player->GetEquippedObject(left); obj && obj->GetFormID() == formID) {
                return true;
            }
        }

        // Selecting "Unarmed" empties the hand rather than putting a form in it,
        // so the hand reads as nothing at all (LoreRim 2026-10-02: FE62CA7F never
        // confirmed). An empty right hand is that selection still standing.
        if (const auto* weap = RE::TESForm::LookupByID<RE::TESObjectWEAP>(formID);
            weap && weap->IsHandToHandMelee() && !player->GetEquippedObject(false)) {
            return true;
        }
        if (const auto* ammo = player->GetCurrentAmmo(); ammo && ammo->GetFormID() == formID) {
            return true;
        }
        if (const auto* form = RE::TESForm::LookupByID(formID); form && form->Is(RE::FormType::Armor)) {
            return player->GetWornArmor(formID) != nullptr;
        }
        return false;
    }

    void SelectionTracker::Select(RE::FormID formID, EquipSource source, std::string via,
                                  std::string attribution)
    {
        if (formID == 0) return;

        // One record per item: a second event for an item already pending is
        // the same selection (both hands, a doubled TESEquipEvent, the equip
        // event that follows a Huginn key press...).
        //
        // One upgrade: an outside selection followed by a Huginn one for the
        // same item is a Huginn pick whose equip event arrived first (Wheeler
        // may equip before it calls back). Huginn's label wins, and the outside
        // attribution is dropped so it never reaches accept%.
        {
            std::lock_guard lock(m_mutex);
            for (auto& p : m_pending) {
                if (p.event.formID != formID) continue;
                if (p.event.source == EquipSource::External && source != EquipSource::External) {
                    logger::info("[Selection] {:08X} pending as External ({}) is a {} pick -- relabelled"sv,
                        formID, p.event.via, EquipSourceToString(source));
                    p.event.source = source;
                    p.event.via = std::move(via);
                    p.event.attribution.clear();
                } else {
                    logger::debug("[Selection] {:08X} already pending ({} via {}) -- same selection"sv,
                        formID, EquipSourceToString(p.event.source), p.event.via);
                }
                return;
            }
        }

        // Capture OUTSIDE m_mutex: it takes StateManager and cache locks.
        const auto now = std::chrono::steady_clock::now();
        EquipEvent event = EquipEventBus::Capture(formID, source);
        event.kind = KindOf(formID);
        event.via = std::move(via);
        event.attribution = std::move(attribution);
        event.shown = PipelineStateCache::GetSingleton().TakeSnapshot();
        event.loadGeneration = g_loadGeneration.load(std::memory_order_relaxed);

        const float windowMs = event.kind == SelectionKind::Consumable
            ? Config::CONSUMPTION_HUGINN_WINDOW_MS
            : Config::SELECTION_CONFIRM_MS;
        const auto deadline = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float, std::milli>(windowMs));

        logger::info("[Selection] Pending {:08X} '{}' src={} via={} kind={}{}{}"sv,
            formID, FormName(formID), EquipSourceToString(source), event.via,
            SelectionKindToString(event.kind),
            event.attribution.empty() ? ""sv : " case="sv, event.attribution);

        std::lock_guard lock(m_mutex);
        for (const auto& p : m_pending) {
            if (p.event.formID == formID) return;  // lost a race to an identical select
        }
        m_pending.push_back(Pending{ std::move(event), now, deadline });
    }

    bool SelectionTracker::OnConsumed(RE::FormID formID)
    {
        std::optional<Pending> hit;
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
                if (it->event.formID == formID) {
                    hit = std::move(*it);
                    m_pending.erase(it);
                    break;
                }
            }
        }
        if (!hit) return false;

        Confirm(hit->event, hit->selectedAt, "consumed");
        return true;
    }

    void SelectionTracker::Update()
    {
        const auto now = std::chrono::steady_clock::now();

        std::vector<Pending> due;
        {
            std::lock_guard lock(m_mutex);
            if (m_pending.empty()) return;
            for (auto it = m_pending.begin(); it != m_pending.end();) {
                if (now >= it->deadline) {
                    due.push_back(std::move(*it));
                    it = m_pending.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (auto& p : due) {
            if (p.event.kind == SelectionKind::Equip && IsStillEquipped(p.event.formID)) {
                Confirm(p.event, p.selectedAt, "still equipped");
                continue;
            }
            // A transition the player caused, and rare -- info, not debug.
            logger::info("[Selection] Not confirmed {:08X} '{}' src={} after {:.0f}ms -- {}"sv,
                p.event.formID, FormName(p.event.formID), EquipSourceToString(p.event.source),
                MsSince(p.selectedAt, now),
                p.event.kind == SelectionKind::Consumable ? "count never dropped"sv
                                                          : "no longer equipped"sv);
        }
    }

    void SelectionTracker::Withdraw(RE::FormID formID, std::string_view why)
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (it->event.formID == formID) {
                logger::info("[Selection] Withdrawn {:08X} '{}' src={} -- {}"sv,
                    formID, FormName(formID), EquipSourceToString(it->event.source), why);
                m_pending.erase(it);
                return;
            }
        }
    }

    void SelectionTracker::Clear()
    {
        std::lock_guard lock(m_mutex);
        if (!m_pending.empty()) {
            logger::info("[Selection] Cleared {} pending selection(s) unconfirmed (reset)"sv, m_pending.size());
        }
        m_pending.clear();
    }

    void SelectionTracker::Confirm(EquipEvent& event, std::chrono::steady_clock::time_point selectedAt,
                                   const char* how)
    {
        event.confirmMs = MsSince(selectedAt, std::chrono::steady_clock::now());

        // Soak telemetry: an outside selection's A-E case, counted once it is
        // real. Counted here, not when the equip event arrived, so a Huginn pick
        // relabelled above, or a pick that never confirmed, stays out of accept%.
        if (event.source == EquipSource::External && !event.attribution.empty()) {
            Telemetry::SoakMetrics::GetSingleton().RecordEquipCase(event.attribution[0]);
        }

        // Log BEFORE dispatch: the log records the learner's prediction for the
        // press-time state, which the dispatch is about to update.
        SelectionLog::Write(event, how);
        EquipEventBus::GetSingleton().Dispatch(event);
    }
}

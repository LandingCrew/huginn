#include "Remembrance.h"
#include "SlotAllocator.h"
#include "SlotLocker.h"
#include <chrono>
#include <spdlog/spdlog.h>

namespace Huginn::Slot
{
    namespace
    {
        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::string_view NameOf(RE::FormID formID)
        {
            if (auto* form = RE::TESForm::LookupByID(formID)) {
                return form->GetName();
            }
            return "?";
        }

        // The hold changes what a slot on the widget page shows, and that
        // slot's lock is still keeping the item the press just equipped (or,
        // at the end, the remembered one). Let go of it so the change shows
        // this run rather than when the lock runs out.
        void ReleaseIfOnScreen(size_t page, size_t slot)
        {
            if (SlotAllocator::GetSingleton().GetCurrentPage() == page) {
                SlotLocker::GetSingleton().UnlockSlot(slot);
            }
        }
    }

    Remembrance& Remembrance::GetSingleton()
    {
        static Remembrance instance;
        return instance;
    }

    bool Remembrance::OnSlotActivated(size_t page, size_t slot, RE::FormID formID, Kind kind)
    {
        if (page >= MAX_PAGES || slot >= MAX_SLOTS_PER_PAGE || formID == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Pressing the remembered item is the undo. It ends the hold and
        // remembers nothing: the item it takes off already had its turn in
        // this slot, and holding it would ping-pong the two forever.
        auto& entry = m_pages[page][slot];
        if (entry.Active() && entry.formID == formID) {
            spdlog::info("[Remembrance] Page {} Slot {}: '{}' re-equipped, hold ended",
                page, slot, NameOf(formID));
            entry = {};
            return true;
        }

        const auto& settings = SlotSettings::GetSingleton();
        if (settings.RemembranceDurationMs() <= 0.0f) {
            return false;
        }
        const auto config = settings.GetSlotConfigs(page);
        if (slot >= config.size() || !config[slot].remembrance) {
            return false;
        }

        // Oldest first out when full; four presses inside the match window
        // is already more than a hand can do.
        if (m_pendingCount == kMaxPending) {
            std::move(m_pending.begin() + 1, m_pending.end(), m_pending.begin());
            --m_pendingCount;
        }
        m_pending[m_pendingCount++] = { page, slot, formID, kind, NowMs() };
        return false;
    }

    void Remembrance::Observe(Track& track, RE::FormID now, int64_t nowMs)
    {
        if (now != track.current) {
            track.before = track.current;
            track.current = now;
            track.changedAtMs = nowMs;
        }
    }

    bool Remembrance::ChangedTo(const Track& track, RE::FormID formID, int64_t sinceMs)
    {
        return track.current == formID && track.changedAtMs >= sinceMs &&
               track.before != 0 && track.before != formID;
    }

    bool Remembrance::Update(float deltaMs, RE::PlayerCharacter* player)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int64_t nowMs = NowMs();
        bool changed = false;

        if (player) {
            auto formOf = [](const RE::TESForm* form) { return form ? form->GetFormID() : RE::FormID{ 0 }; };
            const RE::FormID right = formOf(player->GetEquippedObject(false));
            const RE::FormID left = formOf(player->GetEquippedObject(true));
            const RE::FormID ammo = formOf(player->GetCurrentAmmo());
            if (!m_sampled) {
                // Seed without a change: what is in hand at load was not
                // displaced by anything.
                m_right = { right, 0, INT64_MIN };
                m_left = { left, 0, INT64_MIN };
                m_ammo = { ammo, 0, INT64_MIN };
                m_sampled = true;
            } else {
                Observe(m_right, right, nowMs);
                Observe(m_left, left, nowMs);
                Observe(m_ammo, ammo, nowMs);
            }
        }

        // Resolve pending captures against the hands.
        const float durationMs = SlotSettings::GetSingleton().RemembranceDurationMs();
        size_t kept = 0;
        for (size_t i = 0; i < m_pendingCount; ++i) {
            const auto& p = m_pending[i];
            const int64_t since = p.createdAtMs - kMatchWindowMs;

            RE::FormID displaced = 0;
            if (p.kind == Kind::Ammo) {
                if (ChangedTo(m_ammo, p.formID, since)) displaced = m_ammo.before;
            } else if (ChangedTo(m_right, p.formID, since)) {
                displaced = m_right.before;
            } else if (ChangedTo(m_left, p.formID, since)) {
                displaced = m_left.before;
            }

            if (displaced != 0) {
                if (durationMs > 0.0f) {
                    m_pages[p.page][p.slot] = { displaced, durationMs, kSettleMs };
                    spdlog::info("[Remembrance] Page {} Slot {}: holding '{}' ({:08X}) for {:.0f}s, taken off by '{}'",
                        p.page, p.slot, NameOf(displaced), displaced, durationMs / 1000.0f, NameOf(p.formID));
                    ReleaseIfOnScreen(p.page, p.slot);
                    changed = true;
                }
                continue;  // resolved
            }
            if (nowMs - p.createdAtMs <= kMatchWindowMs) {
                m_pending[kept++] = p;  // still waiting for the equip
            }
            // else: the equip never showed up as a change (already in hand,
            // refused, or a potion-like press) -- nothing to remember.
        }
        m_pendingCount = kept;

        // Age the holds.
        for (size_t page = 0; page < MAX_PAGES; ++page) {
            for (size_t slot = 0; slot < MAX_SLOTS_PER_PAGE; ++slot) {
                auto& entry = m_pages[page][slot];
                if (entry.formID == 0) continue;
                entry.remainingMs -= deltaMs;
                const bool backInHand = player && (entry.formID == m_right.current ||
                    entry.formID == m_left.current || entry.formID == m_ammo.current);
                if (entry.remainingMs <= 0.0f || backInHand) {
                    spdlog::info("[Remembrance] Page {} Slot {}: hold on '{}' {}",
                        page, slot, NameOf(entry.formID), backInHand ? "ended, back in hand" : "expired");
                    entry = {};
                    ReleaseIfOnScreen(page, slot);
                    changed = true;
                    continue;
                }
                if (entry.settleMs > 0.0f) {
                    entry.settleMs -= deltaMs;
                    changed = true;
                }
            }
        }
        return changed;
    }

    Remembrance::PageEntries Remembrance::GetPage(size_t page) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return page < MAX_PAGES ? m_pages[page] : PageEntries{};
    }

    std::vector<RE::FormID> Remembrance::ActiveFormIDs() const
    {
        std::vector<RE::FormID> ids;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& page : m_pages) {
            for (const auto& entry : page) {
                if (entry.Active()) ids.push_back(entry.formID);
            }
        }
        return ids;
    }

    void Remembrance::Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pages = {};
        m_pendingCount = 0;
        m_sampled = false;
    }

}  // namespace Huginn::Slot

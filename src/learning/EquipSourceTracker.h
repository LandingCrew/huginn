#pragma once

#include <array>
#include <chrono>
#include <mutex>

namespace Huginn::Learning
{
    // =========================================================================
    // EQUIP SOURCE TRACKER
    // =========================================================================
    // Tracks whether a recent equip was triggered by Huginn (EquipManager or
    // WheelerClient) vs an external source (inventory menu, favorites, console).
    //
    // Huginn equip sites call MarkHuginnEquip(formID) just before triggering the
    // equip. SpellRegistry::ProcessEvent and ExternalEquipListener check
    // IsRecentHuginnEquip(formID) to distinguish Huginn-mediated equips from
    // external ones.
    //
    // FormID-keyed: a Huginn equip of item X must not suppress learning from a
    // genuine external equip of item Y in the same window. Entries are kept
    // until window expiry (NOT consumed on first match) because the game can
    // fire multiple TESEquipEvents for a single equip action (e.g. both hands);
    // all events for the marked FormID within the window are Huginn-mediated.
    //
    // Storage is a small fixed ring (no heap, no unbounded growth); marks are
    // rare (one per player action) so WINDOW_CAPACITY=8 is generous.
    // =========================================================================

    class EquipSourceTracker
    {
    public:
        // Suppression window. Wider than the old 100ms global window — safe now
        // that matching is per-FormID, so over-suppression of unrelated external
        // equips can no longer occur. Covers event-delivery latency spikes
        // (frame drops, loading) that caused double-learning at 100ms.
        static constexpr float DEFAULT_WINDOW_MS = 400.0f;

        static EquipSourceTracker& GetSingleton()
        {
            static EquipSourceTracker instance;
            return instance;
        }

        // Called by EquipManager and WheelerClient when they trigger equips
        void MarkHuginnEquip(RE::FormID formID)
        {
            const auto now = std::chrono::steady_clock::now();
            const bool hand = IsHandForm(formID);
            std::lock_guard lock(m_mutex);
            m_entries[m_nextSlot] = Entry{formID, now, hand};
            m_nextSlot = (m_nextSlot + 1) % m_entries.size();
        }

        // Called on TESEquipEvent to check if the equip of this specific form
        // was Huginn-mediated (marked within the window).
        [[nodiscard]] bool IsRecentHuginnEquip(RE::FormID formID, float windowMs = DEFAULT_WINDOW_MS) const
        {
            if (formID == 0) {
                return false;  // Never match the empty ring slots (Entry{} has formID 0)
            }
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(m_mutex);
            for (const auto& entry : m_entries) {
                if (entry.formID != formID) {
                    continue;
                }
                const float elapsedMs =
                    std::chrono::duration<float, std::milli>(now - entry.timestamp).count();
                if (elapsedMs <= windowMs) {
                    return true;
                }
            }
            return false;
        }

        // Any Huginn HAND equip (a spell, scroll, weapon, light or shield)
        // marked within the window, whatever its form: an UNequip event
        // names what was taken off, not what Huginn put on, so the
        // displacement a Huginn hand equip causes is matched this way
        // (EquipCallerTrace, 0.24.26). A potion press displaces nothing.
        [[nodiscard]] bool HadRecentHuginnHandEquip(float windowMs) const
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(m_mutex);
            for (const auto& entry : m_entries) {
                if (entry.formID == 0 || !entry.hand) {
                    continue;
                }
                if (std::chrono::duration<float, std::milli>(now - entry.timestamp).count() <= windowMs) {
                    return true;
                }
            }
            return false;
        }

    private:
        EquipSourceTracker() = default;
        ~EquipSourceTracker() = default;
        EquipSourceTracker(const EquipSourceTracker&) = delete;
        EquipSourceTracker& operator=(const EquipSourceTracker&) = delete;

        struct Entry
        {
            RE::FormID formID = 0;
            std::chrono::steady_clock::time_point timestamp{};
            bool hand = false;   // goes into a hand (HadRecentHuginnHandEquip)
        };

        static bool IsHandForm(RE::FormID formID)
        {
            const auto* form = formID ? RE::TESForm::LookupByID(formID) : nullptr;
            if (!form) return false;
            switch (form->GetFormType()) {
            case RE::FormType::Spell:
            case RE::FormType::Scroll:
            case RE::FormType::Weapon:
            case RE::FormType::Light:
                return true;
            case RE::FormType::Armor: {
                const auto* armor = form->As<RE::TESObjectARMO>();
                return armor && armor->IsShield();
            }
            default:
                return false;
            }
        }

        mutable std::mutex m_mutex;
        std::array<Entry, 8> m_entries{};
        size_t m_nextSlot = 0;
    };

}  // namespace Huginn::Learning

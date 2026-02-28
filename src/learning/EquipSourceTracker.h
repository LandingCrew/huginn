#pragma once

#include <atomic>
#include <chrono>

namespace Huginn::Learning
{
    // =========================================================================
    // EQUIP SOURCE TRACKER
    // =========================================================================
    // Tracks whether a recent equip was triggered by Huginn (EquipManager or
    // WheelerClient) vs an external source (inventory menu, favorites, console).
    //
    // Huginn equip sites call MarkHuginnEquip() just before triggering the equip.
    // SpellRegistry::ProcessEvent checks IsRecentHuginnEquip() to distinguish
    // Huginn-mediated equips from external ones.
    //
    // Uses a simple timestamp comparison — if the equip event arrives within
    // windowMs of the last MarkHuginnEquip() call, it's considered Huginn-mediated.
    // =========================================================================

    class EquipSourceTracker
    {
    public:
        static EquipSourceTracker& GetSingleton()
        {
            static EquipSourceTracker instance;
            return instance;
        }

        // Called by EquipManager and WheelerClient when they trigger equips
        void MarkHuginnEquip()
        {
            m_lastHuginnEquipTime.store(
                std::chrono::steady_clock::now(),
                std::memory_order_release);
        }

        // Called by SpellRegistry::ProcessEvent to check if recent equip was Huginn-mediated
        [[nodiscard]] bool IsRecentHuginnEquip(float windowMs = 100.0f) const
        {
            auto lastTime = m_lastHuginnEquipTime.load(std::memory_order_acquire);
            auto elapsed = std::chrono::steady_clock::now() - lastTime;
            auto elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
            return elapsedMs <= windowMs;
        }

    private:
        EquipSourceTracker() = default;
        ~EquipSourceTracker() = default;
        EquipSourceTracker(const EquipSourceTracker&) = delete;
        EquipSourceTracker& operator=(const EquipSourceTracker&) = delete;

        std::atomic<std::chrono::steady_clock::time_point> m_lastHuginnEquipTime{};
    };

}  // namespace Huginn::Learning

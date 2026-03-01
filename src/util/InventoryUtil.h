#pragma once

namespace Huginn::Util
{
    // =============================================================================
    // SAFE INVENTORY ACCESS (v0.13.x)
    // =============================================================================
    // Drop-in replacement for TESObjectREFR::GetInventory() that handles duplicate
    // entries in the inventory changes list. Some mod setups (e.g. LoreRim) can
    // produce duplicate TESBoundObject* entries, which causes an assertion failure
    // in CommonLibSSE-NG's GetInventory() (TESObjectREFR.cpp:339, `it.second`).
    //
    // This reimplements the same logic but uses try_emplace + count accumulation
    // instead of assert-guarded emplace.
    // =============================================================================

    using InventoryItemMap = RE::TESObjectREFR::InventoryItemMap;

    template <typename Filter>
    inline InventoryItemMap GetInventorySafe(RE::TESObjectREFR* ref, Filter&& filter)
    {
        InventoryItemMap results;
        if (!ref) return results;

        // Phase 1: inventory changes (deltas from base container)
        auto* invChanges = ref->GetInventoryChanges();
        if (invChanges && invChanges->entryList) {
            for (auto& entry : *invChanges->entryList) {
                if (entry && entry->object && filter(*entry->object)) {
                    auto [it, inserted] = results.try_emplace(
                        entry->object,
                        std::make_pair(
                            entry->countDelta,
                            std::make_unique<RE::InventoryEntryData>(*entry)));
                    if (!inserted) {
                        // Duplicate — accumulate count, keep first entry
                        it->second.first += entry->countDelta;
                        logger::debug("GetInventorySafe: duplicate inventory entry for {} ({:08X}), delta {}",
                            entry->object->GetName(), entry->object->GetFormID(), entry->countDelta);
                    }
                }
            }
        }

        // Phase 2: base container (static items defined in the ESP)
        // Mirrors CommonLibSSE-NG logic: skip leveled items already in inventory changes
        auto* container = ref->GetContainer();
        if (container) {
            const auto ignore = [&](RE::TESBoundObject* a_object) {
                const auto it = results.find(a_object);
                const auto entryData =
                    it != results.end() ? it->second.second.get() : nullptr;
                return entryData ? entryData->IsLeveled() : false;
            };

            container->ForEachContainerObject([&](RE::ContainerObject& a_entry) {
                if (a_entry.obj && !ignore(a_entry.obj) && filter(*a_entry.obj)) {
                    auto it = results.find(a_entry.obj);
                    if (it != results.end()) {
                        it->second.first += a_entry.count;
                    } else {
                        results.emplace(
                            a_entry.obj,
                            std::make_pair(
                                a_entry.count,
                                std::make_unique<RE::InventoryEntryData>(a_entry.obj, 0)));
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        return results;
    }
}

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
    //
    // RETIRE WHEN: CommonLibSSE-NG's TESObjectREFR::GetInventory() no longer
    // asserts on duplicate TESBoundObject* entries (the upstream assert at
    // TESObjectREFR.cpp:339 `it.second`). As of CommonLibSSE-NG v3.7.0 the assert
    // is still present; once a release tolerates duplicates, callers can switch
    // back to the stock GetInventory() and this helper can be deleted.
    // =============================================================================

    using InventoryItemMap = RE::TESObjectREFR::InventoryItemMap;

    // `filter` must be a side-effect-free predicate: it may be invoked on entries
    // that are ultimately skipped (e.g. leveled base-container duplicates), so its
    // result must depend only on the object, not on call count or order.
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
            container->ForEachContainerObject([&](RE::ContainerObject& a_entry) {
                if (!a_entry.obj || !filter(*a_entry.obj)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                // Single lookup reused for both the leveled-item skip (mirrors
                // CommonLibSSE-NG, which ignores leveled entries already present
                // from Phase 1) and the count merge.
                auto it = results.find(a_entry.obj);
                if (it != results.end()) {
                    if (it->second.second && it->second.second->IsLeveled()) {
                        return RE::BSContainer::ForEachResult::kContinue;  // leveled — skip
                    }
                    it->second.first += a_entry.count;
                } else {
                    results.emplace(
                        a_entry.obj,
                        std::make_pair(
                            a_entry.count,
                            std::make_unique<RE::InventoryEntryData>(a_entry.obj, 0)));
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        return results;
    }
}

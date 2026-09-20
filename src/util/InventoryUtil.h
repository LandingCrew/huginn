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

    // One object's trail through a scan, kept only when it turned up in the
    // changes list more than once. See the summary log at the end of
    // GetInventorySafe for why these particular numbers.
    struct DuplicateTrail
    {
        RE::TESBoundObject* obj = nullptr;
        int32_t firstDelta = 0;    // countDelta of the entry that got there first
        int32_t extraDeltas = 0;   // summed countDelta of the 2nd..nth entries
        int32_t entries = 1;       // how many entries the list held for it
        int32_t extraLists = 0;    // summed extraList count across those entries
        int32_t baseCount = 0;     // what the base container contributed
        bool leveledSkip = false;  // base container skipped: entry is leveled
    };

    // `filter` must be a side-effect-free predicate: it may be invoked on entries
    // that are ultimately skipped (e.g. leveled base-container duplicates), so its
    // result must depend only on the object, not on call count or order.
    template <typename Filter>
    inline InventoryItemMap GetInventorySafe(RE::TESObjectREFR* ref, Filter&& filter)
    {
        InventoryItemMap results;
        if (!ref) return results;

        // Empty on every normal scan: an object appears once in the changes
        // list, and nothing is pushed here. Reserve nothing -- paying an
        // allocation on the 2 Hz path for a case that does not happen would be
        // the wrong trade.
        std::vector<DuplicateTrail> duplicates;

        // Phase 1: inventory changes (deltas from base container)
        auto* invChanges = ref->GetInventoryChanges();
        if (invChanges && invChanges->entryList) {
            for (auto& entry : *invChanges->entryList) {
                if (entry && entry->object && filter(*entry->object)) {
                    // BSSimpleList has no size(); walk it. Only reached for
                    // entries the filter kept, and only to describe duplicates.
                    int32_t lists = 0;
                    if (entry->extraLists) {
                        for (auto* extraList : *entry->extraLists) {
                            if (extraList) ++lists;
                        }
                    }

                    auto [it, inserted] = results.try_emplace(
                        entry->object,
                        std::make_pair(
                            entry->countDelta,
                            std::make_unique<RE::InventoryEntryData>(*entry)));
                    if (!inserted) {
                        // Duplicate — accumulate count, keep first entry
                        const int32_t before = it->second.first;
                        it->second.first += entry->countDelta;

                        auto trail = std::find_if(duplicates.begin(), duplicates.end(),
                            [&](const DuplicateTrail& t) { return t.obj == entry->object; });
                        if (trail == duplicates.end()) {
                            duplicates.push_back(DuplicateTrail{
                                entry->object, before, entry->countDelta, 2, lists, 0, false });
                        } else {
                            trail->extraDeltas += entry->countDelta;
                            trail->entries += 1;
                            trail->extraLists += lists;
                        }
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
                    auto trail = std::find_if(duplicates.begin(), duplicates.end(),
                        [&](const DuplicateTrail& t) { return t.obj == a_entry.obj; });

                    if (it->second.second && it->second.second->IsLeveled()) {
                        if (trail != duplicates.end()) {
                            trail->leveledSkip = true;
                        }
                        return RE::BSContainer::ForEachResult::kContinue;  // leveled — skip
                    }
                    it->second.first += a_entry.count;
                    if (trail != duplicates.end()) {
                        trail->baseCount = a_entry.count;
                    }
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

        // DIAGNOSTIC (2026-09-20): a LoreRim Iron Sword (00012EB7) is being
        // recommended and equipped while the player says it is not in their
        // inventory, and it is the ONLY object in a 13-minute log that appears
        // twice in the changes list -- 19 times, every scan.
        //
        // The line this replaces printed one duplicate's countDelta, which
        // cannot tell a real second stack from a phantom. These numbers can: the
        // first entry's delta and the rest separately (a +1 beside a -1 is a
        // cancellation the sum hides), how many entries and extraLists the list
        // held (a worn or tempered stack splits), what the base container added,
        // and the total every caller then acts on -- the registry, the candidate
        // filter and EquipWeapon's "do you still hold this" guard all read it.
        //
        // Deduped on the numbers, not fired per scan: this runs at 2 Hz and a
        // stable duplicate would otherwise be 2 lines a second. Delete this
        // block once the Iron Sword is understood.
        if (!duplicates.empty()) {
            thread_local std::unordered_map<RE::FormID, uint64_t> s_lastSignature;
            for (const auto& t : duplicates) {
                if (!t.obj) continue;
                const auto found = results.find(t.obj);
                const int32_t total = (found != results.end()) ? found->second.first : 0;

                const uint64_t signature =
                    (static_cast<uint64_t>(static_cast<uint32_t>(t.firstDelta)) << 32) ^
                    (static_cast<uint64_t>(static_cast<uint32_t>(t.extraDeltas)) << 16) ^
                    (static_cast<uint64_t>(static_cast<uint32_t>(total)) << 8) ^
                    (static_cast<uint64_t>(t.entries) << 4) ^
                    static_cast<uint64_t>(t.baseCount) ^
                    (t.leveledSkip ? 0x8000000000000000ull : 0ull);

                const RE::FormID formID = t.obj->GetFormID();
                auto [slot, fresh] = s_lastSignature.try_emplace(formID, signature);
                if (!fresh) {
                    if (slot->second == signature) continue;  // unchanged, stay quiet
                    slot->second = signature;
                }

                logger::warn("[Inventory] {} ({:08X}) appears {}x in the changes list: "
                    "first={} others={} extraLists={} base={}{} -> callers see {}",
                    t.obj->GetName(), formID, t.entries, t.firstDelta, t.extraDeltas,
                    t.extraLists, t.baseCount, t.leveledSkip ? " (leveled, base skipped)" : "",
                    total);
            }
        }

        return results;
    }
}

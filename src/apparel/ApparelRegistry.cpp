#include "ApparelRegistry.h"

#include <format>
#include <string>

#include "util/AtomicGuard.h"
#include "util/ExtraListStability.h"
#include "util/InventoryUtil.h"

namespace Huginn::Apparel
{
   void ApparelRegistry::RebuildRegistry()
   {
      logger::info("Rebuilding apparel registry..."sv);

      std::unique_lock lock(m_mutex);
      m_apparel.clear();
      m_index.clear();

      // Deliberately no scan here. Player enchantments are only readable once
      // extraLists have stabilized, and this runs on the load path. The first
      // ReconcileApparel() past the window fills the registry; see the class
      // comment in ApparelRegistry.h.
   }

   size_t ApparelRegistry::ReconcileApparel()
   {
      // Not "degrade and retry later" — a scan now would classify every
      // player-enchanted piece as CraftSkill::None. Wait for real data.
      if (!Util::IsExtraListStable()) {
         logger::trace("[ApparelRegistry] ReconcileApparel() skipped - extraLists not stable"sv);
         return 0;
      }

      m_isLoading = true;
      Util::AtomicBoolGuard guard{ m_isLoading, false };

      // Scan and classify entirely outside the write lock — classification walks
      // enchantment effect lists and has no business holding up readers.
      const auto scanned = ScanPlayerApparel();

      std::vector<InventoryApparel> classified;
      classified.reserve(scanned.size());
      size_t enchantedSeen = 0;
      for (const auto& sa : scanned) {
         ApparelData data = m_classifier.ClassifyApparel(sa.armor, sa.enchantment);
         if (!data.IsCraftRelevant()) {
            // Report ENCHANTED rejects with the actor values we actually saw.
            // Unenchanted gear is the overwhelming majority and says nothing, but
            // a rejected enchanted piece is the one case where the AV vocabulary
            // in ApparelClassifier may simply not cover what this modlist uses —
            // and without the numbers there is no way to tell that from "the
            // scan never ran". Debug-level, at most once per 30s reconcile.
            if (sa.enchantment) {
               ++enchantedSeen;
               std::string avs;
               for (const auto* effect : sa.enchantment->effects) {
                  if (!effect || !effect->baseEffect) continue;
                  if (!avs.empty()) avs += ", ";
                  avs += std::format("AV={} mag={:.1f}{}",
                     static_cast<int>(effect->baseEffect->data.primaryAV),
                     effect->effectItem.magnitude,
                     effect->baseEffect->IsHostile() ? " (hostile)" : "");
               }
               logger::debug("[ApparelRegistry] Rejected enchanted '{}': [{}]"sv,
                  sa.armor ? sa.armor->GetName() : "?", avs);
            }
            continue;  // the scope guard (#65)
         }

         data.uniqueID = sa.uniqueID;

         // Instance name wins over the base form's. Applied here rather than in
         // the classifier because the classifier is handed a base form and an
         // enchantment; the ExtraTextDisplayData carrying a player's own name for
         // the piece lives on the stack, which only the scan sees.
         if (!sa.displayName.empty()) {
            data.name = sa.displayName;
         }

         InventoryApparel entry{};
         entry.data = std::move(data);
         entry.isEquipped = sa.isEquipped;
         classified.push_back(std::move(entry));
      }

      std::unique_lock lock(m_mutex);

      const size_t before = m_apparel.size();

      // Wholesale replace rather than diff. The set is small by construction
      // (craft-relevant gear only, typically single digits), so an incremental
      // add/remove walk would cost more in code than it saves in work — and a
      // replace cannot drift out of sync with the inventory the way a diff can.
      //
      // Collapse key collisions on the way in. Two distinct player-enchanted
      // pieces of one base form normally differ by ExtraUniqueID, but Skyrim does
      // not guarantee one, and Key() has no other field to separate them with -
      // Wheeler needs the real ExtraUniqueID, so a synthetic id is not an option.
      // Keeping both would leave m_index disagreeing with m_apparel and hand the
      // scorer two candidates that GetDeduplicationKey() then collapses anyway.
      // Prefer the piece that can actually be offered: unworn over worn, then the
      // stronger of the two.
      m_index.clear();
      m_index.reserve(classified.size());
      m_apparel.clear();
      m_apparel.reserve(classified.size());

      size_t collisions = 0;
      for (auto& candidate : classified) {
         const uint64_t key = candidate.Key();
         auto [it, inserted] = m_index.try_emplace(key, m_apparel.size());
         if (inserted) {
            m_apparel.push_back(std::move(candidate));
            continue;
         }

         ++collisions;
         auto& kept = m_apparel[it->second];
         const bool betterOffer = (kept.isEquipped && !candidate.isEquipped) ||
            (kept.isEquipped == candidate.isEquipped &&
             candidate.data.magnitude > kept.data.magnitude);
         if (betterOffer) {
            kept = std::move(candidate);
         }
      }

      if (collisions > 0) {
         logger::debug("[ApparelRegistry] {} entries shared a registry key (no ExtraUniqueID "
                       "to separate them); kept the best offer for each"sv, collisions);
      }

      const size_t after = m_apparel.size();
      const size_t churn = (after > before) ? (after - before) : (before - after);

      // Log UNCONDITIONALLY, not just on churn. The first version only spoke when
      // the count changed, which made "scanned and found nothing" look exactly
      // like "never ran" — and that cost a debugging session, because the only
      // apparel line in a whole log was "Rebuilding apparel registry".
      // An empty result is a finding, so it gets a line.
      size_t equipped = 0;
      for (const auto& a : m_apparel) {
         if (a.isEquipped) ++equipped;
      }
      logger::info("[ApparelRegistry] Reconciled: {} armor scanned, {} enchanted rejected, "
                   "{} craft-relevant ({} of them currently worn, so not offered)"sv,
         scanned.size(), enchantedSeen, after, equipped);

      return churn;
   }

   const InventoryApparel* ApparelRegistry::GetApparel(RE::FormID formID, uint16_t uniqueID) const
   {
      // Build the key through InventoryApparel::Key() rather than re-inlining the
      // bit layout — two copies of a packing expression drift.
      InventoryApparel probe{};
      probe.data.formID = formID;
      probe.data.uniqueID = uniqueID;
      const uint64_t key = probe.Key();

      std::shared_lock lock(m_mutex);
      auto it = m_index.find(key);
      if (it == m_index.end() || it->second >= m_apparel.size()) return nullptr;
      return &m_apparel[it->second];
   }

   size_t ApparelRegistry::GetApparelCount() const noexcept
   {
      std::shared_lock lock(m_mutex);
      return m_apparel.size();
   }

   void ApparelRegistry::LogAllApparel() const
   {
      std::shared_lock lock(m_mutex);
      logger::debug("[ApparelRegistry] {} craft-relevant items:"sv, m_apparel.size());
      for (const auto& apparel : m_apparel) {
         logger::debug("  {}"sv, apparel.ToString());
      }
   }

   std::vector<ApparelRegistry::ScannedApparel> ApparelRegistry::ScanPlayerApparel() const
   {
      std::vector<ScannedApparel> result;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
         logger::debug("[ApparelRegistry] Player not available for apparel scan"sv);
         return result;
      }

      auto inventory = Util::GetInventorySafe(player, [](RE::TESBoundObject& obj) {
         return obj.Is(RE::FormType::Armor);
      });

      result.reserve(16);

      for (auto& [obj, data] : inventory) {
         auto& [count, entry] = data;
         if (count <= 0) continue;

         auto* armor = obj->As<RE::TESObjectARMO>();
         if (!armor) continue;

         // Base-form enchantment (vanilla pre-enchanted gear). Each stack below
         // may override it with a player-applied ExtraEnchantment of its own.
         RE::EnchantmentItem* baseEnchantment = nullptr;
         if (auto* enchantable = armor->As<RE::TESEnchantableForm>(); enchantable) {
            baseEnchantment = enchantable->formEnchanting;
         }

         // ONE ScannedApparel PER EXTRALIST. Folding the extraLists into a single
         // per-base-form record is what the struct comment describes: the fields
         // ended up last-wins, so two enchanted Gold Rings became one entry and
         // its enchantment and uniqueID could come from different rings.
         int32_t plainCopies = count;

         if (entry && entry->extraLists) {
            for (auto* extraList : *entry->extraLists) {
               if (!extraList) continue;

               // Copies this extraList accounts for; the rest of `count` is plain
               // stock of the base form with no per-instance data at all.
               int32_t stackCount = 1;
               if (const auto* extraCount = extraList->GetByType<RE::ExtraCount>(); extraCount) {
                  stackCount = extraCount->count;
               }
               plainCopies -= stackCount;

               ScannedApparel sa{};
               sa.armor = armor;
               sa.enchantment = baseEnchantment;

               if (auto* extraEnch = extraList->GetByType<RE::ExtraEnchantment>(); extraEnch) {
                  sa.enchantment = extraEnch->enchantment;
               }
               if (auto* extraUnique = extraList->GetByType<RE::ExtraUniqueID>(); extraUnique) {
                  sa.uniqueID = extraUnique->uniqueID;
               }

               // Worn is a property of THIS stack. InventoryEntryData::IsWorn()
               // is true if ANY extraList in the entry carries ExtraWorn, so
               // reading it per base form meant wearing a plain Gold Ring also
               // suppressed the enchanted one sharing that form.
               sa.isEquipped = extraList->HasType<RE::ExtraWorn>() ||
                               extraList->HasType<RE::ExtraWornLeft>();

               // A player-applied name renames the INSTANCE. Without this the
               // common case for this feature - gear the player enchanted and
               // named themselves - renders as "Gold Ring" in the widget, and two
               // of them are indistinguishable.
               if (const auto* textData = extraList->GetByType<RE::ExtraTextDisplayData>();
                   textData) {
                  if (const char* custom = textData->displayName.c_str(); custom && *custom) {
                     sa.displayName = custom;
                  }
               }

               result.push_back(std::move(sa));
            }
         }

         if (plainCopies > 0) {
            // The copies with no extraList of their own. Craft-relevant only if
            // the BASE form is enchanted; otherwise classification rejects it.
            ScannedApparel sa{};
            sa.armor = armor;
            sa.enchantment = baseEnchantment;
            result.push_back(std::move(sa));
         }
      }

      return result;
   }

   bool ApparelRegistry::MarkEquipped(RE::FormID formID, uint16_t uniqueID, bool equipped)
   {
      InventoryApparel probe{};
      probe.data.formID = formID;
      probe.data.uniqueID = uniqueID;
      const uint64_t key = probe.Key();

      std::unique_lock lock(m_mutex);
      auto it = m_index.find(key);
      if (it == m_index.end() || it->second >= m_apparel.size()) return false;

      const size_t index = it->second;
      auto& entry = m_apparel[index];

      bool changed = false;
      if (entry.isEquipped != equipped) {
         entry.isEquipped = equipped;
         changed = true;
         logger::debug("[ApparelRegistry] '{}' ({:08X}/{}) now {}"sv,
            entry.data.name, formID, uniqueID, equipped ? "worn" : "not worn");
      }

      // Putting a piece ON takes the previous occupant of that body slot OFF,
      // and the engine does that without telling us. Marking only the new piece
      // left the displaced one flagged worn, so the piece the player just took
      // off stayed out of the pool until the next reconcile - the same 30 s
      // staleness this method exists to remove, pointing the other way.
      //
      // Slot equality is a heuristic, not ground truth: ApparelSlot::Ring is a
      // single value for what a mod may split into several real ring slots, so
      // a sweep can clear a piece that is still genuinely worn. Asking the game
      // instead is not an option here - ActorEquipManager has not applied the
      // equip yet, so ExtraWorn still reads the OLD arrangement at this point.
      // The heuristic errs by offering a worn piece for up to one reconcile;
      // the bug it replaces hid a usable piece for the same span. Unknown and
      // Other are excluded because they lump unrelated slots together.
      if (equipped) {
         const auto slot = entry.data.slot;
         if (slot != ApparelSlot::Unknown && slot != ApparelSlot::Other) {
            for (size_t i = 0; i < m_apparel.size(); ++i) {
               if (i == index) continue;

               auto& other = m_apparel[i];
               if (!other.isEquipped || other.data.slot != slot) continue;

               other.isEquipped = false;
               changed = true;
               logger::debug("[ApparelRegistry] '{}' ({:08X}/{}) displaced from {}"sv,
                  other.data.name, other.data.formID, other.data.uniqueID,
                  ApparelSlotToString(slot));
            }
         }
      }

      return changed;
   }
}

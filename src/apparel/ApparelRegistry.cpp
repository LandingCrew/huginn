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
      m_apparel = std::move(classified);
      m_index.clear();
      m_index.reserve(m_apparel.size());
      for (size_t i = 0; i < m_apparel.size(); ++i) {
         m_index[m_apparel[i].Key()] = i;
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

         ScannedApparel sa{};
         sa.armor = armor;

         // Base-form enchantment first (vanilla pre-enchanted gear), then let a
         // player-applied one override it — ExtraEnchantment is what a player's
         // own Fortify Alchemy gloves carry, and it wins on the stack it is on.
         if (auto* enchantable = armor->As<RE::TESEnchantableForm>(); enchantable) {
            sa.enchantment = enchantable->formEnchanting;
         }

         if (entry && entry->extraLists) {
            for (auto* extraList : *entry->extraLists) {
               if (!extraList) continue;

               if (auto* extraEnch = extraList->GetByType<RE::ExtraEnchantment>(); extraEnch) {
                  sa.enchantment = extraEnch->enchantment;
               }
               if (auto* extraUnique = extraList->GetByType<RE::ExtraUniqueID>(); extraUnique) {
                  sa.uniqueID = extraUnique->uniqueID;
               }
            }
            sa.isEquipped = entry->IsWorn();
         }

         result.push_back(sa);
      }

      return result;
   }
}

#pragma once

#include <algorithm>   // std::max (CraftMagnitudes::Set)
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace Huginn::Apparel
{
   // =============================================================================
   // CRAFT SKILL (#65)
   // =============================================================================
   // The three crafting skills a workstation context can ask for. Apparel is
   // deliberately NOT a general candidate source: only gear that fortifies one of
   // these enters the pool, so the candidate count grows by the size of the
   // player's fortify set rather than the size of their wardrobe (the scope guard
   // in #65). Resist/carry-weight enchantments are out of scope by design.
   // =============================================================================
   enum class CraftSkill : uint8_t
   {
      None = 0,     // Not craft-relevant — rejected at classification
      Alchemy,      // At an alchemy lab
      Smithing,     // At a forge / workbench / grindstone
      Enchanting    // At an arcane enchanter
   };

   /// Number of CraftSkill values, including None — the width of CraftMagnitudes.
   inline constexpr size_t CRAFT_SKILL_COUNT = 4;

   [[nodiscard]] constexpr const char* CraftSkillToString(CraftSkill skill) noexcept
   {
      switch (skill) {
      case CraftSkill::Alchemy:    return "Alchemy";
      case CraftSkill::Smithing:   return "Smithing";
      case CraftSkill::Enchanting: return "Enchanting";
      case CraftSkill::None:       return "None";
      default:                     return "Unknown";
      }
   }

   // =============================================================================
   // APPAREL SLOT
   // =============================================================================
   // Which body slot the piece occupies, from the biped object template. Carried
   // for display and logging only — there is no slot-conflict resolution yet, so
   // two Fortify Alchemy rings can both be recommended. isEquipped already keeps
   // the one you are wearing out of the pool.
   // =============================================================================
   enum class ApparelSlot : uint8_t
   {
      Unknown = 0,
      Head,
      Body,
      Hands,
      Feet,
      Ring,
      Amulet,
      Circlet,
      Other
   };

   /// Number of ApparelSlot values, including Unknown - the width of any
   /// per-slot array. Keep in step with the enum above.
   inline constexpr size_t APPAREL_SLOT_COUNT = 9;

   [[nodiscard]] constexpr const char* ApparelSlotToString(ApparelSlot slot) noexcept
   {
      switch (slot) {
      case ApparelSlot::Head:    return "Head";
      case ApparelSlot::Body:    return "Body";
      case ApparelSlot::Hands:   return "Hands";
      case ApparelSlot::Feet:    return "Feet";
      case ApparelSlot::Ring:    return "Ring";
      case ApparelSlot::Amulet:  return "Amulet";
      case ApparelSlot::Circlet: return "Circlet";
      case ApparelSlot::Other:   return "Other";
      default:                   return "Unknown";
      }
   }

   // =============================================================================
   // CRAFT MAGNITUDES - What this piece fortifies, per skill
   // =============================================================================
   // One float per CraftSkill rather than a single (skill, magnitude) pair. A
   // player-made piece can carry two fortify effects for DIFFERENT crafts, and
   // collapsing that to the largest magnitude made the item invisible at the
   // other one of its own benches: "Fortify Alchemy 5 / Fortify Smithing 20"
   // classified as Smithing, so at an alchemy lab WeightForCandidate read the
   // alchemy weight for CraftSkill::Smithing, got 0.0, and never offered a ring
   // that was genuinely useful there.
   // =============================================================================
   struct CraftMagnitudes
   {
      // Indexed by CraftSkill. Index 0 (None) is never written and never read.
      std::array<float, CRAFT_SKILL_COUNT> values{};

      [[nodiscard]] float For(CraftSkill skill) const noexcept {
         return values[static_cast<size_t>(skill)];
      }

      void Set(CraftSkill skill, float magnitude) noexcept {
         if (skill == CraftSkill::None) return;
         auto& slot = values[static_cast<size_t>(skill)];
         // Keep the strongest per skill: a piece can carry two effects on the
         // same craft, and the larger is the honest representative.
         slot = std::max(slot, magnitude);
      }

      [[nodiscard]] bool Fortifies(CraftSkill skill) const noexcept {
         return For(skill) > 0.0f;
      }

      /// The craft with the largest magnitude, and that magnitude. Used for
      /// display, logging and prior ranking — NOT for the context-weight lookup,
      /// which must consider every skill the piece fortifies.
      [[nodiscard]] CraftSkill Primary() const noexcept {
         CraftSkill best = CraftSkill::None;
         float bestMagnitude = 0.0f;
         for (size_t i = 1; i < CRAFT_SKILL_COUNT; ++i) {
            if (values[i] > bestMagnitude) {
               bestMagnitude = values[i];
               best = static_cast<CraftSkill>(i);
            }
         }
         return best;
      }

      [[nodiscard]] bool Any() const noexcept { return Primary() != CraftSkill::None; }
   };

   // =============================================================================
   // APPAREL DATA - Classification result for one wearable
   // =============================================================================
   struct ApparelData
   {
      RE::FormID     formID = 0;
      std::string    name;                           // Display name (per-instance
                                                     // if the player renamed it)
      CraftSkill     craftSkill = CraftSkill::None;  // Strongest craft — display/ranking
      float          magnitude = 0.0f;               // ...and its magnitude
      CraftMagnitudes magnitudes;                    // Every craft it fortifies
      ApparelSlot    slot = ApparelSlot::Unknown;
      uint16_t       uniqueID = 0;                   // ExtraUniqueID — distinguishes
                                                     // two enchanted copies of one base form

      [[nodiscard]] bool IsCraftRelevant() const noexcept {
         return craftSkill != CraftSkill::None;
      }

      [[nodiscard]] std::string ToString() const
      {
         // Name the second craft when there is one — a dual-fortify ring reading
         // as plain "Smithing+20" in the log is how the classification bug hid.
         std::string extra;
         for (size_t i = 1; i < CRAFT_SKILL_COUNT; ++i) {
            const auto skill = static_cast<CraftSkill>(i);
            if (skill == craftSkill || !magnitudes.Fortifies(skill)) continue;
            extra += std::format(" +{}{:.0f}",
               CraftSkillToString(skill), magnitudes.For(skill));
         }

         return std::format("ApparelData[{}, {}+{:.0f}{}, slot={}]",
            name, CraftSkillToString(craftSkill), magnitude, extra,
            ApparelSlotToString(slot));
      }
   };

   // =============================================================================
   // INVENTORY APPAREL - Registry entry
   // =============================================================================
   // NOTE: no isFavorited here, unlike InventoryWeapon. Nothing reads a favorites
   // flag for apparel — the candidate pool is already narrowed by classification,
   // and Candidate::IsFavorited() reports false for ApparelCandidate. Add it when
   // there is a consumer, not before.
   struct InventoryApparel
   {
      ApparelData data;
      bool isEquipped = false;   // Currently worn — filtered out of the candidate pool

      /// Registry key. Two rings the player enchanted themselves commonly share
      /// one base form (both "Gold Ring"), so formID alone would collapse them
      /// into a single entry and lose whichever was scanned second. ExtraUniqueID
      /// separates the instances, and matches the layout of
      /// CandidateBase::GetDeduplicationKey().
      [[nodiscard]] uint64_t Key() const noexcept {
         return (static_cast<uint64_t>(data.uniqueID) << 32) |
                 static_cast<uint64_t>(data.formID);
      }

      [[nodiscard]] std::string ToString() const
      {
         return std::format("InventoryApparel[{}, eq={}]", data.ToString(), isEquipped);
      }
   };
}

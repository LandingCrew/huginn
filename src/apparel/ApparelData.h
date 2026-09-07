#pragma once

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
   // APPAREL DATA - Classification result for one wearable
   // =============================================================================
   struct ApparelData
   {
      RE::FormID  formID = 0;
      std::string name;                              // Display name
      CraftSkill  craftSkill = CraftSkill::None;     // Which craft it fortifies
      float       magnitude = 0.0f;                  // Fortify magnitude (ranking key)
      ApparelSlot slot = ApparelSlot::Unknown;
      uint16_t    uniqueID = 0;                      // ExtraUniqueID — distinguishes
                                                     // two enchanted copies of one base form

      [[nodiscard]] bool IsCraftRelevant() const noexcept {
         return craftSkill != CraftSkill::None;
      }

      [[nodiscard]] std::string ToString() const
      {
         return std::format("ApparelData[{}, {}+{:.0f}, slot={}]",
            name, CraftSkillToString(craftSkill), magnitude, ApparelSlotToString(slot));
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

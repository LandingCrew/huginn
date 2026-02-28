#include "EquipManager.h"
#include "learning/EquipSourceTracker.h"

// Windows GetObject macro interferes with RE::BGSDefaultObjectManager::GetObject
#ifdef GetObject
#undef GetObject
#endif

namespace Huginn::Input
{
   EquipManager& EquipManager::GetSingleton()
   {
      static EquipManager singleton;
      return singleton;
   }

   RE::BGSEquipSlot* EquipManager::GetEquipSlot(EquipHand /*hand*/, bool isLeftHand)
   {
      // Try BGSDefaultObjectManager first
      auto* defaultObjects = RE::BGSDefaultObjectManager::GetSingleton();
      if (defaultObjects) {
      auto slotType = isLeftHand
        ? RE::DEFAULT_OBJECT::kLeftHandEquip
        : RE::DEFAULT_OBJECT::kRightHandEquip;

      auto* slot = defaultObjects->GetObject<RE::BGSEquipSlot>(slotType);
      if (slot) {
        return slot;
      }
      }

      // Fallback: Look up by hardcoded FormID
      // Left hand: 0x00013F43, Right hand: 0x00013F42
      RE::FormID slotFormID = isLeftHand ? 0x00013F43 : 0x00013F42;
      auto* form = RE::TESForm::LookupByID(slotFormID);
      if (form) {
      auto* slot = form->As<RE::BGSEquipSlot>();
      if (slot) {
        logger::debug("[EquipManager] Using fallback FormID {:08X} for {} hand slot"sv,
           slotFormID, isLeftHand ? "left" : "right");
        return slot;
      }
      }

      logger::warn("[EquipManager] Failed to get {} hand equip slot (FormID {:08X})"sv,
      isLeftHand ? "left" : "right", slotFormID);
      return nullptr;
   }

   bool EquipManager::EquipSpellToHand(RE::SpellItem* spell, bool leftHand)
   {
      if (!spell) {
      logger::warn("[EquipManager] Cannot equip null spell"sv);
      return false;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::warn("[EquipManager] Player not found"sv);
      return false;
      }

      auto* equipManager = RE::ActorEquipManager::GetSingleton();
      if (!equipManager) {
      logger::warn("[EquipManager] ActorEquipManager not found"sv);
      return false;
      }

      // Get the appropriate equip slot
      auto* equipSlot = GetEquipSlot(EquipHand::Right, leftHand);
      if (!equipSlot) {
      logger::warn("[EquipManager] Could not get equip slot for {} hand"sv,
        leftHand ? "left" : "right");
      return false;
      }

      // Equip the spell
      equipManager->EquipSpell(player, spell, equipSlot);

      // Log the equip attempt - verification is unreliable due to async nature
      // The EquipSpell call itself is the source of truth for now
      logger::info("[EquipManager] Equipped '{}' to {} hand (FormID: {:08X})"sv,
      spell->GetName(), leftHand ? "left" : "right", spell->GetFormID());

      // Debug: Log what the game thinks is equipped (may be stale)
      auto* equippedSpell = leftHand
      ? player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kLeftHand]
      : player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];

      if (equippedSpell) {
      logger::debug("[EquipManager] Current {} hand shows: '{}' (FormID: {:08X})"sv,
        leftHand ? "left" : "right", equippedSpell->GetName(), equippedSpell->GetFormID());
      } else {
      logger::debug("[EquipManager] Current {} hand shows: (nothing)"sv,
        leftHand ? "left" : "right");
      }

      return true;  // Trust the EquipSpell call succeeded
   }

   bool EquipManager::UsePotion(RE::FormID formID)
   {
      if (formID == 0) {
      logger::warn("[EquipManager] Cannot use potion with FormID 0"sv);
      return false;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::warn("[EquipManager] Player not found"sv);
      return false;
      }

      auto* equipManager = RE::ActorEquipManager::GetSingleton();
      if (!equipManager) {
      logger::warn("[EquipManager] ActorEquipManager not found"sv);
      return false;
      }

      // Look up the potion by FormID
      auto* form = RE::TESForm::LookupByID(formID);
      if (!form) {
      logger::warn("[EquipManager] Potion FormID {:08X} not found"sv, formID);
      return false;
      }

      auto* alchemyItem = form->As<RE::AlchemyItem>();
      if (!alchemyItem) {
      logger::warn("[EquipManager] FormID {:08X} is not an AlchemyItem"sv, formID);
      return false;
      }

      // Use the potion via EquipObject (for consumables, this "uses" them)
      equipManager->EquipObject(player, alchemyItem);

      logger::info("[EquipManager] Used potion '{}' (FormID: {:08X})"sv,
      alchemyItem->GetName(), formID);

      return true;
   }

   bool EquipManager::EquipWeapon(RE::FormID formID, bool leftHand)
   {
      if (formID == 0) {
      logger::warn("[EquipManager] Cannot equip weapon with FormID 0"sv);
      return false;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      logger::warn("[EquipManager] Player not found"sv);
      return false;
      }

      auto* equipManager = RE::ActorEquipManager::GetSingleton();
      if (!equipManager) {
      logger::warn("[EquipManager] ActorEquipManager not found"sv);
      return false;
      }

      // Look up the weapon by FormID
      auto* form = RE::TESForm::LookupByID(formID);
      if (!form) {
      logger::warn("[EquipManager] Weapon FormID {:08X} not found"sv, formID);
      return false;
      }

      auto* weapon = form->As<RE::TESObjectWEAP>();
      if (!weapon) {
      logger::warn("[EquipManager] FormID {:08X} is not a weapon"sv, formID);
      return false;
      }

      // Get the appropriate equip slot
      auto* equipSlot = GetEquipSlot(EquipHand::Right, leftHand);

      // Equip the weapon
      equipManager->EquipObject(player, weapon, nullptr, 1, equipSlot);

      logger::info("[EquipManager] Equipped weapon '{}' to {} hand (FormID: {:08X})"sv,
      weapon->GetName(), leftHand ? "left" : "right", formID);

      return true;
   }

   bool EquipManager::UseSoulGem(RE::FormID formID)
   {
      if (formID == 0) {
         logger::warn("[EquipManager] Cannot use soul gem with FormID 0"sv);
         return false;
      }

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
         logger::warn("[EquipManager] Player not found"sv);
         return false;
      }

      // Look up the soul gem
      auto* form = RE::TESForm::LookupByID(formID);
      if (!form) {
         logger::warn("[EquipManager] Soul gem FormID {:08X} not found"sv, formID);
         return false;
      }

      auto* soulGem = form->As<RE::TESSoulGem>();
      if (!soulGem) {
         logger::warn("[EquipManager] FormID {:08X} is not a soul gem"sv, formID);
         return false;
      }

      // Determine soul level → charge value (matches vanilla Skyrim values)
      auto soulLevel = soulGem->GetContainedSoul();
      if (soulLevel == RE::SOUL_LEVEL::kNone) {
         logger::debug("[EquipManager] Soul gem '{}' is empty"sv, soulGem->GetName());
         return false;
      }

      float chargeValue = 0.0f;
      float expValue = 0.0f;
      switch (soulLevel) {
         case RE::SOUL_LEVEL::kPetty:   chargeValue = 250.0f;  expValue = 1.0f;  break;
         case RE::SOUL_LEVEL::kLesser:  chargeValue = 500.0f;  expValue = 1.5f;  break;
         case RE::SOUL_LEVEL::kCommon:  chargeValue = 1000.0f; expValue = 2.0f;  break;
         case RE::SOUL_LEVEL::kGreater: chargeValue = 1500.0f; expValue = 3.0f;  break;
         case RE::SOUL_LEVEL::kGrand:   chargeValue = 3000.0f; expValue = 5.0f;  break;
         default:                       chargeValue = 250.0f;  expValue = 1.0f;  break;
      }

      // Find the enchanted weapon to recharge (right hand first, then left)
      // Helper: check if a hand holds an enchanted weapon (including staves)
      auto* rightHand = player->GetEquippedObject(false);
      auto* leftHand = player->GetEquippedObject(true);

      auto tryHand = [](RE::TESForm* hand, RE::ActorValue av)
         -> std::optional<std::pair<RE::ActorValue, float>> {
         if (!hand) return std::nullopt;
         auto* weapon = hand->As<RE::TESObjectWEAP>();
         auto* enchantable = weapon ? weapon->As<RE::TESEnchantableForm>() : nullptr;
         if (enchantable && (enchantable->formEnchanting ||
             weapon->GetWeaponType() == RE::WEAPON_TYPE::kStaff)) {
            float max = static_cast<float>(enchantable->amountofEnchantment);
            if (max > 0.0f) return std::make_pair(av, max);
         }
         return std::nullopt;
      };

      // Try right hand first, then left — also fall through if right hand is already full
      RE::ActorValue chargeAV = RE::ActorValue::kNone;
      float maxCharge = 0.0f;
      float currentCharge = 0.0f;
      float restoreAmount = 0.0f;

      for (auto [hand, av] : {
         std::pair{rightHand, RE::ActorValue::kRightItemCharge},
         std::pair{leftHand, RE::ActorValue::kLeftItemCharge}
      }) {
         if (auto result = tryHand(hand, av)) {
            float cur = player->AsActorValueOwner()->GetActorValue(result->first);
            float restore = std::min(chargeValue, result->second - cur);
            if (restore > 0.0f) {
               chargeAV = result->first;
               maxCharge = result->second;
               currentCharge = cur;
               restoreAmount = restore;
               break;
            }
         }
      }

      if (chargeAV == RE::ActorValue::kNone || restoreAmount <= 0.0f) {
         logger::debug("[EquipManager] No enchanted weapon needs recharging"sv);
         return false;
      }

      // Apply the charge
      player->AsActorValueOwner()->ModActorValue(chargeAV, restoreAmount);

      // Remove the soul gem (1 unit)
      player->RemoveItem(soulGem, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

      // Award enchanting XP
      player->AddSkillExperience(RE::ActorValue::kEnchanting, expValue);

      logger::info("[EquipManager] Recharged weapon with '{}' (+{:.0f} charge, {:.0f}/{:.0f})"sv,
         soulGem->GetName(), restoreAmount, currentCharge + restoreAmount, maxCharge);

      return true;
   }

   void EquipManager::SetSlotContent(size_t index, const UI::SlotContent& content)
   {
      if (index < MAX_SLOTS) {
      std::unique_lock lock(m_slotMutex);
      m_slotContents[index] = content;
      }
   }

   UI::SlotContent EquipManager::GetSlotContent(size_t index) const
   {
      if (index >= MAX_SLOTS) return {};
      std::shared_lock lock(m_slotMutex);
      return m_slotContents[index];
   }

   bool EquipManager::EquipSlot(size_t slotIndex, EquipHand hand)
   {
      // Get the slot content from cache (populated by update loop)
      // Returns by value — safe snapshot for cross-thread access.
      auto content = GetSlotContent(slotIndex);

      if (content.IsEmpty()) {
      logger::debug("[EquipManager] Slot {} is empty, nothing to equip"sv, slotIndex + 1);
      return false;
      }

      bool success = false;

      switch (content.type) {
      case UI::SlotContentType::Spell:
      case UI::SlotContentType::Wildcard:
      {
        // Look up spell by FormID (preferred) or fallback to name
        RE::SpellItem* spell = nullptr;

        if (content.formID != 0) {
           // Direct FormID lookup - fast and unambiguous
           auto* form = RE::TESForm::LookupByID(content.formID);
           if (form) {
            spell = form->As<RE::SpellItem>();
           }
           if (!spell) {
            logger::warn("[EquipManager] Spell FormID {:08X} not found or not a spell"sv, content.formID);
            return false;
           }
        } else {
           // Fallback to name lookup (legacy path)
           auto* dataHandler = RE::TESDataHandler::GetSingleton();
           if (!dataHandler) {
            logger::warn("[EquipManager] TESDataHandler not available"sv);
            return false;
           }

           for (auto* form : dataHandler->GetFormArray<RE::SpellItem>()) {
            if (form && form->GetName() == content.name) {
              spell = form;
              break;
            }
           }

           if (!spell) {
            logger::warn("[EquipManager] Spell '{}' not found by name"sv, content.name);
            return false;
           }
        }

        // Mark as Huginn-mediated equip BEFORE the game API call.
        // EquipSpell fires TESEquipEvent synchronously — ExternalEquipListener checks
        // IsRecentHuginnEquip() within a 100ms window, so the mark must be set first.
        // If the equip fails, the 100ms window expires harmlessly.
        Learning::EquipSourceTracker::GetSingleton().MarkHuginnEquip();

        // Equip based on hand preference
        switch (hand) {
        case EquipHand::Right:
           success = EquipSpellToHand(spell, false);
           break;
        case EquipHand::Left:
           success = EquipSpellToHand(spell, true);
           break;
        case EquipHand::Both:
           success = EquipSpellToHand(spell, false) && EquipSpellToHand(spell, true);
           break;
        }

        // Trigger callback for learning system
        if (success && m_equipCallback) {
           m_equipCallback(spell->GetFormID(), true);
        }
      }
      break;

      case UI::SlotContentType::Potion:
      case UI::SlotContentType::HealthPotion:
      case UI::SlotContentType::MagickaPotion:
      case UI::SlotContentType::StaminaPotion:
      Learning::EquipSourceTracker::GetSingleton().MarkHuginnEquip();
      success = UsePotion(content.formID);
      if (success && m_equipCallback) {
        m_equipCallback(content.formID, true);
      }
      break;

      case UI::SlotContentType::MeleeWeapon:
      case UI::SlotContentType::RangedWeapon:
      {
        Learning::EquipSourceTracker::GetSingleton().MarkHuginnEquip();
        bool leftHand = (hand == EquipHand::Left);
        success = EquipWeapon(content.formID, leftHand);
        if (success && m_equipCallback) {
           m_equipCallback(content.formID, true);
        }
      }
      break;

      case UI::SlotContentType::SoulGem:
      Learning::EquipSourceTracker::GetSingleton().MarkHuginnEquip();
      success = UseSoulGem(content.formID);
      if (success && m_equipCallback) {
        m_equipCallback(content.formID, true);
      }
      break;

      default:
      logger::warn("[EquipManager] Unknown slot content type"sv);
      break;
      }

      return success;
   }
}

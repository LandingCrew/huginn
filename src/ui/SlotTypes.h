#pragma once

#include <string>

namespace Huginn::UI
{
    /**
     * @brief Content type for a slot
     * Used by SlotAllocator, IntuitionMenu, and Wheeler integration.
     */
    enum class SlotContentType
    {
        Empty,          // No content (slot hidden)
        NoMatch,        // No matching candidate - shows "(No damage)" etc. (dimmed)
        Spell,          // Spell recommendation
        Wildcard,       // Wildcard exploration spell (blue)
        Potion,         // Generic potion (any type) - uses formID for equip
        HealthPotion,   // Health potion (red styling)
        MagickaPotion,  // Magicka potion (blue styling)
        StaminaPotion,  // Stamina potion (green styling)
        MeleeWeapon,    // Favorited melee weapon
        RangedWeapon,   // Favorited ranged weapon
        SoulGem         // Soul gem (informational - weapon needs charging)
    };

    /**
     * @brief Content for a single slot
     * Produced by SlotUtils::ToSlotContent(), consumed by display layers.
     */
    struct SlotContent
    {
        SlotContentType type = SlotContentType::Empty;
        std::string name;
        float confidence = 0.0f;  // Only used for Spell/Wildcard types
        RE::FormID formID = 0;    // FormID for direct spell lookup (avoids name collisions)

        // Factory methods
        static SlotContent Empty() { return {}; }
        static SlotContent NoMatch(const std::string& slotTypeName) {
            return { SlotContentType::NoMatch, slotTypeName, 0.0f, 0 };
        }
        static SlotContent Spell(const std::string& name, float confidence, RE::FormID formID = 0) {
            return { SlotContentType::Spell, name, confidence, formID };
        }
        static SlotContent Wildcard(const std::string& name, float confidence, RE::FormID formID = 0) {
            return { SlotContentType::Wildcard, name, confidence, formID };
        }
        static SlotContent Potion(const std::string& name, RE::FormID formID) {
            return { SlotContentType::Potion, name, 0.0f, formID };
        }
        static SlotContent HealthPotion(const std::string& name = "Health Potion", RE::FormID formID = 0) {
            return { SlotContentType::HealthPotion, name, 0.0f, formID };
        }
        static SlotContent MagickaPotion(const std::string& name = "Magicka Potion", RE::FormID formID = 0) {
            return { SlotContentType::MagickaPotion, name, 0.0f, formID };
        }
        static SlotContent StaminaPotion(const std::string& name = "Stamina Potion", RE::FormID formID = 0) {
            return { SlotContentType::StaminaPotion, name, 0.0f, formID };
        }
        static SlotContent MeleeWeapon(const std::string& name = "Equip Melee", RE::FormID formID = 0) {
            return { SlotContentType::MeleeWeapon, name, 0.0f, formID };
        }
        static SlotContent RangedWeapon(const std::string& name = "Equip Ranged", RE::FormID formID = 0) {
            return { SlotContentType::RangedWeapon, name, 0.0f, formID };
        }
        static SlotContent SoulGem(const std::string& name, RE::FormID formID = 0) {
            return { SlotContentType::SoulGem, name, 0.0f, formID };
        }

        bool IsEmpty() const { return type == SlotContentType::Empty; }
        bool IsNoMatch() const { return type == SlotContentType::NoMatch; }
        bool IsPotion() const {
            return type == SlotContentType::Potion ||
                   type == SlotContentType::HealthPotion ||
                   type == SlotContentType::MagickaPotion ||
                   type == SlotContentType::StaminaPotion;
        }
        bool IsWeapon() const { return type == SlotContentType::MeleeWeapon || type == SlotContentType::RangedWeapon; }
        bool IsSpell() const { return type == SlotContentType::Spell || type == SlotContentType::Wildcard; }
        bool IsSoulGem() const { return type == SlotContentType::SoulGem; }
    };
}

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <RE/Skyrim.h>

#include "spell/SpellData.h"
#include "learning/item/ItemData.h"
#include "learning/item/ItemRegistry.h"    // For InventoryItem
#include "weapon/WeaponData.h"
#include "scroll/ScrollData.h"
#include "scroll/ScrollRegistry.h" // For InventoryScroll

namespace Huginn::Candidate
{
    // =============================================================================
    // SOURCE TYPE - Identifies which registry/system the candidate came from
    // =============================================================================
    enum class SourceType : uint8_t
    {
        Spell = 0,
        Potion = 1,
        Scroll = 2,
        Weapon = 3,
        Ammo = 4,
        SoulGem = 5,
        Food = 6,
        Staff = 7,
        _Count       // Sentinel for array sizing - must be last
    };

    /// Number of valid source types (excludes _Count sentinel)
    inline constexpr size_t SOURCE_TYPE_COUNT = static_cast<size_t>(SourceType::_Count);

    // =============================================================================
    // RELEVANCE TAGS - Bitflags indicating WHY an item is contextually relevant
    // These tags are set by the CandidateGenerator based on current game state
    // =============================================================================
    enum class RelevanceTag : uint32_t
    {
        None = 0,

        // Vital-based relevance (player health/magicka/stamina)
        LowHealth       = 1 << 0,   // Health < 50%
        CriticalHealth  = 1 << 1,   // Health < 20%
        LowMagicka      = 1 << 2,   // Magicka < 30%
        LowStamina      = 1 << 3,   // Stamina < 30%

        // Active damage/effect relevance
        OnFire          = 1 << 4,   // Taking fire damage
        Poisoned        = 1 << 5,   // Has active poison effect
        Diseased        = 1 << 6,   // Has disease effect
        TakingFrost     = 1 << 7,   // Taking frost damage
        TakingShock     = 1 << 8,   // Taking shock damage

        // Environment-based relevance
        Underwater      = 1 << 9,   // Player is underwater
        LookingAtLock   = 1 << 10,  // Crosshair on locked container/door
        LookingAtOre    = 1 << 11,  // Crosshair on ore vein
        InDarkness      = 1 << 12,  // Low ambient light
        Falling         = 1 << 13,  // Player is falling (high velocity)

        // Combat-based relevance
        InCombat        = 1 << 14,  // Combat state active
        EnemyNearby     = 1 << 15,  // At least one hostile in range
        MultipleEnemies = 1 << 16,  // 3+ hostiles in range
        EnemyCasting    = 1 << 17,  // Tracked enemy is casting
        AllyInjured     = 1 << 18,  // Follower/ally below 50% health

        // Equipment-based relevance
        WeaponLowCharge = 1 << 19,  // Equipped weapon charge < 20%
        NeedsAmmo       = 1 << 20,  // Bow/crossbow equipped, low ammo
        NoWeapon        = 1 << 21,  // No weapon equipped

        // Target-type relevance
        TargetUndead    = 1 << 22,  // Fighting undead (sun/turn spells)
        TargetDragon    = 1 << 23,  // Fighting dragon

        // Stealth-based relevance
        Sneaking        = 1 << 24,  // Player is sneaking
        DetectedWhileSneaking = 1 << 25,  // Sneaking but detected

        // Workstation-based relevance (crafting stations)
        AtForge         = 1 << 26,  // Looking at forge/smithing station
        AtEnchanter     = 1 << 27,  // Looking at enchanting station
        AtAlchemy       = 1 << 28,  // Looking at alchemy station

        // Rate-based relevance (v0.12.x - sub-threshold vital tracking)
        HealthDeclining = 1u << 29,  // Taking sustained damage (damageRate > 2 HP/s)
        MagickaDraining = 1u << 30,  // Net magicka loss (usage > regen)
        StaminaDraining = 1u << 31,  // Net stamina loss (usage > regen)
    };

    // Bitwise operators for RelevanceTag
    inline constexpr RelevanceTag operator|(RelevanceTag a, RelevanceTag b) noexcept {
        return static_cast<RelevanceTag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline constexpr RelevanceTag operator&(RelevanceTag a, RelevanceTag b) noexcept {
        return static_cast<RelevanceTag>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    inline constexpr RelevanceTag& operator|=(RelevanceTag& a, RelevanceTag b) noexcept {
        return a = a | b;
    }
    inline constexpr RelevanceTag& operator&=(RelevanceTag& a, RelevanceTag b) noexcept {
        return a = a & b;
    }
    inline constexpr RelevanceTag operator~(RelevanceTag a) noexcept {
        return static_cast<RelevanceTag>(~static_cast<uint32_t>(a));
    }
    [[nodiscard]] inline constexpr bool HasTag(RelevanceTag tags, RelevanceTag check) noexcept {
        return (static_cast<uint32_t>(tags) & static_cast<uint32_t>(check)) != 0;
    }
    [[nodiscard]] inline constexpr bool HasAnyTag(RelevanceTag tags, RelevanceTag mask) noexcept {
        return (static_cast<uint32_t>(tags) & static_cast<uint32_t>(mask)) != 0;
    }
    [[nodiscard]] inline constexpr bool HasAllTags(RelevanceTag tags, RelevanceTag mask) noexcept {
        return (static_cast<uint32_t>(tags) & static_cast<uint32_t>(mask)) == static_cast<uint32_t>(mask);
    }

    // =============================================================================
    // CANDIDATE BASE - Common fields shared by all candidate types
    // =============================================================================
    struct CandidateBase
    {
        RE::FormID formID = 0;
        uint16_t uniqueID = 0;       // ExtraUniqueID for Wheeler (weapons/armor only)
        SourceType sourceType = SourceType::Spell;
        std::string name;

        // Pre-computed filter results (set by filters before scoring)
        bool canAfford = true;           // Has enough magicka (spells) or count > 0 (consumables)
        bool isEquipped = false;         // Already equipped in a slot
        bool isOnCooldown = false;       // Recently used, in cooldown period
        bool isBuffAlreadyActive = false; // Effect already active on player (waterbreathing, etc.)

        // Relevance metadata (set by CandidateGenerator based on game state)
        // NOTE: baseRelevance field removed in v0.12.x - replaced by ContextRuleEngine.contextWeight
        RelevanceTag relevanceTags = RelevanceTag::None;  // Why this item is relevant

        // Deduplication key combines source type, unique ID, and form ID.
        // Layout: [sourceType:8][uniqueID:16][formID:32] (bits 48-55, 32-47, 0-31)
        // uniqueID distinguishes enchanted weapon instances sharing the same base formID.
        // For non-weapon candidates uniqueID is 0, so the key still deduplicates correctly.
        [[nodiscard]] constexpr uint64_t GetDeduplicationKey() const noexcept {
            return (static_cast<uint64_t>(sourceType) << 48) |
                   (static_cast<uint64_t>(uniqueID) << 32) |
                   static_cast<uint64_t>(formID);
        }

        // Check if candidate passes basic pre-filters
        [[nodiscard]] constexpr bool PassesBasicFilters() const noexcept {
            return canAfford && !isEquipped && !isOnCooldown && !isBuffAlreadyActive;
        }
    };

    // =============================================================================
    // SPELL CANDIDATE - Wraps SpellData with candidate metadata
    // =============================================================================
    struct SpellCandidate : CandidateBase
    {
        Spell::SpellType type = Spell::SpellType::Unknown;
        Spell::SpellTag tags = Spell::SpellTag::None;
        Spell::MagicSchool school = Spell::MagicSchool::Unknown;
        Spell::ElementType element = Spell::ElementType::None;
        uint32_t baseCost = 0;
        bool isConcentration = false;
        float range = 0.0f;
        bool isFavorited = false;

        SpellCandidate() { sourceType = SourceType::Spell; }

        // Factory: Create SpellCandidate from SpellData
        [[nodiscard]] static SpellCandidate FromSpellData(const Spell::SpellData& data);
    };

    // =============================================================================
    // ITEM CANDIDATE - Wraps ItemData for potions, food, ingredients, soul gems
    // =============================================================================
    struct ItemCandidate : CandidateBase
    {
        Item::ItemType type = Item::ItemType::Unknown;
        Item::ItemTag tags = Item::ItemTag::None;
        Item::ItemTagExt tagsExt = Item::ItemTagExt::None;  // v0.8: Extended tags (Lingering, etc.)
        Item::MagicSchool school = Item::MagicSchool::None;        // Which magic school (for FortifyMagicSchool)
        Item::CombatSkill combatSkill = Item::CombatSkill::None;   // Which combat skill (for FortifyCombatSkill)
        Item::UtilitySkill utilitySkill = Item::UtilitySkill::None; // Which utility skill (for FortifyUtilitySkill)
        float magnitude = 0.0f;   // Effect strength (e.g., restore 50 health)
        float duration = 0.0f;    // Effect duration in seconds
        int32_t count = 0;        // Inventory count

        ItemCandidate() { sourceType = SourceType::Potion; }

        // Factory: Create ItemCandidate from ItemData + inventory count
        [[nodiscard]] static ItemCandidate FromItemData(const Item::ItemData& data, int32_t inventoryCount);

        // Factory: Create ItemCandidate from InventoryItem
        [[nodiscard]] static ItemCandidate FromInventoryItem(const Item::InventoryItem& invItem);
    };

    // =============================================================================
    // WEAPON CANDIDATE - Wraps WeaponData for favorited/relevant weapons
    // =============================================================================
    struct WeaponCandidate : CandidateBase
    {
        Weapon::WeaponType type = Weapon::WeaponType::Unknown;
        Weapon::WeaponTag tags = Weapon::WeaponTag::None;
        float baseDamage = 0.0f;
        float currentCharge = 0.0f;  // 0.0-1.0 for enchanted weapons (charge/maxCharge)
        float maxCharge = 0.0f;
        bool hasEnchantment = false;
        bool isFavorited = false;

        WeaponCandidate() { sourceType = SourceType::Weapon; }

        // Factory: Create WeaponCandidate from WeaponData
        [[nodiscard]] static WeaponCandidate FromWeaponData(const Weapon::WeaponData& data, bool favorited);

        // Factory: Create WeaponCandidate from InventoryWeapon
        [[nodiscard]] static WeaponCandidate FromInventoryWeapon(const Weapon::InventoryWeapon& invWeapon);

        // Helper: Get charge percentage (0.0-1.0)
        [[nodiscard]] float GetChargePercent() const noexcept {
            return maxCharge > 0.0f ? currentCharge / maxCharge : 1.0f;
        }
    };

    // =============================================================================
    // AMMO CANDIDATE - Wraps AmmoData for arrows/bolts
    // =============================================================================
    struct AmmoCandidate : CandidateBase
    {
        Weapon::AmmoType type = Weapon::AmmoType::Unknown;
        Weapon::WeaponTag tags = Weapon::WeaponTag::None;
        float baseDamage = 0.0f;
        bool hasEnchantment = false;
        int32_t count = 0;

        AmmoCandidate() { sourceType = SourceType::Ammo; }

        // Factory: Create AmmoCandidate from AmmoData + count
        [[nodiscard]] static AmmoCandidate FromAmmoData(const Weapon::AmmoData& data, int32_t inventoryCount);

        // Factory: Create AmmoCandidate from InventoryAmmo
        [[nodiscard]] static AmmoCandidate FromInventoryAmmo(const Weapon::InventoryAmmo& invAmmo);
    };

    // =============================================================================
    // SCROLL CANDIDATE - Wraps ScrollData for spell scrolls
    // =============================================================================
    struct ScrollCandidate : CandidateBase
    {
        Scroll::ScrollType type = Scroll::ScrollType::Unknown;
        Scroll::ScrollTag tags = Scroll::ScrollTag::None;
        Scroll::MagicSchool school = Scroll::MagicSchool::Unknown;
        Scroll::ElementType element = Scroll::ElementType::None;
        float magnitude = 0.0f;
        float duration = 0.0f;
        int32_t count = 0;

        ScrollCandidate() { sourceType = SourceType::Scroll; }

        // Factory: Create ScrollCandidate from ScrollData + count
        [[nodiscard]] static ScrollCandidate FromScrollData(const Scroll::ScrollData& data, int32_t inventoryCount);

        // Factory: Create ScrollCandidate from InventoryScroll
        [[nodiscard]] static ScrollCandidate FromInventoryScroll(const Scroll::InventoryScroll& invScroll);
    };

    // =============================================================================
    // CANDIDATE VARIANT - Unified type for polymorphic candidate handling
    // Using std::variant avoids virtual dispatch overhead
    // =============================================================================
    using CandidateVariant = std::variant<
        SpellCandidate,
        ItemCandidate,
        WeaponCandidate,
        AmmoCandidate,
        ScrollCandidate
    >;

    // Helper: Get reference to CandidateBase from variant (const version)
    [[nodiscard]] inline const CandidateBase& GetBase(const CandidateVariant& v) noexcept {
        return std::visit([](const auto& c) -> const CandidateBase& { return c; }, v);
    }

    // Helper: Get reference to CandidateBase from variant (mutable version)
    [[nodiscard]] inline CandidateBase& GetBase(CandidateVariant& v) noexcept {
        return std::visit([](auto& c) -> CandidateBase& { return c; }, v);
    }

    // Helper: Get FormID from variant
    [[nodiscard]] inline RE::FormID GetFormID(const CandidateVariant& v) noexcept {
        return GetBase(v).formID;
    }

    // Helper: Get UniqueID from variant (non-zero for weapons/armor)
    [[nodiscard]] inline uint16_t GetUniqueID(const CandidateVariant& v) noexcept {
        return GetBase(v).uniqueID;
    }

    // Helper: Get SourceType from variant
    [[nodiscard]] inline SourceType GetSourceType(const CandidateVariant& v) noexcept {
        return GetBase(v).sourceType;
    }

    // Helper: Get name from variant
    [[nodiscard]] inline const std::string& GetName(const CandidateVariant& v) noexcept {
        return GetBase(v).name;
    }

    // NOTE: GetRelevance() helper removed in v0.12.x - use ContextRuleEngine.Evaluate() instead

    // Helper: Check if variant is a specific type
    template<typename T>
    [[nodiscard]] inline bool IsType(const CandidateVariant& v) noexcept {
        return std::holds_alternative<T>(v);
    }

    // Helper: Get typed candidate (throws if wrong type)
    template<typename T>
    [[nodiscard]] inline const T& GetAs(const CandidateVariant& v) {
        return std::get<T>(v);
    }

    template<typename T>
    [[nodiscard]] inline T& GetAs(CandidateVariant& v) {
        return std::get<T>(v);
    }

    // Helper: Try to get typed candidate (returns nullptr if wrong type)
    template<typename T>
    [[nodiscard]] inline const T* TryGetAs(const CandidateVariant& v) noexcept {
        return std::get_if<T>(&v);
    }

    template<typename T>
    [[nodiscard]] inline T* TryGetAs(CandidateVariant& v) noexcept {
        return std::get_if<T>(&v);
    }

    // =============================================================================
    // SOURCE TYPE UTILITIES
    // =============================================================================
    [[nodiscard]] inline constexpr const char* SourceTypeToString(SourceType type) noexcept {
        switch (type) {
            case SourceType::Spell:   return "Spell";
            case SourceType::Potion:  return "Potion";
            case SourceType::Scroll:  return "Scroll";
            case SourceType::Weapon:  return "Weapon";
            case SourceType::Ammo:    return "Ammo";
            case SourceType::SoulGem: return "SoulGem";
            case SourceType::Food:    return "Food";
            case SourceType::Staff:   return "Staff";
            default:                  return "Unknown";
        }
    }

    // =============================================================================
    // STATIC ASSERTIONS - Compile-time verification
    // =============================================================================
    static_assert(SOURCE_TYPE_COUNT == 8, "SOURCE_TYPE_COUNT must match number of SourceType values");
    // Note: CandidateBase contains std::string which heap-allocates, so this is a size check
    // rather than a true cache-friendliness guarantee. Consider string_view for hot paths.
    static_assert(sizeof(CandidateBase) <= 72, "CandidateBase struct size check");
    static_assert((RelevanceTag::LowHealth | RelevanceTag::CriticalHealth) != RelevanceTag::None,
                  "Bitwise OR should work correctly");
    static_assert(HasTag(RelevanceTag::LowHealth | RelevanceTag::InCombat, RelevanceTag::LowHealth),
                  "HasTag should detect set bits");
    static_assert(!HasTag(RelevanceTag::LowHealth, RelevanceTag::InCombat),
                  "HasTag should not detect unset bits");

}  // namespace Huginn::Candidate

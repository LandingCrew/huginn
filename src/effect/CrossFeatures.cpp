#include "CrossFeatures.h"

#include "core/CrossFeatures.h"

namespace Huginn::Effect
{
    RuntimeFeatures ComputeRuntimeFeatures(const CatalogEntry& item, const State::PlayerActorState& player,
                                           const StackInfo& stack)
    {
        using namespace Core::Effect;
        RuntimeFeatures f;
        const auto& v = player.vitals;
        // Vitals are kept as 0-1 fractions of the effective max.
        const float deficits[3] = { (1.0f - v.health) * v.maxHealth, (1.0f - v.magicka) * v.maxMagicka,
                                    (1.0f - v.stamina) * v.maxStamina };
        const float maxes[3] = { v.maxHealth, v.maxMagicka, v.maxStamina };
        float* out[3] = { &f.overshootHealth, &f.overshootMagicka, &f.overshootStamina };
        for (std::size_t i = 0; i < 3; ++i) {
            if (item.restoreAmount[i] > 0.0f || item.fullRestore[i]) {
                *out[i] = Overshoot(item.restoreAmount[i], item.fullRestore[i], deficits[i], maxes[i]);
            }
        }

        if (Get(item.cap, Col::weapon_enchanted) > 0.0f || Get(item.cap, Col::kind_staff) > 0.0f) {
            f.weaponCharge = WeaponCharge(stack.charge, stack.maxCharge);
        }
        f.stackCount = StackCount(stack.count);

        if (item.kind == Kind::Ammo) {
            const Launcher inHand = player.hasCrossbowEquipped ? Launcher::Crossbow
                                    : player.hasBowEquipped    ? Launcher::Bow
                                                               : Launcher::None;
            f.ammoMatchesLauncher = AmmoMatchesLauncher(Get(item.cap, Col::ammo_bolt) > 0.0f, inHand);
        }

        if (item.school != School::None) {
            const auto& b = player.buffs;
            SchoolMask mask = 0;
            if (b.hasFortifyAlteration) mask |= SchoolBit(static_cast<std::uint8_t>(School::Alteration));
            if (b.hasFortifyConjuration) mask |= SchoolBit(static_cast<std::uint8_t>(School::Conjuration));
            if (b.hasFortifyDestruction) mask |= SchoolBit(static_cast<std::uint8_t>(School::Destruction));
            if (b.hasFortifyIllusion) mask |= SchoolBit(static_cast<std::uint8_t>(School::Illusion));
            if (b.hasFortifyRestoration) mask |= SchoolBit(static_cast<std::uint8_t>(School::Restoration));
            f.schoolFortified = SchoolFortified(static_cast<std::uint8_t>(item.school), mask);
        }
        return f;
    }
}

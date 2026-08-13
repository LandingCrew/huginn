#include "ContextWeightForCandidate.h"

#include <algorithm>
#include <variant>

namespace Huginn::Context
{
    float WeightForCandidate(
        const Candidate::CandidateVariant& candidate,
        const ContextWeightMap& weights)
    {
        using namespace Spell;
        using namespace Item;

        return std::visit([&weights](const auto& c) -> float {
            using T = std::decay_t<decltype(c)>;

            // =====================================================================
            // SPELL CANDIDATES
            // =====================================================================
            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                // Start with spell baseline (like weaponWeight for weapons)
                // Ensures spells surface on typed slots even without specific context
                float maxWeight = std::max(weights.spellWeight, weights.baseRelevanceWeight);

                // Restoration
                if (HasTag(c.tags, SpellTag::RestoreHealth)) {
                    maxWeight = std::max(maxWeight, weights.healingWeight);
                }
                if (HasTag(c.tags, SpellTag::RestoreMagicka)) {
                    maxWeight = std::max(maxWeight, weights.magickaRestoreWeight);
                }
                if (HasTag(c.tags, SpellTag::RestoreStamina)) {
                    maxWeight = std::max(maxWeight, weights.staminaRestoreWeight);
                }
                if (HasTag(c.tags, SpellTag::Ward)) {
                    maxWeight = std::max(maxWeight, weights.wardWeight);
                }

                // Combat/Damage
                if (c.type == SpellType::Damage) {
                    maxWeight = std::max(maxWeight, weights.damageWeight);
                }
                if (HasTag(c.tags, SpellTag::AOE)) {
                    maxWeight = std::max(maxWeight, weights.aoeWeight);
                }

                // Summons
                if (c.type == SpellType::Summon) {
                    maxWeight = std::max(maxWeight, weights.summonWeight);
                }
                if (HasTag(c.tags, SpellTag::BoundWeapon)) {
                    maxWeight = std::max(maxWeight, weights.boundWeaponWeight);
                }

                // Target-specific
                if (HasTag(c.tags, SpellTag::AntiUndead) || HasTag(c.tags, SpellTag::TurnUndead) ||
                    HasTag(c.tags, SpellTag::Sun)) {
                    maxWeight = std::max(maxWeight, weights.antiUndeadWeight);
                }
                if (HasTag(c.tags, SpellTag::AntiDaedra)) {
                    maxWeight = std::max(maxWeight, weights.antiDaedraWeight);
                }
                if (HasTagExt(c.tagsExt, SpellTagExt::AntiDragon)) {
                    maxWeight = std::max(maxWeight, weights.antiDragonWeight);
                }

                // Stealth
                if (HasTag(c.tags, SpellTag::Stealth) || HasTag(c.tags, SpellTag::Invisibility) ||
                    HasTag(c.tags, SpellTag::Muffle)) {
                    maxWeight = std::max(maxWeight, weights.stealthWeight);
                }

                // Resist spells: Buff/Defensive spells with elemental element map to resist weights.
                // SpellTag is at 32/32 bits — no room for ResistFire/Frost/Shock tags.
                // Instead, use the existing SpellType + ElementType combo set by SpellClassifier.
                // Damage spells have SpellType::Damage and won't match this check.
                if (c.type == SpellType::Buff || c.type == SpellType::Defensive) {
                    switch (c.element) {
                    case Spell::ElementType::Fire:
                        maxWeight = std::max(maxWeight, weights.resistFireWeight);
                        break;
                    case Spell::ElementType::Frost:
                        maxWeight = std::max(maxWeight, weights.resistFrostWeight);
                        break;
                    case Spell::ElementType::Shock:
                        maxWeight = std::max(maxWeight, weights.resistShockWeight);
                        break;
                    case Spell::ElementType::Poison:
                        maxWeight = std::max(maxWeight, weights.resistPoisonWeight);
                        break;
                    default: break;
                    }
                }

                // Environmental (#79). These three, plus AntiDragon above, are
                // why SpellTagExt exists: ContextRuleEngine has always computed
                // their weights and DominantReason has always been willing to
                // name them on the widget, but with SpellTag full there was no
                // bit for a candidate to match, so LookingAtLock, Falling and
                // TargetDragon never moved a ranking in their lives.
                //
                // Waterbreathing was the visible half-measure: the potion arm
                // below reads ItemTag::Waterbreathing, so drinking was ranked
                // and casting was not.
                if (HasTagExt(c.tagsExt, SpellTagExt::Unlock)) {
                    maxWeight = std::max(maxWeight, weights.unlockWeight);
                }
                if (HasTagExt(c.tagsExt, SpellTagExt::SlowFall)) {
                    maxWeight = std::max(maxWeight, weights.slowFallWeight);
                }
                if (HasTagExt(c.tagsExt, SpellTagExt::Waterbreathing)) {
                    maxWeight = std::max(maxWeight, weights.waterbreathingWeight);
                }

                return maxWeight;
            }
            // =====================================================================
            // POTION/ITEM CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                float maxWeight = weights.baseRelevanceWeight;  // Start with noise floor

                // Restoration
                if (HasTag(c.tags, ItemTag::RestoreHealth)) {
                    maxWeight = std::max(maxWeight, weights.healingWeight);
                }
                if (HasTag(c.tags, ItemTag::RestoreMagicka)) {
                    maxWeight = std::max(maxWeight, weights.magickaRestoreWeight);
                }
                if (HasTag(c.tags, ItemTag::RestoreStamina)) {
                    maxWeight = std::max(maxWeight, weights.staminaRestoreWeight);
                }

                // Elemental resistances
                if (HasTag(c.tags, ItemTag::ResistFire)) {
                    maxWeight = std::max(maxWeight, weights.resistFireWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistFrost)) {
                    maxWeight = std::max(maxWeight, weights.resistFrostWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistShock)) {
                    maxWeight = std::max(maxWeight, weights.resistShockWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistPoison)) {
                    maxWeight = std::max(maxWeight, weights.resistPoisonWeight);
                }
                if (HasTag(c.tags, ItemTag::ResistDisease)) {
                    maxWeight = std::max(maxWeight, weights.resistDiseaseWeight);
                }
                // Resist Magic: relevant when an enemy is casting (same
                // perceivable trigger as ward spells)
                if (HasTag(c.tags, ItemTag::ResistMagic)) {
                    maxWeight = std::max(maxWeight, weights.wardWeight);
                }

                // Buff & resist potions: always-on baseline + in-combat boost.
                // Mirrors the weapon/spell baselines — without it these are
                // pinned at baseRelevance (0.05) and can never clear
                // fMinimumUtility (0.1), so they never surface and never learn.
                // Type-based so untagged buffs (e.g. Fortify Jump) are covered.
                if (c.type == Item::ItemType::BuffPotion ||
                    c.type == Item::ItemType::ResistPotion) {
                    maxWeight = std::max(maxWeight, weights.buffPotionWeight);
                    maxWeight = std::max(maxWeight, weights.buffCombatWeight);
                }

                // Workstation fortify potions (FIX: Stage 1g code review)
                // These are split across three different tag/field pairs:
                if (HasTag(c.tags, ItemTag::FortifyCombatSkill)) {
                    if (c.combatSkill == Item::CombatSkill::Smithing) {
                        maxWeight = std::max(maxWeight, weights.fortifySmithingWeight);
                    }
                }
                if (HasTag(c.tags, ItemTag::FortifyMagicSchool)) {
                    if (c.school == Item::MagicSchool::Enchanting) {
                        maxWeight = std::max(maxWeight, weights.fortifyEnchantingWeight);
                    }
                }
                if (HasTag(c.tags, ItemTag::FortifyUtilitySkill)) {
                    if (c.utilitySkill == Item::UtilitySkill::Alchemy) {
                        maxWeight = std::max(maxWeight, weights.fortifyAlchemyWeight);
                    }
                }

                // Environmental (Waterbreathing potions)
                if (HasTag(c.tags, ItemTag::Waterbreathing)) {
                    maxWeight = std::max(maxWeight, weights.waterbreathingWeight);
                }

                // Stealth (Invisibility potions - Muffle doesn't exist as a potion)
                if (HasTag(c.tags, ItemTag::Invisibility)) {
                    maxWeight = std::max(maxWeight, weights.stealthWeight);
                }
                // Fortify Sneak while sneaking (same trigger as invisibility)
                if (HasTag(c.tags, ItemTag::FortifyUtilitySkill) &&
                    c.utilitySkill == Item::UtilitySkill::Sneak) {
                    maxWeight = std::max(maxWeight, weights.stealthWeight);
                }

                // Soul gems. Two weights, not one: weaponChargeWeight is the
                // urgent case and fires only with an enchanted weapon equipped
                // and drained, while soulGemWeight is the always-on baseline
                // that lets a gem appear on a slot that accepts it at all.
                // Without the baseline a player with filled gems and no
                // enchanted weapon equipped never saw one — the same lockout
                // the weapon, spell and buff-potion baselines exist to fix.
                if (c.sourceType == Candidate::SourceType::SoulGem) {
                    maxWeight = std::max(maxWeight, weights.weaponChargeWeight);
                    maxWeight = std::max(maxWeight, weights.soulGemWeight);
                }

                return maxWeight;
            }
            // =====================================================================
            // WEAPON CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                // Baseline: dedicated weapon weight, combat damage, or base relevance.
                // All three are always-on, so for a long time this arm returned a
                // constant — a weapon scored the same in a barrow as in a shop,
                // and was the only candidate type reading none of its own tags (#80).
                float maxWeight = std::max({weights.weaponWeight, weights.damageWeight,
                                            weights.baseRelevanceWeight});

                // Anti-undead. Silver is the material bonus; turn/banish are the
                // enchantments that do the same job. Both sides of this are
                // perceivable — the player can read the weapon and can see that
                // the thing in front of them is a draugr — so it stays inside the
                // "information the player already has" line.
                using Weapon::HasTag;
                using WT = Weapon::WeaponTag;
                if (HasTag(c.tags, WT::Silver) || HasTag(c.tags, WT::EnchantTurnUndead)) {
                    maxWeight = std::max(maxWeight, weights.antiUndeadWeight);
                }

                // Banish is a DIFFERENT axis, not a second anti-undead tag.
                // WeaponClassifier sets it from Archetype::kBanish, which sends
                // summoned daedra back to Oblivion and does nothing to draugr.
                // Same perceivability argument as above — a flame atronach is as
                // visible as a draugr — and antiDaedraWeight already exists,
                // fires on TargetType::Daedra, and is read by the spell arm.
                if (HasTag(c.tags, WT::EnchantBanish)) {
                    maxWeight = std::max(maxWeight, weights.antiDaedraWeight);
                }

                // NOT mapped: WT::Bound → boundWeaponWeight, though it looks like
                // the obvious mirror of the spell arm's BoundWeapon tag. That
                // weight fires only with nothing equipped (ContextRuleEngine.cpp
                // `noWeapon`), and a bound weapon exists as an inventory item
                // only while conjured — which auto-equips it. Tag and weight are
                // mutually exclusive, so the branch would be unreachable.

                // DELIBERATELY ABSENT: EnchantFire/Frost/Shock. Ranking those
                // means keying on what the TARGET resists or is weak to, which
                // the player cannot perceive and which CLAUDE.md puts off-limits
                // ("must NOT read enemy spell lists or abilities"). Undead-ness
                // is visible; elemental weakness is not. Do not "complete the
                // set" here.
                //
                // Also absent: NeedsCharge. A drained enchanted weapon is a
                // reason to rank it LOWER, and weaponChargeWeight already
                // surfaces the soul gem, which is the action actually taken.

                return maxWeight;
            }
            // =====================================================================
            // SCROLL CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                // Scrolls have spell tags, use same logic as spells
                float maxWeight = std::max(weights.spellWeight, weights.baseRelevanceWeight);

                if (HasTag(c.tags, Scroll::ScrollTag::RestoreHealth)) {
                    maxWeight = std::max(maxWeight, weights.healingWeight);
                }
                if (HasTag(c.tags, Scroll::ScrollTag::AOE)) {
                    maxWeight = std::max(maxWeight, weights.aoeWeight);
                }
                if (HasTag(c.tags, Scroll::ScrollTag::AntiUndead) || HasTag(c.tags, Scroll::ScrollTag::Sun)) {
                    maxWeight = std::max(maxWeight, weights.antiUndeadWeight);
                }

                // Extended tags, same four as the spell arm (#79). ScrollTagExt
                // is an alias of SpellTagExt because a scroll IS classified by
                // running the spell classifier over it — so a Scroll of
                // Waterbreathing arrives here already tagged, and leaving this
                // out would revive exactly the asymmetry this issue was about,
                // one candidate type over.
                if (HasTagExt(c.tagsExt, Scroll::ScrollTagExt::Unlock)) {
                    maxWeight = std::max(maxWeight, weights.unlockWeight);
                }
                if (HasTagExt(c.tagsExt, Scroll::ScrollTagExt::SlowFall)) {
                    maxWeight = std::max(maxWeight, weights.slowFallWeight);
                }
                if (HasTagExt(c.tagsExt, Scroll::ScrollTagExt::AntiDragon)) {
                    maxWeight = std::max(maxWeight, weights.antiDragonWeight);
                }
                if (HasTagExt(c.tagsExt, Scroll::ScrollTagExt::Waterbreathing)) {
                    maxWeight = std::max(maxWeight, weights.waterbreathingWeight);
                }

                return maxWeight;
            }
            // =====================================================================
            // AMMO CANDIDATES
            // =====================================================================
            else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                return std::max(weights.ammoWeight, weights.baseRelevanceWeight);
            }
            else {
                // Compile-time exhaustiveness: adding a new CandidateVariant
                // alternative must force a context-weight mapping here.
                static_assert(Candidate::always_false_v<T>,
                    "Unhandled CandidateVariant alternative in WeightForCandidate");
            }
        }, candidate);
    }
}

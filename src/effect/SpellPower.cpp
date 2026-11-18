#include "SpellPower.h"

namespace Huginn::Effect
{
    namespace
    {
        /// The power modifier (Fortify <School>) of a magic school's skill.
        RE::ActorValue PowerModifierOf(RE::ActorValue skill) noexcept
        {
            switch (skill) {
                case RE::ActorValue::kAlteration: return RE::ActorValue::kAlterationPowerModifier;
                case RE::ActorValue::kConjuration: return RE::ActorValue::kConjurationPowerModifier;
                case RE::ActorValue::kDestruction: return RE::ActorValue::kDestructionPowerModifier;
                case RE::ActorValue::kIllusion: return RE::ActorValue::kIllusionPowerModifier;
                case RE::ActorValue::kRestoration: return RE::ActorValue::kRestorationPowerModifier;
                default: return RE::ActorValue::kNone;
            }
        }
    }

    std::vector<SpellEffectPower> SpellPowerOf(RE::Actor* caster, RE::MagicItem* spell)
    {
        std::vector<SpellEffectPower> out;
        if (!caster || !spell) return out;
        auto* avo = caster->AsActorValueOwner();
        std::uint32_t index = 0;
        for (auto* e : spell->effects) {
            // Counted even when empty (a missing master's effect): the
            // catalog's rows are numbered by place in the list.
            const std::uint32_t at = index++;
            if (!e || !e->baseEffect) continue;
            auto* mgef = e->baseEffect;
            SpellEffectPower p;
            p.index = at;
            p.effect = mgef->GetFormID();
            p.name = mgef->GetName() ? mgef->GetName() : "";
            p.baseMagnitude = e->effectItem.magnitude;
            p.baseDuration = e->effectItem.duration;

            float mag = p.baseMagnitude;
            float dur = static_cast<float>(p.baseDuration);
            RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellMagnitude, caster, spell, mgef, &mag);
            RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSpellDuration, caster, spell, mgef, &dur);
            p.perkMagnitude = mag;
            p.perkDuration = dur;

            const auto powerAV = PowerModifierOf(mgef->GetMagickSkill());
            p.powerModifier = (avo && powerAV != RE::ActorValue::kNone) ? avo->GetActorValue(powerAV) : 0.0f;
            p.powerAffectsMagnitude = mgef->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kPowerAffectsMagnitude);
            p.powerAffectsDuration = mgef->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kPowerAffectsDuration);
            const float power = 1.0f + p.powerModifier / 100.0f;
            p.magnitude = p.powerAffectsMagnitude ? mag * power : mag;
            p.duration = p.powerAffectsDuration ? dur * power : dur;
            out.push_back(std::move(p));
        }
        return out;
    }
}

#include "EffectReader.h"

#include "util/FormRead.h"

namespace Huginn::Effect
{
    namespace
    {
        using namespace Core::Effect;

        bool Named(const RE::TESForm* f)
        {
            const char* n = f ? f->GetName() : nullptr;
            return n && *n;
        }

        std::string NameOf(const RE::TESForm* f)
        {
            const char* n = f ? f->GetName() : nullptr;
            return n ? std::string(n) : std::string{};
        }

        ItemRecord Base(Kind kind, const RE::TESBoundObject* form)
        {
            ItemRecord it;
            it.kind = kind;
            it.formId = form->GetFormID();
            it.plugin = std::string(Util::PluginOf(form));
            it.name = NameOf(form);
            it.playable = form->GetPlayable();
            // A spell's "gold value" is its magicka cost: left at 0, as the dump
            // leaves it blank.
            it.value = form->Is(RE::FormType::Spell) ? 0 : form->GetGoldValue();
            it.weight = form->GetWeight();
            if (const auto* kw = form->As<RE::BGSKeywordForm>()) it.keywords = Util::Keywords(kw);
            return it;
        }
    }

    void EffectReader::ReadTomes()
    {
        if (tomesRead_) return;
        tomesRead_ = true;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        // Spells a tome teaches: the line between a player spell and one only
        // NPCs cast, which spellType alone cannot draw.
        for (auto* book : dh->GetFormArray<RE::TESObjectBOOK>()) {
            if (const auto* taught = book ? book->GetSpell() : nullptr) taughtByTome_.insert(taught->GetFormID());
        }
    }

    std::uint32_t EffectReader::EffectIndex(const RE::EffectSetting* m, ReadResult& out, bool withPayload)
    {
        std::uint32_t idx = 0;
        if (const auto it = out.effectIndex.find(m); it != out.effectIndex.end()) {
            idx = it->second;
        }
        else {
            MagicEffectRecord r;
            r.formId = m->GetFormID();
            r.plugin = std::string(Util::PluginOf(m));
            const char* full = m->GetFullName();
            r.name = full ? full : "";
            r.archetype = static_cast<int>(m->data.archetype);
            r.primaryAV = std::string(Util::AvName(m->data.primaryAV));
            r.secondaryAV = std::string(Util::AvName(m->data.secondaryAV));
            r.resistAV = std::string(Util::AvName(m->data.resistVariable));
            r.delivery = static_cast<int>(m->data.delivery);
            r.castingType = static_cast<int>(m->data.castingType);
            r.baseCost = m->data.baseCost;
            r.flags = static_cast<std::uint32_t>(m->data.flags.underlying());
            r.detrimental = m->IsDetrimental();
            r.hostile = m->IsHostile();
            r.keywords = Util::Keywords(m);
            const char* desc = m->magicItemDescription.c_str();
            r.description = desc ? desc : "";
            r.school = std::string(Util::AvName(m->data.associatedSkill));
            // A Light effect's strength is its light form's radius (P x G(radius)).
            // The light is the Light archetype's associated form; data.light is
            // the "casting light" every effect may have (a Fireball's glow).
            if (m->data.archetype == RE::EffectSetting::Archetype::kLight && m->data.associatedForm) {
                if (const auto* l = m->data.associatedForm->As<RE::TESObjectLIGH>()) {
                    r.lightRadius = static_cast<int>(l->data.radius);
                }
            }
            r.payloadKnown = true;  // in game the link is always readable (or absent)
            idx = static_cast<std::uint32_t>(out.effects.size());
            out.effects.push_back(std::move(r));
            out.mgefs.push_back(m);
            out.payloadSpells.push_back(nullptr);
            out.payloadItems.emplace_back();
            out.payloadFilled.push_back(false);
            out.effectIndex.emplace(m, idx);
        }
        if (!withPayload || out.payloadFilled[idx]) return idx;
        out.payloadFilled[idx] = true;

        // A Cloak's payload is its associated spell; a SpawnHazard's is the
        // spell of the hazard it drops. One level deep: a payload's own
        // payload is not followed (a cloak carrying a cloak would recurse).
        const RE::MagicItem* spell = nullptr;
        const auto arch = m->data.archetype;
        auto* assoc = m->data.associatedForm;
        if (arch == RE::EffectSetting::Archetype::kCloak) {
            spell = assoc ? assoc->As<RE::SpellItem>() : nullptr;
        }
        else if (arch == RE::EffectSetting::Archetype::kSpawnHazard) {
            const auto* hazard = assoc ? assoc->As<RE::BGSHazard>() : nullptr;
            spell = hazard ? hazard->data.spell : nullptr;
        }
        if (!spell) return idx;
        out.payloadSpells[idx] = spell;
        std::uint32_t position = 0;
        for (const auto* e : spell->effects) {
            const std::uint32_t at = position++;
            if (!e || !e->baseEffect) continue;
            const std::uint32_t pi = EffectIndex(e->baseEffect, out, false);
            EffectRow row;
            row.effect = pi;
            row.magnitude = e->effectItem.magnitude;
            row.duration = e->effectItem.duration;
            row.area = e->effectItem.area;
            row.cost = e->cost;
            row.index = at;
            out.effects[idx].payload.push_back(row);
            out.payloadItems[idx].push_back(e);
        }
        return idx;
    }

    void EffectReader::AppendEffects(const RE::BSTArray<RE::Effect*>& effects, ItemRecord& item,
                                     std::vector<const RE::Effect*>& items, ReadResult& out)
    {
        std::uint32_t position = 0;
        for (const auto* e : effects) {
            const std::uint32_t at = position++;  // the old dump counted array positions
            if (!e || !e->baseEffect) continue;
            EffectRow row;
            row.effect = EffectIndex(e->baseEffect, out, true);
            row.magnitude = e->effectItem.magnitude;
            row.duration = e->effectItem.duration;
            row.area = e->effectItem.area;
            row.cost = e->cost;
            row.index = at;
            item.effects.push_back(row);
            items.push_back(e);
        }
    }

    bool EffectReader::ReadForm(const RE::TESBoundObject* form, const RE::EnchantmentItem* playerEnchantment,
                                ReadResult& out)
    {
        if (!form) return false;
        ReadTomes();
        std::vector<const RE::Effect*> items;
        ItemRecord it;

        const auto type = form->GetFormType();
        if (type == RE::FormType::Spell) {
            const auto* s = static_cast<const RE::SpellItem*>(form);
            it = Base(Kind::Spell, s);
            it.spellType = static_cast<int>(s->GetSpellType());
            it.castingType = static_cast<int>(s->GetCastingType());
            it.delivery = static_cast<int>(s->GetDelivery());
            it.magickaCost = s->CalculateMagickaCost(nullptr);
            it.taughtByTome = taughtByTome_.contains(s->GetFormID());
            AppendEffects(s->effects, it, items, out);
        }
        else if (type == RE::FormType::Scroll) {
            // By form type, not As<>: a ScrollItem IS-A SpellItem.
            const auto* sc = static_cast<const RE::ScrollItem*>(form);
            it = Base(Kind::Scroll, sc);
            it.spellType = static_cast<int>(sc->GetSpellType());
            it.castingType = static_cast<int>(sc->GetCastingType());
            it.delivery = static_cast<int>(sc->GetDelivery());
            AppendEffects(sc->effects, it, items, out);
        }
        else if (const auto* a = type == RE::FormType::AlchemyItem ? static_cast<const RE::AlchemyItem*>(form) : nullptr) {
            it = Base(a->IsPoison() ? Kind::Poison : a->IsFood() ? Kind::Food : Kind::Potion, a);
            AppendEffects(a->effects, it, items, out);
        }
        else if (const auto* w = type == RE::FormType::Weapon ? static_cast<const RE::TESObjectWEAP*>(form) : nullptr) {
            it = Base(Kind::Weapon, w);
            it.weaponType = static_cast<int>(w->GetWeaponType());
            it.twoHanded = w->IsTwoHandedSword() || w->IsTwoHandedAxe() || w->IsBow() || w->IsCrossbow();
            it.damage = static_cast<float>(w->GetAttackDamage());
            it.speed = w->weaponData.speed;
            it.reach = w->weaponData.reach;
            it.critDamage = static_cast<float>(w->criticalData.damage);
            const RE::EnchantmentItem* ench = playerEnchantment ? playerEnchantment : w->formEnchanting;
            if (ench) {
                it.enchanted = true;
                it.enchantCastingType = static_cast<int>(ench->GetCastingType());
                AppendEffects(ench->effects, it, items, out);
            }
        }
        else if (const auto* am = type == RE::FormType::Ammo ? static_cast<const RE::TESAmmo*>(form) : nullptr) {
            it = Base(Kind::Ammo, am);
            it.damage = am->data.damage;
            it.ammoNonBolt = am->data.flags.all(RE::AMMO_DATA::Flag::kNonBolt);
        }
        else if (const auto* ar = type == RE::FormType::Armor ? static_cast<const RE::TESObjectARMO*>(form) : nullptr) {
            it = Base(Kind::Armour, ar);
            it.slotMask = static_cast<std::uint32_t>(ar->GetSlotMask());
            it.armorRating = const_cast<RE::TESObjectARMO*>(ar)->GetArmorRating();  // a read; not const in CommonLib
            it.armourWeight = ar->IsHeavyArmor()   ? ArmourWeight::Heavy
                              : ar->IsLightArmor() ? ArmourWeight::Light
                                                   : ArmourWeight::Clothing;
            const RE::EnchantmentItem* ench = playerEnchantment ? playerEnchantment : ar->formEnchanting;
            if (ench) {
                it.enchanted = true;
                it.enchantCastingType = static_cast<int>(ench->GetCastingType());
                AppendEffects(ench->effects, it, items, out);
            }
        }
        else if (const auto* g = type == RE::FormType::SoulGem ? static_cast<const RE::TESSoulGem*>(form) : nullptr) {
            it = Base(Kind::SoulGem, g);
            it.soulCapacity = static_cast<int>(g->GetMaximumCapacity());
            it.soulContained = static_cast<int>(g->GetContainedSoul());
        }
        else if (const auto* l = type == RE::FormType::Light ? static_cast<const RE::TESObjectLIGH*>(form) : nullptr) {
            if (!l->CanBeCarried()) return false;
            it = Base(Kind::Light, l);
            it.lightRadius = static_cast<int>(l->data.radius);
        }
        else {
            return false;
        }
        out.items.push_back(std::move(it));
        out.forms.push_back(form);
        out.effectItems.push_back(std::move(items));
        return true;
    }

    ReadResult EffectReader::ReadLoadOrder()
    {
        ReadResult out;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return out;
        ReadTomes();
        // The dump's order: spells, scrolls, alchemy, weapons, ammo, armour,
        // soul gems, lights.
        auto readAll = [&](auto& array) {
            for (auto* f : array) {
                if (!Named(f)) continue;
                ReadForm(f, nullptr, out);
            }
        };
        readAll(dh->GetFormArray<RE::SpellItem>());
        readAll(dh->GetFormArray<RE::ScrollItem>());
        readAll(dh->GetFormArray<RE::AlchemyItem>());
        readAll(dh->GetFormArray<RE::TESObjectWEAP>());
        readAll(dh->GetFormArray<RE::TESAmmo>());
        readAll(dh->GetFormArray<RE::TESObjectARMO>());
        readAll(dh->GetFormArray<RE::TESSoulGem>());
        readAll(dh->GetFormArray<RE::TESObjectLIGH>());
        return out;
    }
}

#include "effect/Facets.h"

#include "effect/EffectCatalog.h"

#include <mutex>

namespace Huginn::Effect
{
    namespace
    {
#ifndef NDEBUG
        std::mutex g_testMutex;
        TestResolver g_testResolver;
#endif

        Core::Facets::School SchoolOf(Spell::MagicSchool s) noexcept
        {
            using S = Core::Facets::School;
            switch (s) {
                case Spell::MagicSchool::Destruction: return S::Destruction;
                case Spell::MagicSchool::Restoration: return S::Restoration;
                case Spell::MagicSchool::Alteration: return S::Alteration;
                case Spell::MagicSchool::Illusion: return S::Illusion;
                case Spell::MagicSchool::Conjuration: return S::Conjuration;
                default: return S::None;
            }
        }
    }

    FormFacets FacetsOfForm(RE::FormID formID)
    {
        FormFacets out;
        auto& catalog = EffectCatalog::GetSingleton();
        if (!catalog.Ready() || formID == 0) return out;
        if (const auto* entry = catalog.FindOrMapId(formID)) {
            out.known = true;
            out.kind = entry->kind;
            out.facets = Core::Facets::FromCap(entry->cap, entry->kind);
        }
        return out;
    }

    Core::Facets::Subject SubjectOf(const Candidate::CandidateVariant& candidate)
    {
        using Core::Facets::Source;
        Core::Facets::Subject s;
        std::visit([&s](const auto& c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                s.source = Source::Spell;
                s.kind = Core::Effect::Kind::Spell;
                s.school = SchoolOf(c.school);
            }
            else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                s.source = Source::Scroll;
                s.kind = Core::Effect::Kind::Scroll;
                s.school = SchoolOf(c.school);
            }
            else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                s.source = Source::Item;
                // Without a catalog entry the source type is the kind (the
                // equip path's own split: potion, food, soul gem).
                s.kind = c.sourceType == Candidate::SourceType::Food      ? Core::Effect::Kind::Food
                       : c.sourceType == Candidate::SourceType::SoulGem   ? Core::Effect::Kind::SoulGem
                                                                          : Core::Effect::Kind::Potion;
            }
            else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                using W = Weapon::WeaponType;
                s.source = Source::Weapon;
                s.kind = Core::Effect::Kind::Weapon;
                s.ranged = c.type == W::Bow || c.type == W::Crossbow || c.type == W::Staff;
                s.melee = c.type == W::OneHandSword || c.type == W::OneHandAxe || c.type == W::OneHandMace ||
                          c.type == W::OneHandDagger || c.type == W::TwoHandSword || c.type == W::TwoHandAxe;
            }
            else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                s.source = Source::Ammo;
                s.kind = Core::Effect::Kind::Ammo;
            }
            else if constexpr (std::is_same_v<T, Candidate::ApparelCandidate>) {
                s.source = Source::Apparel;
                s.kind = Core::Effect::Kind::Armour;
            }
            else if constexpr (std::is_same_v<T, Candidate::TorchCandidate>) {
                s.source = Source::Torch;
                s.kind = Core::Effect::Kind::Light;
            }
        }, candidate);

        const auto ff = FacetsOfForm(Candidate::GetFormID(candidate));
        if (ff.known) {
            s.known = true;
            s.facets = ff.facets;
            // The catalog's kind for an item (a poison, a food the source type
            // calls a potion); the variant's own source decides the rest.
            if (s.source == Source::Item) s.kind = ff.kind;
            return s;
        }
#ifndef NDEBUG
        {
            std::lock_guard lock(g_testMutex);
            if (g_testResolver) {
                if (const auto t = g_testResolver(candidate)) {
                    s.known = t->known;
                    s.facets = t->facets;
                    if (s.source == Source::Item) s.kind = t->kind;
                }
            }
        }
#endif
        return s;
    }

#ifndef NDEBUG
    void SetTestResolver(TestResolver resolver)
    {
        std::lock_guard lock(g_testMutex);
        g_testResolver = std::move(resolver);
    }
#endif
}

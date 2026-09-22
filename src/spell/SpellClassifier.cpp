#include "SpellClassifier.h"
#include "util/NameMatch.h"

namespace Huginn::Spell
{
   void SpellClassifier::LoadOverrides(const std::filesystem::path& iniPath)
   {
      m_overrides.LoadFromFile(iniPath);
   }

   SpellData SpellClassifier::ClassifySpell(RE::SpellItem* spell) const
   {
      if (!spell) {
      logger::error("ClassifySpell called with null spell"sv);
      return SpellData{};
      }

      SpellData data{};
      data.formID = spell->GetFormID();
      data.name = spell->GetName();

      // Check for overrides first (by FormID, then by name)
      std::optional<SpellOverride> override;

      if (m_overrides.HasOverride(data.formID)) {
      override = m_overrides.GetOverride(data.formID);
      logger::debug("Using FormID override for: {} ({:08X})"sv, data.name, data.formID);
      } else if (m_overrides.HasOverride(data.name)) {
      override = m_overrides.GetOverride(data.name);
      logger::debug("Using name override for: {}"sv, data.name);
      }

      // OPTIMIZATION (v0.7.19): Compute costliest effect ONCE for all helpers
      // This reduces O(4n) → O(n) effect iterations per spell at load time
      RE::Effect* costliestEffect = GetCostliestEffect(spell);
      RE::EffectSetting* primaryEffect = costliestEffect ? costliestEffect->baseEffect : nullptr;

      // STEP 1: Compute tags FIRST (single source of name-matching)
      data.tags = override ? override->tags.value_or(DetermineSpellTags(spell))
                           : DetermineSpellTags(spell);

      // Extended tags are NOT overridable from the INI (#79). The override file
      // parses one `tags =` list against SpellTag names, and giving it a second
      // vocabulary for four tags nobody has asked to override yet is more
      // surface than it earns. Auto-detection is API-based for three of them,
      // which is the part an override normally exists to rescue.
      data.tagsExt = DetermineSpellTagsExt(spell);

      // STEP 2: Determine type - API first, then derive from tags
      if (override && override->type) {
      data.type = *override->type;
      } else {
      data.type = DetermineSpellType(spell, primaryEffect);  // API-based

      // A script effect is a closed door: no archetype, no actor value, nothing
      // to read. When one is merely the COSTLIEST effect it hides the rest of
      // the spell, so try again with the costliest effect that is not a script.
      // 66 of the 157 script-primary spells in the 2026-09-21 dump have such an
      // effect; the other 91 are script all the way down and stay Unknown, which
      // is the honest answer for them.
      if (data.type == SpellType::Unknown && primaryEffect &&
          primaryEffect->GetArchetype() == RE::EffectSetting::Archetype::kScript) {
        if (auto* readable = GetCostliestNonScriptEffect(spell)) {
           if (auto* setting = readable->baseEffect) {
            data.type = DetermineSpellType(spell, setting);
           }
        }

        // Still nothing, and the spell is script-driven: ask the NAME before
        // asking the tags. For these spells the tags are themselves guesses,
        // and a guess about an element outranks a word that names the purpose
        // -- which is how "Open Novice Lock" came out as Damage, off a Frost
        // tag, and landed in a combat slot. A purpose word is the better
        // evidence when there is no effect data at all.
        if (data.type == SpellType::Unknown) {
           data.type = DeriveSpellTypeFromName(spell->GetName());
        }
      }

      if (data.type == SpellType::Unknown) {
        data.type = DeriveSpellTypeFromTags(data.tags, data.tagsExt);  // Tag-based fallback
      }

      // Everything the API can say has been said. What is left is script-driven
      // and the name is the only evidence there is.
      if (data.type == SpellType::Unknown) {
        data.type = DeriveSpellTypeFromName(spell->GetName());
      }
      }

      // STEP 3: School - API only (no name fallback needed)
      data.school = DetermineMagicSchool(costliestEffect);

      // STEP 4: Element - API first, then derive from tags
      data.element = DetermineElementType(costliestEffect);  // API-based (resistVariable)
      if (data.element == ElementType::None) {
      data.element = DeriveElementFromTags(data.tags);  // Tag-based fallback
      }
      // Sun damage overrides Magic (Dawnguard spells use kResistMagic but are semantically Sun)
      if (data.element == ElementType::Magic && HasTag(data.tags, SpellTag::Sun)) {
      data.element = ElementType::Sun;
      }
      // Utility spells (Open Lock, Telekinesis) shouldn't have elemental damage
      // Some modded spells incorrectly use elemental resistVariable
      if (data.type == SpellType::Utility && data.element != ElementType::None) {
      data.element = ElementType::None;
      }

      data.baseCost = GetBaseCost(spell);
      data.isConcentration = IsConcentration(spell);
      data.range = GetEffectiveRange(spell, primaryEffect);  // reuse pre-computed effect

      return data;
   }

   SpellType SpellClassifier::DetermineSpellType(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const
   {
      // ONLY API-based checks here
      // Name-based fallback handled by DeriveSpellTypeFromTags() in ClassifySpell()
      // OPTIMIZATION (v0.7.19): primaryEffect is pre-computed by caller

      if (!spell) return SpellType::Unknown;
      if (!primaryEffect) return SpellType::Unknown;

      const auto archetype = primaryEffect->GetArchetype();
      const bool isHostile = primaryEffect->data.flags.any(
      RE::EffectSetting::EffectSettingData::Flag::kHostile);

      // kHostile is the author's INTENT and mod authors set it carelessly; a
      // 2026-09-21 audit found "Insomnia" -- an Illusion nightmare spell whose
      // four identical siblings are all Debuff -- typed Healing purely because
      // its author left the flag clear. kDetrimental is the mechanical fact:
      // this effect moves the value DOWN. Where the two disagree, believe the
      // mechanism.
      const bool isDetrimental = primaryEffect->data.flags.any(
      RE::EffectSetting::EffectSettingData::Flag::kDetrimental);
      const bool harmful = isHostile || isDetrimental;
      // Derive school from primaryEffect directly to avoid redundant GetCostliestEffect call
      const auto school = [&]() -> MagicSchool {
      auto skillAV = primaryEffect->GetMagickSkill();
      switch (skillAV) {
      case RE::ActorValue::kDestruction:  return MagicSchool::Destruction;
      case RE::ActorValue::kRestoration:  return MagicSchool::Restoration;
      case RE::ActorValue::kAlteration:   return MagicSchool::Alteration;
      case RE::ActorValue::kIllusion:     return MagicSchool::Illusion;
      case RE::ActorValue::kConjuration:  return MagicSchool::Conjuration;
      default:                            return MagicSchool::Unknown;
      }
      }();

      // Healing: health restoration that actually RESTORES.
      //
      // Two ways this used to lie. A detrimental health effect with the hostile
      // flag clear was typed Healing and would have been offered to a dying
      // player ("Insomnia", and any blood-magic spell that pays health as a
      // cost). And kPeakValueModifier on health is a FORTIFY -- it raises the
      // maximum and restores nothing -- so "Fortify Attributes" was a heal that
      // heals nobody. Fortifies fall through to the switch below and land in
      // Buff, where they belong.
      if (!harmful && primaryEffect->data.primaryAV == RE::ActorValue::kHealth) {
      if (archetype == RE::EffectSetting::Archetype::kValueModifier) {
        return SpellType::Healing;
      }
      // A peak modifier on health is a FORTIFY or a heal-over-time, and the
      // difference is kRecover: it means the value returns to what it was when
      // the effect ends, which is what a temporary maximum does and what a heal
      // never does.
      //
      // Excluding peak modifiers outright, as the first cut did, turned
      // "Healing Aura" and "Greater Healing Aura" into Buffs along with the four
      // genuine fortifies -- 8 real heals lost to catch 4 impostors (measured
      // 2026-09-21, Healing fell 38 -> 24 and only 4 of that was intended).
      if (archetype == RE::EffectSetting::Archetype::kPeakValueModifier &&
          !primaryEffect->data.flags.any(
             RE::EffectSetting::EffectSettingData::Flag::kRecover)) {
        return SpellType::Healing;
      }
      }

      // Summon: SummonCreature archetype
      if (archetype == RE::EffectSetting::Archetype::kSummonCreature) {
      return SpellType::Summon;
      }

      // Defensive: anything that MITIGATES incoming damage, whatever school
      // casts it and whatever archetype carries it.
      //
      // DamageResist alone left the whole resist-element line as Buff, where
      // Fire Shield competed with Fortify Speech instead of with Ebonyflesh
      // (11 spells, 2026-09-21 audit). By the definition this classifier works
      // to -- "mitigates incoming damage" -- they are the same kind of thing.
      //
      // Ward power is here rather than in the archetype switch because wards
      // are kAccumulateMagnitude, which is also how some overhauls build
      // charge-up damage: the ACTOR VALUE is what makes a ward a ward, and
      // reading it here also rescues the two wards whose costliest effect is a
      // script.
      if (!harmful) {
      switch (primaryEffect->data.primaryAV) {
      case RE::ActorValue::kDamageResist:
      case RE::ActorValue::kPoisonResist:
      case RE::ActorValue::kResistFire:
      case RE::ActorValue::kResistShock:
      case RE::ActorValue::kResistFrost:
      case RE::ActorValue::kResistMagic:
      case RE::ActorValue::kResistDisease:
      case RE::ActorValue::kWardPower:
      case RE::ActorValue::kWardDeflection:
        return SpellType::Defensive;
      default:
        break;
      }
      }

      // =====================================================================
      // ARCHETYPE RULES
      // =====================================================================
      // Everything above keys on school or on one archetype at a time, which
      // left 375 of the 1,106 spells a LoreRim player can LEARN with no type at
      // all (2026-09-21, `hg dump spells`). Grouping those by archetype showed
      // the gap is not subtle: the rules were written for vanilla Destruction
      // and never extended to the archetypes an overhaul actually uses. Counts
      // below are that measurement, and are what each rule is worth.
      //
      // An untyped spell is not dropped -- it is ranked without a type -- so the
      // cost of all this was quietly bad ordering rather than anything visible.
      switch (archetype) {
      // --- what the value-modifier family means, once school is not the test ---
      //
      // A hostile spell that moves the target's HEALTH is a damage spell, and
      // 70 of the 375 were exactly that: Restoration sun damage, Alteration
      // rock spells, Conjuration life-drain. The old rule asked for
      // school == Destruction, which is where vanilla happens to keep them.
      case RE::EffectSetting::Archetype::kValueModifier:
      case RE::EffectSetting::Archetype::kDualValueModifier:
      case RE::EffectSetting::Archetype::kPeakValueModifier:
      {
      const auto av = primaryEffect->data.primaryAV;
      const bool touchesHealth = av == RE::ActorValue::kHealth ||
                                 primaryEffect->data.secondaryAV == RE::ActorValue::kHealth;
      if (harmful) {
        // secondaryAV too: a dual modifier that drains stamina AND health is a
        // damage spell, whichever of the two the author put first.
        return touchesHealth ? SpellType::Damage : SpellType::Debuff;
      }
      if (av == RE::ActorValue::kHealth) {
        // Only kDualValueModifier reaches here -- the other two were answered by
        // the healing check above. A peak modifier on health is a fortify and is
        // deliberately NOT caught there, so it falls past this to Buff.
        if (archetype == RE::EffectSetting::Archetype::kDualValueModifier) {
           return SpellType::Healing;
        }
      }
      // Fortify anything: magicka, stamina, carry weight, speed, a skill.
      // ~40 spells, and the reason "Fortify Carry Weight" had no type.
      return SpellType::Buff;
      }

      // Hazards are placed, then hurt whoever walks in -- runes, ash clouds,
      // fire walls. All 15 in the dump read non-hostile, because the hazard does
      // the harm and the spell that spawns it does not, so hostility is the
      // wrong question to ask of them.
      case RE::EffectSetting::Archetype::kSpawnHazard:
      {
        // A hazard spell is non-hostile because the HAZARD does the harm, not
        // the casting -- so hostility is the wrong question and the rule used to
        // answer Damage regardless. That swallowed the protective circles:
        // Guardian Circle, the "I am in trouble, drop a healing circle" button,
        // was typed Damage (7 spells, 2026-09-21 audit).
        //
        // So ask the spell instead of the flag: if nothing it carries takes a
        // value away, it is not an attack. Walk every effect, because the
        // costliest one on a circle is often the aura rather than the bite.
        for (const auto* effect : spell->effects) {
           if (effect && effect->baseEffect &&
              effect->baseEffect->data.flags.any(
                 RE::EffectSetting::EffectSettingData::Flag::kDetrimental)) {
            return SpellType::Damage;
           }
        }
        return SpellType::Buff;
      }

      // Absorb takes from the target and gives to the caster. Ranking it as
      // damage is what a player expects -- offering Absorb Health as a HEAL at
      // 10% against a lone archer would be a bad recommendation, and the audit
      // agreed. But only health absorption is damage: draining magicka or
      // stamina impairs without hurting, and two absorb spells with the hostile
      // flag clear turn out to be weapon enchants rather than attacks.
      case RE::EffectSetting::Archetype::kAbsorb:
        if (!harmful) {
           return SpellType::Buff;
        }
        return (primaryEffect->data.primaryAV == RE::ActorValue::kHealth)
           ? SpellType::Damage
           : SpellType::Debuff;

      // --- things done TO an enemy that are not damage ---
      case RE::EffectSetting::Archetype::kParalysis:
      case RE::EffectSetting::Archetype::kStagger:
      case RE::EffectSetting::Archetype::kDisarm:
      case RE::EffectSetting::Archetype::kBanish:
      case RE::EffectSetting::Archetype::kTurnUndead:
      case RE::EffectSetting::Archetype::kCalm:
      case RE::EffectSetting::Archetype::kDemoralize:
      case RE::EffectSetting::Archetype::kFrenzy:
      case RE::EffectSetting::Archetype::kGrabActor:
      case RE::EffectSetting::Archetype::kConcussion:
        return SpellType::Debuff;

      // --- things done FOR yourself or an ally ---
      case RE::EffectSetting::Archetype::kRally:          // courage, call to arms
      case RE::EffectSetting::Archetype::kEnhanceWeapon:  // elemental weapon coatings
      case RE::EffectSetting::Archetype::kInvisibility:
      case RE::EffectSetting::Archetype::kNightEye:
      case RE::EffectSetting::Archetype::kWerewolf:       // Beast Form
      case RE::EffectSetting::Archetype::kVampireLord:
      case RE::EffectSetting::Archetype::kEtherealize:
      case RE::EffectSetting::Archetype::kSlowTime:
      case RE::EffectSetting::Archetype::kDisguise:
        return SpellType::Buff;

      // An elemental cloak is a Destruction damage aura wearing a self-cast
      // effect, and typing it Buff did real harm: the element survives into
      // ContextWeightForCandidate, where Buff + Fire reads as a FIRE RESISTANCE
      // spell, so Flame Cloak was promoted when the player was burning. School
      // is the honest test of what a cloak is for.
      case RE::EffectSetting::Archetype::kCloak:
        return (school == MagicSchool::Destruction) ? SpellType::Damage : SpellType::Buff;

      // Dispel strips magic in whichever direction it is aimed: off yourself it
      // is a cure, onto an enemy it is a debuff.
      case RE::EffectSetting::Archetype::kDispel:
        return harmful ? SpellType::Debuff : SpellType::Healing;

      // --- things that put another body on the field ---
      case RE::EffectSetting::Archetype::kReanimate:
      case RE::EffectSetting::Archetype::kBoundWeapon:
      case RE::EffectSetting::Archetype::kCommandSummoned:
        return SpellType::Summon;

      // --- things that act on the world rather than on a fight ---
      case RE::EffectSetting::Archetype::kLight:
      case RE::EffectSetting::Archetype::kDetectLife:
      case RE::EffectSetting::Archetype::kTelekinesis:
      case RE::EffectSetting::Archetype::kOpen:
      case RE::EffectSetting::Archetype::kLock:
      case RE::EffectSetting::Archetype::kSoulTrap:
      case RE::EffectSetting::Archetype::kGuide:
        return SpellType::Utility;

      // --- undoing a condition ---
      case RE::EffectSetting::Archetype::kCureDisease:
      case RE::EffectSetting::Archetype::kCurePoison:
      case RE::EffectSetting::Archetype::kCureParalysis:
      case RE::EffectSetting::Archetype::kCureAddiction:
        return SpellType::Healing;

      default:
        break;
      }

      // School as a LAST resort, not a first one.
      //
      // These two ran before the switch and stole from it: a hostile Destruction
      // stagger or paralysis spell returned Damage without anyone looking at its
      // archetype (42 such spells in the 2026-09-21 dump). Below the switch they
      // cost nothing -- every case they were written for is now answered above --
      // and they still catch a Destruction or Illusion spell whose archetype has
      // no rule.
      if (harmful && school == MagicSchool::Destruction) {
      return SpellType::Damage;
      }
      if (harmful && school == MagicSchool::Illusion) {
      return SpellType::Debuff;
      }

      return SpellType::Unknown;  // Will fall back to tag-based in ClassifySpell
   }

   SpellType SpellClassifier::DeriveSpellTypeFromName(std::string_view name) noexcept
   {
      if (name.empty()) {
      return SpellType::Unknown;
      }

      // Word-boundary matching throughout (Util::NameContainsWord), because a
      // substring match on words this short is how "Lockpick" would make
      // "Blocking" a utility spell.
      //
      // The list is short on purpose. These are words that name what a spell is
      // FOR and that no combat spell uses for flavour; "fire", "storm", "blood"
      // and their kind are deliberately absent, because a script spell called
      // "Bloodstorm" could be anything and Unknown is the better answer.

      // Acting on the world rather than on a fight.
      if (Util::NameContainsWord(name, "unlock") || Util::NameContainsWord(name, "lockpick") ||
          (Util::NameContainsWord(name, "open") && Util::NameContainsWord(name, "lock")) ||
          Util::NameContainsWord(name, "transmute") || Util::NameContainsWord(name, "telekinesis") ||
          Util::NameContainsWord(name, "clairvoyance") || Util::NameContainsWord(name, "detect") ||
          Util::NameContainsWord(name, "teleport") || Util::NameContainsWord(name, "recall") ||
          Util::NameContainsWord(name, "intervention") || Util::NameContainsWord(name, "soultrap") ||
          (Util::NameContainsWord(name, "soul") && Util::NameContainsWord(name, "trap"))) {
      return SpellType::Utility;
      }

      // Putting a body on the field. "Conjure" and "summon" are unambiguous;
      // "raise" is not (Raise Wall, Raise Shield), so it is paired with what it
      // raises.
      if (Util::NameContainsWord(name, "conjure") || Util::NameContainsWord(name, "summon") ||
          Util::NameContainsWord(name, "reanimate") ||
          (Util::NameContainsWord(name, "raise") && (Util::NameContainsWord(name, "dead") ||
                                                     Util::NameContainsWord(name, "zombie") ||
                                                     Util::NameContainsWord(name, "thrall")))) {
      return SpellType::Summon;
      }

      // Mitigation. "Shield" alone is risky -- Shield Charge is an attack -- so
      // it is paired, while "ward" and "barrier" stand alone.
      if (Util::NameContainsWord(name, "ward") || Util::NameContainsWord(name, "barrier") ||
          (Util::NameContainsWord(name, "shield") && !Util::NameContainsWord(name, "charge"))) {
      return SpellType::Defensive;
      }

      return SpellType::Unknown;
   }

   SpellType SpellClassifier::DeriveSpellTypeFromTags(SpellTag tags, SpellTagExt tagsExt) noexcept
   {
      // Priority order matters - check most specific first

      // Healing
      if (HasTag(tags, SpellTag::RestoreHealth)) return SpellType::Healing;


      // Summon (bound weapons + conjuration summons)
      if (HasTag(tags, SpellTag::BoundWeapon) ||
          HasTag(tags, SpellTag::SummonDaedra) ||
          HasTag(tags, SpellTag::SummonUndead) ||
          HasTag(tags, SpellTag::SummonCreature)) {
      return SpellType::Summon;
      }

      // Defensive (wards, armor)
      if (HasTag(tags, SpellTag::Ward) || HasTag(tags, SpellTag::Armor)) {
      return SpellType::Defensive;
      }

      // Debuff (paralysis, calm, fear, frenzy, turn undead, banish)
      if (HasTag(tags, SpellTag::Paralysis) || HasTag(tags, SpellTag::Calm) ||
          HasTag(tags, SpellTag::Fear) || HasTag(tags, SpellTag::Frenzy) ||
          HasTag(tags, SpellTag::TurnUndead) || HasTag(tags, SpellTag::AntiDaedra)) {
      return SpellType::Debuff;
      }

      // Buff (stealth, invisibility, muffle, waterbreathing)
      if (HasTag(tags, SpellTag::Invisibility) || HasTag(tags, SpellTag::Muffle) ||
          HasTag(tags, SpellTag::Stealth)) {
      return SpellType::Buff;
      }

      // Extended tags (#79). BELOW every primary-role check, deliberately.
      // DetermineSpellTagsExt walks every effect, not just the costliest, so an
      // ext tag is often a RIDER — a ward that also grants waterbreathing, a
      // summon with an unlock effect bolted on. The primary tags describe what
      // the spell is FOR; these describe something it also happens to do, and
      // typing a ward as a Buff because of its rider would be a regression.
      // A pure Open Lock or Waterbreathing spell carries no primary tag at all,
      // so it falls through to here, which is the case these exist to catch.
      //
      // Before SpellTagExt the classifier reached these same types by
      // mislabelling: "open" was tagged Telekinesis and "waterbreath" was
      // tagged Stealth, purely so this function would derive Utility and Buff.
      // The Stealth one was actively wrong — the spell arm reads Stealth into
      // stealthWeight, so waterbreathing spells ranked as sneaking tools.
      if (HasTagExt(tagsExt, SpellTagExt::Unlock)) return SpellType::Utility;
      if (HasTagExt(tagsExt, SpellTagExt::AntiDragon)) return SpellType::Debuff;
      if (HasTagExt(tagsExt, SpellTagExt::SlowFall) ||
          HasTagExt(tagsExt, SpellTagExt::Waterbreathing)) {
      return SpellType::Buff;
      }

      // Utility (detect, light, telekinesis)
      if (HasTag(tags, SpellTag::DetectLife) || HasTag(tags, SpellTag::Light) ||
          HasTag(tags, SpellTag::Telekinesis)) {
      return SpellType::Utility;
      }

      // Damage (elements - check last as it's broad)
      if (HasTag(tags, SpellTag::Fire) || HasTag(tags, SpellTag::Frost) ||
          HasTag(tags, SpellTag::Shock) || HasTag(tags, SpellTag::Poison) ||
          HasTag(tags, SpellTag::Sun)) {
      return SpellType::Damage;
      }

      return SpellType::Unknown;
   }

   ElementType SpellClassifier::DeriveElementFromTags(SpellTag tags) noexcept
   {
      // Check in priority order (Sun first as it's most specific)
      if (HasTag(tags, SpellTag::Sun))    return ElementType::Sun;
      if (HasTag(tags, SpellTag::Fire))   return ElementType::Fire;
      if (HasTag(tags, SpellTag::Frost))  return ElementType::Frost;
      if (HasTag(tags, SpellTag::Shock))  return ElementType::Shock;
      if (HasTag(tags, SpellTag::Poison)) return ElementType::Poison;

      return ElementType::None;
   }

   SpellTag SpellClassifier::DetermineSpellTags(RE::SpellItem* spell) const
   {
      if (!spell) return SpellTag::None;

      SpellTag tags = SpellTag::None;
      const std::string_view name = spell->GetName();

      // M6 (v0.7.21): OPTIMIZATION - Lowercase name ONCE, then use case-sensitive find()
      // This reduces ~50+ tolower() calls per character to just one pass over the string.
      // Original: O(name_len * keyword_len) per keyword × ~50 keywords
      // Optimized: O(name_len) once + O(name_len) per keyword with fast find()
      std::string lowerName;
      lowerName.reserve(name.size());
      for (char c : name) {
      lowerName.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }

      // Fast case-sensitive search on pre-lowercased name
      auto contains = [&lowerName](std::string_view keyword) {
      return lowerName.find(keyword) != std::string::npos;
      };

      // Damage type tags
      if (contains("fire") || contains("flame") ||
          contains("incinerate") || contains("burn")) {
      tags |= SpellTag::Fire;
      }
      if (contains("frost") || contains("ice") ||
          contains("freeze") || contains("blizzard")) {
      tags |= SpellTag::Frost;
      }
      if (contains("shock") || contains("lightning") ||
          contains("thunder") || contains("spark")) {
      tags |= SpellTag::Shock;
      }
      if (contains("poison")) {
      tags |= SpellTag::Poison;
      }
      if (contains("sun") || contains("vampire's bane")) {
      tags |= SpellTag::Sun;
      tags |= SpellTag::AntiUndead;
      }

      // Anti-undead tags
      if (contains("turn undead") || contains("circle of protection") ||
          contains("repel undead")) {
      tags |= SpellTag::AntiUndead;
      tags |= SpellTag::TurnUndead;
      }

      // Anti-daedra tags
      if (contains("banish") || contains("expel daedra")) {
      tags |= SpellTag::AntiDaedra;
      }

      // Range/area tags
      if (contains("bolt") || contains("ball") ||
          contains("spear") || contains("blast")) {
      tags |= SpellTag::Ranged;
      }
      if (contains("touch") || contains("grasp")) {
      tags |= SpellTag::Melee;
      }
      if (contains("ball") || contains("cloak") ||
          contains("storm") || contains("rune") ||
          contains("wall") || contains("circle")) {
      tags |= SpellTag::AOE;
      }

      // Concentration tag
      if (IsConcentration(spell)) {
      tags |= SpellTag::Concentration;
      }

      // Restoration specific tags
      if (contains("heal") || contains("cure") ||
          contains("restore health")) {
      tags |= SpellTag::RestoreHealth;
      }
      if (contains("restore magicka")) {
      tags |= SpellTag::RestoreMagicka;
      }
      if (contains("restore stamina")) {
      tags |= SpellTag::RestoreStamina;
      }
      if (contains("ward")) {
      tags |= SpellTag::Ward;
      }

      // Alteration specific tags
      if (contains("armor") || contains("flesh")) {
      tags |= SpellTag::Armor;
      }
      if (contains("detect life")) {
      tags |= SpellTag::DetectLife;
      }
      // Light detection - exclude "lightning" which contains "light"
      if ((contains("light") && !contains("lightning")) ||
          contains("candlelight") || contains("magelight")) {
      tags |= SpellTag::Light;
      }
      if (contains("telekinesis")) {
      tags |= SpellTag::Telekinesis;
      }
      // Open Lock and Waterbreathing used to be forced in here as Telekinesis
      // and Stealth — not because they are those things, but because that was
      // the only way to reach SpellType::Utility and ::Buff with no bit of
      // their own. Both now carry a real SpellTagExt and derive their type from
      // it. The Stealth one had a live consequence: WeightForCandidate reads
      // Stealth into stealthWeight, so every waterbreathing spell was ranked as
      // a sneaking tool (#79).
      if (contains("paralyze")) {
      tags |= SpellTag::Paralysis;
      }

      // Illusion specific tags
      if (contains("calm") || contains("pacify")) {
      tags |= SpellTag::Calm;
      }
      if (contains("fear") || contains("rout")) {
      tags |= SpellTag::Fear;
      }
      if (contains("frenzy") || contains("fury") ||
          contains("mayhem")) {
      tags |= SpellTag::Frenzy;
      }
      if (contains("invisibility")) {
      tags |= SpellTag::Invisibility;
      tags |= SpellTag::Stealth;
      }
      if (contains("muffle")) {
      tags |= SpellTag::Muffle;
      tags |= SpellTag::Stealth;
      }

      // Conjuration specific tags
      if (contains("summon") || contains("conjure")) {
      tags |= SpellTag::Conjuration;

      // Determine summon type
      if (contains("atronach") || contains("dremora")) {
        tags |= SpellTag::SummonDaedra;
      } else if (contains("zombie") || contains("wraith") ||
                 contains("boneman") || contains("mistman")) {
        tags |= SpellTag::SummonUndead;
      } else {
        tags |= SpellTag::SummonCreature;
      }
      }
      // Whole-word, not a raw substring: "bound" appears inside "Unbound Fire"
      // and "Unbounded Flames/Freezing/Storms", four LoreRim DESTRUCTION scrolls
      // that a find() tags BoundWeapon|Conjuration and so types Summon. Same
      // trap the Unlock arm below already avoids, and for the same reason bare
      // "open" was replaced there.
      if (Util::NameContainsWord(name, "bound")) {
      tags |= SpellTag::BoundWeapon;
      tags |= SpellTag::Conjuration;
      }

      return tags;
   }

   SpellTagExt SpellClassifier::DetermineSpellTagsExt(RE::SpellItem* spell) const
   {
      if (!spell) return SpellTagExt::None;

      SpellTagExt tags = SpellTagExt::None;

      // API first, and over EVERY effect rather than the costliest one. These
      // four are routinely the cheap rider on a multi-effect spell — a modded
      // "Diver's Blessing" that restores stamina AND grants waterbreathing
      // costs most of its magicka on the restore, so the costliest effect
      // would miss the half that matters here.
      for (const auto* effect : spell->effects) {
      if (!effect || !effect->baseEffect) continue;
      const auto* base = effect->baseEffect;

      switch (base->GetArchetype()) {
      case RE::EffectSetting::Archetype::kOpen:
        // The archetype the game itself uses for lock-opening effects. Vanilla
        // ships no player-castable Open spell; mod ones (Apocalypse's Knock,
        // Ordinator's) use this, which is why detection is not name-first.
        tags |= SpellTagExt::Unlock;
        break;
      case RE::EffectSetting::Archetype::kEtherealize:
        // Become Ethereal negates fall damage outright, which is the thing
        // slowFallWeight exists to surface. It is a shout in vanilla and so
        // never reaches the spell registry, but mods rebind it as a spell.
        tags |= SpellTagExt::SlowFall;
        break;
      default:
        break;
      }

      // Waterbreathing has no archetype of its own — it is a plain value
      // modifier on the WaterBreathing actor value, which is exactly how the
      // vanilla Alteration spell is built. This is the one of the four that
      // lights up on an unmodded game.
      //
      // Hostile effects excluded: the actor value says WHICH stat is touched,
      // not in which direction, so a curse that strips waterbreathing reads
      // identically here. waterbreathingWeight surfaces something to CAST when
      // drowning, and offering the player the spell that drowns them is the one
      // outcome worse than offering nothing.
      const bool isHostile = base->data.flags.any(
        RE::EffectSetting::EffectSettingData::Flag::kHostile);
      if (!isHostile && base->data.primaryAV == RE::ActorValue::kWaterBreathing) {
        tags |= SpellTagExt::Waterbreathing;
      }
      }

      // Name fallback, for the parts no API describes.
      const std::string_view name = spell->GetName();

      // AntiDragon is name-ONLY: no archetype, no actor value, and nothing in
      // a spell's data says "this is for dragons". Matched against whole words
      // and against a deliberately short list rather than bare "dragon" —
      // Dragonhide is a vanilla self-armour spell that would sail through a
      // substring test, which is the Quicksilver mistake (#81) one enum over.
      if (Util::NameContainsWord(name, "dragonrend") ||
          Util::NameContainsWord(name, "dragonbane")) {
      tags |= SpellTagExt::AntiDragon;
      }

      // Slow Fall has no vanilla effect at all, so a modded one may be built
      // any which way. Whole-word again: "slow" alone is a debuff spell and
      // must not match. Spaced and unspaced spellings both go through
      // NameContainsWord — a raw find() here would be the only case-SENSITIVE
      // name test in the classifier and would miss "slow fall" in lower case.
      if (Util::NameContainsWord(name, "slowfall") ||
          Util::NameContainsWord(name, "slow fall") ||
          Util::NameContainsWord(name, "featherfall") ||
          Util::NameContainsWord(name, "feather fall")) {
      tags |= SpellTagExt::SlowFall;
      }

      // Unlock keeps a name fallback where the others do not, because the
      // archetype above is not the only way a mod builds one: several drive the
      // unlock through kScript, which describes nothing. "open lock" and
      // "unlock" are whole-word and specific — bare "open" is what this
      // replaced, and it matched anything with "open" in the name.
      if (Util::NameContainsWord(name, "unlock") ||
          Util::NameContainsWord(name, "open lock")) {
      tags |= SpellTagExt::Unlock;
      }

      return tags;
   }

   uint32_t SpellClassifier::GetBaseCost(RE::SpellItem* spell) const
   {
      if (!spell) return 0;

      // Get the base cost from the spell
      // This is unmodified by perks or enchantments
      return static_cast<uint32_t>(spell->CalculateMagickaCost(nullptr));
   }

   bool SpellClassifier::IsConcentration(RE::SpellItem* spell) const
   {
      if (!spell) return false;

      // Check spell casting type
      return spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
   }

   float SpellClassifier::GetEffectiveRange(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const
   {
      if (!spell) return 0.0f;

      // Get delivery type to determine range
      const auto delivery = spell->GetDelivery();

      switch (delivery) {
      case RE::MagicSystem::Delivery::kSelf:
      case RE::MagicSystem::Delivery::kTouch:
      return 0.0f;  // Melee range

      case RE::MagicSystem::Delivery::kAimed:
      case RE::MagicSystem::Delivery::kTargetActor:
      case RE::MagicSystem::Delivery::kTargetLocation:
      {
        // Check for projectile data to get actual range.
        // OPTIMIZATION (v0.7.19): primaryEffect is the caller's pre-computed
        // costliest effect — avoids a redundant GetCostliestEffect() pass.
        if (primaryEffect && primaryEffect->data.projectileBase) {
           // Use projectile range if available
           return primaryEffect->data.projectileBase->data.range;
        }
        // Default long range for aimed spells
        return 4096.0f;  // ~64 meters
      }

      default:
      return 0.0f;
      }
   }

   RE::Effect* SpellClassifier::GetCostliestEffect(RE::SpellItem* spell) const
   {
      if (!spell || spell->effects.empty()) return nullptr;

      RE::Effect* costliestEffect = nullptr;
      RE::Effect* firstValidEffect = nullptr;  // L3 (v0.7.21): Track during main loop
      float highestCost = -1.0f;

      for (auto* effect : spell->effects) {
      if (!effect || !effect->baseEffect) continue;

      // L3 (v0.7.21): Track first valid effect during main loop (eliminates fallback loop)
      if (!firstValidEffect) {
        firstValidEffect = effect;
      }

      // Calculate effect cost using Skyrim's actual formula (v0.7.10)
      // Formula: baseCost × (magnitude^1.1) × durationFactor × areaFactor
      float baseCost = effect->baseEffect->data.baseCost;
      float magnitude = effect->effectItem.magnitude;
      float duration = effect->effectItem.duration;
      float area = effect->effectItem.area;

      // Duration factor: 1 for instant, scales with duration otherwise
      float durationFactor = (duration > 0) ? (duration / 10.0f) : 1.0f;

      // Area factor: 1 for single-target, 0.15 × area for AoE spells
      float areaFactor = (area > 0) ? (0.15f * area) : 1.0f;

      // Magnitude uses exponential scaling (1.1 exponent)
      float magnitudeFactor = std::pow(std::max(1.0f, magnitude), 1.1f);

      float cost = baseCost * magnitudeFactor * durationFactor * areaFactor;

      if (cost > highestCost) {
        highestCost = cost;
        costliestEffect = effect;
      }
      }

      // Return costliest if found, otherwise first valid effect (safety fallback)
      return costliestEffect ? costliestEffect : firstValidEffect;
   }

   RE::Effect* SpellClassifier::GetCostliestNonScriptEffect(RE::SpellItem* spell) const
   {
      if (!spell || spell->effects.empty()) return nullptr;

      RE::Effect* best = nullptr;
      float highestCost = -1.0f;

      for (auto* effect : spell->effects) {
      if (!effect || !effect->baseEffect) continue;
      if (effect->baseEffect->GetArchetype() == RE::EffectSetting::Archetype::kScript) {
        continue;
      }

      // Same cost formula as GetCostliestEffect; kept here rather than shared
      // because that one carries a first-valid-effect fallback this must NOT
      // have -- "no readable effect" is a real answer and the caller depends on
      // getting nullptr for it.
      const float baseCost = effect->baseEffect->data.baseCost;
      const float magnitude = effect->effectItem.magnitude;
      const float duration = effect->effectItem.duration;
      const float area = effect->effectItem.area;

      const float durationFactor = (duration > 0) ? (duration / 10.0f) : 1.0f;
      const float areaFactor = (area > 0) ? (0.15f * area) : 1.0f;
      const float magnitudeFactor = std::pow(std::max(1.0f, magnitude), 1.1f);

      const float cost = baseCost * magnitudeFactor * durationFactor * areaFactor;
      if (cost > highestCost) {
        highestCost = cost;
        best = effect;
      }
      }

      return best;
   }

   MagicSchool SpellClassifier::DetermineMagicSchool(RE::Effect* costliestEffect) const
   {
      // OPTIMIZATION (v0.7.19): costliestEffect is pre-computed by caller
      if (!costliestEffect || !costliestEffect->baseEffect) {
      return MagicSchool::Unknown;
      }

      // Use SKSE API: GetMagickSkill() returns the ActorValue for the spell's school
      auto skillAV = costliestEffect->baseEffect->GetMagickSkill();

      switch (skillAV) {
      case RE::ActorValue::kDestruction:  return MagicSchool::Destruction;
      case RE::ActorValue::kRestoration:  return MagicSchool::Restoration;
      case RE::ActorValue::kAlteration:   return MagicSchool::Alteration;
      case RE::ActorValue::kIllusion:     return MagicSchool::Illusion;
      case RE::ActorValue::kConjuration:  return MagicSchool::Conjuration;
      default:                            return MagicSchool::Unknown;
      }
   }

   ElementType SpellClassifier::DetermineElementType(RE::Effect* costliestEffect) const
   {
      // ONLY API-based checks here
      // Name-based fallback handled by DeriveElementFromTags() in ClassifySpell()
      // OPTIMIZATION (v0.7.19): costliestEffect is pre-computed by caller

      if (!costliestEffect || !costliestEffect->baseEffect) {
      return ElementType::None;
      }

      auto resistAV = costliestEffect->baseEffect->data.resistVariable;

      switch (resistAV) {
      case RE::ActorValue::kResistFire:   return ElementType::Fire;
      case RE::ActorValue::kResistFrost:  return ElementType::Frost;
      case RE::ActorValue::kResistShock:  return ElementType::Shock;
      case RE::ActorValue::kPoisonResist: return ElementType::Poison;
      case RE::ActorValue::kResistMagic:  return ElementType::Magic;
      default:                            return ElementType::None;
      }
   }
}

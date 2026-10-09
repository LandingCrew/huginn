#include "EffectRules.h"

#include "core/MiniRegex.h"

#include <array>
#include <cctype>
#include <charconv>
#include <initializer_list>
#include <vector>

namespace Huginn::Core::Effect
{
    namespace
    {
        // =====================================================================
        // One-off patterns (also listed by RulePatterns() for the regex oracle)
        // =====================================================================
        constexpr const char* kPatSuspectAv =
            R"re(SkillAdvance$|^(Fame|Infamy|Mood|Morality|Variable\d\d|VoicePoints|Energy|Assistance|Blindness|Confidence|Aggression)$)re";
        constexpr const char* kPatVitalWord = R"re(health|magicka|stamina|heal)re";
        constexpr const char* kPatNegation = R"re(\b(reduc|lower|weaken|decreas)\w*)re";
        constexpr const char* kPatHealthWord = R"re(\bhealth\b)re";
        constexpr const char* kPatMagickaWord = R"re(\bmagicka\b)re";
        constexpr const char* kPatStaminaWord = R"re(\bstamina\b)re";
        constexpr const char* kPatWeakerDamage = R"re((reduc|lower|weaken)\w* .{0,30}damage)re";
        constexpr const char* kPatSurvivalKeyword = R"re(Hunger|Warmth|CCSM)re";
        constexpr const char* kPatRallyCarried = R"re(silence|paraly|command|bend will|calm|banish|frenzy|fear)re";
        constexpr const char* kPatBadItemName =
            R"re(\b(dummy|test|testing|donotuse|do not use|unused|placeholder|deleted)\b|^zz|donotuse|takeme)re";
        constexpr const char* kPatResistWord = R"re(\bresist)re";

        // =====================================================================
        // Small helpers
        // =====================================================================
        bool StartsWith(std::string_view s, std::string_view p) noexcept { return s.substr(0, p.size()) == p; }
        bool EndsWith(std::string_view s, std::string_view p) noexcept
        {
            return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
        }
        bool Contains(std::string_view s, std::string_view p) noexcept { return s.find(p) != std::string_view::npos; }

        std::string JoinKeywords(const std::vector<std::string>& kws)
        {
            std::string out;
            for (const auto& k : kws) {
                if (!out.empty()) out += ';';
                out += k;
            }
            return out;
        }

        struct Rule
        {
            MiniRegex re;
            std::string spec;
        };
        using Table = std::vector<Rule>;

        Table Build(std::initializer_list<std::pair<const char*, const char*>> rows)
        {
            Table t;
            t.reserve(rows.size());
            for (const auto& [p, s] : rows) t.push_back(Rule{ MiniRegex(p), s });
            return t;
        }

        // =====================================================================
        // Actor-value vocabularies
        // =====================================================================
        const char* Vital(std::string_view av) noexcept
        {
            if (av == "Health") return "health";
            if (av == "Magicka") return "magicka";
            if (av == "Stamina") return "stamina";
            return nullptr;
        }
        const char* Rate(std::string_view av) noexcept
        {
            if (av == "HealRate" || av == "HealRateMult" || av == "CombatHealthRegenMult" ||
                av == "CombatHealthRegenMultiply")
                return "health";
            if (av == "MagickaRate" || av == "MagickaRateMult") return "magicka";
            if (av == "StaminaRate" || av == "StaminaRateMult") return "stamina";
            return nullptr;
        }
        const char* Res(std::string_view av) noexcept
        {
            if (av == "FireResist") return "fire";
            if (av == "FrostResist") return "frost";
            if (av == "ElectricResist") return "shock";
            if (av == "PoisonResist") return "poison";
            if (av == "MagicResist") return "magic";
            if (av == "DiseaseResist") return "disease";
            return nullptr;
        }
        const char* CombatAv(std::string_view av) noexcept
        {
            if (av == "AttackDamageMult") return "attack_damage";
            if (av == "WeaponSpeedMult" || av == "LeftWeaponSpeedMult" || av == "BowSpeedBonus") return "weapon_speed";
            if (av == "UnarmedDamage") return "unarmed";
            if (av == "CritChance") return "crit";
            if (av == "PowerAttackStamina") return "power_attack";
            return nullptr;
        }

        /// One canonical skill per X, XMod, XPowerMod (never XSkillAdvance).
        const char* CanonSkill(std::string_view avIn)
        {
            std::string av(avIn);
            if (const auto p = av.find("PickPocket"); p != std::string::npos) av.replace(p, 10, "Pickpocket");
            static constexpr std::array<std::pair<std::string_view, const char*>, 18> kSkills{ {
                { "OneHanded", "one_handed" }, { "TwoHanded", "two_handed" }, { "Marksman", "archery" },
                { "Block", "block" }, { "Smithing", "smithing" }, { "HeavyArmor", "heavy_armor" },
                { "LightArmor", "light_armor" }, { "Pickpocket", "pickpocket" }, { "Lockpicking", "lockpicking" },
                { "Sneak", "sneak" }, { "Alchemy", "alchemy" }, { "Speechcraft", "speech" },
                { "Alteration", "alteration" }, { "Conjuration", "conjuration" }, { "Destruction", "destruction" },
                { "Illusion", "illusion" }, { "Restoration", "restoration" }, { "Enchanting", "enchanting" },
            } };
            for (const auto& [k, v] : kSkills) {
                if (av == k) return v;
                if (av.size() == k.size() + 3 && StartsWith(av, k) && EndsWith(av, "Mod")) return v;
                if (av.size() == k.size() + 8 && StartsWith(av, k) && EndsWith(av, "PowerMod")) return v;
            }
            return nullptr;
        }

        bool IsValueModifier(int a) noexcept
        {
            return a == kArchValueModifier || a == kArchPeakValueModifier || a == kArchDualValueModifier ||
                   a == kArchValueAndParts || a == kArchAccumulateMagnitude;
        }

        /// Actor values mods reuse: never trusted on their own.
        bool SuspectAv(std::string_view av)
        {
            static const MiniRegex re(kPatSuspectAv);
            return re.ContainsOrBudget(av.empty() ? std::string_view("-") : av);
        }

        // =====================================================================
        // Element axis: keyword first, then the resisted actor value, then
        // (scripted effects only) the name. Unresisted damage is magic.
        // =====================================================================
        const Table& KwElement()
        {
            static const Table t = Build({
                { R"re(MagicDamageFire|LoreBox_LorerimFire)re", "fire" },
                { R"re(MagicDamageFrost|LoreBox_LorerimIce)re", "frost" },
                { R"re(MagicDamageShock|LoreBox_LorerimShock)re", "shock" },
                { R"re(MagicDamageSun|REQ_SunDamage|LoreBox_LorerimSon)re", "sun" },
                { R"re(MagicDamagePoison|LoreBox_LorerimPoison|MagicDamageMiasma)re", "poison" },
                { R"re(MagicDamageBleed)re", "physical" },
                { R"re(Destruction_Arcane|Destruction_Entropic|MagicDamageSonic|Perk_EntropicFocus)re", "magic" },
            });
            return t;
        }
        const Table& NameElement()
        {
            static const Table t = Build({
                { R"re(\bfire|flame|burn|scorch|blaz|inferno|incinerat)re", "fire" },
                { R"re(frost|\bice\b|\bicy\b|freez|blizzard|cold)re", "frost" },
                { R"re(shock|lightning|spark|thunder|storm)re", "shock" },
                { R"re(\bsun\b|holy|radiant|celestial)re", "sun" },
                { R"re(poison|venom|toxic)re", "poison" },
            });
            return t;
        }

        std::string Element(const MagicEffectRecord& r, std::string_view kwJoined, bool useName)
        {
            for (const auto& rule : KwElement()) {
                if (rule.re.Contains(kwJoined)) return rule.spec;
            }
            if (const char* e = Res(r.resistAV)) return e;
            if (r.resistAV == "DamageResist") return "physical";
            if (useName) {
                const std::string n = Lower(r.name);
                for (const auto& rule : NameElement()) {
                    if (rule.re.Contains(n)) return rule.spec;
                }
            }
            return "magic";
        }

        // =====================================================================
        // Keyword table (editor IDs, matched as written against the ';'-joined
        // list). Order matters; first hit wins.
        // =====================================================================
        const Table& KwTable()
        {
            static const Table t = Build({
                { R"re(CCSM_RestoreHunger|StarfrostHunger|Survival_.*Hunger)re", "survival_hunger" },
                { R"re(CCSM_RestoreCold|Survival_MagicAlchFortifyWarmth|\bWarmth\b|FrostfallWarmth)re", "survival_warmth" },
                { R"re(Thirst|Hydrat)re", "survival_thirst" },
                { R"re(MAG_MagicEnchBurden|MagicSlow\b)re", "control_slow" },
                { R"re(MAG_MagicAlchDamageWeapon)re", "control_damage_weapon" },
                { R"re(FortifyPowerAttacks)re", "fortify_combat_power_attack" },
                { R"re(FortifySneakAttacks)re", "fortify_combat_sneak_attack" },
                { R"re(FortifyBash)re", "fortify_combat_bash" },
                { R"re(FortifyUnarmed)re", "fortify_combat_unarmed" },
                { R"re(ResistArrowDamage)re", "defense_resist_ranged" },
                { R"re(ResistPowerAttackDamage)re", "defense_resist_power_attack" },
                { R"re(ResistStagger)re", "defense_resist_stagger" },
                { R"re(ResistSpellDamage)re", "resist_magic" },
                { R"re(MagicEnchFortifyShouts|MagicAlchFortifyShouts)re", "shout_recovery" },
                { R"re(MagicShoutDragonrend)re", "shout_dragonrend" },
                { R"re(MagicEnchFortifyAlchemy)re", "fortify_skill_alchemy" },
                { R"re(MagicEnchFortifyIllusion)re", "fortify_skill_illusion" },
                { R"re(MagicEnchFortifyRestoration)re", "fortify_skill_restoration" },
                { R"re(MagicEnchFortifyPickPocket|MagicAlchFortifyLockpicking)re", "fortify_skill_lockpicking" },
                { R"re(MagicInfluenceFear|Illusion_Nightmare|Illusion_Death)re", "influence_fear" },
                { R"re(MagicInfluenceFrenzy)re", "influence_frenzy" },
                { R"re(MagicInfluenceCharm|MagicInfluenceCalm)re", "influence_calm" },
                { R"re(Illusion_Command)re", "influence_command" },
                { R"re(Illusion_Sleep)re", "control_sleep" },
                { R"re(Illusion_Silence)re", "control_silence" },
                { R"re(Illusion_Blind)re", "control_blind" },
                { R"re(Illusion_Muffle|Illusion_Noise|Illusion_Sound)re", "stealth_muffle" },
                { R"re(Illusion_ShadowStride)re", "stealth_invisibility" },
                { R"re(Illusion_Pain)re", "damage_health" },
                { R"re(MagicParalysis)re", "control_paralysis" },
                { R"re(MagicTurnUndead)re", "influence_turn_undead" },
                { R"re(SoulTrap)re", "soul_trap" },
                { R"re(MagicInvisibility)re", "stealth_invisibility" },
                { R"re(MagicNightEye)re", "vision_night_eye" },
                { R"re(MagicWard\b)re", "defense_ward" },
                { R"re(MagicArmorSpell)re", "defense_armor" },
                { R"re(FortifyAttributes)re", "fortify_vital_health" },
                { R"re(MagicRestoreHealth|MagicAlchRestoreHealth)re", "restore_health" },
                { R"re(MagicAlchRestoreMagicka|MagicRestoreMagicka)re", "restore_magicka" },
                { R"re(MagicAlchRestoreStamina|MagicRestoreStamina)re", "restore_stamina" },
                { R"re(MagicSummon)re", "summon_creature" },
                { R"re(MagicDamage(Fire|Frost|Shock|Sun|Poison)|REQ_SunDamage|Destruction_(Arcane|Entropic)|MagicDamageSonic)re", "damage_health" },
                { R"re(MagicBlessing|PilgrimShrineBlessing)re", "meta_xp_gain" },
            });
            return t;
        }

        // =====================================================================
        // Name table (English; lower-cased input). The _X specs are resolved
        // from the match in NameSpec.
        // =====================================================================
        const Table& NameTable()
        {
            static const Table t = Build({
                { R"re(fortify (health|magicka|stamina) rege?nerat|rege?nerat|fortify heal rate)re", "regen_X" },
                { R"re(^fortify (health|magicka|stamina)\b)re", "fortify_vital_X" },
                { R"re(armou?r penetration)re", "fortify_combat_armor_penetration" },
                { R"re(no fall(ing)? damage|feather ?fall|slow ?fall)re", "movement_jump_fall" },
                { R"re(fortify (melee |attack )?damage|increased damage|increase \w+ damage)re", "fortify_combat_attack_damage" },
                { R"re(bound (shield|arrows|bolts))re", "summon_bound_weapon" },
                { R"re(damage weapon)re", "weaken_combat_attack_damage" },
                { R"re(^fortify (one-handed|two-handed|archery|marksman|block|smithing|heavy armor|light armor|pickpocket|lockpicking|sneak|alchemy|barter|speech|alteration|conjuration|destruction|illusion|restoration|enchanting))re", "fortify_skill_X" },
                { R"re(soul trap|siphon soul|soul tear trap)re", "soul_trap" },
                { R"re(restore thirst|hydrated|\bthirst\b)re", "survival_thirst" },
                { R"re(hunger)re", "survival_hunger" },
                { R"re(warmth|warming|restore cold|\bwarm\b)re", "survival_warmth" },
                { R"re(fatigue|well rested|rested)re", "survival_fatigue" },
                { R"re(drunk|drugged|inebri|alcohol|skooma)re", "survival_intoxication" },
                { R"re(cure disease|cure vampirism|beastblood)re", "cure_disease" },
                { R"re(cure poison)re", "cure_poison" },
                { R"re(cure injur)re", "cure_injury" },
                { R"re(dispel soul gems)re", "NONE" },
                { R"re(\bdispel\b)re", "cure_dispel" },
                { R"re(restore health|heal(ing)?\b|regain .*health)re", "restore_health" },
                { R"re(restore magicka)re", "restore_magicka" },
                { R"re(restore stamina)re", "restore_stamina" },
                { R"re(^fear|nightmare|terror|hysteria)re", "influence_fear" },
                { R"re(^frenzy|strife|fury\b|mayhem)re", "influence_frenzy" },
                { R"re(^calm|pacify|charm)re", "influence_calm" },
                { R"re(command|mind control|enslave|obedience|bend will|possession)re", "influence_command" },
                { R"re(courage|rally)re", "influence_rally" },
                { R"re(turn (greater |lesser )?undead|repel)re", "influence_turn_undead" },
                { R"re(banish)re", "influence_banish" },
                { R"re(paraly|deep freeze|petrif)re", "control_paralysis" },
                { R"re(knockdown|stagger|overthrow|unrelenting force|\bpush\b|repulse)re", "control_stagger" },
                { R"re(disarm)re", "control_disarm" },
                { R"re(\bsleep)re", "control_sleep" },
                { R"re(silence)re", "control_silence" },
                { R"re(\bblind)re", "control_blind" },
                { R"re(^slow\b|\bslow(s|ed)?\b(?! time| fall)|burden|ice form)re", "control_slow" },
                { R"re(telekinetic grab|grip|grasp)re", "control_grab" },
                { R"re(invisib|vanish|shadow stride|chameleon)re", "stealth_invisibility" },
                { R"re(muffl|dampen|\bnoise\b|quiet)re", "stealth_muffle" },
                { R"re(night eye|cat.?s eye)re", "vision_night_eye" },
                { R"re(detect (life|dead|all)|aura whisper|animal senses)re", "vision_detect_life" },
                { R"re(clairvoyance)re", "vision_clairvoyance" },
                { R"re(candlelight|magelight|\blight\b|lantern|torch)re", "vision_light" },
                { R"re(feather ?fall|slow fall|levitat|jump)re", "movement_jump_fall" },
                { R"re(water ?walk)re", "movement_water_walking" },
                { R"re(water ?breath)re", "utility_water_breathing" },
                { R"re(sprint|whirlwind|speed|swift|haste)re", "movement_speed" },
                { R"re(teleport|portal|recall|mark\b|divine intervention|gates)re", "utility_teleport" },
                { R"re(unlock|open lock|knock)re", "utility_unlock" },
                { R"re(telekin)re", "utility_telekinesis" },
                { R"re(become ethereal|ethereal|etherealize)re", "defense_ethereal" },
                { R"re(slow time|time slows)re", "utility_slow_time" },
                { R"re(enlarge|shrink|size)re", "utility_size" },
                { R"re(transmut)re", "utility_transmute" },
                { R"re(carry weight|feather\b)re", "utility_carry_weight" },
                { R"re(conjure|summon|call (dragon|of valor)|bound (sword|bow|dagger|battleaxe|shield)|reanimat|raise (zombie|dead)|dead thrall)re", "summon_creature" },
                { R"re(ward\b)re", "defense_ward" },
                { R"re(reflect)re", "defense_reflect" },
                { R"re(absorb spell|spell absorption)re", "defense_spell_absorb" },
                { R"re((oak|stone|iron|ebony|dragon)hide|(oak|stone|iron|ebony|dragon)flesh|armor rating|mage armor)re", "defense_armor" },
                { R"re(absorb health|drain health|vampiric)re", "absorb_health" },
                { R"re(absorb magicka|drain magicka)re", "absorb_magicka" },
                { R"re(absorb stamina|drain stamina)re", "absorb_stamina" },
                { R"re(fortify shout|shout (power|duration)|unlimited shouts|restore shout)re", "shout_recovery" },
                { R"re(dragonrend)re", "shout_dragonrend" },
                { R"re([a-z] polymorph|shapeshift: )re", "stealth_disguise" },
                { R"re(vampire form|vampire lord|lich)re", "transform_vampire_lord" },
                { R"re(beast form|werewolf)re", "transform_werewolf" },
                { R"re(experience|\bxp\b|blessing)re", "meta_xp_gain" },
                { R"re(fortify potion duration)re", "meta_potion_duration" },
                { R"re(magicka damage|damage magicka)re", "damage_magicka" },
                { R"re(stamina damage|damage stamina)re", "damage_stamina" },
                { R"re(disintegrat|damage health|pain|\bdamage\b|fireball|flame|frost|\bice\b|shock|lightning|thunder|storm|sun|holy|poison|bolt|blast|explos|meteor|spear|ray)re", "damage_health*" },
            });
            return t;
        }

        const char* SkillWord(std::string_view w) noexcept
        {
            static constexpr std::array<std::pair<std::string_view, const char*>, 20> kWords{ {
                { "one-handed", "one_handed" }, { "two-handed", "two_handed" }, { "archery", "archery" },
                { "marksman", "archery" }, { "block", "block" }, { "smithing", "smithing" },
                { "heavy armor", "heavy_armor" }, { "light armor", "light_armor" }, { "pickpocket", "pickpocket" },
                { "lockpicking", "lockpicking" }, { "sneak", "sneak" }, { "alchemy", "alchemy" },
                { "barter", "speech" }, { "speech", "speech" }, { "alteration", "alteration" },
                { "conjuration", "conjuration" }, { "destruction", "destruction" }, { "illusion", "illusion" },
                { "restoration", "restoration" }, { "enchanting", "enchanting" },
            } };
            for (const auto& [k, v] : kWords) {
                if (k == w) return v;
            }
            return nullptr;
        }

        /// Name-table lookup with the _X placeholders resolved; "" if none.
        std::string NameSpec(std::string_view name, bool* catchAll = nullptr)
        {
            if (catchAll) *catchAll = false;
            const std::string ln = Lower(name);
            for (const auto& rule : NameTable()) {
                MiniRegex::Match m;
                if (!rule.re.Search(ln, &m)) continue;
                if (rule.spec == "regen_X") {
                    static const MiniRegex vit(kPatVitalWord);
                    MiniRegex::Match vm;
                    if (vit.Search(ln, &vm)) {
                        const auto w = vm.Group(ln, 0);
                        return "regen_" + std::string(w == "heal" ? std::string_view("health") : w);
                    }
                    return "regen_health";
                }
                if (rule.spec == "fortify_vital_X") return "fortify_vital_" + std::string(m.Group(ln, 1));
                if (rule.spec == "fortify_skill_X") {
                    const char* sk = SkillWord(m.Group(ln, 1));
                    return sk ? std::string("fortify_skill_") + sk : std::string{};
                }
                if (rule.spec == "NONE") return {};
                if (rule.spec == "damage_health*") {
                    // The generic rule: any name with "damage", "frost", "bolt"...
                    if (catchAll) *catchAll = true;
                    return "damage_health";
                }
                return rule.spec;
            }
            return {};
        }

        const MiniRegex& HelperRe()
        {
            static const MiniRegex re(
                R"re(dummy|corrector|visual|\bfx\b|screen ?shake|cooldown|script ai|fake script|empty cloak|^ai$|)re"
                R"re(^blank( effect)?$|null effect|^mad|not user facing|\(hidden\)|description|display effect|priority|placeholder|)re"
                R"re(perk bonus|perk impact|invisible \d|^$|stagger push|stagger area|sound fx|light toggle|marker|tracker|)re"
                R"re(count(er)?\b|playing music|play (lute|flute|drum)|staff enchantment|master of the mind|)re"
                R"re(dispel (cloak|size|jump)|dispel effect|\w dispel$)re");
            return re;
        }

        // =====================================================================
        // Description route (normalised: lower case, <tokens> and numbers -> N)
        // =====================================================================
        const Table& DescTable()
        {
            static const Table t = Build({
                { R"re(soul ?trap|fills? (a |the )?soul ?gems?|vulnerable to soul ?gems?|soul ?gems? on death|trap(s|ped)? (its|the|their) soul)re", "soul_trap" },
                { R"re(teleport|portal|fast travel|transports? (you|the caster)|recall to|swaps? places|shifts? through (the )?shadows)re", "utility_teleport" },
                { R"re(opening locks|opens? (a |the )?locks?\b|\bunlock)re", "utility_unlock" },
                { R"re(time slows|slows? (down )?time|slow time|world (around you )?(seems to )?slow)re", "utility_slow_time" },
                { R"re(carry(ing)? (weight|capacity))re", "utility_carry_weight" },
                { R"re(breathe? underwater|water ?breathing)re", "utility_water_breathing" },
                { R"re(walk on water|water ?walking)re", "movement_water_walking" },
                { R"re(excavat\w* ores?|transmut\w* (one|ore|iron|silver|ingredient))re", "utility_transmute" },
                { R"re((caster.s |your |target.s )size\b|\benlarg|\bshrink)re", "utility_size" },
                { R"re(cures? (all )?(diseases?|vampirism))re", "cure_disease" },
                { R"re(cures? (all )?poison)re", "cure_poison" },
                { R"re(^dispels?\b|removes? (all )?(magic(al)?|spell) effects|dispels? (all )?(cloak|armor|magic))re", "cure_dispel" },
                { R"re((tosses|spawns|throws|drops) (a|an) .{0,40}spider)re", "summon_creature" },
                { R"re(temporary damage)re", "drain_vital_health" },
                { R"re(imbues? .{0,30}weapons? with)re", "damage_health" },
                { R"re(\b(deal|deals|dealing|does|inflicts?|inflicting)\b.{0,40}(?<!more )(?<!less )(?<!extra )(?<!additional )(?<!double )damage(?! taken)|\bdamage to (the target.s )?(health|magicka|stamina))re", "DMG" },
                { R"re(paraly|immobili[sz]|in place\b|frozen solid|suspended in|petrif)re", "control_paralysis" },
                { R"re(disarm)re", "control_disarm" },
                { R"re(silenc)re", "control_silence" },
                { R"re(\bblind)re", "control_blind" },
                { R"re(\bput(s)? .{0,20}to sleep|\bsleep\b)re", "control_sleep" },
                { R"re(\bflee|\bfear|terrif|cower)re", "influence_fear" },
                { R"re(\bcalm|pacif|(will not|won.t|cannot|stop) (fight|attack)|stop and resume)re", "influence_calm" },
                { R"re(frenz|attack (anything|anyone|each other|their allies)|fight each other|turn on their)re", "influence_frenzy" },
                { R"re(summon (a|an) (?!(physical )?wall))re", "summon_creature" },
                { R"re(bends? the will|\bcommand\b|control (an?|the|target)\b|enslave|thrall|\bobey|fight for you)re", "influence_command" },
                { R"re(courage|\brally)re", "influence_rally" },
                { R"re(banish)re", "influence_banish" },
                { R"re(stagger|knock(s|ing|ed)? (them |enemies |targets |all targets )?(down|back)|knockdown|knockback|send(s|ing)? .{0,20}flying|launch(es)? (enemies|targets|them)|push(es|ing)? (away|back)|lose (its|their) balance|flung (away|back))re", "control_stagger" },
                { R"re(\bslow(s|ed|ing)?\b(?! time)|movement speed (is )?reduc|reduc\w* (its |their |the target.s )?movement speed|hinder)re", "control_slow" },
                { R"re(invisib|unseen|become(s)? (hidden|unseen)|cannot detect sneaking)re", "stealth_invisibility" },
                { R"re(movement noise|quieter|muffl|silent(ly)? (move|step)|\blures?\b|distract)re", "stealth_muffle" },
                { R"re(sneak attacks?)re", "fortify_combat_sneak_attack" },
                { R"re(sneak(ing)? (is |effectiveness))re", "fortify_skill_sneak" },
                { R"re(\blocate|lead you to the nearest|guides? you|survey the area|nearest .{0,20}(ore|door|container))re", "vision_clairvoyance" },
                { R"re(night ?eye|see (better )?in the dark|darkvision)re", "vision_night_eye" },
                { R"re(detect (life|dead)|seen through walls|see (through walls|enemies|nearby|living)|\breveal)re", "vision_detect_life" },
                { R"re(hovering light|create(s)? (a )?(ball of )?light\b|illuminat)re", "vision_light" },
                { R"re(fall(ing)? damage|damage when falling|when falling|feather ?fall|levitat|\bfloat|thin air|jump(s|ing)? (higher|further))re", "movement_jump_fall" },
                { R"re((move|moving|run|running|sprint\w*)\b.{0,25}faster|movement speed (is )?increas|increases? movement speed|\bdash\b|haste)re", "movement_speed" },
                { R"re(telekine)re", "utility_telekinesis" },
                { R"re(reanimat|raises? (a |the |up to N )?(dead|corpse|zombie)|brings? a dead)re", "summon_reanimate" },
                { R"re(\binfuse bound|binds? .{0,40}to a summoned)re", "NONE" },
                { R"re(bound (weapon|sword|bow|dagger|axe|quiver|armor|shield)|magic quiver|binds? (a|an) (daedric|bound|quiver|shield|(sword|bow|dagger|axe|mace|shield|arrow|bolt|quiver)\w*-shaped))re", "summon_bound_weapon" },
                { R"re(summon(?! a (physical )?wall)|conjur|manifest|illusions? of|\bclone\b|ghost to attack)re", "summon_creature" },
                { R"re(absorb (N% of )?(hostile )?spells|spell absorption|absorb N% of the magicka from incoming)re", "defense_spell_absorb" },
                { R"re(reflect)re", "defense_reflect" },
                { R"re(\bwards?\b)re", "defense_ward" },
                { R"re((reduc|lower)\w* (its |their |the target.s |enemy |target.s )?armor\b|armor (rating )?(is )?reduced)re", "weakness_armor" },
                { R"re(^take(s)? (double|twice the|N% more|more) damage)re", "TAKEMORE" },
                { R"re((avoid|resist|reduc\w*|ignore\w*) (all )?damage from (ranged|arrows|projectiles)|ranged (attacks?|weapons?) (deal|do) (N% )?less)re", "defense_resist_ranged" },
                { R"re(armor rating|\bdefense\b|damage reduction|reduc\w* (all |incoming |physical )?damage taken|take(s)? (only )?(N% )?(half|less) (physical )?damage|half damage|invulner|ignores? N% of (all )?(physical )?damage|chance to (take no|avoid) damage)re", "defense_armor" },
                { R"re(magic resist\w*|resist\w* (to )?magic)re", "MAGRES" },
                { R"re(weak(er|ness)? to (fire|frost|shock|poison)|(fire|frost|shock|poison) resist\w* (is |by )?(reduc|lower|decreas))re", "WEAK" },
                { R"re(resist (to )?(fire|frost|shock|poison)|(fire|frost|shock|poison) resist)re", "RES" },
                { R"re(healing (effects )?(are |is )?(reduced|halved)|reduc\w* (all )?healing)re", "NONE" },
                { R"re(reduction of N points to (health|stamina|magicka))re", "drain_vital_V" },
                { R"re(absorb\w*\b.{0,30}\b(health|magicka|stamina)|(health|magicka|stamina).{0,15}absorbed|steals? .{0,20}(health|magicka|stamina))re", "absorb_V" },
                { R"re((regenerat\w*|regen)\b.{0,20}(health|magicka|stamina)|(health|magicka|stamina) regen)re", "regen_V" },
                { R"re(\b(restor\w*|replenish\w*|regain\w*|heal|heals|healed|healing)\b.{0,40}(health|magicka|stamina)|\bheals? (you|the caster|N|by)|\bheal(s|ing)?\b)re", "restore_V" },
                { R"re((increase\w*|fortif\w*) (your )?(maximum )?(health|magicka|stamina)|(health|magicka|stamina) (is )?increased)re", "fortify_vital_V" },
                { R"re(critical (hit )?(chance|damage))re", "fortify_combat_crit" },
                { R"re(power attack\w* (stamina )?cost|power attacks? (will )?(deal|do))re", "fortify_combat_power_attack" },
                { R"re(armor penetration)re", "fortify_combat_armor_penetration" },
                { R"re((attack|swing)\w*\b.{0,20}(faster|twice as fast)|attack speed|weapon speed)re", "SPEED" },
                { R"re(prices? (are|is) (N% )?better|buying and selling)re", "fortify_skill_speech" },
                { R"re(\bspells? (do|deal|are) (N% )?(more|stronger|less)|spell ?power|spells? (are|is) N% (stronger|more powerful))re", "NONE" },
                { R"re(\b(one-handed|two-handed|archery|marksman|block(?:ing)?|smithing|heavy armor|light armor|pickpocket(?:ing)?|lockpick(?:ing)?|alchemy|speech|alteration|conjuration|destruction|illusion|restoration|enchanting) (spells )?(cost|are|is|do|deal))re", "SKILL" },
                { R"re((damage|drain)\w*\b.{0,40}\bmagicka\b(?!.{0,40}health)|magicka damage)re", "damage_magicka" },
                { R"re((damage|drain)\w*\b.{0,40}\bstamina\b(?!.{0,40}health)|stamina damage)re", "damage_stamina" },
                { R"re((more|extra|increased|additional|double) (attack |melee |physical |weapon )?damage|attack damage|damage (done )?(is )?increased|increases? damage)re", "ATKDMG" },
                { R"re(\b(deal|deals|dealing|does|do|inflicts?|causing|cause)\b.{0,40}damage|damage (to|per second|each|equal)|\bN damage\b|\bkills?\b|destroys? (all|living)|burn(s|ing)? (those|the target|enemies)|comets?|slash wave|skewer|bleed)re", "damage_health" },
            });
            return t;
        }

        const Table& DescElement()
        {
            static const Table t = Build({
                { R"re(\bfire\b|flame|burn|scorch|blaz)re", "fire" },
                { R"re(frost|\bice\b|\bcold\b|freez|hail|blizzard)re", "frost" },
                { R"re(shock|lightning|electric|spark)re", "shock" },
                { R"re(\bsun\b|sunlight|holy)re", "sun" },
                { R"re(poison|venom|toxic|disease)re", "poison" },
                { R"re(physical|bleed|slash|spike)re", "physical" },
            });
            return t;
        }

        bool IsWord(unsigned char c) noexcept { return std::isalnum(c) != 0 || c == '_'; }

        /// lower case; <...> -> N; a number at a word start -> N (as the
        /// reference does, in that order).
        std::string NormaliseDescription(std::string_view text)
        {
            std::string low = Lower(text);
            for (auto& c : low) {
                if (c == '\r' || c == '\n') c = ' ';
            }
            std::string a;
            a.reserve(low.size());
            for (std::size_t i = 0; i < low.size(); ++i) {
                if (low[i] == '<') {
                    const auto close = low.find('>', i + 1);
                    if (close != std::string::npos) {
                        a += 'N';
                        i = close;
                        continue;
                    }
                }
                a += low[i];
            }
            std::string b;
            b.reserve(a.size());
            for (std::size_t i = 0; i < a.size();) {
                const auto c = static_cast<unsigned char>(a[i]);
                const bool start = i == 0 || !IsWord(static_cast<unsigned char>(a[i - 1]));
                if (std::isdigit(c) && start) {
                    std::size_t j = i;
                    while (j < a.size() && std::isdigit(static_cast<unsigned char>(a[j]))) ++j;
                    if (j + 1 < a.size() && a[j] == '.' && std::isdigit(static_cast<unsigned char>(a[j + 1]))) {
                        ++j;
                        while (j < a.size() && std::isdigit(static_cast<unsigned char>(a[j]))) ++j;
                    }
                    b += 'N';
                    i = j;
                    continue;
                }
                b += a[i];
                ++i;
            }
            return b;
        }

        bool IsPySpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

        std::string_view Strip(std::string_view s) noexcept
        {
            while (!s.empty() && IsPySpace(s.front())) s.remove_prefix(1);
            while (!s.empty() && IsPySpace(s.back())) s.remove_suffix(1);
            return s;
        }

        std::string DescMatch(std::string_view t, bool* none = nullptr)
        {
            static const MiniRegex neg(kPatNegation);
            for (const auto& rule : DescTable()) {
                MiniRegex::Match m;
                if (!rule.re.Search(t, &m)) continue;
                const auto& spec = rule.spec;
                const bool isNeg = neg.ContainsOrBudget(t);
                auto firstGroupIn = [&](std::initializer_list<std::string_view> set) -> std::string {
                    for (std::size_t g = 1; g <= m.groups.size(); ++g) {
                        const auto v = m.Group(t, g);
                        if (v.empty()) continue;
                        for (const auto s : set) {
                            if (v == s) return std::string(v);
                        }
                    }
                    return {};
                };
                if (spec == "NONE") {
                    if (none) *none = true;
                    return {};
                }
                if (spec == "DMG") {
                    const auto after = t.substr(static_cast<std::size_t>(m.whole.begin));
                    static const MiniRegex h(kPatHealthWord);
                    static const MiniRegex mg(kPatMagickaWord);
                    static const MiniRegex st(kPatStaminaWord);
                    if (h.Contains(after)) return "damage_health";
                    if (mg.Contains(after)) return "damage_magicka";
                    if (st.Contains(after)) return "damage_stamina";
                    return "damage_health";
                }
                if (EndsWith(spec, "_V")) {
                    std::string v = firstGroupIn({ "health", "magicka", "stamina" });
                    if (v.empty()) v = "health";
                    return spec.substr(0, spec.size() - 1) + v;
                }
                if (spec == "MAGRES") return isNeg ? "weakness_magic" : "resist_magic";
                if (spec == "WEAK" || spec == "RES") {
                    const std::string el = firstGroupIn({ "fire", "frost", "shock", "poison" });
                    return (spec == "WEAK" ? "weakness_" : "resist_") + el;
                }
                if (spec == "SPEED") return isNeg ? "weaken_combat_weapon_speed" : "fortify_combat_weapon_speed";
                if (spec == "ATKDMG") {
                    static const MiniRegex weak(kPatWeakerDamage);
                    return weak.Contains(t) ? "weaken_combat_attack_damage" : "fortify_combat_attack_damage";
                }
                if (spec == "SKILL") {
                    std::string w(m.Group(t, 1));
                    static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kAlias{ {
                        { "blocking", "block" }, { "pickpocketing", "pickpocket" }, { "lockpick", "lockpicking" },
                        { "lockpicking", "lockpicking" }, { "marksman", "archery" }, { "speech", "speech" },
                    } };
                    for (const auto& [a, b] : kAlias) {
                        if (w == a) w = b;
                    }
                    const char* sk = SkillWord(w);
                    return sk ? std::string("fortify_skill_") + sk : std::string{};
                }
                return spec;
            }
            return {};
        }

        /// (spec, skill-less) from the description; "" if none.
        std::string DescSpec(std::string_view text, bool* none = nullptr)
        {
            if (none) *none = false;
            if (Strip(text).empty()) return {};
            const std::string t = NormaliseDescription(text);
            const std::string_view stripped = Strip(t);
            // The first sentence carries the primary effect: split at the first
            // [.!;] followed by whitespace.
            std::string_view first = stripped;
            for (std::size_t i = 0; i + 1 < stripped.size(); ++i) {
                const char c = stripped[i];
                if ((c == '.' || c == '!' || c == ';') && IsPySpace(stripped[i + 1])) {
                    first = stripped.substr(0, i + 1);
                    break;
                }
            }
            bool firstNone = false;
            std::string spec = DescMatch(first, &firstNone);
            if (spec.empty() && !firstNone && first != stripped) spec = DescMatch(t, none);
            else if (none) *none = firstNone;
            return spec;
        }

        std::string DescElementOf(std::string_view text)
        {
            const std::string t = NormaliseDescription(text);
            for (const auto& rule : DescElement()) {
                if (rule.re.Contains(t)) return rule.spec;
            }
            return "magic";
        }

        // =====================================================================
        // Spec string -> column
        // =====================================================================
        struct Resolved
        {
            Col col = Col::_Count;
            Col col2 = Col::_Count;
        };

        Resolved ToColumns(const std::string& spec, const std::string& element)
        {
            Resolved r;
            if (spec.empty()) return r;
            if (spec == "damage_health") {
                const std::string e = element.empty() ? "magic" : element;
                r.col = FromName("damage_health_" + e).value_or(Col::damage_health_magic);
                return r;
            }
            if (spec == "summon_creature") {
                r.col = Col::summon_creature;
                if (element == "fire") { r.col = Col::summon_creature_fire; r.col2 = Col::summon_creature; }
                if (element == "frost") { r.col = Col::summon_creature_frost; r.col2 = Col::summon_creature; }
                if (element == "shock") { r.col = Col::summon_creature_shock; r.col2 = Col::summon_creature; }
                return r;
            }
            if (StartsWith(spec, "drain_skill")) { r.col = Col::drain_skill; return r; }
            if (spec == "control_damage_weapon") { r.col = Col::weaken_combat_attack_damage; return r; }
            if (spec == "survival_cold_exposure") { r.col = Col::survival; return r; }
            if (spec == "shout_dragonrend") { r.col = Col::shout; return r; }
            if (spec == "cure_addiction") { r.col = Col::cure; return r; }
            if (auto c = FromName(spec)) {
                r.col = *c;
                return r;
            }
            // A spec with no column of its own: its family column (longest
            // family name that prefixes it).
            std::size_t best = 0;
            for (std::size_t i = 0; i < kColumnCount; ++i) {
                if (kColumns[i].level != Level::Family) continue;
                const auto f = kColumns[i].id;
                if (spec.size() > f.size() && StartsWith(spec, f) && spec[f.size()] == '_' && f.size() > best) {
                    best = f.size();
                    r.col = static_cast<Col>(i);
                }
            }
            return r;
        }

        std::string FamilyOfSpec(const std::string& spec)
        {
            // The reference's family_of(): the spec's family key, for the
            // carrier-family check.
            for (const char* f : { "fortify_vital", "drain_vital", "weaken_regen", "fortify_skill", "drain_skill",
                                   "fortify_combat", "weaken_combat", "soul_trap" }) {
                if (StartsWith(spec, f)) return f;
            }
            const auto u = spec.find('_');
            return u == std::string::npos ? spec : spec.substr(0, u);
        }

        // =====================================================================
        // The reference's _classify / classify
        // =====================================================================
        struct Raw
        {
            std::string spec;
            Route route = Route::Unmapped;
            std::string element;
            bool final = false;  // unmapped on purpose: skip the description route
        };

        Raw ClassifyRaw(const MagicEffectRecord& r, const std::string& kw)
        {
            const int a = r.archetype;
            const std::string_view av = r.primaryAV;
            const bool det = r.detrimental;
            const bool rec = r.Recover();
            const std::string ln = Lower(r.name);
            const bool vm = IsValueModifier(a);

            const bool scriptLike = a == kArchScript ||
                                    ((vm || a == kArchAbsorb) && SuspectAv(av) && av != "Confidence" && av != "Aggression") ||
                                    (vm && av.empty());
            if (scriptLike) {
                const bool helperName = HelperRe().ContainsOrBudget(ln);
                if (a == kArchScript && helperName) return { {}, Route::Helper, {} };
                static const MiniRegex survivalKw(kPatSurvivalKeyword);
                if (helperName && !survivalKw.Contains(kw)) return { {}, Route::Helper, {} };
                if (av == "Variable09" && Contains(ln, "warmth")) return { "survival_warmth", Route::Keyword, {} };
                // Simonrim: Fortify Potion Duration rides on AlchemySkillAdvance
                // with the Fortify Alchemy enchantment keyword.
                if (av == "AlchemySkillAdvance" && Contains(kw, "MagicEnchFortifyAlchemy")) {
                    return { "meta_potion_duration", Route::Keyword, {} };
                }
                for (const auto& rule : KwTable()) {
                    if (!rule.re.Contains(kw)) continue;
                    std::string spec = rule.spec;
                    std::string el;
                    if (StartsWith(spec, "damage")) el = Element(r, kw, true);
                    if (spec == "survival_warmth" && det) spec = "survival_cold_exposure";
                    // No element for a scripted summon: script effects carry
                    // MagicSummon* keywords loosely (an Ice Wraith tagged Shock,
                    // "Modify Conjuration" tagged Fire).
                    return { spec, Route::Keyword, el };
                }
                bool catchAll = false;
                std::string spec = NameSpec(r.name, &catchAll);
                // The generic damage name rule ("frost", "bolt", "damage" anywhere)
                // yields to the description when there is one that says what the
                // effect does: "Augmented Frost" is "Frost spells do 25% more
                // damage" (spell power: no column), not frost damage.
                if (catchAll) {
                    bool none = false;
                    const std::string ds = DescSpec(r.description, &none);
                    if (none) return { {}, Route::Unmapped, {}, true };
                    if (!ds.empty()) spec.clear();  // ClassifyColumns takes the description route
                }
                // effects.csv's damage columns need detrimental=1: a name saying
                // "damage" on a beneficial effect, or "resist ... damage"
                // (Simonrim's Adamant "Resist Magicka Damage"), is not damage.
                // Script effects are exempt from the flag: their scripts deal the
                // damage and the flag is often unset.
                if (StartsWith(spec, "damage")) {
                    static const MiniRegex resist(kPatResistWord);
                    if (resist.Contains(ln) || (!det && a != kArchScript)) spec.clear();
                }
                if (!spec.empty()) {
                    std::string el;
                    if (StartsWith(spec, "damage")) el = Element(r, kw, true);
                    return { spec, Route::Name, el };
                }
                if (helperName) return { {}, Route::Helper, {} };
                return { {}, Route::Unmapped, {} };
            }

            const char* simple = nullptr;
            switch (a) {
                case kArchCalm: simple = "influence_calm"; break;
                case kArchDemoralize: simple = "influence_fear"; break;
                case kArchFrenzy: simple = "influence_frenzy"; break;
                case kArchTurnUndead: simple = "influence_turn_undead"; break;
                case kArchBanish: simple = "influence_banish"; break;
                case kArchCommandSummoned: simple = "influence_command"; break;
                case kArchParalysis: simple = "control_paralysis"; break;
                case kArchStagger:
                case kArchConcussion: simple = "control_stagger"; break;
                case kArchDisarm: simple = "control_disarm"; break;
                case kArchGrabActor: simple = "control_grab"; break;
                case kArchLight:
                    // Requiem's "Darkness" is a Light effect with a dark light.
                    if (ln.find("dark") != std::string::npos) return { {}, Route::Unmapped, {}, true };
                    simple = "vision_light";
                    break;
                case kArchNightEye: simple = "vision_night_eye"; break;
                case kArchDetectLife: simple = "vision_detect_life"; break;
                case kArchGuide: simple = "vision_clairvoyance"; break;
                case kArchInvisibility:
                case kArchDarkness: simple = "stealth_invisibility"; break;
                case kArchDisguise: simple = "stealth_disguise"; break;
                case kArchEtherealize: simple = "defense_ethereal"; break;
                case kArchSummonCreature: simple = "summon_creature"; break;
                case kArchReanimate: simple = "summon_reanimate"; break;
                case kArchBoundWeapon: simple = "summon_bound_weapon"; break;
                case kArchCureDisease: simple = "cure_disease"; break;
                case kArchCurePoison: simple = "cure_poison"; break;
                case kArchCureParalysis: simple = "cure_paralysis"; break;
                case kArchCureAddiction: simple = "cure_addiction"; break;
                case kArchDispel: simple = "cure_dispel"; break;
                case kArchSoulTrap: simple = "soul_trap"; break;
                case kArchSlowTime: simple = "utility_slow_time"; break;
                case kArchTelekinesis: simple = "utility_telekinesis"; break;
                case kArchOpen:
                case kArchLock: simple = "utility_unlock"; break;
                case kArchWerewolf:
                case kArchWerewolfFeed: simple = "transform_werewolf"; break;
                case kArchVampireLord: simple = "transform_vampire_lord"; break;
                case kArchEnhanceWeapon: simple = "fortify_combat_weapon_speed"; break;
                case kArchSpawnScriptedRef: return { {}, Route::Unmapped, {} };
                default: break;
            }
            if (simple) {
                std::string spec = simple;
                // LoreRim: a CureParalysis archetype named Cure Disease.
                if (StartsWith(spec, "cure") && Contains(ln, "cure disease")) spec = "cure_disease";
                if (spec == "summon_creature") {
                    if (Contains(kw, "MagicSummonFire")) return { spec, Route::Data, "fire" };
                    if (Contains(kw, "MagicSummonFrost")) return { spec, Route::Data, "frost" };
                    if (Contains(kw, "MagicSummonShock")) return { spec, Route::Data, "shock" };
                }
                return { spec, Route::Data, {} };
            }
            if (a == kArchRally) {
                static const MiniRegex carried(kPatRallyCarried);
                if (carried.Contains(ln)) {
                    const std::string spec = NameSpec(r.name);
                    if (!spec.empty()) return { spec, Route::Name, {} };
                }
                return { "influence_rally", Route::Data, {} };
            }
            if (a == kArchCloak || a == kArchSpawnHazard) {
                // A damage keyword on the wrapper says what it does. A resisted
                // actor value alone does not (the reference read it as damage:
                // Simonrim's Whirlwind Cloak, a speed cloak, carries FrostResist);
                // the payload, or failing that the description, decides.
                if (Contains(kw, "MagicDamage")) return { "damage_health", Route::Data, Element(r, kw, false) };
                return { {}, Route::Wrapper, {} };
            }
            if (a == kArchAbsorb) {
                if (const char* v = Vital(av)) return { std::string("absorb_") + v, Route::Data, {} };
                return { {}, Route::Unmapped, {} };
            }
            if (vm) {
                if (const char* v = Vital(av)) {
                    if (!det) return { std::string(rec ? "fortify_vital_" : "restore_") + v, Route::Data, {} };
                    if (rec) return { std::string("drain_vital_") + v, Route::Data, {} };
                    const std::string vs = v;
                    if (vs == "health") return { "damage_health", Route::Data, Element(r, kw, false) };
                    return { "damage_" + vs, Route::Data, {} };
                }
                if (const char* v = Rate(av)) return { std::string(det ? "weaken_regen_" : "regen_") + v, Route::Data, {} };
                if (const char* e = Res(av)) return { std::string(det ? "weakness_" : "resist_") + e, Route::Data, {} };
                if (av == "DamageResist") return { det ? "weakness_armor" : "defense_armor", Route::Data, {} };
                if (const char* sk = CanonSkill(av)) {
                    return { std::string(det ? "drain_skill_" : "fortify_skill_") + sk, Route::Data, {} };
                }
                if (const char* c = CombatAv(av)) {
                    return { std::string(det ? "weaken_combat_" : "fortify_combat_") + c, Route::Data, {} };
                }
                // Simonrim's "Fortify Acrobatics" boots: SpeedMult carrying the
                // Slowfall keyword ("jump twice as high") -- the keyword decides.
                if (av == "SpeedMult" && !det && Contains(kw, "Slowfall")) return { "movement_jump_fall", Route::Data, {} };
                if (av == "SpeedMult") return { det ? "control_slow" : "movement_speed", Route::Data, {} };
                if (av == "CarryWeight" && det) return { "control_slow", Route::Data, {} };
                static constexpr std::array<std::pair<std::string_view, const char*>, 14> kAv{ {
                    { "ReflectDamage", "defense_reflect" }, { "AbsorbChance", "defense_spell_absorb" },
                    { "WardPower", "defense_ward" }, { "CarryWeight", "utility_carry_weight" },
                    { "WaterBreathing", "utility_water_breathing" }, { "WaterWalking", "movement_water_walking" },
                    { "MovementNoiseMult", "stealth_muffle" }, { "Invisibility", "stealth_invisibility" },
                    { "DetectLifeRange", "vision_detect_life" }, { "Paralysis", "control_paralysis" },
                    { "ShoutRecoveryMult", "shout_recovery" }, { "DragonRend", "shout_dragonrend" },
                    { "JumpingBonus", "movement_jump_fall" }, { "Blindness", "control_blind" },
                } };
                for (const auto& [k, v] : kAv) {
                    if (av == k) return { v, Route::Data, {} };
                }
                if (av == "Confidence") return { det ? "influence_fear" : "influence_rally", Route::Data, {} };
                if (av == "Aggression") return { det ? "influence_frenzy" : "influence_calm", Route::Data, {} };
                return { {}, Route::Unmapped, {} };
            }
            return { {}, Route::Unmapped, {} };
        }

        // Names that override a vital/armour/restore actor value used as a
        // carrier (LoreRim Turn Undead on Health, Simonrim Silence on Magicka,
        // Deep Freeze on Health).
        const Table& StrongName()
        {
            static const Table t = Build({
                { R"re(turn (greater |lesser )?undead|repel (lesser )?undead)re", "influence_turn_undead" },
                { R"re(paraly|deep freeze)re", "control_paralysis" },
                { R"re(^silence)re", "control_silence" },
                { R"re(soul trap)re", "soul_trap" },
                { R"re(^courage|^rally)re", "influence_rally" },
            });
            return t;
        }

    }

    std::string Lower(std::string_view s)
    {
        std::string out(s);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    void OverrideTable::Add(std::string_view plugin, std::uint32_t localId, Col col)
    {
        map_[{ Lower(plugin), localId }] = col;
    }

    std::optional<Col> OverrideTable::Find(std::string_view plugin, std::uint32_t formId) const
    {
        if (map_.empty()) return std::nullopt;
        const auto it = map_.find({ Lower(plugin), LocalId(formId) });
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }

    namespace
    {
        EffectClass ClassifyColumns(const MagicEffectRecord& r, const OverrideTable* overrides);
    }

    EffectClass ClassifyEffect(const MagicEffectRecord& r, const OverrideTable* overrides)
    {
        EffectClass out = ClassifyColumns(r, overrides);
        const std::string ln = Lower(r.name);
        out.hydrated = ln.find("hydrat") != std::string::npos;
        // Only the hidden-row checks read these, and they are the costly part
        // of a classification (the helper pattern cannot skip start bytes).
        if (r.HiddenInUI() && out.Mapped()) {
            out.helperName = HelperRe().ContainsOrBudget(ln);
            if (const auto n = NameColumn(r.name)) out.nameFamily = FamilyKey(*n);
        }
        if (out.route == Route::Wrapper) {
            if (const auto d = DescriptionColumn(r.description, r.detrimental)) out.wrapperDescription = *d;
        }
        return out;
    }

    namespace
    {
    EffectClass ClassifyColumns(const MagicEffectRecord& r, const OverrideTable* overrides)
    {
        EffectClass out;
        if (overrides) {
            if (auto c = overrides->Find(r.plugin, r.formId)) {
                out.col = *c;
                out.route = Route::Override;
                return out;
            }
        }
        const std::string kw = JoinKeywords(r.keywords);
        // Requiem's "Dispel Soul Gems" frees the souls in carried gems -- the
        // opposite of soul trap; no column says it.
        if (Lower(r.name).find("dispel soul gems") != std::string::npos) return out;
        Raw raw = ClassifyRaw(r, kw);

        if (raw.route == Route::Unmapped && !raw.final) {
            std::string ds = DescSpec(r.description);
            if (!ds.empty()) {
                if (r.detrimental) {
                    for (const auto& [a, b] : { std::pair<std::string_view, std::string_view>{ "resist_", "weakness_" },
                                                { "fortify_combat_", "weaken_combat_" },
                                                { "defense_armor", "weakness_armor" } }) {
                        if (StartsWith(ds, a)) ds = std::string(b) + ds.substr(a.size());
                    }
                }
                // "Take double damage": on a detrimental effect a weakness
                // applied (weakness_armor needs detrimental=1); on a beneficial
                // one the wearer's own drawback (Pain of Adoration's mask): no column.
                if (ds == "TAKEMORE") ds = r.detrimental ? "weakness_armor" : "";
                raw.spec = ds;
                raw.route = ds.empty() ? Route::Unmapped : Route::Description;
                raw.element = StartsWith(ds, "damage") ? DescElementOf(r.description) : std::string{};
            }
        }
        if (!raw.spec.empty() && raw.route == Route::Data) {
            const std::string fam = FamilyOfSpec(raw.spec);
            if (fam == "fortify_vital" || fam == "drain_vital" || fam == "defense" || fam == "restore") {
                const std::string ln = Lower(r.name);
                for (const auto& rule : StrongName()) {
                    if (rule.re.Contains(ln)) {
                        raw.spec = rule.spec;
                        raw.route = Route::Name;
                        raw.element.clear();
                        break;
                    }
                }
            }
        }
        if (raw.spec == "summon_creature" && Contains(Lower(r.name), "reanimat")) raw.spec = "summon_reanimate";

        if (raw.spec.empty()) {
            out.route = raw.route;
            return out;
        }
        const auto cols = ToColumns(raw.spec, raw.element);
        out.col = cols.col;
        out.col2 = cols.col2;
        out.route = out.Mapped() ? raw.route : Route::Unmapped;
        // Simonrim Fortify Security: lockpicking and pickpocket in one effect.
        if (out.col == Col::fortify_skill_lockpicking && r.primaryAV == "PickPocketSkillAdvance") {
            out.col2 = Col::fortify_skill_pickpocket;
        }
        if (FamilyKey(out.col) == Col::cure && out.route == Route::Data) {
            const int a = r.archetype;
            out.cureByArchetype = a == kArchCureDisease || a == kArchCurePoison || a == kArchCureParalysis ||
                                  a == kArchCureAddiction || a == kArchDispel;
        }
        return out;
    }
    }  // namespace

    Col SelfHarmColumn(Col c) noexcept
    {
        switch (c) {
            case Col::damage_health_fire:
            case Col::damage_health_frost:
            case Col::damage_health_shock:
            case Col::damage_health_poison:
            case Col::damage_health_magic:
            case Col::damage_health_sun:
            case Col::damage_health_physical:
            case Col::damage_health_disease:
            case Col::drain_vital_health: return Col::self_harm_health;
            case Col::damage_magicka:
            case Col::drain_vital_magicka: return Col::self_harm_magicka;
            case Col::damage_stamina:
            case Col::drain_vital_stamina: return Col::self_harm_stamina;
            default: break;
        }
        if (c == Col::_Count) return Col::_Count;
        switch (FamilyKey(c)) {
            case Col::damage:
            case Col::drain_vital:
            case Col::absorb:
            case Col::weaken_regen:
            case Col::weakness:
            case Col::weaken_combat:
            case Col::control:
            case Col::influence:
            case Col::drain_skill: return Col::self_harm;
            default: return Col::_Count;
        }
    }

    bool HarmsUser(Kind kind, const MagicEffectRecord& m) noexcept
    {
        if (kind != Kind::Food && kind != Kind::Potion) return false;
        return m.detrimental || m.hostile || (m.flags & (kFlagHostile | kFlagDetrimental)) != 0;
    }

    std::optional<Col> NameColumn(std::string_view name)
    {
        const std::string spec = NameSpec(name);
        if (spec.empty()) return std::nullopt;
        // The element does not change the family, which is all the caller uses.
        const auto cols = ToColumns(spec, {});
        if (cols.col == Col::_Count) return std::nullopt;
        return cols.col;
    }

    bool IsHelperName(std::string_view name) { return HelperRe().ContainsOrBudget(Lower(name)); }

    bool DescriptionSaysNone(std::string_view description)
    {
        bool none = false;
        (void)DescSpec(description, &none);
        return none;
    }

    DescNumber DescriptionNumber(std::string_view d)
    {
        for (std::size_t i = d.find('<'); i != std::string_view::npos; i = d.find('<', i + 1)) {
            const auto close = d.find('>', i + 1);
            if (close == std::string_view::npos) break;
            const auto tag = d.substr(i + 1, close - i - 1);
            if (tag.empty() || tag.size() > 12) continue;
            bool numeric = true;
            int dots = 0;
            for (const char c : tag) {
                if (c == '.') ++dots;
                else if (c < '0' || c > '9') numeric = false;
            }
            if (!numeric || dots > 1 || tag.front() == '.') continue;
            float v = 0.0f;
            const auto r = std::from_chars(tag.data(), tag.data() + tag.size(), v);
            if (r.ec == std::errc{} && v > 0.0f) {
                auto rest = d.substr(close + 1);
                while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
                const bool pct = StartsWith(rest, "%") || StartsWith(Lower(rest.substr(0, 7)), "percent");
                return { v, pct };
            }
        }
        return {};
    }

    bool IsBadItemName(std::string_view name)
    {
        static const MiniRegex bad(kPatBadItemName);
        return bad.ContainsOrBudget(Lower(name));
    }

    std::optional<Col> DescriptionColumn(std::string_view description, bool detrimental)
    {
        std::string ds = DescSpec(description);
        if (ds.empty()) return std::nullopt;
        if (detrimental) {
            for (const auto& [a, b] : { std::pair<std::string_view, std::string_view>{ "resist_", "weakness_" },
                                        { "fortify_combat_", "weaken_combat_" },
                                        { "defense_armor", "weakness_armor" } }) {
                if (StartsWith(ds, a)) ds = std::string(b) + ds.substr(a.size());
            }
        }
        if (ds == "TAKEMORE") {
            if (!detrimental) return std::nullopt;  // the wearer's drawback (see ClassifyColumns)
            ds = "weakness_armor";
        }
        const auto cols = ToColumns(ds, StartsWith(ds, "damage") ? DescElementOf(description) : std::string{});
        if (cols.col == Col::_Count) return std::nullopt;
        return cols.col;
    }

    std::vector<std::string> RulePatterns()
    {
        std::vector<std::string> out;
        for (const Table* t : { &KwElement(), &NameElement(), &KwTable(), &NameTable(), &DescTable(), &DescElement(),
                                &StrongName() }) {
            for (const auto& rule : *t) out.push_back(rule.re.Pattern());
        }
        out.push_back(HelperRe().Pattern());
        for (const char* p : { kPatSuspectAv, kPatVitalWord, kPatNegation, kPatHealthWord, kPatMagickaWord, kPatStaminaWord,
                               kPatWeakerDamage, kPatSurvivalKeyword, kPatRallyCarried, kPatBadItemName, kPatResistWord }) {
            out.emplace_back(p);
        }
        return out;
    }

    std::string CheckRuleTables()
    {
        for (const Table* t : { &KwElement(), &NameElement(), &KwTable(), &NameTable(), &DescTable(), &DescElement(),
                                &StrongName() }) {
            for (const auto& rule : *t) {
                if (!rule.re.Valid()) return rule.re.Error();
            }
        }
        if (!HelperRe().Valid()) return HelperRe().Error();
        for (const auto& p : RulePatterns()) {
            if (const MiniRegex re(p); !re.Valid()) return re.Error();
        }
        // Every fixed spec in the tables resolves to a column.
        for (const Table* t : { &KwTable(), &NameTable(), &DescTable(), &StrongName() }) {
            for (const auto& rule : *t) {
                const auto& s = rule.spec;
                if (s.find('_') == std::string::npos || EndsWith(s, "_X") || EndsWith(s, "_V") || EndsWith(s, "*")) continue;
                if (ToColumns(s, {}).col == Col::_Count) return "no column for spec " + s;
            }
        }
        return {};
    }
}

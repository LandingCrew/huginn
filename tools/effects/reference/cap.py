"""Reference extractor for cap(i) (doc 9, 2026-10-07; copied into the repo by R2 as the
test oracle -- see tools/effects/README.md). One fix from the original: a stray backspace
byte where the fortify_vital name rule meant \b.

Reference extractor for cap(i): effect row -> column, following effects.csv.
Order: helper/visibility -> repurposed-AV resolution (keyword, override, name) ->
archetype/AV/flags -> element / skill axes."""
import re

DESC_ON = False  # v2: description route for rows nothing else maps (set by run.py)

VITAL = {'Health': 'health', 'Magicka': 'magicka', 'Stamina': 'stamina'}
RATE = {'HealRate': 'health', 'HealRateMult': 'health', 'CombatHealthRegenMult': 'health', 'CombatHealthRegenMultiply': 'health',
        'MagickaRate': 'magicka', 'MagickaRateMult': 'magicka', 'StaminaRate': 'stamina', 'StaminaRateMult': 'stamina'}
RES = {'FireResist': 'fire', 'FrostResist': 'frost', 'ElectricResist': 'shock', 'PoisonResist': 'poison',
       'MagicResist': 'magic', 'DiseaseResist': 'disease'}
SKILLS = {'OneHanded': 'one_handed', 'TwoHanded': 'two_handed', 'Marksman': 'archery', 'Block': 'block',
          'Smithing': 'smithing', 'HeavyArmor': 'heavy_armor', 'LightArmor': 'light_armor', 'Pickpocket': 'pickpocket',
          'Lockpicking': 'lockpicking', 'Sneak': 'sneak', 'Alchemy': 'alchemy', 'Speechcraft': 'speech',
          'Alteration': 'alteration', 'Conjuration': 'conjuration', 'Destruction': 'destruction',
          'Illusion': 'illusion', 'Restoration': 'restoration', 'Enchanting': 'enchanting'}
COMBAT = {'AttackDamageMult': 'attack_damage', 'WeaponSpeedMult': 'weapon_speed', 'LeftWeaponSpeedMult': 'weapon_speed',
          'UnarmedDamage': 'unarmed', 'CritChance': 'crit', 'BowSpeedBonus': 'weapon_speed', 'PowerAttackStamina': 'power_attack'}
# AVs that mods repurpose: never trusted on their own.
SUSPECT_AV = re.compile(r'SkillAdvance$|^(Fame|Infamy|Mood|Morality|Variable\d\d|VoicePoints|Energy|Assistance|Blindness|Confidence|Aggression)$')
VM = {'ValueModifier', 'PeakValueModifier', 'DualValueModifier', 'ValueAndParts', 'AccumulateMagnitude'}


def canon_skill(av):
    a = av.replace('PickPocket', 'Pickpocket')
    for k, v in SKILLS.items():
        if a in (k, k + 'Mod', k + 'PowerMod'):
            return v
    return None


# ---- element axis: keyword first, then resistAV, then name (scripted only) ----
KW_ELEM = [(r'MagicDamageFire|LoreBox_LorerimFire', 'fire'), (r'MagicDamageFrost|LoreBox_LorerimIce', 'frost'),
           (r'MagicDamageShock|LoreBox_LorerimShock', 'shock'), (r'MagicDamageSun|REQ_SunDamage|LoreBox_LorerimSon', 'sun'),
           (r'MagicDamagePoison|LoreBox_LorerimPoison|MagicDamageMiasma', 'poison'), (r'MagicDamageBleed', 'physical'),
           (r'Destruction_Arcane|Destruction_Entropic|MagicDamageSonic|Perk_EntropicFocus', 'magic')]
NAME_ELEM = [(r'\bfire|flame|burn|scorch|blaz|inferno|incinerat', 'fire'), (r'frost|\bice\b|\bicy\b|freez|blizzard|cold', 'frost'),
             (r'shock|lightning|spark|thunder|storm', 'shock'), (r'\bsun\b|holy|radiant|celestial', 'sun'),
             (r'poison|venom|toxic', 'poison')]


def element(r, use_name=False):
    kw = r['effectKeywords']
    for p, el in KW_ELEM:
        if re.search(p, kw):
            return el, 'keyword'
    ra = r['resistAV']
    if ra in RES:
        return RES[ra], 'data'
    if ra == 'DamageResist':
        return 'physical', 'data'
    if use_name:
        n = r['effectName'].lower()
        for p, el in NAME_ELEM:
            if re.search(p, n):
                return el, 'name'
    return 'magic', 'data'


# ---- keyword table (language independent; vanilla + known mod vocabularies) ----
# order matters; first hit wins. value = specific column id
KW_TABLE = [
    (r'CCSM_RestoreHunger|StarfrostHunger|Survival_.*Hunger', 'survival_hunger'),
    (r'CCSM_RestoreCold|Survival_MagicAlchFortifyWarmth|\bWarmth\b|FrostfallWarmth', 'survival_warmth'),
    (r'Thirst|Hydrat', 'survival_thirst'),
    (r'MAG_MagicEnchBurden|MagicSlow\b', 'control_slow'),
    (r'MAG_MagicAlchDamageWeapon', 'control_damage_weapon'),
    (r'FortifyPowerAttacks', 'fortify_combat_power_attack'),
    (r'FortifySneakAttacks', 'fortify_combat_sneak_attack'),
    (r'FortifyBash', 'fortify_combat_bash'),
    (r'FortifyUnarmed', 'fortify_combat_unarmed'),
    (r'ResistArrowDamage', 'defense_resist_ranged'),
    (r'ResistPowerAttackDamage', 'defense_resist_power_attack'),
    (r'ResistStagger', 'defense_resist_stagger'),
    (r'ResistSpellDamage', 'resist_magic'),
    (r'MagicEnchFortifyShouts|MagicAlchFortifyShouts', 'shout_recovery'),
    (r'MagicShoutDragonrend', 'shout_dragonrend'),
    (r'MagicEnchFortifyAlchemy', 'fortify_skill_alchemy'),
    (r'MagicEnchFortifyIllusion', 'fortify_skill_illusion'),
    (r'MagicEnchFortifyRestoration', 'fortify_skill_restoration'),
    (r'MagicEnchFortifyPickPocket|MagicAlchFortifyLockpicking', 'fortify_skill_lockpicking'),
    (r'MagicInfluenceFear|Illusion_Nightmare|Illusion_Death', 'influence_fear'),
    (r'MagicInfluenceFrenzy', 'influence_frenzy'),
    (r'MagicInfluenceCharm|MagicInfluenceCalm', 'influence_calm'),
    (r'Illusion_Command', 'influence_command'),
    (r'Illusion_Sleep', 'control_sleep'),
    (r'Illusion_Silence', 'control_silence'),
    (r'Illusion_Blind', 'control_blind'),
    (r'Illusion_Muffle|Illusion_Noise|Illusion_Sound', 'stealth_muffle'),
    (r'Illusion_ShadowStride', 'stealth_invisibility'),
    (r'Illusion_Pain', 'damage_health'),
    (r'MagicParalysis', 'control_paralysis'),
    (r'MagicTurnUndead', 'influence_turn_undead'),
    (r'SoulTrap', 'soul_trap'),
    (r'MagicInvisibility', 'stealth_invisibility'),
    (r'MagicNightEye', 'vision_night_eye'),
    (r'MagicWard\b', 'defense_ward'),
    (r'MagicArmorSpell', 'defense_armor'),
    (r'FortifyAttributes', 'fortify_vital_health'),
    (r'MagicRestoreHealth|MagicAlchRestoreHealth', 'restore_health'),
    (r'MagicAlchRestoreMagicka', 'restore_magicka'),
    (r'MagicAlchRestoreStamina', 'restore_stamina'),
    (r'MagicSummon', 'summon_creature'),
    (r'MagicDamage(Fire|Frost|Shock|Sun|Poison)|REQ_SunDamage|Destruction_(Arcane|Entropic)|MagicDamageSonic', 'damage_health'),
    (r'MagicBlessing|PilgrimShrineBlessing', 'meta_xp_gain'),
]

# ---- name table (English; last resort for Script and suspect AVs) ----
NAME_TABLE = [
    (r'fortify (health|magicka|stamina) regenerat|regenerat|fortify heal rate', 'regen_X'),
    (r'^fortify (health|magicka|stamina)\b', 'fortify_vital_X'),
    (r'armou?r penetration', 'fortify_combat_armor_penetration'),
    (r'no fall(ing)? damage|feather ?fall|slow ?fall', 'movement_jump_fall'),
    (r'fortify (melee |attack )?damage|increased damage|increase \w+ damage', 'fortify_combat_attack_damage'),
    (r'bound (shield|arrows|bolts)', 'summon_bound_weapon'),
    (r'damage weapon', 'weaken_combat_attack_damage'),
    (r'^fortify (one-handed|two-handed|archery|marksman|block|smithing|heavy armor|light armor|pickpocket|lockpicking|sneak|alchemy|barter|speech|alteration|conjuration|destruction|illusion|restoration|enchanting)', 'fortify_skill_X'),
    (r'soul trap|siphon soul|soul tear trap', 'soul_trap'),
    (r'restore thirst|hydrated|\bthirst\b', 'survival_thirst'),
    (r'hunger', 'survival_hunger'), (r'warmth|warming|restore cold|\bwarm\b', 'survival_warmth'),
    (r'fatigue|well rested|rested', 'survival_fatigue'),
    (r'drunk|drugged|inebri|alcohol|skooma', 'survival_intoxication'),
    (r'cure disease|cure vampirism|beastblood', 'cure_disease'), (r'cure poison', 'cure_poison'),
    (r'cure injur', 'cure_injury'), (r'\bdispel\b', 'cure_dispel'),
    (r'restore health|heal(ing)?\b|regain .*health', 'restore_health'), (r'restore magicka', 'restore_magicka'),
    (r'restore stamina', 'restore_stamina'),
    (r'^fear|nightmare|terror|hysteria', 'influence_fear'), (r'^frenzy|strife|fury\b|mayhem', 'influence_frenzy'),
    (r'^calm|pacify|charm', 'influence_calm'), (r'command|mind control|enslave|obedience|bend will|possession', 'influence_command'),
    (r'courage|rally', 'influence_rally'), (r'turn (greater |lesser )?undead|repel', 'influence_turn_undead'),
    (r'banish', 'influence_banish'),
    (r'paraly|deep freeze|petrif', 'control_paralysis'), (r'knockdown|stagger|overthrow|unrelenting force|\bpush\b|repulse', 'control_stagger'),
    (r'disarm', 'control_disarm'), (r'\bsleep', 'control_sleep'), (r'silence', 'control_silence'), (r'\bblind', 'control_blind'),
    (r'^slow\b|\bslow(s|ed)?\b(?! time| fall)|burden|ice form', 'control_slow'), (r'telekinetic grab|grip|grasp', 'control_grab'),
    (r'invisib|vanish|shadow stride|chameleon', 'stealth_invisibility'), (r'muffl|dampen|\bnoise\b|quiet', 'stealth_muffle'),
    (r'night eye|cat.?s eye', 'vision_night_eye'), (r'detect (life|dead|all)|aura whisper|animal senses', 'vision_detect_life'),
    (r'clairvoyance', 'vision_clairvoyance'), (r'candlelight|magelight|\blight\b|lantern|torch', 'vision_light'),
    (r'feather ?fall|slow fall|levitat|jump', 'movement_jump_fall'), (r'water ?walk', 'movement_water_walking'),
    (r'water ?breath', 'utility_water_breathing'), (r'sprint|whirlwind|speed|swift|haste', 'movement_speed'),
    (r'teleport|portal|recall|mark\b|divine intervention|gates', 'utility_teleport'),
    (r'unlock|open lock|knock', 'utility_unlock'), (r'telekin', 'utility_telekinesis'),
    (r'slow time|time slows|become ethereal|ethereal|etherealize', 'utility_slow_time'),
    (r'enlarge|shrink|size', 'utility_size'), (r'transmut', 'utility_transmute'),
    (r'carry weight|feather\b', 'utility_carry_weight'),
    (r'conjure|summon|call (dragon|of valor)|bound (sword|bow|dagger|battleaxe|shield)|reanimat|raise (zombie|dead)|dead thrall', 'summon_creature'),
    (r'ward\b', 'defense_ward'), (r'reflect', 'defense_reflect'), (r'absorb spell|spell absorption', 'defense_spell_absorb'),
    (r'(oak|stone|iron|ebony|dragon)hide|(oak|stone|iron|ebony|dragon)flesh|armor rating|mage armor', 'defense_armor'),
    (r'absorb health|drain health|vampiric', 'absorb_health'), (r'absorb magicka|drain magicka', 'absorb_magicka'),
    (r'absorb stamina|drain stamina', 'absorb_stamina'),
    (r'fortify shout|shout (power|duration)|unlimited shouts|restore shout', 'shout_recovery'), (r'dragonrend', 'shout_dragonrend'),
    (r'beast form|werewolf|vampire form|polymorph', 'transform_werewolf'), (r'vampire lord|lich', 'transform_vampire_lord'),
    (r'experience|\bxp\b|blessing', 'meta_xp_gain'),
    (r'fortify potion duration', 'meta_potion_duration'),
    (r'disintegrat|damage health|pain|\bdamage\b|fireball|flame|frost|\bice\b|shock|lightning|thunder|storm|sun|holy|poison|bolt|blast|explos|meteor|spear|ray', 'damage_health'),
    (r'magicka damage|damage magicka', 'damage_magicka'), (r'stamina damage|damage stamina', 'damage_stamina'),
]

HELPER = re.compile(r'(?i)dummy|corrector|visual|\bfx\b|screen ?shake|cooldown|script ai|fake script|empty cloak|^ai$|'
                    r'^blank|null effect|^mad|not user facing|\(hidden\)|description|display effect|priority|placeholder|'
                    r'perk bonus|perk impact|invisible \d|^$|stagger push|stagger area|sound fx|light toggle|marker|tracker|'
                    r'count(er)?\b|playing music|play (lute|flute|drum)|staff enchantment|master of the mind|'
                    r'dispel (cloak|size|jump|soul gems)|dispel effect|\w dispel$')

SKILL_WORDS = {'one-handed': 'one_handed', 'two-handed': 'two_handed', 'archery': 'archery', 'marksman': 'archery',
               'block': 'block', 'smithing': 'smithing', 'heavy armor': 'heavy_armor', 'light armor': 'light_armor',
               'pickpocket': 'pickpocket', 'lockpicking': 'lockpicking', 'sneak': 'sneak', 'alchemy': 'alchemy',
               'barter': 'speech', 'speech': 'speech', 'alteration': 'alteration', 'conjuration': 'conjuration',
               'destruction': 'destruction', 'illusion': 'illusion', 'restoration': 'restoration', 'enchanting': 'enchanting'}


def name_spec(n):
    """Name-table lookup with the _X placeholders resolved. -> (spec, skill) or (None, None)."""
    ln = n.lower()
    for p, spec in NAME_TABLE:
        m = re.search(p, ln)
        if not m:
            continue
        if spec == 'regen_X':
            v = re.search(r'health|magicka|stamina|heal', ln)
            return ('regen_' + ({'heal': 'health'}.get(v.group(0), v.group(0)) if v else 'health')), None
        if spec == 'fortify_vital_X':
            return 'fortify_vital_' + m.group(1), None
        if spec == 'fortify_skill_X':
            sk = SKILL_WORDS[m.group(1)]
            return 'fortify_skill_' + sk, sk
        return spec, None
    return None, None


# Names that override a vital/armour AV used as a carrier (LoreRim Turn Undead on Health,
# Simonrim Silence on Magicka, Deep Freeze on Health).
STRONG_NAME = [(r'turn (greater |lesser )?undead|repel (lesser )?undead', 'influence_turn_undead'),
               (r'paraly|deep freeze', 'control_paralysis'), (r'^silence', 'control_silence'),
               (r'soul trap', 'soul_trap'), (r'^courage|^rally', 'influence_rally')]
CARRIER_FAMILIES = {'fortify_vital', 'drain_vital', 'defense', 'restore'}

# families whose hidden rows carry real mechanics (the hidden-row whitelist)
HIDDEN_WHITELIST_FAMILIES = {'damage', 'absorb', 'restore', 'fortify_vital', 'regen', 'drain_vital', 'weaken_regen', 'resist',
                             'weakness', 'defense', 'control', 'fortify_skill', 'fortify_combat', 'weaken_combat',
                             'movement', 'survival', 'cure', 'stealth'}
HIDDEN_WHITELIST_EXCLUDE = {'stealth_invisibility'}  # hidden invisibility rows are perk/cloak variants


def family_of(spec):
    for f in ('fortify_vital', 'drain_vital', 'weaken_regen', 'fortify_skill', 'drain_skill', 'fortify_combat',
              'weaken_combat', 'soul_trap'):
        if spec.startswith(f):
            return f
    return spec.split('_')[0]


def _classify(r):
    """-> (specific, source, element, skill). specific None = unmapped. source in
    data/keyword/override/name/unmapped/helper."""
    a, av, det, rec, n = r['arch'], r['primaryAV'], bool(r['det']), bool(r['recover']), r['effectName']
    kw = r['effectKeywords']
    ln = n.lower()
    if a == 'Script' or (a in VM | {'Absorb'} and SUSPECT_AV.search(av or '-') and av not in ('Confidence', 'Aggression')) or (a in VM and av == ''):
        if a == 'Script' and HELPER.search(n):
            return None, 'helper', None, None
        if HELPER.search(n) and not re.search(r'Hunger|Warmth|CCSM', kw):
            return None, 'helper', None, None
        if SUSPECT_AV.search(av or '-') and av in ('Variable09',) and 'warmth' in ln:
            return 'survival_warmth', 'keyword', None, None
        for p, spec in KW_TABLE:
            if re.search(p, kw):
                el = element(r, True)[0] if spec.startswith('damage') else None
                if spec == 'survival_warmth' and det:
                    spec = 'survival_cold_exposure'
                return spec, 'keyword', el, (spec[len('fortify_skill_'):] if spec.startswith('fortify_skill_') else None)
        spec, sk = name_spec(n)
        if spec:
            el = element(r, True)[0] if spec.startswith('damage') else None
            return spec, 'name', el, sk
        if HELPER.search(n):
            return None, 'helper', None, None
        return None, 'unmapped', None, None
    simple = {
        'Calm': 'influence_calm', 'Demoralize': 'influence_fear', 'Frenzy': 'influence_frenzy', 'TurnUndead': 'influence_turn_undead',
        'Banish': 'influence_banish', 'CommandSummoned': 'influence_command', 'Paralysis': 'control_paralysis',
        'Stagger': 'control_stagger', 'Concussion': 'control_stagger', 'Disarm': 'control_disarm', 'GrabActor': 'control_grab',
        'Light': 'vision_light', 'NightEye': 'vision_night_eye', 'DetectLife': 'vision_detect_life', 'Guide': 'vision_clairvoyance',
        'Invisibility': 'stealth_invisibility', 'Darkness': 'stealth_invisibility', 'Disguise': 'stealth_disguise',
        'Etherealize': 'defense_ethereal', 'SummonCreature': 'summon_creature', 'Reanimate': 'summon_reanimate',
        'BoundWeapon': 'summon_bound_weapon', 'CureDisease': 'cure_disease', 'CurePoison': 'cure_poison',
        'CureParalysis': 'cure_paralysis', 'CureAddiction': 'cure_addiction', 'Dispel': 'cure_dispel', 'SoulTrap': 'soul_trap',
        'SlowTime': 'utility_slow_time', 'Telekinesis': 'utility_telekinesis', 'Open': 'utility_unlock', 'Lock': 'utility_unlock',
        'Werewolf': 'transform_werewolf', 'VampireLord': 'transform_vampire_lord', 'WerewolfFeed': 'transform_werewolf',
        'EnhanceWeapon': 'fortify_combat_weapon_speed', 'SpawnScriptedRef': None}
    if a in simple:
        spec = simple[a]
        if spec is None:
            return None, 'unmapped', None, None
        # archetype vs name conflict on cures (LoreRim: CureParalysis named Cure Disease)
        if spec.startswith('cure') and 'cure disease' in ln:
            spec = 'cure_disease'
        if spec == 'summon_creature':
            for p, el in (('MagicSummonFire', 'fire'), ('MagicSummonFrost', 'frost'), ('MagicSummonShock', 'shock')):
                if p in kw:
                    return spec, 'data', el, None
        return spec, 'data', None, None
    if a == 'Rally':
        if re.search(r'silence|paraly|command|bend will|calm|banish|frenzy|fear', ln):
            spec, _ = name_spec(n)
            if spec:
                return spec, 'name', None, None
        return 'influence_rally', 'data', None, None
    if a in ('Cloak', 'SpawnHazard'):
        # wrapper: payload is another spell; resistAV / keyword gives element when present
        if r['resistAV'] in RES or re.search(r'MagicDamage', kw):
            return 'damage_health', 'data', element(r)[0], None
        return None, 'wrapper', None, None
    if a == 'Absorb':
        v = VITAL.get(av)
        return ('absorb_' + v if v else None), ('data' if v else 'unmapped'), None, None
    if a in VM:
        if av in VITAL:
            v = VITAL[av]
            if not det:
                return ('fortify_vital_' + v if rec else 'restore_' + v), 'data', None, None
            if rec:
                return 'drain_vital_' + v, 'data', None, None
            el, src = element(r)
            return 'damage_' + v, src, (el if v == 'health' else None), None
        if av in RATE:
            return ('weaken_regen_' if det else 'regen_') + RATE[av], 'data', None, None
        if av in RES:
            return ('weakness_' if det else 'resist_') + RES[av], 'data', None, None
        if av == 'DamageResist':
            return ('weakness_armor' if det else 'defense_armor'), 'data', None, None
        sk = canon_skill(av)
        if sk:
            return ('drain_skill_' if det else 'fortify_skill_') + sk, 'data', None, sk
        if av in COMBAT:
            return ('weaken_combat_' if det else 'fortify_combat_') + COMBAT[av], 'data', None, None
        m = {'ReflectDamage': 'defense_reflect', 'AbsorbChance': 'defense_spell_absorb', 'WardPower': 'defense_ward',
             'CarryWeight': 'utility_carry_weight', 'WaterBreathing': 'utility_water_breathing',
             'WaterWalking': 'movement_water_walking', 'MovementNoiseMult': 'stealth_muffle', 'Invisibility': 'stealth_invisibility',
             'DetectLifeRange': 'vision_detect_life', 'Paralysis': 'control_paralysis', 'ShoutRecoveryMult': 'shout_recovery',
             'DragonRend': 'shout_dragonrend', 'JumpingBonus': 'movement_jump_fall', 'Blindness': 'control_blind'}
        if av == 'SpeedMult':
            return ('control_slow' if det else 'movement_speed'), 'data', None, None
        if av == 'CarryWeight' and det:
            return 'control_slow', 'data', None, None
        if av in m:
            return m[av], 'data', None, None
        if av in ('Confidence', 'Aggression'):
            return ('influence_fear' if (av == 'Confidence' and det) else 'influence_frenzy' if av == 'Aggression' and det else 'influence_rally' if av == 'Confidence' else 'influence_calm'), 'data', None, None
        return None, 'unmapped', None, None
    return None, 'unmapped', None, None


# ---- description patterns: moved to desc.py (v2) ----
from desc import desc_spec, desc_element, classify_desc, classify_desc_v1  # noqa: E402


def classify(r):
    spec, src, el, sk = _classify(r)
    if DESC_ON and src == 'unmapped':
        ds, dsk = desc_spec(r.get('effectDescription', ''))
        if ds:
            # the detrimental flag flips a beneficial reading ("magic resist by N%" on a debuff)
            if r['det']:
                for a, b in (('resist_', 'weakness_'), ('fortify_combat_', 'weaken_combat_'), ('defense_armor', 'weakness_armor')):
                    if ds.startswith(a):
                        ds = b + ds[len(a):]
            el = desc_element(r['effectDescription']) if ds.startswith('damage') else None
            return ds, 'desc', el, dsk
    if spec and src == 'data' and family_of(spec) in CARRIER_FAMILIES:
        ln = r['effectName'].lower()
        for p, s2 in STRONG_NAME:
            if re.search(p, ln):
                return s2, 'name', None, None
    if spec == 'summon_creature' and 'reanimat' in r['effectName'].lower():
        return 'summon_reanimate', src, el, sk
    return spec, src, el, sk

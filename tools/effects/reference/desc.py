"""Description route (effectDescription) for effect rows nothing else maps.

v1 (DESC_TABLE_V1, copied from effects/scripts/cap.py) was built on 142 Arcaneum
spell texts. v2 (DESC_TABLE) is rewritten on the real MGEF descriptions from the
fixed dump: <tokens> and numbers normalised to N, word boundaries, ordered
specific -> generic, detrimental wording ("reduces ...") split from beneficial,
and the _V / skill / element placeholders resolved."""
import re

DESC_TABLE_V1 = [
    (r'soul ?trap|soul gem', 'soul_trap'), (r'teleport|portal|transport', 'utility_teleport'),
    (r'paraly', 'control_paralysis'), (r'stagger|knock(s|ed)? (down|back)|push', 'control_stagger'),
    (r'\bslow', 'control_slow'), (r'disarm', 'control_disarm'), (r'silenc', 'control_silence'),
    (r'flee|fear|terrif', 'influence_fear'), (r'calm|pacif|won.t fight', 'influence_calm'), (r'frenz|attack anything|fight each other', 'influence_frenzy'),
    (r'command|control|enslave|thrall', 'influence_command'),
    (r'invisib', 'stealth_invisibility'), (r'quieter|muffl|sound', 'stealth_muffle'),
    (r'summon|conjur|raise|reanimat|bound', 'summon_creature'),
    (r'(restore|regain|heal)s?\b.*health|heal', 'restore_health'), (r'(restore|regain)s?\b.*magicka', 'restore_magicka'),
    (r'(restore|regain)s?\b.*stamina', 'restore_stamina'),
    (r'ward|absorb.*spell|block.*spell', 'defense_ward'), (r'armor rating|damage reduction|reduc\w+ (incoming )?damage', 'defense_armor'),
    (r'resist', 'resist_magic'), (r'reflect', 'defense_reflect'),
    (r'light|illuminat', 'vision_light'), (r'detect|see (through|enemies)|reveal', 'vision_detect_life'),
    (r'jump|fall|levitat|float', 'movement_jump_fall'), (r'faster|speed|sprint|dash', 'movement_speed'),
    (r'time slows|slow(s)? time', 'utility_slow_time'), (r'transmut|ore', 'utility_transmute'),
    (r'(points of )?(fire|frost|shock|poison|sun|magic|holy)?\s*damage|burn|freez|electr|explod|blast', 'damage_health'),
    (r'drain|absorb', 'absorb_health'),
    (r'fortif|increas|boost|empower|stronger|more damage', 'fortify_combat_attack_damage'),
]

V = r'(health|magicka|stamina)'
SKILLW = (r'\b(one-handed|two-handed|archery|marksman|block(?:ing)?|smithing|heavy armor|light armor|pickpocket(?:ing)?|'
          r'lockpick(?:ing)?|alchemy|speech|alteration|conjuration|destruction|illusion|restoration|enchanting)')
SKILL_ID = {'one-handed': 'one_handed', 'two-handed': 'two_handed', 'archery': 'archery', 'marksman': 'archery',
            'block': 'block', 'blocking': 'block', 'smithing': 'smithing', 'heavy armor': 'heavy_armor',
            'light armor': 'light_armor', 'pickpocket': 'pickpocket', 'pickpocketing': 'pickpocket',
            'lockpick': 'lockpicking', 'lockpicking': 'lockpicking', 'alchemy': 'alchemy', 'speech': 'speech',
            'alteration': 'alteration', 'conjuration': 'conjuration', 'destruction': 'destruction',
            'illusion': 'illusion', 'restoration': 'restoration', 'enchanting': 'enchanting'}
ELEM = r'(fire|frost|shock|poison)'

# (pattern, spec). Placeholders: _V = vital from the match; RES/WEAK = element;
# SKILL = skill word; MAGRES / SPEED / ATKDMG = sign decided by "reduce/lower" wording.
DESC_TABLE = [
    # meta / utility: very specific wording first
    (r'soul ?trap|soul gems?\b|trap(s|ped)? (its|the|their) soul', 'soul_trap'),
    (r'teleport|portal|fast travel|transports? (you|the caster)|recall to|swaps? places|shifts? through (the )?shadows', 'utility_teleport'),
    (r'opening locks|opens? (a |the )?locks?\b|\bunlock', 'utility_unlock'),
    (r'time slows|slows? (down )?time|slow time', 'utility_slow_time'),
    (r'carry(ing)? (weight|capacity)', 'utility_carry_weight'),
    (r'breathe? underwater|water ?breathing', 'utility_water_breathing'),
    (r'walk on water|water ?walking', 'movement_water_walking'),
    (r'excavat\w* ores?|transmut\w* (one|ore|iron|silver|ingredient)', 'utility_transmute'),
    (r"(caster.s |your |target.s )size\b|\benlarg|\bshrink", 'utility_size'),
    # cures
    (r'cures? (all )?(diseases?|vampirism)', 'cure_disease'), (r'cures? (all )?poison', 'cure_poison'),
    (r'^dispels?\b|removes? (all )?(magic(al)?|spell) effects|dispels? (all )?(cloak|armor|magic)', 'cure_dispel'),
    # primary damage verb ("deals N frost damage to health and stamina"): before the
    # control/summon/resist words that usually describe a side effect in the same text.
    # DMG = vital decided from the words after the match.
    (r'\b(deal|deals|dealing|does|inflicts?|inflicting)\b.{0,40}(?<!more )(?<!less )(?<!extra )(?<!additional )(?<!double )damage(?! taken)|\bdamage to (the target.s )?(health|magicka|stamina)', 'DMG'),
    # control
    (r'paraly|immobili[sz]|in place\b|frozen solid|suspended in|petrif', 'control_paralysis'),
    (r'disarm', 'control_disarm'), (r'silenc', 'control_silence'), (r'\bblind', 'control_blind'),
    (r'\bput(s)? .{0,20}to sleep|\bsleep\b', 'control_sleep'),
    # influence
    (r'\bflee|\bfear|terrif|cower', 'influence_fear'),
    (r'\bcalm|pacif|(will not|won.t|cannot|stop) (fight|attack)|stop and resume', 'influence_calm'),
    (r'frenz|attack (anything|anyone|each other|their allies)|fight each other|turn on their', 'influence_frenzy'),
    (r'bends? the will|\bcommand\b|control (an?|the|target)\b|enslave|thrall|\bobey|fight for you', 'influence_command'),
    (r'courage|\brally', 'influence_rally'),
    (r'banish', 'influence_banish'),
    (r'stagger|knock(s|ing|ed)? (them |enemies |targets |all targets )?(down|back)|knockdown|knockback|send(s|ing)? .{0,20}flying|launch(es)? (enemies|targets|them)|push(es|ing)? (away|back)|lose (its|their) balance', 'control_stagger'),
    (r'\bslow(s|ed|ing)?\b(?! time)|movement speed (is )?reduc|reduc\w* (its |their |the target.s )?movement speed|hinder', 'control_slow'),
    # stealth / vision
    (r'invisib|unseen|become(s)? (hidden|unseen)|cannot detect sneaking', 'stealth_invisibility'),
    (r'movement noise|quieter|muffl|silent(ly)? (move|step)|\blures?\b|distract', 'stealth_muffle'),
    (r'sneak attacks?', 'fortify_combat_sneak_attack'),
    (r'sneak(ing)? (is |effectiveness)', 'fortify_skill_sneak'),
    (r'\blocate|lead you to the nearest|guides? you|survey the area|nearest .{0,20}(ore|door|container)', 'vision_clairvoyance'),
    (r'night ?eye|see (better )?in the dark|darkvision', 'vision_night_eye'),
    (r'detect (life|dead)|seen through walls|see (through walls|enemies|nearby|living)|\breveal', 'vision_detect_life'),
    (r'hovering light|create(s)? (a )?(ball of )?light\b|illuminat', 'vision_light'),
    # movement
    (r'fall(ing)? damage|damage when falling|when falling|feather ?fall|levitat|\bfloat|thin air|jump(s|ing)? (higher|further)', 'movement_jump_fall'),
    (r'(move|moving|run|running|sprint\w*)\b.{0,25}faster|movement speed (is )?increas|increases? movement speed|\bdash\b|haste', 'movement_speed'),
    (r'telekine', 'utility_telekinesis'),
    # summon
    (r'reanimat|raises? (a |the |up to N )?(dead|corpse|zombie)|brings? a dead', 'summon_reanimate'),
    (r'summon(?! a (physical )?wall)|conjur|manifest|illusions? of|\bclone\b|bound (weapon|sword|bow|dagger|axe|quiver|armor)|magic quiver|binds? a (daedric|bound)|ghost to attack', 'summon_creature'),
    # defence
    (r'absorb (N% of )?(hostile )?spells|spell absorption|absorb N% of the magicka from incoming', 'defense_spell_absorb'),
    (r'reflect', 'defense_reflect'),
    (r'\bwards?\b', 'defense_ward'),
    (r'(reduc|lower)\w* (its |their |the target.s |enemy |target.s )?armor\b|armor (rating )?(is )?reduced', 'weakness_armor'),
    (r'armor rating|\bdefense\b|damage reduction|reduc\w* (all |incoming |physical )?damage taken|take(s)? (only )?(N% )?(half|less) (physical )?damage|half damage|invulner', 'defense_armor'),
    (r'magic resist\w*|resist\w* (to )?magic', 'MAGRES'),
    (r'weak(er|ness)? to ' + ELEM + r'|' + ELEM + r' resist\w* (is |by )?(reduc|lower)', 'WEAK'),
    (r'resist (to )?' + ELEM + r'|' + ELEM + r' resist', 'RES'),
    # vitals
    (r'absorb\w*\b.{0,30}\b' + V + r'|' + V + r'.{0,15}absorbed|steals? .{0,20}' + V, 'absorb_V'),
    (r'(regenerat\w*|regen)\b.{0,20}' + V + r'|' + V + r' regen', 'regen_V'),
    (r'\b(restor\w*|replenish\w*|regain\w*|heal|heals|healed|healing)\b.{0,40}' + V + r'|\bheals? (you|the caster|N|by)|\bheal(s|ing)?\b', 'restore_V'),
    (r'(increase\w*|fortif\w*) (your )?(maximum )?' + V + r'|' + V + r' (is )?increased', 'fortify_vital_V'),
    # combat
    (r'critical (hit )?(chance|damage)', 'fortify_combat_crit'),
    (r'power attack\w* (stamina )?cost', 'fortify_combat_power_attack'),
    (r'armor penetration', 'fortify_combat_armor_penetration'),
    (r'(attack|swing)\w*\b.{0,20}(faster|twice as fast)|attack speed|weapon speed', 'SPEED'),
    (r'prices? (are|is) (N% )?better|buying and selling', 'fortify_skill_speech'),
    (SKILLW + r' (spells )?(cost|are|is|do|deal)', 'SKILL'),
    # damage last
    (r'(damage|drain)\w*\b.{0,40}\bmagicka\b(?!.{0,40}health)|magicka damage', 'damage_magicka'),
    (r'(damage|drain)\w*\b.{0,40}\bstamina\b(?!.{0,40}health)|stamina damage', 'damage_stamina'),
    # spell power ("Sonic spells do 100% more damage") has no column: stop here, unmapped
    (r'\bspells? (do|deal|are) (N% )?(more|stronger|less)|spell ?power|spells? (are|is) N% (stronger|more powerful)', 'NONE'),
    (r'(more|extra|increased|additional|double) (attack |melee |physical |weapon )?damage|attack damage|damage (done )?(is )?increased|increases? damage', 'ATKDMG'),
    (r'\b(deal|deals|dealing|does|do|inflicts?|causing|cause)\b.{0,40}damage|damage (to|per second|each|equal)|\bN damage\b|\bkills?\b|destroys? (all|living)|burn(s|ing)? (those|the target|enemies)|comets?|slash wave|skewer|bleed', 'damage_health'),
]
ELEM_WORDS = [(r'\bfire\b|flame|burn|scorch|blaz', 'fire'), (r'frost|\bice\b|\bcold\b|freez|hail|blizzard', 'frost'),
              (r'shock|lightning|electric|spark', 'shock'), (r'\bsun\b|sunlight|holy', 'sun'),
              (r'poison|venom|toxic|disease', 'poison'), (r'physical|bleed|slash|spike', 'physical')]
NEG = re.compile(r'\b(reduc|lower|weaken|decreas)\w*')


def norm(text):
    t = text.lower()
    t = re.sub(r'<[^>]*>', 'N', t)          # <mag>, <dur>, <25>, <Global=...>
    t = re.sub(r'\b\d+(\.\d+)?', 'N', t)
    return t


def desc_element(text):
    t = norm(text)
    for p, el in ELEM_WORDS:
        if re.search(p, t):
            return el
    return 'magic'


def _match(t):
    for p, spec in DESC_TABLE:
        m = re.search(p, t)
        if not m:
            continue
        groups = [g for g in m.groups() if g]
        neg = bool(NEG.search(t))
        if spec == 'NONE':
            return None, None
        if spec == 'DMG':
            after = t[m.start():]
            vit = [w for w in ('health', 'magicka', 'stamina') if re.search(r'\b' + w + r'\b', after)]
            if not vit or 'health' in vit:
                return 'damage_health', None
            return 'damage_' + vit[0], None
        if spec.endswith('_V'):
            v = next((g for g in groups if g in ('health', 'magicka', 'stamina')), 'health')
            return spec[:-1] + v, None
        if spec == 'MAGRES':
            return ('weakness_magic' if neg else 'resist_magic'), None
        if spec in ('WEAK', 'RES'):
            el = next(g for g in groups if g in ('fire', 'frost', 'shock', 'poison'))
            return ('weakness_' if spec == 'WEAK' else 'resist_') + el, None
        if spec == 'SPEED':
            return ('weaken_combat_weapon_speed' if neg else 'fortify_combat_weapon_speed'), None
        if spec == 'ATKDMG':
            weak = re.search(r'(reduc|lower|weaken)\w* .{0,30}damage', t)
            return ('weaken_combat_attack_damage' if weak else 'fortify_combat_attack_damage'), None
        if spec == 'SKILL':
            sk = SKILL_ID[m.group(1)]
            return 'fortify_skill_' + sk, sk
        return spec, None
    return None, None


def desc_spec(text):
    """Description -> (spec, skill) or (None, None). The first sentence carries the
    primary effect, so it is matched alone first, then the whole text."""
    if not text or not text.strip():
        return None, None
    t = norm(text)
    first = re.split(r'(?<=[.!;])\s', t.strip(), maxsplit=1)[0]
    spec, sk = _match(first)
    if spec is None and first != t.strip():
        spec, sk = _match(t)
    return spec, sk


def classify_desc(text):
    return desc_spec(text)[0]


def classify_desc_v1(text):
    t = text.lower()
    for p, spec in DESC_TABLE_V1:
        if re.search(p, t):
            return spec
    return None

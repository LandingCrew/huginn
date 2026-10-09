"""Build tests/core/fixtures/effects_<list>.csv from three hg dump all CSVs.

Expected values come from the Python reference extractor (tools/effects/reference/),
adapted to effects.csv column names, plus the C++ mapper's documented
deviations (EffectRules.h), which are applied here as explicit oracle rules and
listed per row in `expectNote`. The C++ output is NOT used to make expectations.

usage: python -I tools/effects/make_fixtures.py <repo> vanilla=<dump> simonrim=<dump> lorerim=<dump>
R2's inputs (0.23.12 test-mode dumps, all with the catalog view and effectLightRadius):
vanilla = Huginn_All_vanilla_r4.csv (vanilla+ profile, 2026-10-08 23:51); simonrim =
Huginn_All_simonrim_r4.csv (Simonrim Essentials, 2026-10-08 23:52); lorerim =
Huginn_All_lorerim_r2.csv (LoreRim-5 Ultra, the R2 gate run of 2026-10-09 00:05). Dumps are user
data, not in the repo.
"""
import sys, csv, re, random, collections
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "reference"))
import warnings; warnings.filterwarnings('ignore')
import pandas as pd
import base, cap
cap.DESC_ON = True
# The reference's one byte bug (a backspace where \b was meant): fixed as the C++ does.
cap.NAME_TABLE[1] = (r'^fortify (health|magicka|stamina)\b', 'fortify_vital_X')
# Deviation: the misspelt "Regneration" (Simonrim) reads as regen, as in the C++.
cap.NAME_TABLE[0] = (r'fortify (health|magicka|stamina) rege?nerat|rege?nerat|fortify heal rate', 'regen_X')
# Deviation (verifier round 1, N3): helper names no longer swallow "Blank Slate" or
# Requiem's "Dispel Soul Gems"; the latter is not a dispel either (unmapped by name).
cap.HELPER = re.compile(cap.HELPER.pattern.replace('^blank|', '^blank( effect)?$|').replace(
    'dispel (cloak|size|jump|soul gems)', 'dispel (cloak|size|jump)'))
_i = next(i for i, (pat, sp) in enumerate(cap.NAME_TABLE) if sp == 'cure_dispel')
cap.NAME_TABLE.insert(_i, (r'dispel soul gems', None))
# Deviations (verifier round 2): the description table's soul_trap rule no longer takes a
# bare "soul gem"; "decreased" weakens like "reduced"; the NONE rule (spell power) is
# made visible to the oracle as a sentinel (it maps to no column either way).
import desc as _desc
for _k, (_pat, _sp) in enumerate(_desc.DESC_TABLE):
    if _sp == 'soul_trap':
        _desc.DESC_TABLE[_k] = (_pat.replace(r'soul gems?\b|', r'fills? (a |the )?soul ?gems?|vulnerable to soul ?gems?|soul ?gems? on death|'), _sp)
    elif _sp == 'WEAK':
        _desc.DESC_TABLE[_k] = (_pat.replace('(reduc|lower)', '(reduc|lower|decreas)'), _sp)
    elif _sp == 'NONE':
        _desc.DESC_TABLE[_k] = (_pat, 'NONEHIT')


def _idx(spec, contains=''):
    return next(k for k, (pt, sp) in enumerate(_desc.DESC_TABLE) if sp == spec and contains in pt)


def _set(spec, pat, contains=''):
    k = _idx(spec, contains)
    _desc.DESC_TABLE[k] = (pat, spec)


# Deviations (verifier round 3): the description table as the C++ has it (EffectRules.h).
_set('utility_slow_time', r'time slows|slows? (down )?time|slow time|world (around you )?(seems to )?slow')
_k = _idx('DMG')
_desc.DESC_TABLE[_k:_k] = [(r'(tosses|spawns|throws|drops) (a|an) .{0,40}spider', 'summon_creature'),
                          (r'temporary damage', 'drain_vital_health'),
                          (r'imbues? .{0,30}weapons? with', 'damage_health')]
_k = _idx('summon_creature', 'conjur')
_desc.DESC_TABLE[_k:_k + 1] = [(r'bound (weapon|sword|bow|dagger|axe|quiver|armor|shield)|magic quiver|binds? (a|an) (daedric|bound|quiver|shield|\w+-shaped)', 'summon_bound_weapon'),
                              (r'summon(?! a (physical )?wall)|conjur|manifest|illusions? of|\bclone\b|ghost to attack', 'summon_creature')]
_k = _idx('influence_command')
_desc.DESC_TABLE.insert(_k, (r'summon (a|an) (?!(physical )?wall)', 'summon_creature'))
_k = _idx('control_stagger')
_desc.DESC_TABLE[_k] = (_desc.DESC_TABLE[_k][0] + '|flung (away|back)', 'control_stagger')
_k = _idx('weakness_armor')
_desc.DESC_TABLE[_k] = (_desc.DESC_TABLE[_k][0] + '|^take(s)? (double|twice the|N% more|more) damage', 'weakness_armor')
_desc.DESC_TABLE.insert(_k + 1, (r'(avoid|resist|reduc\w*|ignore\w*) (all )?damage from (ranged|arrows|projectiles)|ranged (attacks?|weapons?) (deal|do) (N% )?less', 'defense_resist_ranged'))
_k = _idx('defense_armor', 'armor rating')
_desc.DESC_TABLE[_k] = (_desc.DESC_TABLE[_k][0] + '|ignores? N% of (all )?(physical )?damage|chance to (take no|avoid) damage', 'defense_armor')
_k = _idx('absorb_V')
_desc.DESC_TABLE.insert(_k, (r'healing (effects )?(are |is )?(reduced|halved)|reduc\w* (all )?healing', 'NONEHIT'))
_set('fortify_combat_power_attack', r'power attack\w* (stamina )?cost|power attacks? (will )?(deal|do)')
_k = _idx('NONEHIT', 'spell ?power')
_none = _desc.DESC_TABLE.pop(_k)
_desc.DESC_TABLE.insert(_idx('fortify_skill_speech') + 1, _none)
assert any('vulnerable to soul' in pt for pt, sp in _desc.DESC_TABLE)
assert any('decreas' in pt for pt, sp in _desc.DESC_TABLE if sp == 'WEAK')
_CATCHALL = cap.NAME_TABLE[-3][0]  # the generic damage rule ("disintegrat|damage health|...")
assert _CATCHALL.startswith('disintegrat'), _CATCHALL
_VM = {'ValueModifier', 'PeakValueModifier', 'DualValueModifier', 'ValueAndParts', 'AccumulateMagnitude'}


def script_like(r):
    """The C++'s script-like test (Script archetype, or a value modifier on a reused AV)."""
    a, av = r['arch'], r['primaryAV']
    return a == 'Script' or ((a in _VM or a == 'Absorb') and cap.SUSPECT_AV.search(av or '-') and
                             av not in ('Confidence', 'Aggression')) or (a in _VM and av == '')


def desc_number(text):
    """(value, percent): the first number a description states in a literal tag (<80>),
    and whether "%" or "percent" follows it; (0, False) if none."""
    for m in re.finditer(r'<([0-9][0-9.]*)>( *(%|percent))?', text or '', re.I):
        try:
            v = float(m.group(1))
        except ValueError:
            continue
        if v > 0 and m.group(1).count('.') <= 1:
            return v, bool(m.group(2))
    return 0.0, False


def desc_none(text):
    return cap.desc_spec(text)[0] == 'NONEHIT'


def desc_col(r):
    """The description route, as the C++ takes it for a row nothing else maps."""
    ds, _ = cap.desc_spec(r['effectDescription'])
    if not ds or ds == 'NONEHIT':
        return ''
    if r['det']:
        for a, b in (('resist_', 'weakness_'), ('fortify_combat_', 'weaken_combat_'), ('defense_armor', 'weakness_armor')):
            if ds.startswith(a):
                ds = b + ds[len(a):]
    return to_col(ds, cap.desc_element(r['effectDescription']) if ds.startswith('damage') else None)

repo = sys.argv[1]


def check_desc_table():
    """The mirrored description table must equal EffectRules.cpp's DescTable, rule for rule
    (the C++ writes NONE where the oracle writes NONEHIT)."""
    src = open(repo + '/src/core/EffectRules.cpp', encoding='utf-8').read()
    block = src[src.index('const Table& DescTable()'):src.index('const Table& DescElement()')]
    cpp = [(m.group(1), m.group(2)) for m in re.finditer(r'\{ R"re\((.*?)\)re", "([A-Za-z_]+)" \}', block)]
    py = [(pt, 'NONE' if sp == 'NONEHIT' else sp) for pt, sp in _desc.DESC_TABLE]
    if cpp != py:
        for k in range(max(len(cpp), len(py))):
            a = cpp[k] if k < len(cpp) else None
            b = py[k] if k < len(py) else None
            if a != b:
                print('description table differs at rule', k, '\n  C++   ', a, '\n  oracle', b)
                break
        sys.exit('the oracle description table is not the C++ one')


check_desc_table()
cols = [r for r in csv.DictReader(open(repo + '/docs/architecture/9-data/effects.csv', encoding='utf-8'))]
colset = {r['id'] for r in cols}
fams = [r['id'] for r in cols if r['level'] == 'family']
family_of_col = {r['id']: (r['family'] if r['level'] == 'specific' and r['family'] in fams else '') for r in cols}

BAD = re.compile(r'\b(dummy|test|testing|donotuse|do not use|unused|placeholder|deleted)\b|^zz|donotuse|takeme')


def to_col(spec, el):
    if not spec:
        return ''
    if spec == 'damage_health':
        return 'damage_health_' + (el or 'magic')
    if spec == 'summon_creature':
        return 'summon_creature_' + el if el in ('fire', 'frost', 'shock') else 'summon_creature'
    if spec.startswith('drain_skill'):
        return 'drain_skill'
    special = {'control_damage_weapon': 'weaken_combat_attack_damage', 'survival_cold_exposure': 'survival',
               'shout_dragonrend': 'shout', 'cure_addiction': 'cure'}
    if spec in special:
        return special[spec]
    if spec in colset:
        return spec
    best = ''
    for f in fams:
        if spec.startswith(f + '_') and len(f) > len(best):
            best = f
    return best


def oracle(r):
    """-> (column, note) for one effect row (reference + documented deviations)."""
    n = r['effectName'].lower()
    kw = r['effectKeywords']
    # Deviation (round 3): a Light effect that makes darkness is not light.
    if r['arch'] == 'Light' and 'dark' in n:
        return '', 'unmapped', 'dev:darkness'
    # Deviation (round 2): Requiem's "Dispel Soul Gems" gets no column at all.
    if 'dispel soul gems' in n:
        return '', 'unmapped', 'dev:dispel-soul-gems'
    # Deviation (round 2): a Slowfall keyword on SpeedMult is jump/fall.
    if r['arch'] in _VM and r['primaryAV'] == 'SpeedMult' and not r['det'] and 'Slowfall' in kw:
        return 'movement_jump_fall', 'data', 'dev:slowfall'
    spec, src, el, sk = cap.classify(r)
    if spec == 'NONEHIT':
        spec, src = None, 'unmapped'
    note = ''
    # Deviation (round 2): on a script effect, the generic damage name rule yields to the
    # description when it says something (NONE -> no column).
    if src == 'name' and spec == 'damage_health' and script_like(r):
        hit = next((sp for pt, sp in cap.NAME_TABLE if re.search(pt, n)), None)
        if hit == 'damage_health' and re.search(_CATCHALL, n) and \
                not any(re.search(pt, n) for pt, sp in cap.NAME_TABLE[:-3] if sp) and \
                not re.search(r'magicka damage|damage magicka|stamina damage|damage stamina', n):
            ds, _ = cap.desc_spec(r['effectDescription'])
            if ds == 'NONEHIT':
                return '', 'unmapped', 'dev:name-catchall-desc'
            if ds:
                return desc_col(r), 'desc', 'dev:name-catchall-desc'
    # deviation: scripted summons get no element (keyword route); data summons keep it
    if spec == 'summon_creature' and src == 'keyword':
        el = None
    c = to_col(spec, el)
    # Deviation (N6): a Cloak/hazard is damage only by a MagicDamage keyword, not by
    # its resisted actor value alone; otherwise it is a wrapper (payload / description).
    if r['arch'] in ('Cloak', 'SpawnHazard') and src == 'data' and 'MagicDamage' not in kw:
        return '', 'wrapper', 'dev:wrapper-not-resist'
    # Deviation (S5): a "damage" name needs detrimental=1 (script effects exempt) and
    # no "resist" ("Resist Magicka Damage"); else the description route decides.
    if src == 'name' and c.startswith('damage') and (re.search(r'\bresist', n) or (not r['det'] and r['arch'] != 'Script')):
        return desc_col(r), 'desc', 'dev:name-damage-needs-detrimental'
    if src == 'name' and spec == 'utility_slow_time' and 'ethereal' in n:
        c, note = 'defense_ethereal', 'dev:ethereal'
    if re.search(r'[a-z] polymorph|shapeshift: ', n) and src in ('name', 'unmapped', 'desc'):
        if r['arch'] == 'Script' or src == 'name':
            c, note = 'stealth_disguise', 'dev:disguise'
    elif spec == 'transform_werewolf' and src == 'name' and re.search(r'polymorph|shapeshift', n):
        # no name rule left: the description route, as for anything unmapped
        ds, _ = cap.desc_spec(r['effectDescription'])
        if ds and r['det']:
            for a, b in (('resist_', 'weakness_'), ('fortify_combat_', 'weaken_combat_'), ('defense_armor', 'weakness_armor')):
                if ds.startswith(a):
                    ds = b + ds[len(a):]
        c = to_col(ds, cap.desc_element(r['effectDescription']) if ds and ds.startswith('damage') else None) if ds else ''
        note = 'dev:polymorph-name-dropped'
    if spec == 'transform_werewolf' and src == 'name' and re.search(r'vampire form', n):
        c, note = 'transform_vampire_lord', 'dev:vampire-form'
    if r['primaryAV'] == 'AlchemySkillAdvance' and 'MagicEnchFortifyAlchemy' in kw:
        c, note = 'meta_potion_duration', 'dev:potion-duration'
    if src == 'name' and spec == 'damage_health' and re.search(r'magicka damage|damage magicka', n):
        c, note = 'damage_magicka', 'dev:name-damage-magicka'
    if src == 'name' and spec == 'damage_health' and re.search(r'stamina damage|damage stamina', n):
        c, note = 'damage_stamina', 'dev:name-damage-stamina'
    return c, src, note


def in_scope(item, rows):
    k = item['kind']
    if BAD.search(item['name'].lower()):
        return False
    num = lambda v: float(v) if v not in ('', None) else 0.0
    if k == 'Spell':
        if item['spellType'] != '0' or item['castingType'] not in ('1', '2'):
            return False
        if not num(item['magickaCost']) < 5000:
            return False
        if not any(not rr['hide'] for rr in rows):
            return False
        if 'taughtByTome' in item and item['taughtByTome'] != '':
            return item['taughtByTome'] == '1'
        return num(item['magickaCost']) > 0.5
    if k == 'Scroll':
        return num(item['value']) > 0
    if k in ('Potion', 'Poison', 'Food'):
        return item['playable'] == '1'
    if k == 'Weapon':
        return item['playable'] == '1' and num(item['value']) > 0 and item['weaponType'] != '0'
    if k in ('Ammo', 'Armor'):
        return item['playable'] == '1' and num(item['value']) > 0
    return k in ('SoulGem', 'Light')


HW = set(cap.HIDDEN_WHITELIST_FAMILIES)


def famkey(c):
    if not c:
        return ''
    f = family_of_col.get(c, '')
    return f if f else c


def name_col(name):
    """The C++ NameColumn: the name table's column (element dropped)."""
    ns = cap.name_spec(name)[0]
    return to_col(ns, None) if ns else ''


def keep_hidden(col, name):
    fk = famkey(col)
    # the C++ whitelist is by family column: the reference's family_of names map 1:1
    if col == 'stealth_invisibility' or not col:
        return False
    if fk not in HW:
        return False
    if cap.HELPER.search(name):
        return False
    ns = cap.name_spec(name)[0]
    if ns:
        nf = famkey(to_col(ns, None))
        if nf and nf != fk and not ({nf, fk} <= {'damage', 'absorb'}):
            return False
    return True


# =============================================================================
# Expected VALUES (verifier round 1, S1): a second implementation of the value
# rules written down in src/core/EffectMapper.h and effects.csv's `how` column,
# over the fixture's own in-scope items (the C++ test grades the same population).
# =============================================================================
import math

AMOUNT = ('restore_', 'damage_', 'absorb_')
LEVEL = ('fortify_vital_', 'regen_', 'drain_vital_', 'weaken_regen_', 'resist_', 'weakness_', 'fortify_skill_',
         'fortify_combat_', 'weaken_combat_', 'defense_')
SPECIAL = {
    'defense_ethereal': 'PD', 'control_paralysis': 'PD', 'control_stagger': 'PG', 'control_slow': 'PGD',
    'control_disarm': 'P', 'control_grab': 'P', 'control_silence': 'PD', 'control_sleep': 'PD', 'control_blind': 'PD',
    'summon_reanimate': 'PG', 'vision_light': 'PArea', 'vision_detect_life': 'PArea', 'vision_night_eye': 'PD',
    'vision_clairvoyance': 'P', 'movement_speed': 'Level', 'movement_jump_fall': 'PD', 'movement_water_walking': 'P',
    'utility_carry_weight': 'Level', 'utility_water_breathing': 'PD', 'utility_telekinesis': 'P', 'utility_unlock': 'PG',
    'utility_slow_time': 'PD', 'utility_teleport': 'P', 'utility_transmute': 'P', 'utility_size': 'P',
    'shout_recovery': 'Level', 'soul_trap': 'PD', 'survival_hunger': 'Hunger', 'survival_thirst': 'Thirst',
    'survival_warmth': 'Level', 'survival_fatigue': 'P', 'survival_intoxication': 'P', 'meta_potion_duration': 'Level',
    'drain_skill': 'Level'}


def rule_of(c):
    if c in fams:
        return 'P'
    if c in SPECIAL:
        return SPECIAL[c]
    if c.startswith(AMOUNT):
        return 'Amount'
    if c.startswith(LEVEL):
        return 'Level'
    if c.startswith('influence_'):
        return 'PGD'
    if c.startswith(('summon_', 'stealth_')):
        return 'PD'
    return 'P'


def clip_dur(d):
    return 3600.0 if d >= 86400 else min(float(d), 3600.0)


def dfac(d):
    return 1.0 if d == 0 else math.log1p(clip_dur(d)) / math.log1p(3600.0)


def hunger_size(name, kws):
    for k in kws.split(';'):
        if k.startswith('CCSM_RestoreHunger'):
            return {'Tiny': 0.25, 'Small': 0.5, 'Medium': 0.75, 'Large': 1.0}.get(k[18:], None) or 0.5
    n = name.lower()
    if 'very small' in n or 'tiny' in n:
        return 0.25
    for w, v in (('small', 0.5), ('medium', 0.75), ('large', 1.0)):
        if w in n:
            return v
    return 0.5


def row_quantity(col, kr, constant):
    """-> (raw, post, graded, full) for one kept row and one of its columns."""
    mag = abs(kr['mag'])
    d = kr['dur']
    rule = rule_of(col)
    raw, post, graded, full = 0.0, 1.0, False, False
    if rule == 'Amount':
        raw, graded = mag * max(clip_dur(d), 1.0), True
        full = col in ('restore_health', 'restore_magicka', 'restore_stamina') and kr['mag'] >= 9999
    elif rule == 'Level':
        raw, graded = (mag if (constant or d == 0) else mag * clip_dur(d) / 3600.0), True
    elif rule == 'PD':
        post = dfac(d)
    elif rule == 'PG':
        raw, graded = mag, True
    elif rule == 'PGD':
        raw, graded, post = mag, True, dfac(d)
    elif rule == 'PArea':
        if kr.get('light'):
            raw = float(kr.get('radius', 0)) if float(kr.get('radius', 0)) > 0 else mag
        else:
            raw = max(float(kr['area']), mag)
        graded = True
    elif rule == 'Hunger':
        post = hunger_size(kr['name'], kr['kws'])
    elif rule == 'Thirst':
        post = 1.0 if kr['hydrated'] else 0.5
    if graded and not raw > 0 and not full:
        raw = 0.0
        if kr['route'] != 'data':
            graded = False
    if kr.get('factor'):
        # an unknown strength (0.5) or a stated percentage (p/100): not graded
        raw, graded, post = 0.0, False, post * kr['factor']
    return raw, post, graded, full


def expected_values(items):
    """items: list of dicts {scope, constant, rows: [kept row dicts]} -> list of {col: value}."""
    def eligible_cols(it):
        vis = set()
        for kr in it['rows']:
            if kr['visible']:
                vis.update(kr['cols'])
        out = []
        for kr in it['rows']:
            for c in kr['cols']:
                if kr['visible'] or c not in vis:
                    out.append((kr, c))
        return out

    pops = collections.defaultdict(list)
    for it in items:
        if not it['scope']:
            continue
        best = {}
        for kr, c in eligible_cols(it):
            raw, post, graded, full = row_quantity(c, kr, it['constant'])
            if graded and not full and raw > 0:
                best[c] = max(best.get(c, 0.0), raw)
        for c, v in best.items():
            pops[c].append(v)
    for v in pops.values():
        v.sort()

    import bisect

    def pct(c, x):
        v = pops.get(c, [])
        if not v:
            return 1.0
        return max(bisect.bisect_right(v, x), 1) / len(v)

    results = []
    for it in items:
        vals = {}
        if it['scope']:
            heals_others = False
            for kr, c in eligible_cols(it):
                raw, post, graded, full = row_quantity(c, kr, it['constant'])
                if full:
                    v = 1.0
                elif graded and not raw > 0:
                    v = 1.0 / (len(pops.get(c, [])) + 1)
                elif graded:
                    v = pct(c, raw) * post
                else:
                    v = post
                vals[c] = max(vals.get(c, 0.0), v)
            for kr in it['rows']:
                if 'restore_health' in kr['cols'] and (kr['delivery'] != '0' or kr['area'] > 0):
                    heals_others = True
            if heals_others and vals.get('restore_health', 0) > 0:
                vals['restore_health_other'] = vals['restore_health']
            for c, v in list(vals.items()):
                f = family_of_col.get(c)
                if f:
                    vals[f] = max(vals.get(f, 0.0), v)
        results.append({c: v for c, v in vals.items() if v > 0})
    return results


FORCE = set("""
8D005E70 8D00633C 872819C4 000240D2 9215EFE9 04020960 040206DB 040206D9 FE762842 FE079842 5A008402
000CDB70 000E0CD6 87410740 8748263E 66032C8B 6603682F 89000A0D 301B7EF9 FE6448DE FE6448DF A8061994
A8061EFB 040275B7 FE715828 673727C3 8D005FD0 2804F8FC 8702F1C2 00028532 00027EB6 0009B2B2 00043323
00043324 220A23DE FE350801 0401CAB0 04027490
""".split())


def main():
    random.seed(20261008)
    out_dir = repo + '/tests/core/fixtures/'
    for arg in sys.argv[2:]:
        lst, path = arg.split('=', 1)
        base.DUMPS[lst] = path
        d = base.load(lst)
        if 'payloadOf' not in d.columns:
            d['payloadOf'] = ''
        d['rowno'] = range(len(d))
        payload_rows = d[d.payloadOf != '']
        direct = d[(d.payloadOf == '')]
        groups = {k: g for k, g in direct.groupby(['kind', 'formID'], sort=False)}
        # one oracle call per MGEF
        KEY = ['effectFormID', 'effectName', 'arch', 'primaryAV', 'resistAV', 'det', 'recover', 'effectKeywords', 'effectDescription']
        u = d[d.hasEff][KEY].drop_duplicates('effectFormID')
        orc = {}
        for _, r in u.iterrows():
            orc[r['effectFormID']] = oracle(r)
        desc_of = dict(zip(u.effectFormID, u.effectDescription))
        det_of = dict(zip(u.effectFormID, u.det))

        def wrapper_desc(fid):
            ds, _ = cap.desc_spec(desc_of.get(fid, ''))
            if not ds:
                return ''
            if det_of.get(fid):
                for a, b in (('resist_', 'weakness_'), ('fortify_combat_', 'weaken_combat_'), ('defense_armor', 'weakness_armor')):
                    if ds.startswith(a):
                        ds = b + ds[len(a):]
            el = cap.desc_element(desc_of.get(fid, '')) if ds.startswith('damage') else None
            return to_col(ds, el)

        # sample items
        by_col = collections.defaultdict(list)
        specials = []
        items_of_mgef = collections.defaultdict(list)
        for key, g in groups.items():
            for fid in g.effectFormID:
                if fid:
                    items_of_mgef[fid].append(key)
        for fid, (c, src, note) in orc.items():
            by_col[c or ('<' + src + '>')].append(fid)
            if note:
                specials.append(fid)
        chosen = set()
        for c, fids in by_col.items():
            random.shuffle(fids)
            per = 6 if c.startswith('<') else 3
            for fid in fids[:per]:
                chosen.add(fid)
        chosen |= set(specials)
        pick = []
        seen = set()
        scope_of = {}
        for key, g in groups.items():
            rows_ = [dict(r) for _, r in g.iterrows()]
            scope_of[key] = in_scope(g.iloc[0], rows_)
        for fid in sorted(chosen):
            cands = items_of_mgef.get(fid, [])
            cands = sorted(cands, key=lambda k: not scope_of[k])
            for key in cands[:1]:
                if key not in seen:
                    seen.add(key)
                    pick.append(key)
        # The items the verifier named in rounds 2-3 (unknown strengths, descriptions that
        # decide, the catch-all damage name), in every list that has them.
        for key in groups:
            fx = key[1] if isinstance(key[1], str) else '%08X' % int(key[1])
            if fx.upper() in FORCE and key not in seen:
                seen.add(key)
                pick.append(key)
        # plain gear and misc kinds, and some out of scope
        for kind, n in (('Weapon', 12), ('Armor', 12), ('Ammo', 6), ('SoulGem', 4), ('Light', 3)):
            ks = [k for k in groups if k[0] == kind and k not in seen]
            random.shuffle(ks)
            for k in ks[:n]:
                seen.add(k); pick.append(k)
        pick.sort(key=lambda k: groups[k].rowno.iloc[0])

        keep_cols = [c for c in d.columns if c in (
            'kind,formID,plugin,name,playable,value,weight,keywords,spellType,castingType,delivery,magickaCost,taughtByTome,'
            'weaponType,twoHanded,damage,speed,reach,critDamage,armorSlots,armorRating,armorType,soulCapacity,soulContained,'
            'lightRadius,enchantment,enchantmentCharge,effectIndex,effectFormID,effectName,archetype,primaryAV,secondaryAV,'
            'resistAV,effectDelivery,effectCasting,magnitude,duration,area,effectBaseCost,effectCost,detrimental,hostile,'
            'effectFlags,effectKeywords,effectDescription,ammoNonBolt,enchantCasting,effectPlugin,effectSchool,effectLightRadius,payloadOf').split(',')]
        # effectCost after base.load is the renamed base cost: write it back under the dump's own name
        hdr = list(pd.read_csv(path, nrows=0).columns)
        # base.load renamed effectBaseCost to effectCost when the dump had no per-effect cost (0.23.7)
        has_base = 'effectBaseCost' in hdr and 'effectCost' not in hdr
        rows_out = []
        stats = collections.Counter()
        value_items = []
        first_row_index = []
        for key in pick:
            g = groups[key]
            item = g.iloc[0]
            rows = [dict(r) for _, r in g.iterrows()]
            scope = in_scope(item, rows)
            # expected per row, and the item's effect columns
            item_cols = set()
            exp_rows = []
            kept_rows = []
            pending = []            # visible rows of unknown strength nothing reads: (index, no description)
            kept_visible = [False]  # does the item keep any visible row?
            hydrated = any('hydrat' in str(r['effectName']).lower() for r in rows if r['effectFormID'])
            num = lambda v: float(v) if v not in ('', None) and not (isinstance(v, float) and math.isnan(v)) else 0.0

            def zero_case(rr, c_, src_):
                """The C++'s ZeroCase for a data-route row of unknown strength: None (strength
                known), ('unmapped',), ('describe', col, number, percent), ('named',) or
                ('unresolved', desc_empty)."""
                if src_ not in ('data', 'keyword') or script_like(rr) or not c_:
                    return None
                rule = rule_of(c_)
                light = str(rr['archetype']) in ('12', 'Light')
                if rule == 'PArea':
                    known = num(rr.get('effectLightRadius', 0)) > 0 if light else (num(rr['area']) > 0 or num(rr['magnitude']) != 0)
                elif rule in ('Amount', 'Level', 'PG', 'PGD'):
                    known = num(rr['magnitude']) != 0
                else:
                    known = True
                if known:
                    return None
                text = rr['effectDescription'] if isinstance(rr['effectDescription'], str) else ''
                if desc_none(text):
                    return ('unmapped',)
                dc = desc_col(rr)
                if dc:
                    v, pct = desc_number(text)
                    return ('describe', dc, v, pct)
                ns = name_col(rr['effectName'])
                if ns and famkey(ns) == famkey(c_):
                    return ('named',)
                return ('unresolved', not text.strip())

            def zero_kept(rr, z, c_):
                """(column, route, magnitude override or None, factor or None) of a describe/named zero."""
                col = z[1] if z[0] == 'describe' else c_
                route = 'desc' if z[0] == 'describe' else 'name'
                mag, factor = None, None
                if rule_of(col) in ('Amount', 'Level', 'PG', 'PGD', 'PArea'):
                    if z[0] == 'describe' and z[2] > 0 and not z[3]:
                        mag = z[2]
                    elif z[0] == 'describe' and z[2] > 0:
                        factor = min(z[2] / 100.0, 1.0)
                    else:
                        factor = 0.5
                return col, route, mag, factor

            def kept_row(rr, cols_, visible, route, mag=None, factor=None):
                kept_rows.append({'cols': cols_, 'mag': num(rr['magnitude']) if mag is None else mag,
                                  'factor': factor, 'light': str(rr['archetype']) in ('12', 'Light'),
                                  'dur': int(num(rr['duration'])),
                                  'area': int(num(rr['area'])), 'radius': num(rr.get('effectLightRadius', 0)),
                                  'delivery': str(rr['effectDelivery']),
                                  'visible': visible, 'route': route, 'name': str(rr['effectName']),
                                  'kws': str(rr['effectKeywords']), 'hydrated': hydrated})
            for r in rows:
                fid = r['effectFormID']
                if not fid:
                    exp_rows.append(('', ''))
                    continue
                c, src, note = orc[fid]
                if src == 'wrapper':
                    pls = payload_rows[(payload_rows.kind == key[0]) & (payload_rows.formID == key[1]) & (payload_rows.payloadOf == r['effectIndex'])]
                    if len(pls) == 0 and not r['hide']:
                        wc = wrapper_desc(fid)
                        if wc:
                            c, note = wc, 'dev:wrapper-description'
                            item_cols.add(wc)
                            kept_row(r, [wc], True, 'desc')
                            kept_visible[0] = True
                    elif not r['hide']:
                        for _, p in pls.iterrows():
                            pc, psrc, _ = orc[p['effectFormID']]
                            if pc and psrc != 'helper':
                                pd_ = dict(p)
                                z = zero_case(pd_, pc, psrc)
                                proute = 'data' if (psrc in ('data', 'keyword') and not script_like(pd_)) else psrc
                                if z and z[0] in ('unmapped', 'unresolved'):
                                    continue
                                pmag, pfac = None, None
                                if z:
                                    pc, proute, pmag, pfac = zero_kept(pd_, z, pc)
                                item_cols.add(pc)
                                if pc.startswith('summon_creature_'):
                                    item_cols.add('summon_creature')
                                kept_row(pd_, [pc] + (['summon_creature'] if pc.startswith('summon_creature_') else []),
                                         True, proute, pmag, pfac)
                                kept_visible[0] = True
                    exp_rows.append((c, note))
                    continue
                if not c or src == 'helper':
                    exp_rows.append((c, note))
                    continue
                kept = (not r['hide']) or keep_hidden(c, r['effectName'])
                z = zero_case(r, c, src) if kept else None
                route = 'data' if (src in ('data', 'keyword') and not script_like(r)) else src
                zmag, zfac = None, None
                if z and z[0] == 'unmapped':
                    kept = False
                    if not r['hide']:
                        c = ''
                    note = (note + ' ' if note else '') + 'zero:unmapped'
                elif z and z[0] == 'unresolved':
                    kept = False
                    if not r['hide']:
                        pending.append((len(exp_rows), z[1]))
                    note = (note + ' ' if note else '') + ('zero:hidden-dropped' if r['hide'] else 'zero:unresolved')
                elif z:
                    note = (note + ' ' if note else '') + 'zero:' + z[0] + ('-pct' if z[0] == 'describe' and z[3] and z[2] > 0 else '')
                    c, route, zmag, zfac = zero_kept(r, z, c)
                exp_rows.append((c, note))
                if kept:
                    cols_ = [c]
                    if c.startswith('summon_creature_'):
                        cols_.append('summon_creature')
                    if c == 'fortify_skill_lockpicking' and r['primaryAV'] == 'PickPocketSkillAdvance':
                        cols_.append('fortify_skill_pickpocket')
                    kept_row(r, cols_, not r['hide'], route, zmag, zfac)
                    if not r['hide']:
                        kept_visible[0] = True
                    item_cols.add(c)
                    if c.startswith('summon_creature_'):
                        item_cols.add('summon_creature')
                    if c == 'fortify_skill_lockpicking' and r['primaryAV'] == 'PickPocketSkillAdvance':
                        item_cols.add('fortify_skill_pickpocket')
                    if c == 'restore_health' and (r['effectDelivery'] != '0' or (r['area'] not in ('', '0'))):
                        item_cols.add('restore_health_other')
            # Deviation (round 3): a carrier only as a companion -- no description, next to a
            # visible row the item keeps; otherwise counted as unmapped.
            for idx, empty in pending:
                c0, note0 = exp_rows[idx]
                if kept_visible[0] and empty:
                    exp_rows[idx] = (c0, note0.replace('zero:unresolved', 'zero:carrier'))
                else:
                    exp_rows[idx] = ('', note0.replace('zero:unresolved', 'zero:unmapped'))
            full = set(item_cols)
            for c in item_cols:
                if family_of_col.get(c):
                    full.add(family_of_col[c])
            stats['items'] += 1
            stats['inScope'] += scope
            ench_cast = str(item.get('enchantCasting', ''))
            constant = item['kind'] == 'Armor' or str(item['castingType']) == '0' or ench_cast == '0'
            value_items.append({'scope': scope, 'constant': constant, 'rows': kept_rows})
            first_row_index.append(len(rows_out))
            for i, r in enumerate(rows):
                o = {c: ('' if pd.isna(r.get(c, '')) else r.get(c, '')) for c in keep_cols}
                if has_base:
                    o['effectBaseCost'] = r['effectCost']
                o['expectColumn'] = exp_rows[i][0]
                o['expectNote'] = exp_rows[i][1]
                o['expectInScope'] = ('1' if scope else '0') if i == 0 else ''
                o['expectEffects'] = ';'.join(sorted(full)) if i == 0 else ''
                rows_out.append(o)
                stats['rows'] += 1
            # payload rows of this item, after the item's rows
            if 'payloadOf' in d.columns:
                pls = payload_rows[(payload_rows.kind == key[0]) & (payload_rows.formID == key[1])]
                for _, p in pls.iterrows():
                    o = {c: ('' if pd.isna(p.get(c, '')) else p.get(c, '')) for c in keep_cols}
                    if has_base:
                        o['effectBaseCost'] = p['effectCost']
                    o.update(expectColumn=orc[p['effectFormID']][0], expectNote='payload', expectInScope='', expectEffects='')
                    rows_out.append(o)
        for vals, at in zip(expected_values(value_items), first_row_index):
            rows_out[at]['expectValues'] = ';'.join(f'{c}={v:.6g}' for c, v in sorted(vals.items()))
        header = [c for c in keep_cols if not (has_base and c == 'effectCost')] + (['effectBaseCost'] if has_base else []) + \
            ['expectColumn', 'expectNote', 'expectInScope', 'expectEffects', 'expectValues']
        if not has_base:
            header = [h for h in header]
        # payload rows must follow their wrapper row: reorder per item
        final = []
        by_item = collections.OrderedDict()
        for o in rows_out:
            by_item.setdefault((o['kind'], o['formID']), []).append(o)
        for k, lst_rows in by_item.items():
            directs = [o for o in lst_rows if not o.get('payloadOf')]
            pays = [o for o in lst_rows if o.get('payloadOf')]
            for o in directs:
                final.append(o)
                final.extend(p for p in pays if p['payloadOf'] == o['effectIndex'] and o['effectFormID'])
        with open(out_dir + f'effects_{lst}.csv', 'w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, fieldnames=header, extrasaction='ignore')
            w.writeheader()
            for o in final:
                w.writerow(o)
        print(lst, dict(stats), 'notes', collections.Counter(o['expectNote'] for o in final if o['expectNote']))


main()

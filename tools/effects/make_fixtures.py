"""Build tests/core/fixtures/effects_<list>.csv from three hg dump all CSVs.

Expected values come from the Python reference extractor (tools/effects/reference/),
adapted to effects.csv column names, plus the C++ mapper's documented
deviations (EffectRules.h), which are applied here as explicit oracle rules and
listed per row in `expectNote`. The C++ output is NOT used to make expectations.

usage: python -I tools/effects/make_fixtures.py <repo> vanilla=<dump> simonrim=<dump> lorerim=<dump>
(R2 used: vanilla+ 0.23.10 test-mode dump, Simonrim 2026-10-07 dump, LoreRim 2026-10-07 v2 dump.)
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

repo = sys.argv[1]
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
    spec, src, el, sk = cap.classify(r)
    note = ''
    n = r['effectName'].lower()
    kw = r['effectKeywords']
    # deviation: scripted summons get no element (keyword route); data summons keep it
    if spec == 'summon_creature' and src == 'keyword':
        el = None
    c = to_col(spec, el)
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
            'effectFlags,effectKeywords,effectDescription,ammoNonBolt,enchantCasting,effectPlugin,effectSchool,payloadOf').split(',')]
        # effectCost after base.load is the renamed base cost: write it back under the dump's own name
        hdr = list(pd.read_csv(path, nrows=0).columns)
        # base.load renamed effectBaseCost to effectCost when the dump had no per-effect cost (0.23.7)
        has_base = 'effectBaseCost' in hdr and 'effectCost' not in hdr
        rows_out = []
        stats = collections.Counter()
        for key in pick:
            g = groups[key]
            item = g.iloc[0]
            rows = [dict(r) for _, r in g.iterrows()]
            scope = in_scope(item, rows)
            # expected per row, and the item's effect columns
            item_cols = set()
            exp_rows = []
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
                    elif not r['hide']:
                        for _, p in pls.iterrows():
                            pc, psrc, _ = orc[p['effectFormID']]
                            if pc and psrc != 'helper':
                                item_cols.add(pc)
                                if pc.startswith('summon_creature_'):
                                    item_cols.add('summon_creature')
                    exp_rows.append((c, note))
                    continue
                exp_rows.append((c, note))
                if not c or src == 'helper':
                    continue
                kept = (not r['hide']) or keep_hidden(c, r['effectName'])
                if kept:
                    item_cols.add(c)
                    if c.startswith('summon_creature_'):
                        item_cols.add('summon_creature')
                    if c == 'fortify_skill_lockpicking' and r['primaryAV'] == 'PickPocketSkillAdvance':
                        item_cols.add('fortify_skill_pickpocket')
                    if c == 'restore_health' and (r['effectDelivery'] != '0' or (r['area'] not in ('', '0'))):
                        item_cols.add('restore_health_other')
            full = set(item_cols)
            for c in item_cols:
                if family_of_col.get(c):
                    full.add(family_of_col[c])
            stats['items'] += 1
            stats['inScope'] += scope
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
        header = [c for c in keep_cols if not (has_base and c == 'effectCost')] + (['effectBaseCost'] if has_base else []) + \
            ['expectColumn', 'expectNote', 'expectInScope', 'expectEffects']
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

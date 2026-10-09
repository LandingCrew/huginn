"""Compare the C++ mapper's per-MGEF classification (huginn_effect_report --mgef-out)
with the Python reference extractor (tools/effects/reference/) on the same dump.

usage: python -I tools/effects/compare_mgef.py <dump.csv> <mgef_out.csv> <report.csv>
Writes one row per MGEF where they disagree, plus a summary."""
import sys, csv, re, collections
import os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "reference"))
import warnings; warnings.filterwarnings('ignore')
import pandas as pd
import base
import cap

dump, mgef_out, report = sys.argv[1], sys.argv[2], sys.argv[3]
base.DUMPS['x'] = dump
cap.DESC_ON = True
d = base.load('x')
e = d[d.hasEff]
KEY = ['effectFormID', 'effectName', 'arch', 'primaryAV', 'resistAV', 'det', 'recover', 'effectKeywords', 'effectDescription']
u = e[KEY].drop_duplicates('effectFormID')

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
cols = [r['id'] for r in csv.DictReader(open(os.path.join(REPO, 'docs/architecture/9-data/effects.csv'), encoding='utf-8'))]
colset = set(cols)
fams = [r for r in cols[:25]]


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


mine = {r['effectFormID']: r for r in csv.DictReader(open(mgef_out, encoding='utf-8'))}
agree = 0
dis = []
by = collections.Counter()
for _, r in u.iterrows():
    spec, src, el, sk = cap.classify(r)
    pc = to_col(spec, el)
    m = mine.get(r['effectFormID'])
    if m is None:
        continue
    mc = m['column']
    # the C++ side reports helper/wrapper/unmapped with an empty column
    if pc == mc:
        agree += 1
        continue
    dis.append({'effectFormID': r['effectFormID'], 'effectName': r['effectName'], 'arch': r['arch'], 'primaryAV': r['primaryAV'],
                'det': r['det'], 'proto_spec': spec or '', 'proto_src': src, 'proto_col': pc, 'cpp_col': mc,
                'cpp_route': m['route'], 'desc': r['effectDescription'][:160]})
    by[(pc, mc)] += 1

with open(report, 'w', newline='', encoding='utf-8') as f:
    if dis:
        w = csv.DictWriter(f, fieldnames=list(dis[0].keys()))
        w.writeheader()
        w.writerows(dis)
print(f'MGEFs compared: {agree + len(dis)}; agree {agree}; disagree {len(dis)}')
for (a, b), n in by.most_common(60):
    print(f'  {n:5d}  proto={a or "-":35s} cpp={b or "-"}')

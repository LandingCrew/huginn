"""Follow-ups: curve changes between builds (same input -> different output), the summoned/
daedra family vs the player's own summons, and names of forms missing from the catalog."""
import json
from collections import Counter, defaultdict
from common import *

D = load()
needs = D['needs']; NI = {n: i for i, n in enumerate(needs)}; cols = D['cols']; caps = D['caps']
ctxs = sorted(D['ctxs'].values(), key=lambda c: c['utc'])
out = out_file('sensors2.txt')


def p(*a):
    print(*a, file=out)


# 1. curve changes: for each need, input (rounded) -> set of outputs per build
p('== needs whose input->output map differs between builds ==')
for k, n in enumerate(needs):
    m = defaultdict(lambda: defaultdict(set))
    for c in ctxs:
        if k in c['in']:
            b = D['heads'][c['seg']]['build'].split()[0]
            m[round(c['in'][k], 3)][b].add(round(c['need'].get(k, 0), 2))
    diffs = []
    for x, byb in m.items():
        outs = {o for s in byb.values() for o in s}
        if len(outs) > 1 and all(len(s) == 1 for s in byb.values()) and len(byb) > 1:
            diffs.append((x, {b: sorted(s)[0] for b, s in sorted(byb.items())}))
    if diffs:
        p(n, len(diffs), 'inputs; e.g.', diffs[:2])

# 2. player summons: time of last summon pick
summon_cols = {j for j, c in enumerate(cols) if c.startswith('summon')}
daedra_like = {'Flame Atronach', 'Frost Atronach', 'Storm Atronach', 'Dremora'}
P = sorted(picks(D), key=lambda d: d['utc'])
summ_times = []
for d in P:
    ch = d['chosen']
    if ch and ch[CAP] >= 0 and any(j in summon_cols for j, v in caps[ch[CAP]]):
        summ_times.append((T(d['utc']), d['name'], d['launch']))
p('\nplayer summon picks:', Counter(n for _, n, _ in summ_times).most_common(15))


def last_summon(c, launch, window=120):
    t = T(c['utc'])
    best = None
    for ts, n, l in summ_times:
        if l == launch and 0 <= (t - ts).total_seconds() <= window:
            best = n
    return best


launch_of = {}
for key, c in D['ctxs'].items():
    launch_of[id(c)] = key[0]
fams = ['target_summoned', 'target_daedra', 'target_spectral', 'target_troll', 'target_undead', 'target_element_fire', 'target_element_frost']
humanoid_races = {'NordRace', 'ImperialRace', 'DarkElfRace', 'OrcRace', 'HighElfRace', 'RedguardRace', 'WoodElfRace', 'BretonRace', 'ArgonianRace', 'KhajiitRace'}
tab = defaultdict(Counter)
for c in ctxs:
    if c['need'].get(NI['in_combat'], 0) < 0.5:
        continue
    s = last_summon(c, launch_of[id(c)])
    for f in fams:
        on = c['need'].get(NI[f], 0) >= 0.5
        tab[f][('summon<=120s' if s else 'no summon', on)] += 1
p('\nin-combat contexts: family flag vs a player summon picked in the last 120 s')
for f in fams:
    t = tab[f]
    a = t[('summon<=120s', True)] / max(1, t[('summon<=120s', True)] + t[('summon<=120s', False)])
    b = t[('no summon', True)] / max(1, t[('no summon', True)] + t[('no summon', False)])
    p(f'  {f:22} P(on | recent summon) {a:.3f}  P(on | none) {b:.3f}   n {dict(t)}')
# humanoid-race primary target with daedra flag: which summon was recent?
hd = Counter()
for c in ctxs:
    if c['race'] in humanoid_races and c['need'].get(NI['target_daedra'], 0) >= 0.5:
        hd[last_summon(c, launch_of[id(c)], 180)] += 1
p('\nhumanoid-race target + daedra flag: last player summon within 180 s ->', hd.most_common())
hs = Counter()
for c in ctxs:
    if c['need'].get(NI['target_summoned'], 0) >= 0.5:
        hs[last_summon(c, launch_of[id(c)], 180)] += 1
p('target_summoned on: last player summon within 180 s ->', hs.most_common())

# 3. names of uncatalogued forms: from v3 decisions and from the v2 log
names = {}
for d in D['decs']:
    if d.get('form'):
        names[d['form']] = d['name']
v2 = v2_path(D)
for line in (open(v2, encoding='utf-8', errors='replace') if v2 else []):
    try:
        r = json.loads(line)
    except ValueError:
        continue
    if r.get('form'):
        names.setdefault(r['form'], r.get('name'))
miss = Counter(); kinds = {}
for c in ctxs:
    for r in c['rows']:
        if r[CAP] < 0 and r[FLAGS] & F['eligible']:
            miss[r[FORM]] += 1; kinds[r[FORM]] = r[SRC]
p('\n== eligible forms not in the catalog ==')
for f, n in miss.most_common(30):
    p(f'  {f} {kinds[f]:8} {n:6} ctx-rows  name {names.get(f)}')
p('distinct', len(miss))
out.close()

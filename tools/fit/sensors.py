"""Sensor sanity checks on the v3 contexts: curve value vs input, target family vs race,
flicker, darkness, step needs that stay on, and items missing from the catalog."""
from collections import Counter, defaultdict
import statistics as st
from common import *

D = load()
needs = D['needs']; NI = {n: i for i, n in enumerate(needs)}
ctxs = sorted(D['ctxs'].values(), key=lambda c: c['utc'])
out = out_file('sensors.txt')


def p(*a):
    print(*a, file=out)


# 1. curve output vs input for selected needs
for n in ('surrounded', 'enemy_count', 'enemy_close', 'enemy_mid', 'enemy_far', 'darkness', 'hunger', 'thirst',
          'warmth_deficit', 'encumbrance', 'boss_fight', 'downtime', 'combat_onset', 'health_falling', 'drop_ahead'):
    k = NI[n]
    pairs = Counter()
    for c in ctxs:
        if k in c['in']:
            pairs[(round(c['in'][k], 2), round(c['need'].get(k, 0), 3))] += 1
    p(f'{n}: (input, output) most common', pairs.most_common(12))
    # output when input absent
    absent = Counter(round(c['need'].get(k, 0), 4) for c in ctxs if k not in c['in'])
    p(f'   output when input absent/zero:', absent.most_common(4))

# 2. target families vs race
fam = [n for n in needs if n.startswith('target_') and n not in ('target_caster', 'target_archer', 'target_health_low', 'target_magicka_low', 'target_stamina_low')]
racefam = defaultdict(Counter)
for c in ctxs:
    if c['need'].get(NI['in_combat'], 0) < 0.5 and not c['race']:
        continue
    fs = tuple(f[7:] for f in fam if c['need'].get(NI[f], 0) >= 0.5)
    racefam[c['race']][fs] += 1
p('\n== race -> target family sets (contexts) ==')
for r, cnt in sorted(racefam.items(), key=lambda kv: -sum(kv[1].values()))[:40]:
    p(f'{r}: {sum(cnt.values())}', cnt.most_common(6))

# 3. flicker: consecutive contexts <= 5 s apart, both in combat, same race: family-set changes
flips = Counter(); pairs_n = 0
prev = None
for c in ctxs:
    if prev and c['race'] and c['race'] == prev['race'] and (T(c['utc']) - T(prev['utc'])).total_seconds() <= 5:
        a = frozenset(f for f in fam if prev['need'].get(NI[f], 0) >= 0.5)
        b = frozenset(f for f in fam if c['need'].get(NI[f], 0) >= 0.5)
        pairs_n += 1
        if a != b:
            flips[(c['race'], tuple(sorted(x[7:] for x in a ^ b)))] += 1
    prev = c
p(f'\n== target family flips between consecutive contexts (<=5 s, same race): {sum(flips.values())} of {pairs_n} pairs')
p(flips.most_common(25))

# race null while in combat with families set
p('\ncontexts in combat with race null:', sum(1 for c in ctxs if c['need'].get(NI['in_combat'], 0) >= 0.5 and not c['race']),
  'of in-combat', sum(1 for c in ctxs if c['need'].get(NI['in_combat'], 0) >= 0.5))
p('families set but not in combat:', sum(1 for c in ctxs if c['need'].get(NI['in_combat'], 0) < 0.5 and any(c['need'].get(NI[f], 0) >= 0.5 for f in fam)))

# 4. darkness: value distribution, by in_combat/race, flicker
dk = NI['darkness']
p('\n== darkness ==')
p('input distribution (1-light; absent = open daylight or 0):', Counter(round(c['in'].get(dk, -1), 2) for c in ctxs).most_common(15))
# darkness episode churn: count onsets per launch-hour
ep = [d for d in D['decs'] if d['out'] == 'nothing' and d['ep']['gi'] == dk]
p('darkness nothing episodes', len(ep), 'durSec median', st.median(d['ep']['durSec'] for d in ep),
  'share < 5 s', round(sum(d['ep']['durSec'] < 5 for d in ep) / len(ep), 3), 'share < 2 s', round(sum(d['ep']['durSec'] < 2 for d in ep) / len(ep), 3))
# torches / Light items held?
lights = Counter()
for c in ctxs:
    for r in c['rows']:
        if r[KIND] == 'Light':
            lights[r[FORM]] += 1
p('Light rows seen in contexts:', lights.most_common(5))
nv = Counter()
for c in ctxs[-500:]:
    for r in c['rows']:
        if r[CAP] >= 0 and any(D['cols'][j] in ('vision_night_eye', 'utility_light') for j, v in D['caps'][r[CAP]]):
            nv[r[FORM]] += 1
p('rows with night-eye/light columns (last 500 ctx):', nv.most_common(5))

# 5. step needs: run lengths (how long they stay on, in consecutive contexts)
for n in ('drain_health', 'drain_magicka', 'drain_stamina', 'diseased', 'poisoned', 'regen_suppressed', 'combat_regen_ok', 'merchant_in_crosshair'):
    k = NI[n]
    runs = []; start = None; last = None
    for c in ctxs:
        on = c['need'].get(k, 0) >= 0.5
        t = T(c['utc'])
        if on and start is None:
            start = t
        if not on and start is not None:
            runs.append((last - start).total_seconds()); start = None
        if on:
            last = t
    if runs:
        p(f'{n}: runs {len(runs)}, median {st.median(runs):.0f}s, max {max(runs):.0f}s')
# drain_health picks: which items picked while drain_health on
dh = NI['drain_health']
p('picks while drain_health on:', Counter(d['name'] for d in picks(D) if d['C']['need'].get(dh, 0) >= 0.5).most_common(10))

# 6. items not in the catalog (cap -1) among held and shown rows
miss = Counter(); missk = Counter()
for c in ctxs:
    for r in c['rows']:
        if r[CAP] < 0 and r[FLAGS] & (F['shown'] | F['eligible']):
            miss[r[FORM]] += 1; missk[(r[SRC], r[FORM][:2])] += 1
p('\n== eligible/shown rows not in the catalog (cap -1) ==', sum(miss.values()), missk.most_common(10))
names = {}
for d in picks(D):
    names[d['form']] = d['name']
p([(f, names.get(f), n) for f, n in miss.most_common(15)])

# 7. cap of the same form across builds (0.23.34 tables changed mappings)
formcaps = defaultdict(set)
for c in ctxs:
    b = D['heads'][c['seg']]['build']
    for r in c['rows']:
        if r[CAP] >= 0 and r[UID] == 0:
            formcaps[r[FORM]].add((b.split()[0], r[CAP]))
changed = {f: s for f, s in formcaps.items() if len({cap for b, cap in s}) > 1}
p(f'\nforms whose cap changed across launches: {len(changed)} of {len(formcaps)}')
bych = Counter()
for f, s in changed.items():
    byb = defaultdict(set)
    for b, cap in s:
        byb[b].add(cap)
    bs = sorted(byb)
    for a, b in zip(bs, bs[1:]):
        if byb[a] != byb[b]:
            bych[b] += 1
p('first build where a form cap differs from the previous build seen:', sorted(bych.items()))

# 8. pick rate of 'nothing' by need relative to onsets of that need w/ answering pick: answered vs unanswered
p('\n== need episodes: nothing records vs open-at-pick ==')
openc = Counter()
for d in picks(D):
    for k in d['open']:
        openc[k] += 1
noth = Counter(d['ep']['gi'] for d in D['decs'] if d['out'] == 'nothing')
rows = sorted(set(openc) | set(noth), key=lambda k: -(openc[k] + noth[k]))
for k in rows[:45]:
    p(f'  {needs[k]:24} picks with it open {openc[k]:5}  nothing {noth[k]:5}')
out.close()

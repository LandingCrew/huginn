"""Paired differences (model - baseline) with launch-bootstrap CIs, from cv_picks.pkl;
plus a per-case split of the menu picks and per-kind split of key picks."""
import pickle
from collections import Counter, defaultdict
import numpy as np
from fitlib import paired, summarize
from common import out_file, work

R = pickle.load(open(work('cv_picks.pkl'), 'rb'))   # written by cv.py picks
res, meta = R['results'], R['meta']
out = out_file('pairs.txt')


def p(*a):
    print(*a, file=out)


M = 'theta + b (doc 9)'
metrics = {
    'key page hit@1': lambda r: r.get('page_hit1') if r['out'] in ('key', 'wheel') else None,
    'key page NLL': lambda r: -r['page_logp'] if r['out'] in ('key', 'wheel') and 'page_logp' in r else None,
    'key full hit@8': lambda r: r.get('full_hit8') if r['out'] in ('key', 'wheel') else None,
    'menu full hit@8': lambda r: r.get('full_hit8') if r['out'] == 'menu' else None,
    'menu set NLL': lambda r: -r['menu_logp'] if r['out'] == 'menu' and 'menu_logp' in r else None,
    'pick NLL': lambda r: -r['logp'] if r['out'] != 'nothing' and r.get('logp') is not None else None,
}
for model in (M, 'theta + b + position', 'theta + b + ctx fit'):
    p(f'\n=== {model} minus baseline (mean diff [95% launch-bootstrap CI], n) ===')
    for base in ('kappa only', 'position', 'b only', 'A* fit (ln util)', 'context fit (ln ctx,corr,potion,fav)', 'cheap test (class x ctx)',
                 'context fit + position', 'A* ranking (logged util)', 'B ranking (ctx x mult)', 'live page (shown)', 'theta (doc 9)'):
        if base not in res or base == model:
            continue
        p(f'  vs {base}')
        for mn, fn in metrics.items():
            try:
                d = paired(res[model], res[base], fn)
            except (ValueError, ZeroDivisionError):
                continue
            if d[3]:
                p(f'      {mn:16} {d[0]:+.4f} [{d[1]:+.4f}, {d[2]:+.4f}] n={d[3]}')

# menu picks by old case and by kind
p('\n=== menu picks: full hit@8 by attribution case (theta+b | A* ranking | live page) ===')
names = {}
for case in sorted({m['case'] for m in meta if m['out'] == 'menu'}):
    idx = [i for i, m in enumerate(meta) if m['out'] == 'menu' and m['case'] == case]
    row = []
    for model in (M, 'A* ranking (logged util)', 'live page (shown)', 'b only'):
        v = [res[model][i].get('full_hit8') for i in idx]
        row.append(f'{sum(bool(x) for x in v)}/{len(v)}')
    p(f'  {case:28} ' + '  '.join(row))
p('\n=== key/wheel picks by chosen kind: page hit@1, full hit@8 (theta+b vs A* ranking vs position) ===')
kinds = Counter(m['kind'] for m in meta)
for kd in ('equip', 'consume'):
    idx = [i for i, m in enumerate(meta) if m['out'] in ('key', 'wheel') and m['kind'] == kd]
    row = []
    for model in (M, 'A* ranking (logged util)', 'position', 'theta + b + position'):
        h1 = np.mean([bool(res[model][i].get('page_hit1')) for i in idx])
        h8 = np.mean([bool(res[model][i].get('full_hit8')) for i in idx])
        row.append(f'{model[:22]:22} p@1 {h1:.3f} f@8 {h8:.3f}')
    p(f'  {kd} (n={len(idx)}): ' + ' | '.join(row))
# per launch for the doc-9 model
p('\n=== per launch: n key, theta+b page hit@1, A* page hit@1, n menu, theta+b menu hit@8, A* menu hit@8 ===')
by = defaultdict(list)
for i, m in enumerate(meta):
    by[m['launch']].append(i)
for l, idx in sorted(by.items()):
    k = [i for i in idx if meta[i]['out'] in ('key', 'wheel')]
    mm = [i for i in idx if meta[i]['out'] == 'menu']
    f = lambda model, ii, key: (np.mean([bool(res[model][i].get(key)) for i in ii]) if ii else float('nan'))
    p(f'  {l} {len(k):4} {f(M, k, "page_hit1"):.2f} {f("A* ranking (logged util)", k, "page_hit1"):.2f}   {len(mm):3} {f(M, mm, "full_hit8"):.2f} {f("A* ranking (logged util)", mm, "full_hit8"):.2f}')
out.close()

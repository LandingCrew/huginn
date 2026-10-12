"""Leave-one-launch-out CV of the doc-9 model and its baselines. Writes cv_<group>.pkl and prints a table.
usage: python cv.py picks|three|sens   (after v3load.py; writes cv_<group>.pkl in the work directory)"""
import pickle
import sys
import time

import numpy as np

from fitlib import *

group = sys.argv[1] if len(sys.argv) > 1 else 'picks'
D = load()
v2 = load_v2()
three = group == 'three'
S = build(D, include_nothing=three, v2=v2)
S_latest = build(D, include_nothing=three, v2=v2, latest_caps=True) if group in ('picks',) else None
obv = obvious_pairs(S['needs'], S['cols'])

if group == 'picks':
    specs = {
        'kappa only': dict(theta=False, item=False, outside=False),
        'position': dict(theta=False, item=False, outside=False, feat_spec=['pos']),
        'b only': dict(theta=False, item=True, outside=False),
        'A* fit (ln util)': dict(theta=False, item=False, outside=False, feat_spec=['astar']),
        'context fit (ln ctx,corr,potion,fav)': dict(theta=False, item=False, outside=False, feat_spec=['ctx']),
        'cheap test (class x ctx)': dict(theta=False, item=False, outside=False, feat_spec=['cheap']),
        'theta (doc 9)': dict(item=False, outside=False),
        'theta + b (doc 9)': dict(outside=False),
        'theta + b, latest caps': dict(outside=False, _latest=True),
        'theta + b + bias need': dict(outside=False, bias_need=True),
        'theta + b + position': dict(outside=False, feat_spec=['pos']),
        'context fit + position': dict(theta=False, item=False, outside=False, feat_spec=['ctx', 'pos']),
        'A* fit + position': dict(theta=False, item=False, outside=False, feat_spec=['astar', 'pos']),
        'theta + b + ctx fit': dict(outside=False, feat_spec=['ctx']),
    }
elif group == 'sens':
    specs = {
        'theta+b T0=0': dict(outside=False, T0=0.0),
        'theta+b T0=2': dict(outside=False, T0=2.0),
        'theta+b sd=0.1': dict(outside=False, sig_dense=0.1),
        'theta+b sd=1.0': dict(outside=False, sig_dense=1.0),
        'theta+b sb=0.25': dict(outside=False, sig_b=0.25),
        'theta+b sb=1.0': dict(outside=False, sig_b=1.0),
        'theta+b obvious only (sd=0.01)': dict(outside=False, sig_dense=0.01),
    }
else:
    specs = {
        'kappa + u0(n)': dict(theta=False, item=False),
        'A* fit + u0(n)': dict(theta=False, item=False, feat_spec=['astar']),
        'theta + b + u0(n) (doc 9, 3 outcomes)': dict(),
        'theta + b + u0(n) + position': dict(feat_spec=['pos']),
    }

launches = S['launches']
results = {}
for name, spec in specs.items():
    spec = dict(spec)
    use_latest = spec.pop('_latest', False)
    SS = S_latest if use_latest else S
    t0 = time.time()
    res = [None] * len(SS['Y'])
    for li in range(len(launches)):
        te = np.where(SS['LA'] == li)[0]
        tr = np.where(SS['LA'] != li)[0]
        if not (SS['Y'][te] >= 0).any():
            continue
        Str, Ste = subset(SS, tr), subset(SS, te)
        M = Model(Str, obvious=obv, nitems=len(SS['items']), **spec)
        p = fit(M, Str, maxiter=300)
        M2 = Model(Ste, obvious=obv, nitems=len(SS['items']), **spec)
        for i, r in zip(te, evaluate(M2, Ste, p)):
            res[i] = r
    res = [r for r in res if r is not None]
    results[name] = res
    summ = summarize(res)
    print(f'{name}  ({time.time() - t0:.0f}s)', flush=True)
    for k, v in summ.items():
        print(f'    {k:16} {v[0]:.4f}  [{v[1]:.4f}, {v[2]:.4f}]  n={v[3]}', flush=True)

# non-fitted rankings: A* (logged util) and B (ctx x corr x potion x fav), on the same decisions
if group == 'picks':
    U = np.nan_to_num(S['UTIL'], nan=-1.0)
    V = S['V2']
    B = np.where(np.isnan(V[..., 0]), -1.0, np.nan_to_num(V[..., 0] * V[..., 1] * V[..., 2] * V[..., 3], nan=-1.0))
    for name, sc, tb in [('A* ranking (logged util)', U, None), ('B ranking (ctx x mult)', B, U)]:
        res = metrics_from_scores(S, sc, sc, None, tiebreak=None if tb is None else tb / (np.abs(tb).max() + 1))
        results[name] = res
        summ = summarize(res)
        print(name)
        for k, v in summ.items():
            print(f'    {k:16} {v[0]:.4f}  [{v[1]:.4f}, {v[2]:.4f}]  n={v[3]}')
    # live page: was the chosen item shown?
    live = []
    for d in range(len(S['Y'])):
        y = S['Y'][d]
        live.append({'launch': S['LA'][d], 'out': S['OUT'][d], 'logp': None,
                     'full_hit8': bool(S['INHA'][d, y] == 0)})
    results['live page (shown)'] = live
    print('live page: menu picks whose item was on the page', summarize(live).get('menu_full_hit8'))

with open(work(f'cv_{group}.pkl'), 'wb') as f:
    pickle.dump({'results': results, 'meta': S['meta'], 'launches': launches}, f)

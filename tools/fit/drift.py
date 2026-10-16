"""Fit on all launches; theta drift from theta0 with P4 information; u0 need weights;
evidence per obvious pair / per need (R6: how far the data is from what the fit needs)."""
import pickle
from collections import Counter, defaultdict

import numpy as np

from fitlib import *

D = load()
v2 = load_v2()
import sys
MODES = sys.argv[1:] or ['picks', 'three']
out = out_file('drift_' + '_'.join(MODES) + '.txt')


def p_(*a):
    print(*a, file=out, flush=True)


for mode in MODES:
    S = build(D, include_nothing=(mode == 'three'), v2=v2)
    obv = obvious_pairs(S['needs'], S['cols'])
    spec = dict(outside=(mode == 'three'))
    M = Model(S, obvious=obv, **spec)
    p = fit(M, S, maxiter=500)
    q = M.unpack(p)
    I = M.info(S, p)
    th, th0, sig = q['theta'], M.theta0, M.sig
    post_sd = 1.0 / np.sqrt(I + 1.0 / sig ** 2)
    needs, cols = S['needs'], S['cols']
    p_(f'\n===== {mode}: {len(S["Y"])} decisions; kappa = {q["kappa"]:.3f} =====')
    dl = th - th0
    order = np.argsort(-np.abs(dl).ravel())
    p_('-- largest |theta - theta0| (need x effect: theta0 -> theta, info, posterior sd, obvious?)')
    for f in order[:45]:
        k, j = divmod(f, M.J)
        p_(f'  {needs[k]:24} x {cols[j]:28} {th0[k, j]:+.2f} -> {th[k, j]:+.3f}  info {I[k, j]:7.2f}  sd {post_sd[k, j]:.3f}  {"OBV" if (k, j) in obv else ""}')
    p_('-- obvious pairs: drift, sorted by information')
    ob = sorted(obv, key=lambda kj: -I[kj])
    for k, j in ob[:60]:
        p_(f'  {needs[k]:24} x {cols[j]:28} {th0[k, j]:+.2f} -> {th[k, j]:+.3f}  info {I[k, j]:7.2f}  z {(th[k, j] - th0[k, j]) / sig[k, j]:+.2f}')
    zero_info = sum(1 for kj in obv if I[kj] < 0.5)
    p_(f'obvious pairs with info < 0.5 (effectively never tested): {zero_info} of {len(obv)}')
    p_(f'obvious pairs with info >= 5: {sum(1 for kj in obv if I[kj] >= 5)}; >= 20: {sum(1 for kj in obv if I[kj] >= 20)}')
    p_(f'obvious pairs that moved down by > 0.5 (toward or past zero): ' +
       str([(needs[k], cols[j], round(th[k, j], 2), round(I[k, j], 1)) for k, j in obv if th[k, j] - th0[k, j] < -0.5]))
    if mode == 'three':
        uo = q['uout']
        p_(f'-- u0: constant {uo[-1]:+.3f}; need weights (largest |w|):')
        for k in np.argsort(-np.abs(uo[:-1]))[:30]:
            p_(f'  {needs[k]:24} {uo[k]:+.3f}')
    b = q['b']; inv = {v: k for k, v in S['items'].items()}
    names = {}
    for d in D['decs']:
        if d.get('form'):
            names[d['form']] = d['name']
    p_('-- largest b_i:', [(names.get(inv[i], inv[i]), round(b[i], 2)) for i in np.argsort(-b)[:12]])
    p_('-- smallest b_i:', [(names.get(inv[i], inv[i]), round(b[i], 2)) for i in np.argsort(b)[:8]])
    pickle.dump({'p': p, 'theta': th, 'theta0': th0, 'I': I, 'needs': needs, 'cols': cols, 'kappa': q['kappa'],
                 'uout': q['uout'], 'b': b, 'items': S['items']}, open(work(f'fit_all_{mode}.pkl'), 'wb'))

# evidence per need: picks with the need >= 0.5, and with an answering (obvious-pair) item chosen
S = build(D, include_nothing=False, v2=v2)
obv = obvious_pairs(S['needs'], S['cols'])
byneed = defaultdict(set)
for k, j in obv:
    byneed[k].add(j)
cnt_active = Counter(); cnt_answer = Counter(); cnt_shown_answer = Counter()
for d in range(len(S['Y'])):
    y = S['Y'][d]
    cap = S['CapMat'][S['CAPID'][d, y]].copy()
    cap[S['xcol']] += S['X'][d, y]
    onA = S['MASK'][d] & (S['INHA'][d] == 0)
    for k in np.where(S['N'][d] >= 0.5)[0]:
        cnt_active[k] += 1
        js = list(byneed.get(k, ()))
        if js and (cap[js] > 0).any():
            cnt_answer[k] += 1
        if js:
            caps_shown = S['CapMat'][S['CAPID'][d][onA]][:, js]
            if (caps_shown > 0).any():
                cnt_shown_answer[k] += 1
p_('\n===== evidence per need (picks only) =====')
p_('need: picks with need>=0.5 | of those, the chosen item answers it (obvious pair) | an answering item was on the page')
for k in sorted(cnt_active, key=lambda k: -cnt_answer[k]):
    p_(f'  {S["needs"][k]:24} {cnt_active[k]:5} | {cnt_answer[k]:5} | {cnt_shown_answer[k]:5}')
out.close()

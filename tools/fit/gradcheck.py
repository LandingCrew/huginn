"""fitlib self-check: the analytic gradient of the choice-model loss against finite
differences on a subsample, and the time of one full loss evaluation. Prints only."""
import time, numpy as np
from fitlib import *
D = load()
v2 = load_v2()
t=time.time(); S = build(D, include_nothing=True, v2=v2); print('build', time.time()-t, S['CAPID'].shape, len(S['items']), 'v2ok picks', S['v2ok'][S['Y']>=0].mean())
import collections; print(collections.Counter(S['OUT']))
obv = obvious_pairs(S['needs'], S['cols'])
idx = np.arange(0, len(S['Y']), 40)
Ss = subset(S, idx)
for spec in [dict(), dict(feat_spec=['astar','pos'], theta=False), dict(bias_need=True, feat_spec=['ctx'])]:
    M = Model(Ss, obvious=obv, **spec)
    Fm,_ = row_features(Ss, M.feat_spec)
    p = M.init() + np.random.default_rng(0).normal(0, 0.1, M.P)
    f, g, _, _ = M.loss(Ss, p, Fm)
    rng = np.random.default_rng(1)
    for i in rng.choice(M.P, 6, replace=False):
        e = np.zeros(M.P); e[i] = 1e-5
        fd = (M.loss(Ss, p+e, Fm)[0] - M.loss(Ss, p-e, Fm)[0]) / 2e-5
        print(spec.get('feat_spec'), i, round(g[i],6), round(fd,6))
t=time.time(); M = Model(S, obvious=obv); Fm=None
for _ in range(3): M.loss(S, M.init())
print('loss eval full', (time.time()-t)/3)

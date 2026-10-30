"""Cross-validation of the doc-9 model and its baselines, held out by launch. Writes cv_<group>.pkl and
cv_<group>.txt (the tables, also on stdout) in the work directory.

usage: python cv.py [picks|three|sens] [--folds K] [--seed S] [--loo] [--spec TEXT ...]
                    [--since V] [--until V] [--split-at V ...] [--split-needs all|NEED ...]
                    [--laps once|all|none]
(after v3load.py)

Folds: grouped K-fold by launch (fitlib.launch_folds: default 10 folds, seed 1, deterministic,
balanced by key/wheel picks), so no launch is in two folds. --loo is leave-one-launch-out, one fit
per launch (~9 h on 288 launches; the 10 folds take about a tenth of that).

--spec keeps the specs whose name contains any of the given texts (a smoke run: --folds 2 --spec
"A* fit (ln util)" "theta + b (doc 9)"). --since / --until keep the decisions logged by plugin
versions in that range (inclusive); --split-at cuts them into build segments: the yardstick is
also reported per segment, and the 'split' specs give the needs in --split-needs (default: those
whose sensor changed at 0.23.41, and the target families) one column per segment.

--laps (the three group): the nothing records of lapped stretches, 0.23.51 (fitlib.build: 'once',
the default, keeps one per lap tick, fitlib.lap_tick; 'all' one per open need; 'none' drops them,
the data of a build before 0.23.51). The picks group reads no nothing record. Logs written before
0.23.51 have no laps and read a quarterstaff as loadout_two_handed: fit the three group with
--since 0.23.51, or --split-at 0.23.51 (the default --split-needs splits the needs concerned;
README, 'Reading logs written before 0.23.51').

The yardstick (fitlib.YARDSTICK), for every spec against A* (the old engine's ranking replayed
from the logged utilities, fitlib.astar_results) on the same choice sets: hit@8 for key/wheel
picks, for menu picks and for all picks pooled, page hit@1 and within-page NLL (key/wheel picks
whose chosen row was on the page), each with a bootstrap-over-launches 95% interval on model
minus A*. Every result list in cv_<group>.pkl is aligned with its `meta` (one entry per decision;
a decision in a fold that was not fitted carries no metrics)."""
import argparse
import pickle
import time

import numpy as np

from fitlib import *

CV_MAXITER = 300     # L-BFGS iterations per fold fit (stored in cv_<group>.pkl for honest.py)


def spec_key(spec):
    """A spec's definition as text. Results are reused by it, not by the spec's name (honest.py's
    caches, and cv_<group>.pkl's 'specs'), so a spec redefined under the same name is refitted."""
    return repr(sorted(spec.items()))

PICKS = {
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
    # doc 11 steps 1-2 (roadmap R6): presence grading, recent_<family>
    'theta + b, presence grading': dict(outside=False, _with=['presence']),
    'theta + b, presence + strength': dict(outside=False, _with=['presence+strength']),
    'theta + b + recent_<family>': dict(outside=False, _with=['recent']),
    # the old engine's utility as one more feature (the 2026-10-28 rehearsal's best variant)
    'theta + b + A* feature (ln util)': dict(outside=False, feat_spec=['astar']),
    'theta + b + recent + A* feature': dict(outside=False, feat_spec=['astar'], _with=['recent']),
    # without b: what transfers to a new character (b is per item, of this one; the R6 verifier
    # of 2026-10-28 found b costs held-out hits on items the training set never picked)
    'theta + recent + A* feature (no b)': dict(outside=False, item=False, feat_spec=['astar'], _with=['recent']),
    # the old score's components (the R8 component test of 2026-10-29, ported for the real R6 fit;
    # fitlib.row_features): which part of ln(old util) does the work. They read the v2 log.
    'theta + b + recent + learn mult (D2)': dict(outside=False, feat_spec=['learn'], _with=['recent']),
    'theta + b + recent + learn mult + fav/corr/potion (D5)': dict(outside=False, feat_spec=['learn', 'mults'],
                                                                  _with=['recent']),
    'theta + b + recent + old learner state (L1)': dict(outside=False, feat_spec=['learner'], _with=['recent']),
    # 0.24.19: D2's columns as the live score read them (the v3 row's lm / um; 0 / 1 under engine
    # B): the fit of B on logs from 0.24.17 on (README, 'Logs from 0.24.17 on')
    'theta + b + recent + learn mult as the live score read it (lm/um, B)': dict(
        outside=False, feat_spec=['learn_live'], _with=['recent']),
}
PICKS_SPLIT = {   # only with --split-at
    'theta + b, needs split by build': dict(outside=False, _with=['split']),
    'theta + b, split + recent': dict(outside=False, _with=['split', 'recent']),
}
SENS = {
    'theta+b T0=0': dict(outside=False, T0=0.0),
    'theta+b T0=2': dict(outside=False, T0=2.0),
    'theta+b sd=0.1': dict(outside=False, sig_dense=0.1),
    'theta+b sd=1.0': dict(outside=False, sig_dense=1.0),
    'theta+b sb=0.25': dict(outside=False, sig_b=0.25),
    'theta+b sb=1.0': dict(outside=False, sig_b=1.0),
    'theta+b obvious only (sd=0.01)': dict(outside=False, sig_dense=0.01),
}
THREE = {
    'kappa + u0(n)': dict(theta=False, item=False),
    'A* fit + u0(n)': dict(theta=False, item=False, feat_spec=['astar']),
    'theta + b + u0(n) (doc 9, 3 outcomes)': dict(),
    'theta + b + u0(n) + position': dict(feat_spec=['pos']),
    'theta + b + u0(n) + recent': dict(_with=['recent']),
}
THREE_SPLIT = {'theta + b + u0(n), split + recent': dict(_with=['split', 'recent'])}


def parse(argv=None):
    ap = argparse.ArgumentParser(description='Cross-validation by launch of the doc-9 model and its baselines.')
    ap.add_argument('group', nargs='?', default='picks', choices=('picks', 'three', 'sens'))
    ap.add_argument('--folds', type=int, default=10, help='grouped K-fold by launch (default 10)')
    ap.add_argument('--seed', type=int, default=1, help="the folds' seed (default 1)")
    ap.add_argument('--loo', action='store_true', help='leave-one-launch-out instead of the folds (slow)')
    ap.add_argument('--spec', nargs='+', metavar='TEXT', help='keep the specs whose name contains any TEXT')
    ap.add_argument('--since', metavar='V', help='keep decisions logged by plugin version V or later (0.23.41)')
    ap.add_argument('--until', metavar='V', help='keep decisions logged by plugin version V or earlier')
    ap.add_argument('--split-at', nargs='+', default=[], metavar='V', help='build segments: the versions that start one')
    ap.add_argument('--split-needs', nargs='+', metavar='NEED',
                    help="the needs the 'split' specs split by segment ('all'; default: the 0.23.41 changes)")
    ap.add_argument('--nboot', type=int, default=2000, help='bootstrap draws (default 2000)')
    ap.add_argument('--laps', choices=('once', 'all', 'none'), default='once',
                    help="lapped-stretch nothing records (0.23.51): one per lap tick (default), all, or none")
    a = ap.parse_args(argv)
    for v in [a.since, a.until] + a.split_at:
        if v is not None and version(v) is None:
            ap.error(f'{v!r} is not a version (0.23.41)')
    if a.folds < 1:
        ap.error('--folds must be at least 1')
    if a.split_needs == ['all']:
        a.split_needs = 'all'
    return a


def main(argv=None):
    a = parse(argv)
    t_start = time.time()
    specs = {'picks': PICKS, 'sens': SENS, 'three': THREE}[a.group]
    if a.split_at:
        specs = specs | {'picks': PICKS_SPLIT, 'sens': {}, 'three': THREE_SPLIT}[a.group]
    if a.spec:
        specs = {k: v for k, v in specs.items() if any(t in k for t in a.spec)}
    three = a.group == 'three'
    D = load()
    v2 = load_v2()
    kw = dict(include_nothing=three, v2=v2, since=a.since, until=a.until, split_at=a.split_at, laps=a.laps)
    S = build(D, **kw)
    S_latest = build(D, latest_caps=True, **kw) if any(s.get('_latest') for s in specs.values()) else None
    obv = obvious_pairs(S['needs'], S['cols'])
    launches = S['launches']
    fold_of = np.arange(len(launches), dtype=np.int32) if a.loo else launch_folds(S, a.folds, a.seed)
    FOLD = fold_of[S['LA']]
    nf = int(fold_of.max()) + 1
    kwp = np.bincount(FOLD[np.isin(S['OUT'], KW)], minlength=nf)

    out = out_file(f'cv_{a.group}.txt')

    def P(*x):
        print(*x, flush=True)
        print(*x, file=out, flush=True)

    P(f'{len(S["Y"])} decisions, {(S["Y"] >= 0).sum()} picks, {len(launches)} launches; builds '
      f'{a.since or "any"} .. {a.until or "any"}; segments {S["segments"]}')
    P(f'folds: {"leave-one-launch-out, " if a.loo else ""}{nf} (seed {a.seed}); key/wheel picks per fold '
      f'{kwp.tolist() if nf <= 20 else f"min {kwp.min()} max {kwp.max()}"}')

    results, timing = {}, {}
    for name, spec in specs.items():
        SS = S_latest if split_spec(spec)[2] else S
        t0 = time.time()
        # a fold that was not fitted (no picks held out) gets stubs, so the list stays aligned with meta
        results[name] = cv_results(SS, obv, spec, FOLD, a.split_needs, maxiter=CV_MAXITER)
        timing[name] = time.time() - t0
        P(f'[{timing[name]:.0f}s] {name}')

    # non-fitted rankings on the same decisions: A* (logged util), B (ctx x corr x potion x fav)
    results['A* ranking (logged util)'], betas = astar_results(S, FOLD)
    P(f'A* within-page temperature on ln util, per fold: {", ".join(f"{b:.3f}" for b in betas.values())}')
    U = np.nan_to_num(S['UTIL'], nan=-1.0)
    V = S['V2']
    B = np.where(np.isnan(V[..., 0]), -1.0, np.nan_to_num(V[..., 0] * V[..., 1] * V[..., 2] * V[..., 3], nan=-1.0))
    results['B ranking (ctx x mult)'] = metrics_from_scores(S, B, B, None, tiebreak=U / (np.abs(U).max() + 1))
    results['live page (shown)'] = [
        {'launch': S['LA'][d], 'out': S['OUT'][d], 'logp': None,
         'full_hit8': bool(S['INHA'][d, S['Y'][d]] == 0) if S['Y'][d] >= 0 else None}
        for d in range(len(S['Y']))]

    P('\n=== held-out results (mean [95% launch-bootstrap CI] n) ===')
    for name, res in results.items():
        P(name)
        for k, v in summarize(res, nboot=a.nboot).items():
            P(f'    {k:16} {v[0]:.4f}  [{v[1]:.4f}, {v[2]:.4f}]  n={v[3]}')

    astar = results['A* ranking (logged util)']
    ys = {}
    P('\n=== yardstick: each model against A* on the same choice sets (diff = model - A*) ===')
    for name in specs:
        ys[name] = yardstick(results[name], astar, nboot=a.nboot)
        P('\n'.join(yardstick_lines(name, ys[name])))
    if len(S['segments']) > 1:
        P('\n=== yardstick per build segment ===')
        for g, label in enumerate(S['segments']):
            keep = S['SEG'] == g
            for name in specs:
                ys[(name, label)] = yardstick(results[name], astar, nboot=a.nboot, keep=keep)
                P('\n'.join(yardstick_lines(f'[{label}] {name}', ys[(name, label)])))

    with open(work(f'cv_{a.group}.pkl'), 'wb') as f:
        pickle.dump({'results': results, 'meta': S['meta'], 'launches': launches, 'fold_of': fold_of,
                     'segments': S['segments'], 'SEG': S['SEG'], 'yardstick': ys, 'astar_beta': betas,
                     'timing': timing, 'args': vars(a), 'maxiter': CV_MAXITER, 'data_key': data_key(S, obv),
                     'specs': {name: spec_key(spec) for name, spec in specs.items()}}, f)
    P(f'[{time.time() - t_start:.0f}s] wrote {work(f"cv_{a.group}.pkl")}')
    out.close()


if __name__ == '__main__':
    main()

"""The honest yardstick (roadmap R6): a train-on-past ONLINE replay of each spec and A*, reported
beside the grouped K-fold CV on the same decisions, with R6's gate verdict for both. Writes
honest_<group>.txt (also on stdout) and honest_<group>.pkl in the work directory.

usage: python honest.py [picks|three] [--every N] [--min-train N] [--spec TEXT ...] [--folds K] [--seed S]
                        [--since V] [--until V] [--nboot N] [--maxiter N] [--no-cache]
(after v3load.py)

Why. Grouped K-fold trains on launches before AND after the held-out one, so a held-out pick
of an item the player only started using later is fitted from its future (the R6 verifier of
2026-10-28: 45 of 1416 key/wheel picks were of items unseen in the other folds; 178 of 1294
online). The online replay is what R8 would have scored live: for each launch in log order
(once the earlier launches hold --min-train picks, default 150), refit every --every picks
(default 5) on every earlier launch plus the launch's earlier decisions, warm-started from the
previous refit, and score the next --every picks (fitlib.online_chunks, online_results). A*'s
page temperature is fitted on each chunk's past too (fitlib.astar_online); its ranking, and so
its hit@8, is the same on both schedules.

Reported, for each spec against A* on the same choice sets (fitlib.YARDSTICK): key/wheel hit@8,
menu hit@8, all picks pooled hit@8, page hit@1 and page NLL, each with the model's level and
the bootstrap-over-launches 95% interval on model - A*, for
  CV (all)     grouped K-fold over every decision (cv.py's numbers);
  CV (online)  the same CV results, restricted to the decisions the online replay tested;
  online       the train-on-past replay.
and R6's gate on each (fitlib.gate: menu lower end > 0; pooled upper end >= 0). The online
verdict is the honest one; CV (online) vs online isolates the schedule from the decision set.

Specs: --spec keeps the cv.py specs whose name contains any TEXT; default HONEST_DEFAULT.
Cost: one fit per chunk (~375 on the mage's 1,865 picks at --every 5); about 40 min per spec
at 6 threads (the verifier's run), so run specs in parallel processes when the game is not up.

Caching (--no-cache turns it off): each spec's online results and CV results are cached in the
work directory (honest_cache_<kind>_<group>_<hash>.pkl), keyed by the decisions, the spec, the
schedule and the fit settings, so parallel runs with different --spec, then one run naming them
all, combine without refitting. CV results are also taken from cv_<group>.pkl when it was
written for the same decisions, folds and seed (cv.py; its specs of the same name)."""
import argparse
import hashlib
import os
import pickle
import time

import numpy as np

import cv
from fitlib import *

HONEST_DEFAULT = ('theta + b (doc 9)', 'theta + b + recent_<family>', 'theta + b + recent + A* feature',
                  'theta + recent + A* feature (no b)')
ASTAR = 'A* ranking (logged util)'
METHODS = ('CV (all)', 'CV (online)', 'online')


def parse(argv=None):
    ap = argparse.ArgumentParser(description='The honest yardstick: online (train-on-past) replay beside grouped CV.')
    ap.add_argument('group', nargs='?', default='picks', choices=('picks', 'three'))
    ap.add_argument('--every', type=int, default=5, help='refit every N picks (default 5)')
    ap.add_argument('--min-train', type=int, default=150, help='picks before the first tested launch (default 150)')
    ap.add_argument('--spec', nargs='+', metavar='TEXT', help='keep the specs whose name contains any TEXT')
    ap.add_argument('--folds', type=int, default=10)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--since', metavar='V')
    ap.add_argument('--until', metavar='V')
    ap.add_argument('--nboot', type=int, default=2000)
    ap.add_argument('--maxiter', type=int, default=300)
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args(argv)
    if a.every < 1:
        ap.error('--every must be at least 1')
    for v in (a.since, a.until):
        if v is not None and version(v) is None:
            ap.error(f'{v!r} is not a version (0.23.41)')
    return a


def select_specs(group, texts):
    table = cv.THREE if group == 'three' else cv.PICKS
    if texts:
        specs = {k: v for k, v in table.items() if any(t in k for t in texts)}
    else:
        specs = {k: table[k] for k in HONEST_DEFAULT if k in table} or dict(table)
    bad = [k for k, v in specs.items() if split_spec(v)[2] or 'split' in split_spec(v)[1]]
    if bad:
        raise SystemExit(f'not on the online schedule (diagnostics): {bad}')
    if not specs:
        raise SystemExit(f'no {group} spec matches {texts}')
    return specs


def decisions_key(S):
    """A digest of the decision set (launch, outcome, name, chosen row), for the caches."""
    h = hashlib.sha1()
    for m, y in zip(S['meta'], S['Y']):
        h.update(f'{m["launch"]}|{m["out"]}|{m["name"]}|{int(y)}\n'.encode('utf-8', 'replace'))
    return h.hexdigest()


def cache_path(kind, group, parts):
    h = hashlib.sha1(repr(parts).encode()).hexdigest()[:16]
    return work(f'honest_cache_{kind}_{group}_{h}.pkl')


def cached(path, use, compute):
    # The caches are this script's own output in the work directory (as v3.pkl is v3load.py's,
    # common.py): never load one from anywhere else.
    if use and path.exists():
        with open(path, 'rb') as f:
            return pickle.load(f), True
    val = compute()
    if use:
        tmp = path.with_suffix(f'.{os.getpid()}.tmp')
        with open(tmp, 'wb') as f:
            pickle.dump(val, f)
        os.replace(tmp, path)          # atomic: parallel runs never read a half-written cache
    return val, False


def cv_from_pickle(a, S):
    """cv_<group>.pkl's results, when written for the same decisions, folds and seed; else {}."""
    p = work(f'cv_{a.group}.pkl')
    if not p.exists():
        return {}
    with open(p, 'rb') as f:
        C = pickle.load(f)
    args = C.get('args', {})
    same = (args.get('folds') == a.folds and args.get('seed') == a.seed and not args.get('loo')
            and args.get('since') == a.since and args.get('until') == a.until
            and len(C['meta']) == len(S['meta'])
            and all((m['launch'], m['out'], m['name']) == (n['launch'], n['out'], n['name'])
                    for m, n in zip(C['meta'], S['meta'])))
    return C['results'] if same else {}


def table_lines(rows, title):
    """rows: [(label, ys, levels, verdict)] -> text: one line per label."""
    mets = list(YARDSTICK)
    w = 40
    out = [title, f'  {"":42} ' + ''.join(f'{m:>{w}}' for m in mets) + '   gate']
    for label, ys, lv, v in rows:
        cells = []
        for m in mets:
            if m in ys:
                ma, mb, d, lo, hi, n = ys[m]
                if m == 'page NLL':
                    cells.append(f'{ma:5.3f} ({d:+6.3f} [{lo:+6.3f},{hi:+6.3f}]) {n:4}'.rjust(w))
                else:
                    cells.append(f'{ma * 100:5.1f} ({d * 100:+5.1f} [{lo * 100:+5.1f},{hi * 100:+5.1f}]) {n:4}'.rjust(w))
            else:
                cells.append(' ' * w)
        out.append(f'  {label[:42]:42} ' + ''.join(cells) + f'   {verdict_word(v)}')
    return out


def main(argv=None):
    a = parse(argv)
    t_start = time.time()
    specs = select_specs(a.group, a.spec)
    three = a.group == 'three'
    D = load()
    S = build(D, include_nothing=three, v2=None, since=a.since, until=a.until)
    obv = obvious_pairs(S['needs'], S['cols'])
    fold_of = launch_folds(S, a.folds, a.seed)
    FOLD = fold_of[S['LA']]
    dkey = decisions_key(S)
    out = out_file(f'honest_{a.group}.txt')

    def P(*x):
        print(*x, flush=True)
        print(*x, file=out, flush=True)

    chunks = list(online_chunks(S, a.every, a.min_train))
    tested = np.zeros(len(S['Y']), dtype=bool)
    for _, te in chunks:
        tested[te] = True
    P(f'{len(S["Y"])} decisions, {(S["Y"] >= 0).sum()} picks, {len(S["launches"])} launches; builds '
      f'{a.since or "any"} .. {a.until or "any"}')
    P(f'online: refit every {a.every} picks once {a.min_train} picks precede the launch: {len(chunks)} refits, '
      f'{tested.sum()} decisions tested ({(tested & (S["Y"] >= 0)).sum()} picks) in '
      f'{len(set(S["LA"][tested].tolist()))} launches')
    P(f'CV: grouped {int(fold_of.max()) + 1}-fold by launch (seed {a.seed})')
    P(f'specs: {", ".join(specs)}\n')

    from_cv = cv_from_pickle(a, S)
    use = not a.no_cache
    res_cv, res_on, timing = {}, {}, {}
    for name, spec in specs.items():
        t0 = time.time()
        common_parts = (dkey, a.group, name, repr(sorted(spec.items())), a.maxiter)
        if name in from_cv:
            res_cv[name], how_cv = from_cv[name], 'cv pickle'
        else:
            res_cv[name], hit = cached(cache_path('cv', a.group, common_parts + (a.folds, a.seed)), use,
                                       lambda: cv_results(S, obv, spec, FOLD, maxiter=a.maxiter))
            how_cv = 'cache' if hit else 'fitted'

        def prog(n, L, name=name):
            if n % 25 == 0:
                print(f'    [{name}] {n} of {len(chunks)} refits (launch {L}) {time.time() - t0:.0f}s', flush=True)

        (res_on[name], fits), hit = cached(cache_path('online', a.group, common_parts + (a.every, a.min_train)), use,
                                           lambda: online_results(S, obv, spec, a.every, a.min_train,
                                                                  maxiter=a.maxiter, progress=prog))
        timing[name] = time.time() - t0
        P(f'[{timing[name]:.0f}s] {name}: CV {how_cv}, online {"cache" if hit else f"fitted ({fits} refits)"}')

    astar_cv, betas = astar_results(S, FOLD)
    astar_on = astar_online(S, a.every, a.min_train)
    P(f'A* page temperature, CV folds: {", ".join(f"{b:.3f}" for b in betas.values())}')

    ys, lv, verdicts = {}, {}, {}
    for name in specs:
        for meth, res, base, keep in (('CV (all)', res_cv[name], astar_cv, None),
                                      ('CV (online)', res_cv[name], astar_cv, tested),
                                      ('online', res_on[name], astar_on, tested)):
            y = yardstick(res, base, nboot=a.nboot, keep=keep)
            ys[(name, meth)] = y
            lv[(name, meth)] = {m: level(res, m, a.nboot, keep=keep) for m in YARDSTICK}
            verdicts[(name, meth)] = gate(y)
    for meth, res, keep in (('CV (all)', astar_cv, None), ('CV (online)', astar_cv, tested), ('online', astar_on, tested)):
        lv[(ASTAR, meth)] = {m: level(res, m, a.nboot, keep=keep) for m in YARDSTICK}

    P('\n=== levels (mean [95% launch-bootstrap CI] n; hit rates in %, page NLL in nats) ===')
    for meth in METHODS:
        P(f'-- {meth}')
        for name in (ASTAR,) + tuple(specs):
            cells = []
            for m in YARDSTICK:
                b = lv[(name, meth)][m]
                if not b:
                    cells.append(f'{m} -')
                elif m == 'page NLL':
                    cells.append(f'{m} {b[0]:5.3f} [{b[1]:5.3f},{b[2]:5.3f}] n={b[3]}')
                else:
                    cells.append(f'{m} {b[0] * 100:5.1f} [{b[1] * 100:5.1f},{b[2] * 100:5.1f}] n={b[3]}')
            P(f'  {name[:40]:40}  ' + ' | '.join(cells))

    P('\n=== yardstick: model - A* on the same choice sets, with R6\'s gate ===')
    for name in specs:
        for meth in METHODS:
            P('\n'.join(yardstick_lines(f'[{meth}] {name}', ys[(name, meth)])))
            v, why = verdicts[(name, meth)]
            P(f'    gate: {verdict_word(v)}  ({"; ".join(why)})')

    P('\n=== summary: model level (model - A* [95% CI]) n, per method ===')
    for meth in METHODS:
        P('\n'.join(table_lines([(name, ys[(name, meth)], lv[(name, meth)], verdicts[(name, meth)][0]) for name in specs],
                                f'-- {meth}')))
    P('\n=== R6 gate (menu lower end > 0 and pooled upper end >= 0) ===')
    for name in specs:
        P(f'  {name[:44]:44} ' + '   '.join(f'{meth}: {verdict_word(verdicts[(name, meth)][0]):10}' for meth in METHODS))

    with open(work(f'honest_{a.group}.pkl'), 'wb') as f:
        pickle.dump({'cv': res_cv, 'online': res_on, 'astar_cv': astar_cv, 'astar_online': astar_on, 'tested': tested,
                     'meta': S['meta'], 'launches': S['launches'], 'fold_of': fold_of, 'yardstick': ys, 'levels': lv,
                     'verdicts': verdicts, 'args': vars(a), 'timing': timing}, f)
    P(f'[{time.time() - t_start:.0f}s] wrote {work(f"honest_{a.group}.pkl")}')
    out.close()
    return {'ys': ys, 'verdicts': verdicts, 'tested': tested, 'online': res_on, 'cv': res_cv}


if __name__ == '__main__':
    main()

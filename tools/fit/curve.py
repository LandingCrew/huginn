"""The encumbrance curve fit (roadmap R6; doc 11 section 3, decision 1): the response curve of
one need, fitted from the player's picks inside doc 9's choice model, from a documented prior.
Writes curve_<need>.txt and curve_<need>.json in the work directory.

usage: python curve.py [--need encumbrance] [--col utility_carry_weight] [--spec NAME] [--three]
                       [--all-pairs] [--prior-centre 0.9] [--prior-centre-sd 0.05] [--prior-slope 40]
                       [--prior-slope-sd 0.5] [--rounds 10] [--quick-profile] [--since V] [--until V]
(after v3load.py)

The model. The need's column of N is replaced by its curve applied to the logged input
(`ctx.in`; a context without one keeps its logged need value):
    n_k(d) = logistic(r(d); c, s) = 1 / (1 + exp(-s (r(d) - c)))     (core/ResponseCurve.h)
and the curve's centre c and slope s are fitted with the model: a coarse start over c (the
posterior with the model refitted at each START_GRID centre and the prior slope), then
coordinate ascent -- fit the model (theta, b, kappa, ...) with the curve fixed; fit (c, ln s)
with the model fixed; repeat (--rounds, default 10, or until c moves less than 1e-4).

Only the need's obvious pairs read the curve (the default). The need's theta row is held at its
prior centre except at its obvious pairs (effects.csv: encumbrance x utility_carry_weight at
+T0, and its `~strength` twin under presence + strength), so the curve is fitted "from the
player's carry-weight picks" (roadmap R6) and nothing else. With every pair free (--all-pairs,
kept for comparison), a low centre turns the need into a "heavily loaded" indicator whose 243
dense weights fit whatever correlates with the load: on the mage's log (2026-10-28) the data's
NLL then fell monotonically to the grid's lowest centre, 0.70, while the ascent from the prior
stopped at 0.90. The spec (--spec, a cv.py name) decides the grading; the default is presence +
strength (doc 11 decision 5: Feather's carry-weight grade is 0.0046, which cancels any pair
term under the current grading).

The prior (documented here and in tools/fit/README.md; the user's answer of 2026-10-10,
doc 11 decision 1: "nearing" encumbered is a real want, so the curve is fitted from the picks
starting near 0.9, not moved by hand):
    c ~ Normal(0.9, 0.05^2)          the user's "nearing": a statement, not a derived value
    ln s ~ Normal(ln 40, 0.5^2)      the shipped slope (needs.csv, `logistic 0.95 40`)
At-cap picks (ratio >= 1) cannot tell a centre of 0.9 from one of 0.95 (both read ~1 there),
so with few picks below the cap the posterior stays near the prior; picks below 0.9, and
menu visits that pass a held carry-weight item over near 0.9, are what move it. --three
adds the `nothing` outcomes (doc 11: passing it over pushes c back up).

Reported: the picks of items with the paired column and their inputs; the start scan; the
fitted (c, s) with a Laplace interval (model held at its fit); the pair's weights; the profile
over c with the model refitted at every point (--quick-profile holds it at its fit instead:
faster, but conditional, favouring the fitted centre), with a WARNING when its best centre is on
the grid's edge. The json is what export.py --curve reads (the bootstrap's `curves`; export.py
re-reads the need through the curve before it fits, so theta matches it)."""
import argparse
import json
import math
import time

import numpy as np

from fitlib import *

CURVE_PRIOR = {'centre': 0.9, 'centre_sd': 0.05, 'slope': 40.0, 'slope_sd_log': 0.5}
START_GRID = tuple(float(c) for c in np.round(np.arange(0.70, 1.0601, 0.04), 2))
PROFILE_GRID = tuple(float(c) for c in np.round(np.arange(0.70, 1.0601, 0.02), 2))
H_STEP = np.array([1e-4, 1e-3])     # finite-difference steps for (c, ln s)


def logistic(x, c, s):
    """core/ResponseCurve.h's logistic; NaN reads 0 as there."""
    x = np.asarray(x, dtype=float)
    with np.errstate(over='ignore'):
        y = 1.0 / (1.0 + np.exp(-s * (np.nan_to_num(x, nan=0.0) - c)))
    return np.where(np.isnan(x), 0.0, y)


def with_curve(S, k, c, s):
    """S with need column k re-read through logistic(in; c, s) where the input was logged."""
    out = dict(S)
    N = S['N'].copy()
    r = S['IN'][:, k]
    have = np.isfinite(r)
    N[have, k] = logistic(r[have], c, s)
    out['N'] = N
    return out


def prior_penalty(c, ls, prior):
    return (0.5 * ((c - prior['centre']) / prior['centre_sd']) ** 2
            + 0.5 * ((ls - math.log(prior['slope'])) / prior['slope_sd_log']) ** 2)


def pair_only_mask(Sv, ov, k):
    """Model's theta_fixed: row k held, except at its obvious pairs (and their ~strength twins)."""
    m = np.zeros((len(Sv['needs']), Sv['J']), dtype=bool)
    m[k, :] = True
    CI = {c: j for j, c in enumerate(Sv['cols'])}
    for kk, j in ov:
        if kk == k:
            m[k, j] = False
            tw = CI.get(Sv['cols'][j] + '~strength')
            if tw is not None:
                m[k, tw] = False
    return m


class CurveFit:
    """The model at a given curve: S re-read through the curve, then the spec's variants."""

    def __init__(self, S, obv, spec, k, pair_only=True, split_names=None, maxiter=300):
        self.S, self.obv, self.k = S, obv, k
        self.pair_only, self.split_names, self.maxiter = pair_only, split_names, maxiter
        self.model_kw, self.which, _ = split_spec(spec)

    def model(self, c, s):
        Sv, ov = variant(with_curve(self.S, self.k, c, s), self.obv, self.which, self.split_names)
        fixed = pair_only_mask(Sv, ov, self.k) if self.pair_only else None
        M = Model(Sv, obvious=ov, nitems=len(self.S['items']), theta_fixed=fixed, **self.model_kw)
        Fm, _ = row_features(Sv, M.feat_spec)
        return Sv, M, Fm

    @staticmethod
    def hold(M, p):
        """p with the held theta weights put back at their prior centre."""
        if M.fixed is None:
            return p
        p = p.copy()
        a, b = M.off['theta']
        th = p[a:b].reshape(M.K, M.J)
        th[M.fixed] = M.theta0[M.fixed]
        p[a:b] = th.ravel()
        return p

    def fit(self, c, s, p0=None):
        """(p, the model's posterior, the data NLL, Sv, M) at curve (c, s), from p0 if given."""
        Sv, M, Fm = self.model(c, s)
        p0 = M.init() if p0 is None else self.hold(M, p0)
        p, f, _ = lbfgs(lambda q: M.loss(Sv, q, Fm)[:2], p0, maxiter=self.maxiter)
        return p, f, M.loss(Sv, p, Fm, prior=False)[0], Sv, M

    def nll(self, p, c, ls):
        """The data NLL at curve (c, e^ls) with the model held at p."""
        Sv, M, Fm = self.model(c, math.exp(ls))
        return M.loss(Sv, p, Fm, prior=False)[0]


def _num_grad(f, x, h):
    g = np.zeros_like(x)
    for i in range(len(x)):
        e = np.zeros_like(x)
        e[i] = h[i]
        g[i] = (f(x + e) - f(x - e)) / (2 * h[i])
    return g


def _num_hess(f, x, h):
    n = len(x)
    H = np.zeros((n, n))
    for i in range(n):
        for j in range(n):
            ei, ej = np.zeros(n), np.zeros(n)
            ei[i], ej[j] = h[i], h[j]
            H[i, j] = (f(x + ei + ej) - f(x + ei - ej) - f(x - ei + ej) + f(x - ei - ej)) / (4 * h[i] * h[j])
    return 0.5 * (H + H.T)


def fit_curve(S, obv, spec, need='encumbrance', prior=None, rounds=10, maxiter=300, split_names=None, log=None,
              pair_only=True, start_grid=START_GRID):
    """The coarse start, then coordinate ascent on the posterior of (model, c, ln s) (module
    docstring). Returns a dict: centre, slope, the model's parameters p, the model and its S, the
    Laplace sd of c and ln s, the start scan, the rounds and the posterior."""
    prior = dict(CURVE_PRIOR, **(prior or {}))
    k = S['needs'].index(need)
    cf = CurveFit(S, obv, spec, k, pair_only, split_names, maxiter)
    ls0 = math.log(prior['slope'])
    scan, p = [], None
    for c in start_grid:
        p, f, _, _, _ = cf.fit(c, prior['slope'], p)
        scan.append((float(c), float(f + prior_penalty(c, ls0, prior)), p))
    best = min(scan, key=lambda t: t[1])
    if log:
        log('  start scan, posterior - min: ' + '  '.join(f'{c:.2f}:{f - best[1]:.2f}' for c, f, _ in scan))
    x = np.array([best[0], ls0])
    p = best[2]
    hist = []
    for rnd in range(rounds):
        p, _, _, _, _ = cf.fit(x[0], math.exp(x[1]), p)

        def post(z, p=p):
            return cf.nll(p, z[0], z[1]) + prior_penalty(z[0], z[1], prior)

        x_new, f, _ = lbfgs(lambda z: (post(z), _num_grad(post, z, H_STEP)), x, maxiter=100, tol=1e-5)
        hist.append((rnd, float(x_new[0]), float(math.exp(x_new[1])), float(f)))
        if log:
            log(f'  round {rnd + 1}: centre {x_new[0]:.4f}  slope {math.exp(x_new[1]):.2f}  posterior {f:.3f}')
        moved = abs(x_new[0] - x[0])
        x = x_new
        if moved < 1e-4 and rnd > 0:
            break
    p, fm, _, Sv, M = cf.fit(x[0], math.exp(x[1]), p)    # the model at the final curve

    def post(z):
        return cf.nll(p, z[0], z[1]) + prior_penalty(z[0], z[1], prior)

    try:
        sd = np.sqrt(np.maximum(np.diag(np.linalg.inv(_num_hess(post, x, H_STEP * 10))), 0.0))
    except np.linalg.LinAlgError:
        sd = np.array([np.nan, np.nan])
    return {'need': need, 'k': k, 'centre': float(x[0]), 'slope': float(math.exp(x[1])), 'sd_centre': float(sd[0]),
            'sd_log_slope': float(sd[1]), 'p': p, 'model': M, 'S': Sv, 'prior': prior, 'rounds': hist,
            'scan': [(c, f) for c, f, _ in scan], 'posterior': float(fm + prior_penalty(x[0], x[1], prior)),
            'pair_only': pair_only, 'cf': cf}


def profile(fitres, grid=PROFILE_GRID, refit=True):
    """The profile over c at the fitted slope: (data NLL, posterior), each relative to its minimum
    over the grid. refit (the default) refits the model at every point, warm-started along the
    grid, and the posterior then carries the model's own prior terms; refit=False holds the model
    at its fit (conditional: it favours the fitted centre, since the pair was fitted with it)."""
    cf, prior, s = fitres['cf'], fitres['prior'], fitres['slope']
    ls = math.log(s)
    nll, post, p = [], [], fitres['p']
    for c in grid:
        if refit:
            p, f, d, _, _ = cf.fit(c, s, p)
            nll.append(d)
            post.append(f + prior_penalty(c, ls, prior))
        else:
            d = cf.nll(fitres['p'], c, ls)
            nll.append(d)
            post.append(d + prior_penalty(c, ls, prior))
    nll, post = np.array(nll), np.array(post)
    return nll - nll.min(), post - post.min()


def edge_warnings(grid, dn, dp):
    """WARNING lines when a profile's best centre is on the grid's edge (the data do not pin it)."""
    out = []
    for label, v in (('data NLL', dn), ('posterior', dp)):
        i = int(np.argmin(v))
        if i in (0, len(grid) - 1):
            out.append(f'WARNING: the {label} is lowest at the grid edge ({grid[i]:.2f}): not pinned by the data')
    return out


def pair_picks(S, col, k):
    """The picks of an item whose cap has the paired column: (decision index, input, grade)."""
    j = S['cols'].index(col)
    out = []
    for d in np.where(S['Y'] >= 0)[0]:
        g = S['CapMat'][S['CAPID'][d, S['Y'][d]], j]
        if g > 0:
            out.append((int(d), float(S['IN'][d, k]), float(g)))
    return out


def ini_line(need, c, s):
    """The [Needs] line for Huginn.ini (core/ResponseCurve.h's text form)."""
    return f'{need} = logistic {c:.4g} {s:.4g}'


def curve_json(fitres, spec_name, three, npicks):
    return {'need': fitres['need'], 'kind': 'logistic', 'centre': round(fitres['centre'], 6),
            'slope': round(fitres['slope'], 6), 'sd_centre': round(fitres['sd_centre'], 6),
            'sd_log_slope': round(fitres['sd_log_slope'], 6), 'prior': fitres['prior'], 'spec': spec_name,
            'outcomes': 'three' if three else 'picks', 'pair_only': fitres['pair_only'], 'pair_picks': npicks,
            'ini': ini_line(fitres['need'], fitres['centre'], fitres['slope'])}


def parse(argv=None):
    ap = argparse.ArgumentParser(description="Fit one need's response curve (default: encumbrance) from the picks.")
    ap.add_argument('--need', default='encumbrance')
    ap.add_argument('--col', default='utility_carry_weight', help="the need's paired effect column (for the report)")
    ap.add_argument('--spec', default='theta + b, presence + strength',
                    help='a cv.py spec name (picks specs; with --three, a three-outcome spec)')
    ap.add_argument('--three', action='store_true', help='the three-outcome fit (nothing records included)')
    ap.add_argument('--all-pairs', action='store_true', help="let every pair of the need read the curve (comparison)")
    ap.add_argument('--prior-centre', type=float, default=CURVE_PRIOR['centre'])
    ap.add_argument('--prior-centre-sd', type=float, default=CURVE_PRIOR['centre_sd'])
    ap.add_argument('--prior-slope', type=float, default=CURVE_PRIOR['slope'])
    ap.add_argument('--prior-slope-sd', type=float, default=CURVE_PRIOR['slope_sd_log'], help='sd of ln slope')
    ap.add_argument('--rounds', type=int, default=10)
    ap.add_argument('--quick-profile', action='store_true',
                    help='hold the model at its fit along the profile (fast, but conditional)')
    ap.add_argument('--since', metavar='V')
    ap.add_argument('--until', metavar='V')
    return ap.parse_args(argv)


def main(argv=None):
    import cv
    a = parse(argv)
    t0 = time.time()
    table = cv.THREE if a.three else cv.PICKS
    if a.spec not in table:
        raise SystemExit(f'--spec {a.spec!r} is not a {"three" if a.three else "picks"} spec of cv.py: {", ".join(table)}')
    spec = table[a.spec]
    if 'split' in split_spec(spec)[1] or split_spec(spec)[2]:
        raise SystemExit('split and latest-caps specs are diagnostics, not fitted here')
    prior = {'centre': a.prior_centre, 'centre_sd': a.prior_centre_sd, 'slope': a.prior_slope,
             'slope_sd_log': a.prior_slope_sd}
    D = load()
    S = build(D, include_nothing=a.three, v2=None, since=a.since, until=a.until)
    if a.need not in S['needs']:
        raise SystemExit(f'need {a.need!r} is not in the log')
    k = S['needs'].index(a.need)
    obv = obvious_pairs(S['needs'], S['cols'])
    if not any(kk == k for kk, _ in obv) and not a.all_pairs:
        raise SystemExit(f'{a.need} has no obvious pair (effects.csv): nothing would read its curve (--all-pairs?)')
    out = out_file(f'curve_{a.need}.txt')

    def P(*x):
        print(*x, flush=True)
        print(*x, file=out, flush=True)

    have = np.isfinite(S['IN'][:, k])
    P(f'{len(S["Y"])} decisions, {(S["Y"] >= 0).sum()} picks; {have.sum()} with a logged {a.need} input; spec {a.spec!r}'
      f'{" (three outcomes)" if a.three else ""}; {"every pair" if a.all_pairs else "the obvious pairs only"} read the curve')
    P(f'prior: centre ~ N({prior["centre"]}, {prior["centre_sd"]}^2), ln slope ~ N(ln {prior["slope"]}, '
      f'{prior["slope_sd_log"]}^2)')
    picks = pair_picks(S, a.col, k) if a.col in S['cols'] else []
    if picks:
        P(f'picks of an item with {a.col} ({len(picks)}):')
        for d, r, g in picks:
            m = S['meta'][d]
            P(f'    {m["launch"]}  {m["out"]:6} {str(m["name"])[:40]:40} input {r:.4f}  grade {g:.4f}')
    fr = fit_curve(S, obv, spec, a.need, prior, rounds=a.rounds, log=P, pair_only=not a.all_pairs)
    P(f'\nfitted: centre {fr["centre"]:.4f} (sd {fr["sd_centre"]:.4f}), slope {fr["slope"]:.2f} '
      f'(ln sd {fr["sd_log_slope"]:.3f}; Laplace, model held at its fit)')
    P(f'  [Needs] line: {ini_line(a.need, fr["centre"], fr["slope"])}')
    q = fr['model'].unpack(fr['p'])
    cols = fr['S']['cols']
    if q['theta'] is not None and a.col in cols:
        j = cols.index(a.col)
        P(f'  theta[{a.need}, {a.col}] = {q["theta"][k, j]:+.4f}  (prior centre {fr["model"].theta0[k, j]:+.2f})')
        if a.col + '~strength' in cols:
            P(f'  theta[{a.need}, {a.col}~strength] = {q["theta"][k, cols.index(a.col + "~strength")]:+.4f}')
    grid = PROFILE_GRID
    dn, dp = profile(fr, grid, refit=not a.quick_profile)
    how = ('held at its fit: conditional, it favours the fitted centre' if a.quick_profile
           else 'refitted at every point; posterior = data + every prior')
    P(f'\nprofile over the centre (slope at its fit; model {how}; 0 = best on the grid):')
    P(f'    {"centre":>6}  {"data dNLL":>9}  {"posterior":>9}  curve at 0.90 / 0.95 / 1.00')
    for c, x, y in zip(grid, dn, dp):
        v = logistic(np.array([0.9, 0.95, 1.0]), c, fr['slope'])
        P(f'    {c:6.2f}  {x:9.3f}  {y:9.3f}  {v[0]:.2f} / {v[1]:.2f} / {v[2]:.2f}')
    for w in edge_warnings(grid, dn, dp):
        P(w)
    js = curve_json(fr, a.spec, a.three, len(picks))
    with open(work(f'curve_{a.need}.json'), 'w', encoding='utf-8') as f:
        json.dump(js, f, indent=1)
    P(f'[{time.time() - t0:.0f}s] wrote {work(f"curve_{a.need}.json")}')
    out.close()
    return fr


if __name__ == '__main__':
    main()

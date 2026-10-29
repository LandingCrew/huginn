"""The bootstrap export for R8: fit one spec on every kept decision and write the bootstrap file
(format version 1, docs/architecture/9-bootstrap-format.md; bootstrap.py writes and checks it).

usage: python export.py [--spec NAME] [--three] [--forms Huginn_All.csv] [--curve curve_encumbrance.json ...]
                        [--out bootstrap.json] [--since V] [--until V] [--maxiter N]
(after v3load.py; --curve after curve.py)

What is written: theta (needs x effect columns) and its diagonal precision, theta_strength
for the presence + strength grading, theta_bias for a bias-need spec, kappa, u0 per need
(three-outcome specs), the recent_<family> weights (as rows of theta for each family and time
constant), the extra row-feature weights (ln util, util missing, slot N), the per-item b
keyed by plugin + local FormID, and the fitted response curves (--curve). A curve's
need is re-read through it (curve.with_curve) before the fit, so theta matches the curve the
file ships.

Items. The log names forms by their runtime FormID, which changes with the load order; the
file keys each item by the plugin that owns its FormID's load-order index and its FormID
within that plugin (LookupForm(local, plugin) rebuilds it), read from the `hg dump all` CSV
(--forms; default: Huginn_All.csv beside the v3 file v3load.py read; bootstrap.index_owners:
the owner is the defining plugin except for a record injected into a master's FormID
space). A dynamic (FF) form, a form the dump lacks and a form whose index owner the dump
cannot tell are dropped and counted (`items_dropped`). The dump's load
order must be the log's -- light (FE) plugins shift when a list update adds or removes one --
so an item whose catalog kind differs from the dump's, or a picked item whose logged name
differs from the dump's, is dropped and counted too, with a WARNING. That drops the wrong keys
these checks can see, not all of them: a never-picked item whose kind matches by chance, or an
index index_owners attributes wrongly, still gets a wrong key (the format doc, Items: R8's
reader cross-checks the resolved form's kind and name). b is this character's (it may not
transfer to a new one); theta is meant to.

The output goes to the work directory (bootstrap.json by default): never into the repository."""
import argparse
import datetime as dt
import time
from collections import Counter
from pathlib import Path

import numpy as np

import bootstrap
import curve
from fitlib import *

EXPORTABLE_FEATURES = {'astar', 'pos'}


def default_forms(D):
    src = D.get('source')
    if src:
        p = Path(src).with_name('Huginn_All.csv')
        if p.exists():
            return p
    return None


def build_range(meta):
    """[oldest, newest] build of the decisions: by version, then by log order (two builds of one
    version, '0.23.44 (aadfade)' and '0.23.44 (b1c2d3e)', are ordered by when they were logged,
    never by their hashes). A build that reads as no version sorts first."""
    first, last = {}, {}
    for i, m in enumerate(meta):
        first.setdefault(m['build'], i)
        last[m['build']] = i
    key = lambda b: version(b) or (0,)
    return [min(first, key=lambda b: (key(b), first[b])), max(last, key=lambda b: (key(b), last[b]))]


def check_spec(spec):
    model_kw, which, latest = split_spec(spec)
    if latest or 'split' in which:
        raise SystemExit('split and latest-caps specs are diagnostics: not exported')
    bad = set(model_kw.get('feat_spec', ())) - EXPORTABLE_FEATURES
    if bad:
        raise SystemExit(f'features {sorted(bad)} read the v2 log (the old engine context weights): not exported')
    grading = 'graded'
    for w in which:
        if w in ('presence', 'presence+strength'):
            grading = w
    if model_kw.get('bias_need') and grading == 'presence+strength':
        raise SystemExit('a bias need with presence + strength has a strength row the format does not carry')
    if not model_kw.get('theta', True):
        raise SystemExit('a spec without theta is a baseline: the file carries theta (not exported)')
    return model_kw, which, grading


def fit_all(S, obv, spec, maxiter=400):
    """The spec fitted on every decision of S: (variant S, model, p, final NLL)."""
    model_kw, which, _ = split_spec(spec)
    Sv, ov = variant(S, obv, which)
    M = Model(Sv, obvious=ov, nitems=len(S['items']), **model_kw)
    Fm, _ = row_features(Sv, M.feat_spec)
    p, f, it = lbfgs(lambda q: M.loss(Sv, q, Fm)[:2], M.init(), maxiter=maxiter)
    return Sv, M, p, f, it


def item_precision(M, Sv, p):
    """Diagonal precision of b: sum over choice-set rows of P (1 - P), plus the prior's 1 / sig_b^2."""
    Fm, _ = row_features(Sv, M.feat_spec)
    _, _, _, P = M.loss(Sv, p, Fm, prior=False)
    R = Sv['ITEM'].shape[1]
    Pr = P[:, :R]
    w = np.where(Sv['MASK'], Pr * (1 - Pr), 0.0)
    return np.bincount(Sv['ITEM'].ravel(), weights=w.ravel(), minlength=M.nitems)[:M.nitems] + 1.0 / M.sig_b ** 2


def pick_names(S):
    """{form: name} as the log recorded it for picks (the chosen row's form)."""
    out = {}
    inv = {i: f for f, i in S['items'].items()}
    for d in np.where(S['Y'] >= 0)[0]:
        out.setdefault(inv[int(S['ITEM'][d, S['Y'][d]])], S['meta'][d]['name'])
    return out


def log_kinds_of(D):
    """{form: catalog kind} as the log's rows name it (the first row of a form)."""
    out = {}
    for c in D['ctxs'].values():
        for r in c['rows']:
            if r[KIND]:
                out.setdefault(r[FORM], r[KIND])
    for d in D['decs']:
        for r in d.get('add') or []:
            if r[KIND]:
                out.setdefault(r[FORM], r[KIND])
    return out


def document(S, Sv, M, p, spec_name, grading, three, forms, curves, source, log=print, log_kinds=None):
    """The bootstrap document (bootstrap.make_document) from a fit on S (Sv: S with the variants).
    forms: bootstrap.read_forms; log_kinds: log_kinds_of(D), to check the form map's kinds."""
    q = M.unpack(p)
    K, J = len(S['needs']), len(S['cols'])        # the base needs and effect columns
    Fm, _ = row_features(Sv, M.feat_spec)
    info = M.info(Sv, p, Fm) if M.theta else None
    theta = q['theta'] if M.theta else np.zeros((M.K, Sv['J']))
    prec = (info + 1.0 / M.sig ** 2) if M.theta else np.zeros((M.K, Sv['J']))
    strength = grading == 'presence+strength'
    blk = lambda m, rows: (m[rows, :J], m[rows, J:2 * J] if strength else None)
    base = slice(0, K)
    nrec = len(Sv['needs']) - K                  # the recent_<family> rows (variant 'recent')
    th, ths = blk(theta, base)
    pr, prs = blk(prec, base)
    theta_bias = theta[M.K - 1, :J].tolist() if M.bias_need else None
    u0 = None
    if three:
        uo = q['uout']
        # the bias need is a constant 1, so its u0 weight (uout[M.K - 1]) folds into u0.bias
        u0 = {'bias': float(uo[-1] + (uo[M.K - 1] if M.bias_need else 0.0)),
              'need': {n: float(uo[i]) for i, n in enumerate(S['needs'])}}
    recent = None
    if nrec:
        feats = []
        for r, name in enumerate(Sv['needs'][K:K + nrec]):
            fam, tau = name[len('recent_'):].rsplit('@', 1)
            row = K + r
            f = {'family': fam, 'tau_s': float(tau.rstrip('s')), 'theta': theta[row, :J].tolist(),
                 'precision': prec[row, :J].tolist()}
            if strength:
                f['theta_strength'] = theta[row, J:2 * J].tolist()
            if three:
                f['u0'] = float(q['uout'][row])
            feats.append(f)
        taus = sorted({f['tau_s'] for f in feats})
        recent = {'window_s': max(taus), 'taus_s': taus, 'features': feats}
    features = list(zip(M.fnames, q['w'].tolist()))
    items, dropped = [], Counter()
    if M.item:
        bprec = item_precision(M, Sv, p)
        npick = Counter()
        for d in np.where(S['Y'] >= 0)[0]:
            npick[int(S['ITEM'][d, S['Y'][d]])] += 1
        names = pick_names(S)
        owners = bootstrap.index_owners(forms)
        mismatch, injected, unknown = [], [], []
        for form, i in S['items'].items():
            key, why = bootstrap.item_key(form, forms, owners)
            if key is None:
                dropped[why] += 1
                if why == 'index owner unknown':
                    unknown.append(form)
                continue
            dplugin, dname, dkind = forms[int(form, 16)]
            # A form map from another load order points this FormID at another record: drop the
            # wrong keys we can see. The kind is checked for every item, the name for picked ones.
            lk = (log_kinds or {}).get(form)
            if lk and dkind and lk != dkind:
                dropped['kind differs from the form map'] += 1
                mismatch.append((form, f'kind {lk}', f'kind {dkind}'))
                continue
            if form in names and names[form] and dname and names[form] != dname:
                dropped['name differs from the form map'] += 1
                mismatch.append((form, names[form], dname))
                continue
            if dplugin.lower() != key[0].lower():
                injected.append((form, dplugin, key[0]))
            items.append((key[0], key[1], dname or names.get(form) or '', float(q['b'][i]), float(bprec[i]), npick[i]))
        if unknown:
            at = sorted({bootstrap.index_of(int(f, 16)) for f in unknown})
            log(f'WARNING: {len(unknown)} items dropped: the form map cannot tell which plugin owns their load-order '
                f'index ({", ".join(f"{x:X}" for x in at[:5])}; seen there: '
                f'{sorted({forms[i][0] for i in forms if bootstrap.index_of(i) in at[:5]})[:6]})')
        if injected:
            log(f'{len(injected)} items are records injected into the FormID space of another plugin, keyed by the index '
                f'owner (bootstrap.index_owners): ' + ', '.join(f'{f} ({d} in {o})' for f, d, o in injected[:5]))
        if mismatch:
            log(f'WARNING: {len(mismatch)} forms disagree with the form map and were dropped (a load order change? '
                f'run `hg dump all` on the load order the log was written with):')
            for form, a, b in mismatch[:10]:
                log(f'    {form}: log {a!r}, map {b!r}')
    return bootstrap.make_document(
        source=source, spec=spec_name, outcomes='three' if three else 'picks', grading=grading,
        needs=S['needs'], cols=S['cols'], theta=th.tolist(), theta_precision=pr.tolist(),
        theta_strength=None if ths is None else ths.tolist(),
        theta_strength_precision=None if prs is None else prs.tolist(), theta_bias=theta_bias,
        kappa=float(q['kappa']), u0=u0, recent=recent, features=features, items=items, items_dropped=dict(dropped),
        curves=curves, created_utc=dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d %H:%M:%S'))


def parse(argv=None):
    ap = argparse.ArgumentParser(description='Fit one spec on every decision and write the R8 bootstrap file.')
    ap.add_argument('--spec', default='theta + b + recent_<family>', help='a cv.py spec name')
    ap.add_argument('--three', action='store_true', help='a three-outcome spec (cv.py THREE)')
    ap.add_argument('--forms', help="`hg dump all` CSV (default: Huginn_All.csv beside the v3 file)")
    ap.add_argument('--curve', nargs='+', default=[], metavar='JSON', help="curve.py's curve_<need>.json")
    ap.add_argument('--out', default='bootstrap.json', help='file name in the work directory')
    ap.add_argument('--since', metavar='V')
    ap.add_argument('--until', metavar='V')
    ap.add_argument('--maxiter', type=int, default=400)
    return ap.parse_args(argv)


def main(argv=None):
    import json
    import cv
    a = parse(argv)
    t0 = time.time()
    table = cv.THREE if a.three else cv.PICKS
    if a.spec not in table:
        raise SystemExit(f'--spec {a.spec!r} is not a {"three" if a.three else "picks"} spec of cv.py: {", ".join(table)}')
    spec = table[a.spec]
    _, _, grading = check_spec(spec)
    if Path(a.out).name != a.out:
        raise SystemExit('--out is a file name in the work directory')
    D = load()
    forms_path = Path(a.forms) if a.forms else default_forms(D)
    if forms_path is None or not forms_path.exists():
        raise SystemExit('no form map: pass --forms <Huginn_All.csv> (`hg dump all` in game writes it)')
    forms = bootstrap.read_forms(forms_path)
    curves = {}
    for path in a.curve:
        with open(path, encoding='utf-8') as f:
            c = json.load(f)
        if c.get('kind') != 'logistic':
            raise SystemExit(f'{path}: only logistic curves are written (format version 1)')
        curves[c['need']] = (c['kind'], c['centre'], c['slope'])
    S = build(D, include_nothing=a.three, v2=None, since=a.since, until=a.until)
    for need, (_, c, s) in curves.items():
        # theta is fitted on the need as the shipped curve will read it (curve.py's fit)
        if need not in S['needs']:
            raise SystemExit(f'curve for {need!r}: not a need of the log')
        S = curve.with_curve(S, S['needs'].index(need), c, s)
    obv = obvious_pairs(S['needs'], S['cols'])
    Sv, M, p, f, it = fit_all(S, obv, spec, a.maxiter)
    print(f'{len(S["Y"])} decisions, {(S["Y"] >= 0).sum()} picks, {len(S["launches"])} launches; '
          f'{a.spec!r} fitted: posterior {f:.2f} after {it + 1} iterations')
    builds = build_range(S['meta'])
    chars = sorted({str(d['char']) for d in D['decs']})
    if not D.get('char') and len(chars) > 1:
        print(f'WARNING: {len(chars)} characters in v3.pkl ({", ".join(chars)}): b mixes them (v3load.py --char)')
    source = {'char': D.get('char') or (chars[0] if len(chars) == 1 else None), 'launches': len(S['launches']), 'first_launch': S['launches'][0],
              'last_launch': S['launches'][-1], 'decisions': int(len(S['Y'])), 'picks': int((S['Y'] >= 0).sum()),
              'builds': builds, 'forms': forms_path.name, 'tool': 'tools/fit/export.py'}
    doc = document(S, Sv, M, p, a.spec, grading, a.three, forms, curves, source, log_kinds=log_kinds_of(D))
    errs = bootstrap.validate(doc)
    if errs:
        raise SystemExit('the document does not validate: ' + '; '.join(errs))
    text = bootstrap.dumps(doc)
    with open(work(a.out), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(text)
    print(f'items: {len(doc["items"])} keyed, dropped {doc["items_dropped"] or "none"}; curves {list(curves) or "none"}')
    print(f'[{time.time() - t0:.0f}s] wrote {work(a.out)} ({len(text) // 1024} KiB)')
    return doc


if __name__ == '__main__':
    main()

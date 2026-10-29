"""R8.2 replay equivalence: fitlib's per-row logits under a bootstrap file, written as a fixture
the C++ core (core/ChoiceScore.h, tests/core/ChoiceReplayTests.cpp) must reproduce to 1e-5.

usage:
  python export_scores.py --synthetic [--decisions N] [--seed S] [--out FILE]
        a synthetic log (two launches, menu co-picks, light and dynamic forms, the bootstrap's
        own items) run through fitlib.build, scored under the shipped bootstrap. With --write-fixture
        it writes tests/core/fixtures/choice/replay_synthetic.jsonl (checked in).
  python export_scores.py [--bootstrap FILE] [--forms Huginn_All.csv] [--until V] [--out FILE]
        the decisions of v3.pkl (after v3load.py --char <id>; the v2 log for ln learn mult, as the
        fit reads it) scored under the bootstrap; written to the work directory, never the repo.
  python export_scores.py --compare CPP_OUT.jsonl [FIXTURE]
        reads the C++ scores (HUGINN_REPLAY_OUT, written by huginn_core_tests' replay test) beside
        the fixture: the largest difference in mu and in the logit, and hit@8, page hit@1 and the
        page / pick NLL of both (fitlib.metrics_from_scores on the same decisions).

What "fitlib's logits" are: fitlib.Model.scores with p built from the bootstrap -- theta rows for
the dataset's needs (a pair the file lacks at its effects.csv prior, as the C++ reader fills it),
the recent_<family> rows appended as needs (fitlib.with_recency; the file's rows, the others 0),
b per form through the form map (bootstrap.item_key, as the plugin's LookupForm resolves it),
kappa, and the feature weights of the spec ('learn': ln learn mult, util missing). The need column
of each curve the file ships is re-read through it (curve.with_curve), as export.py fitted it.
theta_covered is 0 (format v1 does not carry it; the fit did not read covered).

The fixture (JSON lines):
  head  {"t":"head","bootstrap":<path>,"needs":[...],"cols":[...],"items":{FORM:[plugin,local,name]},
         "decisions":n,"source":...}
  pick  {"t":"pick","lg":<launch|gen>,"utc":<seconds>,"cms":<confirmMs>,"ts":<fitlib pick time>,"id":<k>,
         "name":...,"list":...,"out":...,"ctx":bool,"chosen":bool,"use":bool,"cap":bool,"cols":[col index...]}
        EVERY pick of the log, unfiltered, in logged-utc order: the C++ test applies
        Choice::UsablePick and the confirmMs subtraction (Choice::PickTime) itself, as
        ChoiceSubscriber does live, and checks both against fitlib.recent_families' timeline
        ("use") and fitlib.pick_time ("ts", null for a pick fitlib does not use)
  dec   {"t":"dec","i":<S index>,"lg":...,"utc":...,"cms":...,"ts":<pick time>,"id":<k>,"n":[[need,v]...],"in":[[need,v]...],
         "rec":{"family@tau":v},"y":<chosen row>,"out":"key"/"wheel"/"menu",
         "rows":[[cap pairs],[cross pairs by column],off,FORM,lm,um,mu,logit]...}
"""
import argparse
import csv
import datetime as dt
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

import bootstrap
import curve
from common import *
from fitlib import *

FIXTURE = REPO / 'tests' / 'core' / 'fixtures' / 'choice' / 'replay_synthetic.jsonl'
SHIPPED = REPO / 'configs' / 'Huginn_Bootstrap.json'


# ---------------------------------------------------------------------------
# the logits under a bootstrap
# ---------------------------------------------------------------------------
def fit_decisions(D, include_nothing=False, since=None, until=None):
    """fitlib.build's decision list, in its order (the S index of each)."""
    decs = [d for d in D['decs'] if d['list'] != SIMONRIM and d['C'] is not None]
    decs = [d for d in decs if d['name'] not in TRANSIENT]
    if not include_nothing:
        decs = [d for d in decs if d['out'] != 'nothing']
    lo, hi = (version(since) if since else None), (version(until) if until else None)
    if lo is not None or hi is not None:
        decs = [d for d in decs if version(d['build']) is not None
                and (lo is None or version(d['build']) >= lo) and (hi is None or version(d['build']) <= hi)]
    decs.sort(key=lambda d: (d['launch'], d['seq']))
    return decs


def timeline(D):
    """fitlib.recent_families' usable picks: (key, time, d)."""
    out = []
    for d in D['decs']:
        if (d['out'] != 'nothing' and d['list'] != SIMONRIM and d['C'] is not None and d['name'] not in TRANSIENT
                and d['chosen'] is not None):
            out.append(((d['launch'], d.get('gen', 1)), pick_time(d), d))
    out.sort(key=lambda x: (x[0], x[1]))
    return out


def recent_full(D, decs, cols, caps, taus):
    """REC with every column kept (fitlib.recent_families drops the all-zero ones, which are 0)."""
    M, names = recent_families(D, decs, cols, caps, taus)
    return M, names


def model_and_p(S, doc, forms_of_item):
    """The variant dataset and fitlib.Model with p from the bootstrap document (module docstring)."""
    NI = {n: i for i, n in enumerate(S['needs'])}
    CI = {c: j for j, c in enumerate(S['cols'])}
    K, J = len(S['needs']), len(S['cols'])
    obv = obvious_pairs(S['needs'], S['cols'])
    TH = np.zeros((K, J))
    for (k, j), sg in obv.items():
        TH[k, j] = sg * 1.0
    fN = {n: a for a, n in enumerate(doc['needs'])}
    fC = {c: b for b, c in enumerate(doc['cols'])}
    for n, k in NI.items():
        if n not in fN:
            continue
        row = doc['theta'][fN[n]]
        for c, j in CI.items():
            if c in fC:
                TH[k, j] = row[fC[c]]
    # recent rows: in the document's order
    feats = (doc['recent'] or {}).get('features', [])
    rec_names = [f"recent_{f['family']}@{f['tau_s']:g}s" for f in feats]
    RTH = np.zeros((len(feats), J))
    for r, f in enumerate(feats):
        for c, j in CI.items():
            if c in fC:
                RTH[r, j] = f['theta'][fC[c]]
    REC = np.zeros((len(S['Y']), len(feats)))
    have = {n: i for i, n in enumerate(S['recency_names'])}
    for r, n in enumerate(rec_names):
        if n in have:
            REC[:, r] = S['REC'][:, have[n]]
    Sv = dict(S)
    Sv['N'] = np.hstack([S['N'], REC])
    Sv['needs'] = list(S['needs']) + rec_names
    Sv['K'] = K + len(feats)
    spec_feats = []
    W = {f['name']: f['w'] for f in doc['features']}
    if 'ln learn mult' in W or 'util missing' in W:
        spec_feats.append('learn')
    M = Model(Sv, theta=True, item=True, kappa=True, outside=False, feat_spec=spec_feats, nitems=len(S['items']))
    p = np.zeros(M.P)
    a, b = M.off['theta']
    p[a:b] = np.vstack([TH, RTH]).ravel()
    a, b = M.off['b']
    bvec = np.zeros(M.nitems)
    for form, i in S['items'].items():
        bvec[i] = forms_of_item.get(form, (None, 0.0))[1]
    p[a:b] = bvec
    p[M.off['kappa'][0]] = doc['kappa']
    a, b = M.off['w']
    p[a:b] = [W.get(n, 0.0) for n in M.fnames]
    return Sv, M, p, REC, rec_names


def item_map(doc, forms):
    """{form hex: ((plugin, local, name), b)} for the document's items through the form map."""
    owners = bootstrap.index_owners(forms)
    loc_of = {}
    for fid in forms:
        key, _ = bootstrap.item_key(fid, forms, owners)
        if key:
            loc_of[(key[0].lower(), key[1])] = f'{fid:08X}'
    out = {}
    for it in doc['items']:
        f = loc_of.get((it['plugin'].lower(), it['local']))
        if f:
            out[f] = ((it['plugin'], it['local'], it.get('name', '')), it['b'])
    return out


def fmt(v):
    return float(f'{v:.12g}')


def write_fixture(path, D, S, decs, doc, doc_path, imap, source):
    curves = doc.get('curves') or {}
    NI = {n: i for i, n in enumerate(S['needs'])}
    for need, (_, c, s) in curves.items():
        if need in NI:
            S = curve.with_curve(S, NI[need], c, s)
    Sv, M, p, REC, rec_names = model_and_p(S, doc, imap)
    Fm, _ = row_features(Sv, M.feat_spec)
    s_item, logit, _ = M.scores(Sv, p, Fm)
    inv_item = {i: f for f, i in S['items'].items()}
    usable = {id(d) for _, _, d in timeline(D)}
    picks = list(D['decs'])
    ids = {id(d): k for k, d in enumerate(picks)}
    lg = lambda key: f'{key[0]}|{key[1]}'

    def logged_utc(d):
        # the pick's own utc (a record without one: its context's), as the log carries it
        if d.get('utc'):
            return T(d['utc'])
        return T(d['C']['utc']) if d.get('C') else None

    t0 = min([u for u in (logged_utc(d) for d in picks) if u is not None] + [pick_time(d) for d in decs])
    secs = lambda t: (t - t0).total_seconds()
    utc_s = lambda d: secs(logged_utc(d)) if logged_utc(d) is not None else 0.0
    cms = lambda d: fmt(float(d.get('confirmMs') or 0))
    curve_needs = set(curves) & set(NI)
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(json.dumps({'t': 'head', 'bootstrap': str(doc_path), 'needs': S['needs'], 'cols': S['cols'],
                             'items': {f: list(v[0]) for f, v in imap.items() if f in S['items']},
                             'decisions': len(decs), 'source': source, 'kappa': doc['kappa'],
                             'recent': rec_names}) + '\n')
        for k in sorted(range(len(picks)), key=lambda k: (utc_s(picks[k]), k)):
            d = picks[k]
            ch = d.get('chosen')
            cid = ch[CAP] if ch is not None else -1
            cols = [j for j, v in D['caps'][cid] if v > 0] if cid >= 0 else []
            use = id(d) in usable
            fh.write(json.dumps({'t': 'pick', 'lg': lg((d['launch'], d.get('gen', 1))), 'utc': utc_s(d), 'cms': cms(d),
                                 'ts': secs(pick_time(d)) if use else None, 'id': k, 'name': d['name'],
                                 'list': d['list'], 'out': d['out'], 'ctx': d.get('C') is not None,
                                 'chosen': ch is not None, 'use': use, 'cap': cid >= 0,
                                 'cols': sorted(set(cols))}) + '\n')
        Dn, R = S['CAPID'].shape
        for i, d in enumerate(decs):
            rows = []
            for ri in range(R):
                if not S['MASK'][i, ri]:
                    continue
                cap = S['CapMat'][S['CAPID'][i, ri]]
                capp = [[int(j), fmt(cap[j])] for j in np.nonzero(cap)[0]]
                xs = defaultdict(float)
                for xi, j in enumerate(S['xcol']):
                    v = S['X'][i, ri, xi]
                    if v != 0.0:
                        xs[int(j)] += float(v)
                rows.append([capp, [[j, fmt(v)] for j, v in sorted(xs.items())], int(S['INHA'][i, ri]),
                             inv_item[int(S['ITEM'][i, ri])], fmt(S['LRN'][i, ri, 0]),
                             1 if np.isnan(S['UTIL'][i, ri]) else 0, fmt(s_item[i, ri]), fmt(logit[i, ri])])
            n = [[S['needs'][k], fmt(v)] for k, v in enumerate(S['N_raw'][i]) if v != 0.0]
            inp = [[nm, fmt(S['IN'][i, NI[nm]])] for nm in sorted(curve_needs) if np.isfinite(S['IN'][i, NI[nm]])]
            rec = {nm[len('recent_'):-1]: fmt(REC[i, r]) for r, nm in enumerate(rec_names) if REC[i, r] != 0.0}
            fh.write(json.dumps({'t': 'dec', 'i': i, 'lg': lg((d['launch'], d.get('gen', 1))), 'utc': utc_s(d),
                                 'cms': cms(d), 'ts': secs(pick_time(d)),
                                 'id': ids.get(id(d), -1), 'n': n, 'in': inp, 'rec': rec, 'y': int(S['Y'][i]),
                                 'out': d['out'], 'rows': rows}) + '\n')
    return Sv, M, p, s_item, logit


def scores_for(D, doc, forms, v2=None, until=None, since=None, lrn_seed=None):
    S = build(D, include_nothing=False, v2=v2, since=since, until=until)
    S['N_raw'] = S['N'].copy()
    if lrn_seed is not None:
        # synthetic: a learner multiplier on the scored rows (the v2 join a log would give)
        rng = np.random.default_rng(lrn_seed)
        scored = ~np.isnan(S['UTIL'])
        S['LRN'] = S['LRN'].copy()
        S['LRN'][..., 0] = np.where(scored, rng.uniform(-0.4, 1.6, scored.shape), 0.0)
    decs = fit_decisions(D, since=since, until=until)
    if len(decs) != len(S['Y']):
        raise SystemExit(f'decision order mismatch: {len(decs)} vs {len(S["Y"])}')
    for i, d in enumerate(decs):
        if S['meta'][i]['launch'] != d['launch'] or S['meta'][i]['out'] != d['out']:
            raise SystemExit(f'decision {i}: S and the decision list disagree')
    return S, decs


# ---------------------------------------------------------------------------
# the synthetic log
# ---------------------------------------------------------------------------
def synthetic_D(doc, n_dec=48, seed=5):
    """A log in v3.pkl's shape (common.load) whose items include the bootstrap's own: two launches
    of two load generations each, menu co-picks sharing a context, one transient and one SimonRim
    pick (they never count), a pick without a cap, light (FE) and dynamic (FF) forms."""
    rng = np.random.default_rng(seed)
    cols = [r['id'] for r in csv.DictReader(open(EFFECTS_CSV, encoding='utf-8'))]
    CI = {c: j for j, c in enumerate(cols)}
    needs = list(doc['needs'][:40]) + ['time_to_edge']          # one the file lacks: its prior
    cross = ['overshoot_health', 'overshoot_magicka', 'overshoot_stamina', 'weapon_charge', 'stack_count',
             'ammo_matches_launcher', 'school_fortified', 'covered']
    fams = [c for c in cols if c in ('restore', 'damage', 'resist', 'summon', 'survival')]
    specifics = {'restore': ['restore_health', 'restore_magicka', 'restore_stamina'], 'damage': ['damage_health_fire'],
                 'resist': ['resist_fire'], 'summon': ['summon_creature'], 'survival': ['survival_hunger']}
    kinds = ['kind_potion', 'kind_spell', 'kind_food', 'kind_scroll']
    caps = []
    for _ in range(14):
        c = {}
        f = fams[rng.integers(len(fams))]
        v = float(np.round(rng.uniform(0.1, 1.0), 4))
        c[CI[f]] = v
        c[CI[specifics[f][rng.integers(len(specifics[f]))]]] = v
        c[CI[kinds[rng.integers(len(kinds))]]] = 1.0
        if rng.random() < 0.4:
            c[CI['duration']] = float(np.round(rng.uniform(0.1, 1.0), 4))
        caps.append(tuple(sorted(c.items())))
    # forms: the bootstrap's first items (through a synthetic form map), plus light and dynamic ones
    forms, fmap = [], {}
    idx = {}
    for it in doc['items'][:10]:
        pl = it['plugin']
        if pl.lower().endswith('.esl'):
            i = idx.setdefault(pl, len(idx))
            fid = (0xFE << 24) | ((0x100 + i) << 12) | (int(it['local'], 16) & 0xFFF)
        else:
            i = idx.setdefault(pl, len(idx) + 1)
            fid = (i << 24) | int(it['local'], 16)
        forms.append(f'{fid:08X}')
        fmap[fid] = (pl, it.get('name', ''), 'Spell')
    forms += ['FE123456', 'FF000801', '00ABCDEF', '00012345']
    names = {f: f'Item {f}' for f in forms}
    for fid, (pl, nm, _) in fmap.items():
        names[f'{fid:08X}'] = nm or names[f'{fid:08X}']
    cap_of = {f: int(rng.integers(len(caps))) for f in forms}
    cap_of['00012345'] = -1                                      # outside the catalog
    ctxs, decs = {}, []
    launches = ['20261020-100000', '20261021-100000']
    seq = defaultdict(int)
    t = dt.datetime(2026, 10, 20, 10, 0, 0)
    for k in range(n_dec):
        launch = launches[0] if k < n_dec // 2 else launches[1]
        gen = 1 if (k % (n_dec // 2)) < n_dec // 4 else 2
        t += dt.timedelta(seconds=float(rng.choice([1.5, 3.0, 6.0, 12.0, 25.0, 40.0])))
        rows = []
        for f in forms:
            if rng.random() < 0.15:
                continue
            fl = F['held']
            if rng.random() < 0.6:
                fl |= F['eligible']
            if rng.random() < 0.35:
                fl |= F['shown']
            if rng.random() < 0.08:
                fl |= F['equipped']
            util = float(np.round(rng.uniform(0.05, 3.0), 4)) if (fl & F['eligible']) and rng.random() < 0.7 else None
            if util is not None:
                fl |= F['scored']
            x = []
            if rng.random() < 0.3:
                x.append((0, float(np.round(rng.uniform(-1, 1), 4))))
            if rng.random() < 0.3:
                x.append((4, float(np.round(rng.uniform(0, 1), 4))))
            if rng.random() < 0.2:
                x.append((7, float(np.round(rng.uniform(0, 1), 4))))   # covered: not a column
            rows.append((f, 0, 'Spell', None, cap_of[f], fl, 2 if fl & F['shown'] else -1, util, tuple(x), None,
                         None, None, None, None))
        if not rows:
            continue
        ckey = (launch, k)
        utc = (t - dt.timedelta(milliseconds=300)).strftime('%Y-%m-%d %H:%M:%S.%f')[:23]
        need = {}
        for j in rng.choice(len(needs), 4, replace=False):
            need[int(j)] = float(np.round(rng.uniform(0.05, 1.0), 4))
        enc = needs.index('encumbrance') if 'encumbrance' in needs else None
        inn = {}
        if enc is not None:
            inn[enc] = float(np.round(rng.uniform(0.5, 1.2), 4))
            need[enc] = float(np.round(1 / (1 + math.exp(-40 * (inn[enc] - 0.95))), 4))
        ctxs[ckey] = {'utc': utc, 'rows': rows, 'need': need, 'in': inn}
        picks = 2 if rng.random() < 0.2 else 1                    # a menu co-pick: two picks of one context
        for c in range(picks):
            ri = int(rng.integers(len(rows)))
            out = 'menu' if picks == 2 else ('key' if rows[ri][FLAGS] & F['shown'] else rng.choice(['menu', 'wheel']))
            seq[launch] += 1
            d = {'launch': launch, 'seq': seq[launch], 'gen': gen, 'out': str(out), 'list': 'LoreRim-5',
                 'name': names[rows[ri][FORM]], 'form': rows[ri][FORM], 'row': ri, 'add': [], 'ctxkey': ckey,
                 'utc': (t + dt.timedelta(seconds=0.4 * c)).strftime('%Y-%m-%d %H:%M:%S.%f')[:23],
                 'confirmMs': int(rng.integers(0, 3000)), 'build': '0.23.56 (synthetic)', 'case': '', 'ep': None,
                 'char': 'FEFD1B039AC78799', 'kind': '', 'repeat': 0}
            decs.append(d)
    # a transient spell's pick, a SimonRim pick, a nothing record and a pick without a context:
    # in no choice set and no timeline (the C++ replay applies Choice::UsablePick to every pick)
    first = decs[3]
    decs.append(dict(first, seq=999, name='Arcane Anchor', utc=first['utc'][:-3] + '901'))
    decs.append(dict(first, seq=1000, list=SIMONRIM, utc=first['utc'][:-3] + '902'))
    decs.append(dict(first, seq=1001, out='nothing', utc=first['utc'][:-3] + '903', confirmMs=0))
    decs.append(dict(first, seq=1002, ctxkey=('none', -1), utc=first['utc'][:-3] + '904'))
    D = {'needs': needs, 'cols': cols, 'cross': cross, 'caps': caps, 'ctxs': ctxs, 'decs': decs, 'heads': []}
    for d in D['decs']:
        c = D['ctxs'].get(d['ctxkey'])
        d['C'] = c
        d['rows'] = (c['rows'] if c else []) + d['add']
        d['chosen'] = d['rows'][d['row']] if 0 <= d['row'] < len(d['rows']) else None
    return D, fmap


# ---------------------------------------------------------------------------
# the comparison
# ---------------------------------------------------------------------------
def compare(cpp_path, fixture_path):
    fx = [json.loads(l) for l in open(fixture_path, encoding='utf-8')]
    decs = [r for r in fx if r['t'] == 'dec']
    cpp = {}
    for line in open(cpp_path, encoding='utf-8'):
        r = json.loads(line)
        cpp[r['i']] = r
    R = max(len(d['rows']) for d in decs)
    Dn = len(decs)
    S = {'CAPID': np.zeros((Dn, R), dtype=np.int32), 'MASK': np.zeros((Dn, R), dtype=bool),
         'INHA': np.zeros((Dn, R)), 'Y': np.full(Dn, -1, dtype=np.int32), 'LA': np.zeros(Dn, dtype=np.int32),
         'OUT': np.empty(Dn, dtype=object)}
    sf, lf = np.full((Dn, R), -np.inf), np.full((Dn, R), -np.inf)
    sc, lc = np.full((Dn, R), -np.inf), np.full((Dn, R), -np.inf)
    lgs = {}
    dmu = dlg = 0.0
    missing = 0
    for di, d in enumerate(decs):
        S['Y'][di] = d['y']
        S['OUT'][di] = d['out']
        S['LA'][di] = lgs.setdefault(d['lg'].split('|')[0], len(lgs))
        c = cpp.get(d['i'])
        if c is None:
            missing += 1
            continue
        for ri, row in enumerate(d['rows']):
            S['MASK'][di, ri] = True
            S['INHA'][di, ri] = row[2]
            sf[di, ri], lf[di, ri] = row[6], row[7]
            sc[di, ri], lc[di, ri] = c['mu'][ri], c['logit'][ri]
            dmu = max(dmu, abs(row[6] - c['mu'][ri]))
            dlg = max(dlg, abs(row[7] - c['logit'][ri]))

    def logp(L):
        m = L.max(axis=1, keepdims=True)
        lse = m[:, 0] + np.log(np.exp(L - m).sum(axis=1))
        return np.array([L[d, S['Y'][d]] - lse[d] for d in range(Dn)])

    out = {}
    for name, s, L in (('fitlib', sf, lf), ('C++', sc, lc)):
        res = metrics_from_scores(S, s, s, logp(L), tiebreak=np.zeros((Dn, R)))
        hit8 = np.mean([r['full_hit8'] for r in res if 'full_hit8' in r])
        ph1 = [r['page_hit1'] for r in res if r['out'] in ('key', 'wheel') and 'page_hit1' in r]
        pnll = [-r['page_logp'] for r in res if r['out'] in ('key', 'wheel') and 'page_logp' in r]
        nll = [-r['logp'] for r in res if r['logp'] is not None]
        out[name] = (float(hit8), float(np.mean(ph1)) if ph1 else float('nan'),
                     float(np.mean(pnll)) if pnll else float('nan'), float(np.mean(nll)))
    print(f'{Dn} decisions ({missing} without C++ scores), {int(S["MASK"].sum())} rows')
    print(f'largest |mu C++ - fitlib| {dmu:.3g}, |logit C++ - fitlib| {dlg:.3g}')
    print(f'{"":8} {"hit@8":>8} {"page hit@1":>11} {"page NLL":>10} {"pick NLL":>10}')
    for name, (h, p1, pn, n) in out.items():
        print(f'{name:8} {h:8.4f} {p1:11.4f} {pn:10.6f} {n:10.6f}')
    same = out['fitlib'][:2] == out['C++'][:2] and abs(out['fitlib'][2] - out['C++'][2]) < 1e-6 \
        and abs(out['fitlib'][3] - out['C++'][3]) < 1e-6
    print('identical hit@8 and page hit@1, NLL within 1e-6' if same else 'METRICS DIFFER')
    return dmu, dlg, out, same


def main(argv=None):
    ap = argparse.ArgumentParser(description='fitlib per-row logits under a bootstrap, for the C++ replay test.')
    ap.add_argument('--synthetic', action='store_true')
    ap.add_argument('--decisions', type=int, default=48)
    ap.add_argument('--seed', type=int, default=5)
    ap.add_argument('--write-fixture', action='store_true', help='--synthetic: write the checked-in fixture')
    ap.add_argument('--bootstrap', default=str(SHIPPED))
    ap.add_argument('--forms', help='Huginn_All.csv (logs: default beside the v3 file)')
    ap.add_argument('--until', metavar='V')
    ap.add_argument('--since', metavar='V')
    ap.add_argument('--out', default='replay_scores.jsonl', help='file name in the work directory')
    ap.add_argument('--compare', nargs='+', metavar='FILE', help='CPP_OUT [FIXTURE]')
    a = ap.parse_args(argv)
    if a.compare:
        fixture = a.compare[1] if len(a.compare) > 1 else str(work(a.out))
        return compare(a.compare[0], fixture)
    doc_path = Path(a.bootstrap)
    doc = json.loads(doc_path.read_text(encoding='utf-8'))
    errs = bootstrap.validate(doc)
    if errs:
        raise SystemExit('the bootstrap does not validate: ' + '; '.join(errs))
    rel = doc_path.resolve()
    try:
        rel = rel.relative_to(REPO)
    except ValueError:
        pass
    if a.synthetic:
        D, fmap = synthetic_D(doc, a.decisions, a.seed)
        imap = item_map(doc, fmap)
        S, decs = scores_for(D, doc, fmap, lrn_seed=a.seed)
        path = FIXTURE if a.write_fixture else work(a.out)
        if a.write_fixture:
            path.parent.mkdir(parents=True, exist_ok=True)
        write_fixture(path, D, S, decs, doc, rel.as_posix() if isinstance(rel, Path) else rel, imap,
                      f'synthetic (seed {a.seed})')
        print(f'wrote {path}: {len(decs)} decisions, {int(S["MASK"].sum())} rows')
        return path
    D = load()
    forms_path = Path(a.forms) if a.forms else Path(D['source']).with_name('Huginn_All.csv')
    forms = bootstrap.read_forms(forms_path)
    imap = item_map(doc, forms)
    v2 = load_v2() if any(f['name'] == 'ln learn mult' for f in doc['features']) else None
    S, decs = scores_for(D, doc, forms, v2=v2, until=a.until, since=a.since)
    path = work(a.out)
    write_fixture(path, D, S, decs, doc, rel.as_posix() if isinstance(rel, Path) else rel, imap,
                  f'{len(D.get("sources", []))} v3 file(s), char {D.get("char")}')
    print(f'wrote {path}: {len(decs)} decisions, {int(S["MASK"].sum())} rows, {len(imap)} items mapped')
    return path


if __name__ == '__main__':
    main()

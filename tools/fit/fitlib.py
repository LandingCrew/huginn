"""R6 trial fit: dataset builder, the conditional logit (doc 9 / theory page), baselines,
an L-BFGS optimiser and the metrics. numpy only.

Model (theory page, 'Model and notation', P11):
    s_i(d)  = n(d)^T Theta c_i  +  b_i  +  w . f_i(d)    (f = baseline row features, optional)
    menu    : items in H \\ A get  s_i - kappa
    outside : u0(d) = theta_out . n(d) + theta_out0        (only in the 3-outcome fit)
    P(y)    = exp(s_y) / (exp(u0) + sum_A exp(s_j) + sum_{H\\A} exp(s_j - kappa))
Priors: Theta_kj ~ N(theta0_kj, sigma^2) with theta0 = 0 except the obvious pairs of
effects.csv (theta0 = +T0, combat_regen_ok x kind_food = -T0, overshoot_* pairs 0: the
logged overshoot is signed and not curved), sigma_obv for obvious pairs, sigma_dense
otherwise; b_i ~ N(0, sigma_b^2).
"""
import bisect
import csv
import datetime as dt
import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

from common import *

SIMONRIM = 'simonrim-essentails'
TRANSIENT = {'Arcane Anchor'}   # LoreRim tome-learning spell (memory: churn expected)


# ---------------------------------------------------------------------------
# v2 join (context weights for the baselines)
# ---------------------------------------------------------------------------
def load_v2(path=None, since='2026-10-09 14'):
    """The v2 log's candidates by form, for the old-engine baselines (common.v2_path when
    no path is given). Empty when there is no v2 log: those baselines then read NaN."""
    idx = defaultdict(list)
    path = path or v2_path()
    if path is None or not Path(path).exists():
        print('no v2 log (Huginn_Selections.jsonl): the context-weight baselines read NaN', file=sys.stderr)
        return idx
    for line in open(path, encoding='utf-8', errors='replace'):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        if r.get('utc', '') < since or 'cands' not in r:
            continue
        cols = r['cols']
        cands = {}
        for c in r['cands']:
            c = dict(zip(cols, c))
            cl = 'FoodAny' if c['type'] == 'Food' else c['need']
            if c['form'] not in cands:
                cands[c['form']] = (c['ctx'], c['corr'], c['potion'], c['fav'], cl, c['util'])
        idx[r['form']].append((T(r['utc']), cands))
    return idx


def join_v2(idx, d):
    best = None
    for t, cands in idx.get(d['form'], []):
        dt_ = abs((t - T(d['utc'])).total_seconds())
        if dt_ < 2 and (best is None or dt_ < best[0]):
            best = (dt_, cands)
    return best[1] if best else None


# ---------------------------------------------------------------------------
# obvious pairs
# ---------------------------------------------------------------------------
def obvious_pairs(needs, cols, path=EFFECTS_CSV):
    NI = {n: i for i, n in enumerate(needs)}; CI = {c: j for j, c in enumerate(cols)}
    out = {}
    for r in csv.DictReader(open(path, encoding='utf-8')):
        for t in r['obvious_needs'].split(';'):
            t = t.strip()
            if t and t in NI and r['id'] in CI:
                sign = -1.0 if (t == 'combat_regen_ok' and r['id'] == 'kind_food') else 1.0
                if r['id'].startswith('overshoot_'):
                    sign = 0.0
                out[(NI[t], CI[r['id']])] = sign
    return out


# ---------------------------------------------------------------------------
# dataset
# ---------------------------------------------------------------------------
def version(build):
    """The plugin version in a head's `build` ('0.23.40 (f7a02d2)' -> (0, 23, 40)), or in a
    version given on the command line ('0.23.41'); None when it does not read as one."""
    try:
        return tuple(int(x) for x in str(build).split()[0].split('.'))
    except (ValueError, IndexError):
        return None


def segment_names(split_at):
    """Labels of the build segments split_at cuts ('0.23.41' -> ['<0.23.41', '>=0.23.41'])."""
    cuts = sorted(split_at, key=version)
    if not cuts:
        return ['all']
    return [f'<{cuts[0]}'] + [f'>={c}' + (f',<{n}' if n else '') for c, n in zip(cuts, cuts[1:] + [None])]


def build(D, include_nothing=True, latest_caps=False, v2=None, since=None, until=None, split_at=(),
          recency=(5.0, 30.0), laps='once'):
    """The dataset: one entry per decision, its choice set and the arrays the model reads.

    laps: the nothing records of lapped stretches (0.23.51: ep.why "lap", one record per
    need whose episode was open at a lap tick of the 60 s grid; core/NeedEpisodes.h).
      'once'  (default) one per LAP TICK (lap_tick: the records' launch, gen and utc; the
              stretches one tick cut are judged together and share one utc), so the k
              needs open then do not weigh one unanswered 60 s slice k times, while a pick
              is one record however many needs are on. The stretches of one tick do not
              all share a context: a stretch begun by a lap (ep.lap > 0) carries the
              previous lap's context, an episode's first stretch its onset's. The kept
              record is the lowest-seq one begun by a lap (its context is the slice's
              start), else the lowest-seq one.
      'all'   every one, as written.
      'none'  none: the data a build before 0.23.51 would have logged (ended episodes only).
    Ended-episode records (why "end", every record before 0.23.51) are kept as before. A
    record's meta carries ep_why ("end", "lap" or None for a pick).

    Build segmentation (the head's `build` of the segment that logged the decision):
      since, until  keep the decisions logged by plugin versions in [since, until] (both
                    inclusive, '0.23.41'; either may be None). A decision whose build does not
                    read as a version is kept only when neither is given.
      split_at      versions that cut the kept decisions into segments: SEG (per decision)
                    is the number of cuts at or below its version, `segments` their labels.
                    split_needs() turns that into per-segment need columns; the CV reports
                    its yardstick per segment too.
    recency: the time constants (s) of the recent_<family> features (doc 11 step 2): REC
    (decisions x features), exp(-dt / tau) for each effect family (and kind_*) of the item
    picked last in the launch when that pick is at most max(recency) s old; recency_names.
    Only the non-zero columns are kept. () skips them.
    IN (decisions x needs) holds each context's logged curve inputs (`ctx.in`, the value a
    need's response curve read), NaN where the context has none: the curve fit (curve.py)
    re-applies a curve to them."""
    needs, cols, caps, cross = D['needs'], D['cols'], D['caps'], D['cross']
    K, J = len(needs), len(cols)
    CI = {c: j for j, c in enumerate(cols)}
    # Cross-features that name an effect column scale that column; the others
    # (0.23.46: `covered`, not an effect column) go to arrays of their own.
    xpos = {i: n for n, i in enumerate(i for i, c in enumerate(cross) if c in CI)}
    xcol = np.array([CI[c] for c in cross if c in CI], dtype=int)
    COVIX = cross.index('covered') if 'covered' in cross else -1
    ncap = len(caps)
    CapMat = np.zeros((ncap + 1, J))                    # last row = no cap (not in catalog)
    for g, c in enumerate(caps):
        for j, v in c:
            CapMat[g, j] = v
    # latest cap per form (0.23.34 changed the tables): optional remap
    latest = {}
    if latest_caps:
        for key, c in sorted(D['ctxs'].items(), key=lambda kv: kv[1]['utc'] or ''):
            for r in c['rows']:
                if r[CAP] >= 0 and r[UID] == 0:
                    latest[r[FORM]] = r[CAP]
    decs = [d for d in D['decs'] if d['list'] != SIMONRIM and d['C'] is not None]
    decs = [d for d in decs if d['name'] not in TRANSIENT]
    if not include_nothing:
        decs = [d for d in decs if d['out'] != 'nothing']
    lo, hi = (version(since) if since else None), (version(until) if until else None)
    if lo is not None or hi is not None:
        decs = [d for d in decs if version(d['build']) is not None
                and (lo is None or version(d['build']) >= lo) and (hi is None or version(d['build']) <= hi)]
    if not decs:
        raise ValueError(f'no decisions to fit (builds {since or "any"} .. {until or "any"})')
    if any(version(v) is None for v in split_at):
        raise ValueError(f'split_at: {list(split_at)} are not all versions (0.23.41)')
    cuts = sorted(version(v) for v in split_at)
    decs.sort(key=lambda d: (d['launch'], d['seq']))
    if laps not in ('once', 'all', 'none'):
        raise ValueError(f"laps: {laps!r} is not 'once', 'all' or 'none'")
    if laps != 'all':
        rep = {}
        if laps == 'once':
            for i, d in enumerate(decs):
                if ep_why(d) != 'lap':
                    continue
                k = lap_tick(d)
                better = (d['ep'].get('lap', 0) > 0, -i)
                if k not in rep or better > rep[k][0]:
                    rep[k] = (better, i)
        keep = {i for _, i in rep.values()}
        decs = [d for i, d in enumerate(decs) if ep_why(d) != 'lap' or i in keep]
        if not decs:
            raise ValueError('no decisions to fit (only lap records, and laps="none")')
    # Plackett-Luce: picks sharing a context (menu co-picks), in seq order
    taken = defaultdict(set)
    items = {}
    launches = sorted({d['launch'] for d in decs})
    LI = {l: i for i, l in enumerate(launches)}
    recs = []
    for d in decs:
        ch = d['chosen']
        rows = []
        seen = set()
        for r in d['rows']:
            fl = r[FLAGS]
            is_ch = r is ch
            key = (r[FORM], r[UID])
            if not is_ch:
                if key in taken[d['ctxkey']]:
                    continue
                if fl & F['equipped']:
                    continue
                if not fl & (F['shown'] | F['held'] | F['eligible']):
                    continue
            else:
                if d['out'] in ('key', 'wheel') and not fl & F['shown']:
                    pass  # 2+2 picks whose chosen row was not flagged shown: treated as on the page below
            if key in seen and not is_ch:
                continue
            seen.add(key)
            rows.append((r, is_ch))
        if d['out'] != 'nothing':
            taken[d['ctxkey']].add((ch[FORM], ch[UID]))
        recs.append((d, rows))
    R = max(len(rows) for d, rows in recs)
    Dn = len(recs)
    CAPID = np.full((Dn, R), ncap, dtype=np.int32)
    XA = np.zeros((Dn, R, len(xcol)), dtype=np.float64)
    COV = np.zeros((Dn, R), dtype=np.float64)          # covered(i), 0 before 0.23.46
    MASK = np.zeros((Dn, R), dtype=bool)
    INHA = np.zeros((Dn, R))
    ITEM = np.zeros((Dn, R), dtype=np.int32)
    SLOTA = np.full((Dn, R), -1, dtype=np.int32)
    UTILA = np.full((Dn, R), np.nan)
    V2 = np.full((Dn, R, 4), np.nan)          # ctx corr potion fav
    CLS = np.full((Dn, R), -1, dtype=np.int32)
    Y = np.full(Dn, -1, dtype=np.int32)
    N = np.zeros((Dn, K))
    IN = np.full((Dn, K), np.nan)             # the logged curve inputs (ctx.in), NaN when absent
    OUT = np.empty(Dn, dtype=object)
    LA = np.zeros(Dn, dtype=np.int32)
    v2ok = np.zeros(Dn, dtype=bool)
    SEG = np.zeros(Dn, dtype=np.int32)
    classes = {}
    meta = []
    for di, (d, rows) in enumerate(recs):
        for k, v in d['C']['need'].items():
            N[di, k] = v
        for k, v in (d['C'].get('in') or {}).items():
            if v is not None:
                IN[di, k] = v
        OUT[di] = d['out']; LA[di] = LI[d['launch']]
        SEG[di] = bisect.bisect_right(cuts, version(d['build']) or (0,))
        cands = join_v2(v2, d) if (v2 is not None and d['out'] != 'nothing') else None
        v2ok[di] = cands is not None
        for ri, (r, is_ch) in enumerate(rows):
            cid = r[CAP]
            if latest_caps and r[UID] == 0 and r[FORM] in latest:
                cid = latest[r[FORM]]
            CAPID[di, ri] = cid if cid >= 0 else ncap
            for j, v in r[X]:
                if j in xpos:
                    XA[di, ri, xpos[j]] = v
                elif j == COVIX:
                    COV[di, ri] = v
            MASK[di, ri] = True
            on_page = bool(r[FLAGS] & F['shown']) or (is_ch and d['out'] in ('key', 'wheel'))
            INHA[di, ri] = 0.0 if on_page else 1.0
            ITEM[di, ri] = items.setdefault(r[FORM], len(items))
            if r[FLAGS] & F['shown'] and r[SLOT] >= 0:
                SLOTA[di, ri] = min(r[SLOT], 7)
            if r[UTIL] is not None:
                UTILA[di, ri] = r[UTIL]
            if cands and r[FORM] in cands:
                c = cands[r[FORM]]
                V2[di, ri] = c[:4]
                CLS[di, ri] = classes.setdefault(c[4], len(classes))
            if is_ch:
                Y[di] = ri
        meta.append({'launch': d['launch'], 'out': d['out'], 'name': d['name'], 'kind': d['kind'],
                     'ctxkey': d['ctxkey'], 'build': d['build'], 'case': d['case'], 'need_ep': d['ep']['gi'] if d['ep'] else None,
                     'ep_why': ep_why(d)})
    REC, rec_names = recent_families(D, [d for d, _ in recs], cols, caps, recency)
    return dict(CapMat=CapMat, xcol=xcol, CAPID=CAPID, X=XA, COV=COV, MASK=MASK, INHA=INHA, ITEM=ITEM, SLOT=SLOTA, UTIL=UTILA,
                V2=V2, CLS=CLS, Y=Y, N=N, IN=IN, OUT=OUT, LA=LA, SEG=SEG, REC=REC, v2ok=v2ok, launches=launches, items=items,
                classes=classes, meta=meta, K=K, J=J, needs=needs, cols=cols, R=R,
                segments=segment_names(list(split_at)), recency_names=rec_names)


def ep_why(d):
    """A nothing record's stretch kind: 'lap' (a lapped stretch of a still-open episode,
    0.23.51) or 'end' (the episode expired; every nothing record before 0.23.51); None for
    a pick."""
    if d['out'] != 'nothing':
        return None
    ep = d.get('ep') or {}
    return ep.get('why', 'end')


def lap_tick(d):
    """The lap tick a lapped-stretch nothing record belongs to: (launch, gen, utc). The
    stretches one tick cut are judged on one tick and written with one utc (0.23.51,
    SelectionLogV3); two lap ticks are at least 60 active seconds apart."""
    return (d['launch'], d.get('gen'), d.get('utc'))


def pick_time(d):
    """When the decision was made: a pick's utc less its confirmation delay, a nothing's context utc."""
    if d['out'] == 'nothing':
        return T(d['C']['utc'])
    return T(d['utc']) - dt.timedelta(milliseconds=d.get('confirmMs') or 0)


def recent_families(D, decs, cols, caps, taus):
    """recent_<family> (doc 11 step 2) for each decision of decs: see build(). The previous pick
    is the last usable pick (a context, a chosen row, not simonrim or transient) of the same
    launch and load generation strictly before the decision."""
    if not taus:
        return np.zeros((len(decs), 0)), []
    fams = [r['id'] for r in csv.DictReader(open(EFFECTS_CSV, encoding='utf-8')) if r['level'] == 'family']
    fams += [c for c in cols if c.startswith('kind_')]
    CI = {c: j for j, c in enumerate(cols)}
    famj = [CI[f] for f in fams if f in CI]
    window = max(taus)
    timeline = defaultdict(list)                    # (launch, gen) -> [(t, dec)] in time order
    for d in D['decs']:
        if (d['out'] != 'nothing' and d['list'] != SIMONRIM and d['C'] is not None and d['name'] not in TRANSIENT
                and d['chosen'] is not None):
            timeline[(d['launch'], d.get('gen', 1))].append((pick_time(d), d))
    for v in timeline.values():
        v.sort(key=lambda x: x[0])
    times = {k: [t for t, _ in v] for k, v in timeline.items()}
    M = np.zeros((len(decs), len(famj) * len(taus)))
    for i, d in enumerate(decs):
        key = (d['launch'], d.get('gen', 1))
        lst = timeline.get(key)
        if not lst:
            continue
        t = pick_time(d)
        k = bisect.bisect_left(times[key], t) - 1   # the last pick strictly before t
        while k >= 0 and lst[k][1] is d:
            k -= 1
        if k < 0:
            continue
        age = (t - lst[k][0]).total_seconds()
        cid = lst[k][1]['chosen'][CAP]
        if age > window or cid < 0:
            continue
        cap = dict(caps[cid])
        for fi, j in enumerate(famj):
            if cap.get(j, 0) > 0:
                for ti, tau in enumerate(taus):
                    M[i, ti * len(famj) + fi] = math.exp(-age / tau)
    names = [f'recent_{cols[j]}@{tau:g}s' for tau in taus for j in famj]
    keep = M.any(axis=0)
    return M[:, keep], [n for n, k in zip(names, keep) if k]


def subset(S, idx):
    out = dict(S)
    for k in ('CAPID', 'X', 'COV', 'MASK', 'INHA', 'ITEM', 'SLOT', 'UTIL', 'V2', 'CLS', 'Y', 'N', 'IN', 'OUT', 'LA', 'SEG',
              'REC', 'v2ok'):
        out[k] = S[k][idx]
    out['meta'] = [S['meta'][i] for i in idx]
    return out


# ---------------------------------------------------------------------------
# variants of a (sub)set: each returns a new S and obvious-pair map, S unchanged
# ---------------------------------------------------------------------------
# The needs whose sensor changed meaning at 0.23.41 (README, 'Reading logs written by
# 0.23.41 and later'), plus every target_<family>: split_needs' default. Since 0.23.51 also
# the needs whose data changed then (README, 'Reading logs written before 0.23.51'): the
# persistent needs, whose nothing contexts before the laps were only other needs' onsets
# (a mild, mixed bias), and loadout_two_handed, which read a quarterstaff as two-handed
# before. Split at 0.23.51 (--split-at 0.23.51), their old-segment columns take the old
# sampling instead of the shared ones.
LAP_SPLIT_NEEDS = ('hunger', 'thirst', 'diseased', 'cold', 'warmth_deficit', 'fatigue', 'loadout_two_handed')
BUILD_SPLIT_NEEDS = ('follower_present', 'regen_suppressed', 'darkness', 'enemy_count', 'surrounded',
                     'boss_fight') + LAP_SPLIT_NEEDS


def split_need_names(S, names=None):
    """names, or BUILD_SPLIT_NEEDS and every target_<family>; 'all' = every need."""
    if names == 'all':
        return list(S['needs'])
    if names is None:
        return list(BUILD_SPLIT_NEEDS) + [n for n in S['needs'] if n.startswith('target_')]
    return list(names)


def split_needs(S, obv, names=None):
    """Each named need becomes one column per build segment (S['SEG'], build's split_at): the
    first segment keeps the column, every later one gets a copy '<need>@<segment>' that reads
    the need only in that segment (and the original reads 0 there). Obvious pairs are copied
    to the new columns. A no-op with one segment."""
    nseg = len(S['segments'])
    NI = {n: i for i, n in enumerate(S['needs'])}
    ks = [NI[n] for n in split_need_names(S, names) if n in NI]
    if nseg < 2 or not ks:
        return S, obv
    N = S['N'].copy()
    seg = S['SEG']
    extra, names_out, obv2 = [], [], dict(obv)
    for g in range(1, nseg):
        for k in ks:
            for (kk, j), sg in obv.items():
                if kk == k:
                    obv2[(S['K'] + len(extra), j)] = sg
            extra.append(N[:, k] * (seg == g))
            names_out.append(f"{S['needs'][k]}@{S['segments'][g]}")
    N[:, ks] *= (seg == 0)[:, None]
    out = dict(S)
    out['N'] = np.hstack([N, np.stack(extra, axis=1)])
    out['needs'] = list(S['needs']) + names_out
    out['K'] = S['K'] + len(extra)
    return out, obv2


def with_recency(S, obv):
    """The recent_<family> features (build's REC) appended as needs, so theta pairs them with
    every effect column (no obvious pairs: they start at 0)."""
    out = dict(S)
    out['N'] = np.hstack([S['N'], S['REC']])
    out['needs'] = list(S['needs']) + list(S['recency_names'])
    out['K'] = S['K'] + S['REC'].shape[1]
    return out, obv


def presence_grading(S, obv, strength=False):
    """Doc 11 step 1: an effect column reads 1 where the item has it (presence) instead of its
    grade; with strength, a second block of columns adds (grade - 0.5) where present. The
    obvious pairs stay on the presence columns."""
    out = dict(S)
    C = S['CapMat']
    P = (C > 0).astype(float)
    if strength:
        out['CapMat'] = np.hstack([P, (C - 0.5) * P])
        out['cols'] = list(S['cols']) + [c + '~strength' for c in S['cols']]
        out['J'] = 2 * S['J']
    else:
        out['CapMat'] = P
    return out, obv


VARIANTS = ('split', 'recent', 'presence', 'presence+strength')


def variant(S, obv, which, split_names=None):
    """S and obv with the named VARIANTS applied in order."""
    for w in which:
        if w == 'split':
            S, obv = split_needs(S, obv, split_names)
        elif w == 'recent':
            S, obv = with_recency(S, obv)
        elif w in ('presence', 'presence+strength'):
            S, obv = presence_grading(S, obv, strength=w == 'presence+strength')
        else:
            raise ValueError(f'unknown variant {w!r} (one of {VARIANTS})')
    return S, obv


# ---------------------------------------------------------------------------
# folds: grouped by launch
# ---------------------------------------------------------------------------
KW = ('key', 'wheel')


def launch_folds(S, k=10, seed=1):
    """Grouped K-fold by launch: the fold of every launch (S['launches'] order), so no launch is
    in two folds. Deterministic for a seed: the launches are shuffled by the seed, ordered by
    key/wheel picks, then picks, then decisions (descending; a stable sort, so ties keep the
    shuffled order), and each goes to the fold with the fewest key/wheel picks, then picks,
    then decisions, then the lowest index. k is capped at the launch count (k = launches is
    leave-one-launch-out); every fold gets at least one launch."""
    L = len(S['launches'])
    k = max(1, min(int(k), L))
    kw = np.bincount(S['LA'][np.isin(S['OUT'], KW)], minlength=L)
    pk = np.bincount(S['LA'][S['Y'] >= 0], minlength=L)
    nd = np.bincount(S['LA'], minlength=L)
    perm = np.random.default_rng(seed).permutation(L)
    order = sorted(perm.tolist(), key=lambda l: (-kw[l], -pk[l], -nd[l]))
    fold = np.full(L, -1, dtype=np.int32)
    load = [(0, 0, 0)] * k
    for l in order:
        f = min(range(k), key=lambda f: (load[f], f))
        fold[l] = f
        load[f] = (load[f][0] + int(kw[l]), load[f][1] + int(pk[l]), load[f][2] + int(nd[l]))
    return fold


# ---------------------------------------------------------------------------
# A*: the old engine's ranking replayed from the logged utilities
# ---------------------------------------------------------------------------
def astar_scores(S):
    """(rank score, ln util): the logged utility (a row without one ranks last) and its log,
    the score the slot code ranks by since R7. A row without a utility reads the lowest
    ln util on its page (else in its choice set, else 0) -- not ln 1e-6, which pulled the
    fitted page temperature from 0.71 to 0.20 (22 of 1422 key/wheel picks, verifier of
    #218) and left A*'s page NLL near uniform."""
    U = S['UTIL']
    miss = np.isnan(U)
    L = np.log(np.maximum(np.where(miss, 1.0, U), 1e-6))
    valid = ~miss & S['MASK']
    page = valid & (S['INHA'] == 0)
    lo_page = np.where(page, L, np.inf).min(axis=1, keepdims=True)
    lo_all = np.where(valid, L, np.inf).min(axis=1, keepdims=True)
    lo = np.where(np.isfinite(lo_page), lo_page, np.where(np.isfinite(lo_all), lo_all, 0.0))
    return np.nan_to_num(U, nan=-1.0), np.where(miss, lo, L)


def page_temperature(S, z, idx, iters=50):
    """beta maximising the within-page log-likelihood of softmax(beta * z) over the key/wheel
    picks among idx whose chosen row is on the page (concave in beta; Newton). Floored at
    1e-3, so the ranking within the page stays z's."""
    rows = np.array([d for d in idx if S['OUT'][d] in KW and S['Y'][d] >= 0 and S['INHA'][d, S['Y'][d]] == 0], dtype=int)
    if not len(rows):
        return 1.0
    on = S['MASK'][rows] & (S['INHA'][rows] == 0)
    Z = np.where(on, z[rows], 0.0)
    zy = z[rows, S['Y'][rows]]
    beta = 1.0
    for _ in range(iters):
        a = np.where(on, beta * Z, -np.inf)
        a -= a.max(axis=1, keepdims=True)
        p = np.exp(a); p /= p.sum(axis=1, keepdims=True)
        ez = (p * Z).sum(axis=1)
        g = (zy - ez).sum()
        h = -(p * (Z - ez[:, None]) ** 2).sum()
        if h > -1e-12:
            break
        step = g / h
        beta = max(1e-3, beta - step)
        if abs(step) < 1e-8:
            break
    return float(beta)


def astar_results(S, fold_of_dec):
    """The A* ranking's per-decision metrics on S (metrics_from_scores): hit@k by the logged
    utility; within the page by softmax(beta * ln util) with beta fitted on the other folds'
    key/wheel pages (page_temperature), so its page NLL is a fair yardstick and not the raw
    utilities read as logits. Returns (results, {fold: beta})."""
    rank, z = astar_scores(S)
    sp = np.zeros_like(z)
    betas = {}
    for f in np.unique(fold_of_dec):
        te = fold_of_dec == f
        b = page_temperature(S, z, np.where(~te)[0] if (~te).any() else np.where(te)[0])
        sp[te] = b * z[te]
        betas[int(f)] = b
    return metrics_from_scores(S, sp, rank, None), betas


# ---------------------------------------------------------------------------
# the yardstick: model against A* on the same choice sets
# ---------------------------------------------------------------------------
YARDSTICK = {
    'key/wheel hit@8': lambda r: r.get('full_hit8') if r['out'] in KW else None,
    'menu hit@8': lambda r: r.get('full_hit8') if r['out'] == 'menu' else None,
    'all picks hit@8': lambda r: r.get('full_hit8') if r['out'] != 'nothing' else None,
    'page hit@1': lambda r: r.get('page_hit1') if r['out'] in KW else None,
    'page NLL': lambda r: -r['page_logp'] if r['out'] in KW and 'page_logp' in r else None,
}


def boot_mean(by_launch, nboot=2000, seed=2):
    """(mean, 2.5%, 97.5%, n) of the values in {launch: [values]}, launches resampled with
    replacement (each draw: the mean over every value of the drawn launches); None without
    values."""
    ls = [l for l in sorted(by_launch) if by_launch[l]]
    if not ls:
        return None
    sums = np.array([sum(by_launch[l]) for l in ls], dtype=float)
    cnts = np.array([len(by_launch[l]) for l in ls], dtype=float)
    draw = np.random.default_rng(seed).integers(0, len(ls), size=(nboot, len(ls)))
    means = sums[draw].sum(axis=1) / cnts[draw].sum(axis=1)
    return float(sums.sum() / cnts.sum()), float(np.percentile(means, 2.5)), float(np.percentile(means, 97.5)), int(cnts.sum())


def yardstick(res, base, metrics=YARDSTICK, nboot=2000, seed=2, keep=None):
    """{metric: (model mean, base mean, model - base, CI low, CI high, n)} over the decisions
    where both have the metric (res and base aligned by decision; keep: an optional bool mask
    over decisions). The CI is a bootstrap over launches of the paired difference."""
    out = {}
    for name, fn in metrics.items():
        a_sum = b_sum = 0.0
        d_by = defaultdict(list)
        for i, (a, b) in enumerate(zip(res, base)):
            if a is None or b is None or (keep is not None and not keep[i]):
                continue
            va, vb = fn(a), fn(b)
            if va is None or vb is None:
                continue
            a_sum += float(va); b_sum += float(vb)
            d_by[a['launch']].append(float(va) - float(vb))
        d = boot_mean(d_by, nboot, seed)
        if d is not None:
            out[name] = (a_sum / d[3], b_sum / d[3], d[0], d[1], d[2], d[3])
    return out


def yardstick_lines(name, ys, base='A*'):
    """The yardstick as text lines."""
    lines = [f'{name}  vs  {base}',
             f'    {"metric":16} {"model":>7} {base:>7} {"diff":>8}  {"95% CI":>19}  {"n":>5}']
    for m, (ma, mb, d, lo, hi, n) in ys.items():
        lines.append(f'    {m:16} {ma:7.4f} {mb:7.4f} {d:+8.4f}  [{lo:+.4f}, {hi:+.4f}]  {n:5}')
    return lines


def level(res, metric, nboot=2000, seed=2, keep=None):
    """boot_mean of one YARDSTICK metric of one result list on its own (not paired): the
    model's level with its launch-bootstrap interval. None without values."""
    fn = YARDSTICK[metric]
    by = defaultdict(list)
    for i, r in enumerate(res):
        if r is None or (keep is not None and not keep[i]):
            continue
        v = fn(r)
        if v is not None:
            by[r['launch']].append(float(v))
    return boot_mean(by, nboot, seed)


# ---------------------------------------------------------------------------
# R6's offline gate (roadmap R6, the user's criterion of 2026-10-28)
# ---------------------------------------------------------------------------
# On the 95% launch-bootstrap interval of model - A*: menu picks caught (hit@8) ABOVE A*
# (the interval's lower end > 0), and all picks pooled (key + wheel + menu) NOT WORSE
# (its upper end >= 0). Key hit@8 alone is reported, not gated.
GATE = (('menu hit@8', 'lower', '>', 0.0), ('all picks hit@8', 'upper', '>=', 0.0))


def gate(ys):
    """R6's gate on a yardstick (GATE): (verdict, lines). verdict is False when any condition
    fails, else None when a gated metric is missing, else True; lines say why, one per condition."""
    failed = missing = False
    lines = []
    for metric, end, op, bound in GATE:
        if metric not in ys:
            lines.append(f'{metric}: missing, no verdict')
            missing = True
            continue
        _, _, d, lo, hi, n = ys[metric]
        x = lo if end == 'lower' else hi
        ok = x > bound if op == '>' else x >= bound
        lines.append(f'{metric}: diff {d:+.4f}, {end} end {x:+.4f} {op} {bound:g}: {"ok" if ok else "FAILS"}')
        failed |= not ok
    return (False if failed else None if missing else True), lines


def verdict_word(v):
    return {True: 'PASS', False: 'FAIL', None: 'no verdict'}[v]


# ---------------------------------------------------------------------------
# held-out schedules: grouped K-fold by launch, and train-on-past (online)
# ---------------------------------------------------------------------------
def fit_eval(Str, Ste, obv, model_kw, nitems, p0=None, maxiter=300):
    """Fit model_kw on Str (from p0, else the model's init) and evaluate on Ste: (results, p)."""
    M = Model(Str, obvious=obv, nitems=nitems, **model_kw)
    Fm, _ = row_features(Str, M.feat_spec)
    p, _, _ = lbfgs(lambda q: M.loss(Str, q, Fm)[:2], M.init() if p0 is None else p0, maxiter=maxiter)
    M2 = Model(Ste, obvious=obv, nitems=nitems, **model_kw)
    return evaluate(M2, Ste, p), p


def split_spec(spec):
    """A cv.py spec dict -> (Model keyword arguments, variants, latest caps?)."""
    spec = dict(spec)
    latest = spec.pop('_latest', False)
    which = spec.pop('_with', [])
    return spec, which, latest


def cv_results(S, obv, spec, FOLD, split_names=None, maxiter=300):
    """Grouped K-fold held-out results of one spec (cv.py): one entry per decision of S; a fold
    with no picks held out is not fitted and its decisions carry stubs (no metrics)."""
    model_kw, which, _ = split_spec(spec)
    res = [None] * len(S['Y'])
    for f in np.unique(FOLD):
        te = np.where(FOLD == f)[0]
        tr = np.where(FOLD != f)[0]
        if not (S['Y'][te] >= 0).any() or not len(tr):
            continue
        Str, ov = variant(subset(S, tr), obv, which, split_names)
        Ste, _ = variant(subset(S, te), obv, which, split_names)
        r, _ = fit_eval(Str, Ste, ov, model_kw, len(S['items']), maxiter=maxiter)
        for i, x in zip(te, r):
            res[i] = x
    return [r if r is not None else {'launch': S['LA'][i], 'out': S['OUT'][i], 'logp': None} for i, r in enumerate(res)]


def online_chunks(S, every=5, min_train=150):
    """The train-on-past schedule (the honest yardstick): yields (train, test) index arrays.

    Decisions are taken in log order -- build() sorts them by (launch, seq), and launch ids
    (UTC 'YYYYMMDD-HHMMSS') sort chronologically, so the order is the order the game wrote
    them, i.e. what was known when. A launch is evaluated only when the earlier launches hold at
    least min_train picks (and at least one); within it the decisions are cut into consecutive chunks of `every`
    picks (a chunk ends at its every-th pick; the last may be shorter), each tested by a fit on
    every earlier launch plus the decisions of this launch before the chunk. Decisions of a
    launch below min_train are never tested but enter the later training sets."""
    LA = np.asarray(S['LA'])
    if len(LA) and (np.diff(LA) < 0).any():
        raise ValueError('online_chunks: decisions are not in launch order (build() sorts them)')
    pick = np.asarray(S['Y']) >= 0
    every = max(1, int(every))
    for L in range(len(S['launches'])):
        idxL = np.where(LA == L)[0]
        if not len(idxL):
            continue
        past = np.where(LA < L)[0]
        if pick[past].sum() < max(1, min_train):        # never a fit on no picks
            continue
        start = 0
        while start < len(idxL):
            end, n = start, 0
            while end < len(idxL) and n < every:
                n += int(pick[idxL[end]])
                end += 1
            yield np.concatenate([past, idxL[:start]]), idxL[start:end]
            start = end


def online_results(S, obv, spec, every=5, min_train=150, split_names=None, maxiter=300, progress=None):
    """The online replay of one spec over online_chunks: one refit per chunk, warm-started from
    the previous chunk's fit (itself fitted on strictly earlier decisions). Returns (results,
    fits): results aligned with S, None for a decision never tested."""
    model_kw, which, _ = split_spec(spec)
    res = [None] * len(S['Y'])
    p, fits = None, 0
    for tr, te in online_chunks(S, every, min_train):
        Str, ov = variant(subset(S, tr), obv, which, split_names)
        Ste, _ = variant(subset(S, te), obv, which, split_names)
        r, p = fit_eval(Str, Ste, ov, model_kw, len(S['items']), p0=p, maxiter=maxiter)
        for i, x in zip(te, r):
            res[i] = x
        fits += 1
        if progress:
            progress(fits, int(S['LA'][te[0]]))
    return res, fits


def astar_online(S, every=5, min_train=150):
    """A* on the online schedule: the same ranking (hit@k does not depend on the schedule), its
    page temperature fitted on each chunk's training decisions only. None where not tested."""
    rank, z = astar_scores(S)
    sp = np.zeros_like(z)
    tested = np.zeros(len(S['Y']), dtype=bool)
    for tr, te in online_chunks(S, every, min_train):
        sp[te] = page_temperature(S, z, tr) * z[te]
        tested[te] = True
    res = metrics_from_scores(S, sp, rank, None)
    return [r if tested[i] else None for i, r in enumerate(res)]


# ---------------------------------------------------------------------------
# row features for baselines
# ---------------------------------------------------------------------------
def row_features(S, spec):
    """spec: list of names among 'astar', 'ctx', 'cheap', 'pos'."""
    feats = []
    names = []
    U = S['UTIL']; V = S['V2']
    if 'astar' in spec:
        miss = np.isnan(U)
        feats += [np.where(miss, 0.0, np.log(np.maximum(np.nan_to_num(U, nan=1.0), 1e-6))), miss.astype(float)]
        names += ['ln util', 'util missing']
    if 'ctx' in spec:
        miss = np.isnan(V[..., 0])
        for i, n in enumerate(['ctx', 'corr', 'potion', 'fav']):
            feats.append(np.where(miss, 0.0, np.log(np.maximum(np.nan_to_num(V[..., i], nan=1.0), 1e-6))))
            names.append('ln ' + n)
        feats.append(miss.astype(float)); names.append('ctx missing')
    if 'cheap' in spec:
        miss = np.isnan(V[..., 0])
        ctx = np.nan_to_num(V[..., 0], nan=0.0)
        inv = {v: k for k, v in S['classes'].items()}
        for c in range(len(S['classes'])):
            feats.append(np.where(S['CLS'] == c, ctx, 0.0)); names.append('ctx x ' + inv[c])
        feats.append(miss.astype(float)); names.append('ctx missing')
    if 'pos' in spec:
        for s in range(8):
            feats.append((S['SLOT'] == s).astype(float)); names.append(f'slot {s}')
    if not feats:
        return None, []
    return np.stack(feats, axis=-1), names


# ---------------------------------------------------------------------------
# model
# ---------------------------------------------------------------------------
class Model:
    def __init__(self, S, theta=True, item=True, kappa=True, outside=True, feat_spec=(), T0=1.0,
                 sig_obv=1.0, sig_dense=0.3, sig_b=0.5, sig_u=1.0, sig_w=10.0, bias_need=False, obvious=None,
                 nitems=None, theta_fixed=None):
        """theta_fixed: an optional bool mask (needs x columns, without the bias need) of theta
        weights held at their prior centre theta0: their gradient is zeroed, so a fit started from
        init() (or from a p that holds them at theta0) never moves them (curve.py)."""
        self.theta, self.item, self.kappa, self.outside = theta, item, kappa, outside
        self.feat_spec = list(feat_spec)
        self.K = S['K'] + (1 if bias_need else 0); self.J = S['J']
        self.bias_need = bias_need
        self.nitems = nitems if nitems is not None else len(S['items'])
        self.T0, self.sig_obv, self.sig_dense, self.sig_b, self.sig_u, self.sig_w = T0, sig_obv, sig_dense, sig_b, sig_u, sig_w
        self.theta0 = np.zeros((self.K, self.J)); self.sig = np.full((self.K, self.J), sig_dense)
        for (k, j), sgn in (obvious or {}).items():
            self.theta0[k, j] = sgn * T0; self.sig[k, j] = sig_obv
        self.fixed = None
        if theta_fixed is not None:
            self.fixed = np.zeros((self.K, self.J), dtype=bool)
            self.fixed[:theta_fixed.shape[0], :theta_fixed.shape[1]] = theta_fixed
        F_, self.fnames = row_features(S, self.feat_spec)
        self.nf = len(self.fnames)
        # layout
        self.sizes = [('theta', self.K * self.J if theta else 0), ('b', self.nitems if item else 0),
                      ('kappa', 1 if kappa else 0), ('uout', self.K + 1 if outside else 0), ('w', self.nf)]
        self.off = {}
        o = 0
        for n, s in self.sizes:
            self.off[n] = (o, o + s); o += s
        self.P = o

    def init(self):
        p = np.zeros(self.P)
        if self.theta:
            a, b = self.off['theta']; p[a:b] = self.theta0.ravel()
        if self.kappa:
            p[self.off['kappa'][0]] = 3.0
        if self.outside:
            a, b = self.off['uout']; p[b - 1] = 0.0
        return p

    def unpack(self, p):
        g = lambda n: p[self.off[n][0]:self.off[n][1]]
        return {'theta': g('theta').reshape(self.K, self.J) if self.theta else None, 'b': g('b') if self.item else None,
                'kappa': g('kappa')[0] if self.kappa else 0.0, 'uout': g('uout') if self.outside else None, 'w': g('w')}

    def Nmat(self, S):
        return np.hstack([S['N'], np.ones((len(S['N']), 1))]) if self.bias_need else S['N']

    def scores(self, S, p, Fm=None):
        """Row item scores (no kappa), the logits with kappa, and the outside logit."""
        q = self.unpack(p)
        Dn, R = S['CAPID'].shape
        s = np.zeros((Dn, R))
        N = self.Nmat(S)
        if self.theta:
            NT = N @ q['theta']                                # D x J
            Wcap = NT @ S['CapMat'].T                          # D x ncap+1
            s += np.take_along_axis(Wcap, S['CAPID'], axis=1)
            s += np.einsum('drx,dx->dr', S['X'], NT[:, S['xcol']])
        if self.item:
            s += q['b'][np.minimum(S['ITEM'], self.nitems - 1)] * (S['ITEM'] < self.nitems)
        if self.nf:
            s += Fm @ q['w']
        logit = s - q['kappa'] * S['INHA']
        logit = np.where(S['MASK'], logit, -np.inf)
        if self.outside:
            u0 = N @ q['uout'][:-1] + q['uout'][-1]
        else:
            u0 = np.full(Dn, -np.inf)
        return s, logit, u0

    def loss(self, S, p, Fm=None, prior=True):
        q = self.unpack(p)
        s, logit, u0 = self.scores(S, p, Fm)
        Dn, R = logit.shape
        full = np.concatenate([logit, u0[:, None]], axis=1)
        m = full.max(axis=1, keepdims=True)
        e = np.exp(full - m)
        Z = e.sum(axis=1, keepdims=True)
        P = e / Z
        y = np.where(S['Y'] >= 0, S['Y'], R)
        logp = (full[np.arange(Dn), y] - m[:, 0] - np.log(Z[:, 0]))
        f = -logp.sum()
        G = P.copy(); G[np.arange(Dn), y] -= 1.0            # dF/dlogit
        Gr = G[:, :R]; Go = G[:, R]
        grad = np.zeros(self.P)
        N = self.Nmat(S)
        if self.theta:
            ncap = S['CapMat'].shape[0]
            flat = (np.arange(Dn)[:, None] * ncap + S['CAPID']).ravel()
            Gcap = np.bincount(flat, weights=Gr.ravel(), minlength=Dn * ncap).reshape(Dn, ncap)
            E = Gcap @ S['CapMat']                              # D x J
            E[:, S['xcol']] += np.einsum('drx,dr->dx', S['X'], Gr)
            gT = N.T @ E
            if self.fixed is not None:
                gT[self.fixed] = 0.0
            a, b = self.off['theta']; grad[a:b] = gT.ravel()
        if self.item:
            a, b = self.off['b']
            grad[a:b] = np.bincount(S['ITEM'].ravel(), weights=Gr.ravel(), minlength=self.nitems)[:self.nitems]
        if self.kappa:
            grad[self.off['kappa'][0]] = -(Gr * S['INHA']).sum()
        if self.outside:
            a, b = self.off['uout']
            grad[a:b - 1] = N.T @ Go; grad[b - 1] = Go.sum()
        if self.nf:
            a, b = self.off['w']; grad[a:b] = np.einsum('drf,dr->f', Fm, Gr)
        if prior:
            if self.theta:
                a, b = self.off['theta']
                dlt = (p[a:b] - self.theta0.ravel()); s2 = self.sig.ravel() ** 2
                if self.fixed is not None:
                    dlt = np.where(self.fixed.ravel(), 0.0, dlt)
                f += 0.5 * (dlt ** 2 / s2).sum(); grad[a:b] += dlt / s2
            if self.item:
                a, b = self.off['b']
                f += 0.5 * (p[a:b] ** 2).sum() / self.sig_b ** 2; grad[a:b] += p[a:b] / self.sig_b ** 2
            if self.outside:
                a, b = self.off['uout']
                f += 0.5 * (p[a:b - 1] ** 2).sum() / self.sig_u ** 2; grad[a:b - 1] += p[a:b - 1] / self.sig_u ** 2
                f += 0.5 * p[b - 1] ** 2 / 100.0; grad[b - 1] += p[b - 1] / 100.0
            if self.kappa:
                i = self.off['kappa'][0]; f += 0.5 * p[i] ** 2 / 100.0; grad[i] += p[i] / 100.0
            if self.nf:
                a, b = self.off['w']; f += 0.5 * (p[a:b] ** 2).sum() / self.sig_w ** 2; grad[a:b] += p[a:b] / self.sig_w ** 2
        return f, grad, logp, P

    def info(self, S, p, Fm=None):
        """P4: per-weight information sum_d Var_p(x_kj) over the choice set and the outside option."""
        _, _, _, P = self.loss(S, p, Fm, prior=False)
        Dn, R1 = P.shape; R = R1 - 1
        ncap = S['CapMat'].shape[0]
        flat = (np.arange(Dn)[:, None] * ncap + S['CAPID']).ravel()
        Pc = np.bincount(flat, weights=P[:, :R].ravel(), minlength=Dn * ncap).reshape(Dn, ncap)
        Ec = Pc @ S['CapMat']; Ec2 = Pc @ (S['CapMat'] ** 2)
        Ex = np.einsum('drx,dr->dx', S['X'], P[:, :R]); Ex2 = np.einsum('drx,dr->dx', S['X'] ** 2, P[:, :R])
        Ec[:, S['xcol']] += Ex; Ec2[:, S['xcol']] += Ex2
        V = np.maximum(Ec2 - Ec ** 2, 0)
        N = self.Nmat(S)
        return (N ** 2).T @ V


def lbfgs(fun, x0, maxiter=300, m=12, tol=1e-6, verbose=False):
    x = x0.copy(); f, g = fun(x)
    S_, Y_ = [], []
    for it in range(maxiter):
        q = g.copy(); al = []
        for s, y in reversed(list(zip(S_, Y_))):
            a = s @ q / (y @ s); al.append(a); q -= a * y
        if S_:
            q *= (S_[-1] @ Y_[-1]) / (Y_[-1] @ Y_[-1])
        else:
            q *= 1.0 / max(1.0, np.linalg.norm(g))
        for (s, y), a in zip(zip(S_, Y_), reversed(al)):
            bb = y @ q / (y @ s); q += s * (a - bb)
        d = -q
        gd = g @ d
        if gd >= 0:
            d = -g; gd = -(g @ g); S_, Y_ = [], []
        t = 1.0
        while True:
            xn = x + t * d; fn, gn = fun(xn)
            if fn <= f + 1e-4 * t * gd or t < 1e-10:
                break
            t *= 0.5
        s = xn - x; y = gn - g
        if s @ y > 1e-10:
            S_.append(s); Y_.append(y)
            if len(S_) > m:
                S_.pop(0); Y_.pop(0)
        rel = abs(f - fn) / max(1.0, abs(f))
        x, f, g = xn, fn, gn
        if verbose and it % 20 == 0:
            print(it, f, np.linalg.norm(g))
        if rel < 1e-9 or np.linalg.norm(g, np.inf) < tol:
            break
    return x, f, it


def fit(model, S, maxiter=400, verbose=False):
    Fm, _ = row_features(S, model.feat_spec)
    fun = lambda p: model.loss(S, p, Fm)[:2]
    p, f, it = lbfgs(fun, model.init(), maxiter=maxiter, verbose=verbose)
    return p


# ---------------------------------------------------------------------------
# metrics on a (held-out) set
# ---------------------------------------------------------------------------
def evaluate(model, S, p, item_rank_without_pos=True):
    """Per decision: logp (the model's own likelihood), and for picks: page hit@1 / page logp
    (within the shown page), full-set hit@1/hit@8 (rank of the chosen item among every
    candidate by item score: no kappa, no slot term), menu-set hit@8 / logp (within H\\A)."""
    Fm, _ = row_features(S, model.feat_spec)
    f, g, logp, P = model.loss(S, p, Fm, prior=False)
    s, logit, u0 = model.scores(S, p, Fm)
    if item_rank_without_pos and 'pos' in model.feat_spec:
        q = model.unpack(p)
        pos_idx = [i for i, n in enumerate(model.fnames) if n.startswith('slot')]
        s_rank = s - Fm[..., pos_idx] @ q['w'][pos_idx]
    else:
        s_rank = s
    return metrics_from_scores(S, s, s_rank, logp)


def metrics_from_scores(S, s_page, s_rank, logp=None, tiebreak=None):
    """s_page: score used within the page; s_rank: score used to build a new page."""
    res = []
    Dn, R = S['CAPID'].shape
    rng = np.random.default_rng(0)
    for d in range(Dn):
        y = S['Y'][d]
        rec = {'launch': S['LA'][d], 'out': S['OUT'][d], 'logp': None if logp is None else float(logp[d])}
        if y >= 0:
            mask = S['MASK'][d]
            onA = mask & (S['INHA'][d] == 0)
            onH = mask & (S['INHA'][d] == 1)
            tb = tiebreak[d] if tiebreak is not None else rng.random(R) * 1e-9
            sr = np.where(mask, s_rank[d] + tb * 1e-6, -np.inf)
            rank = int((sr > sr[y]).sum())
            rec['full_rank'] = rank
            rec['full_hit1'] = rank == 0; rec['full_hit8'] = rank < 8
            if onA[y]:
                sp = np.where(onA, s_page[d] + tb * 1e-6, -np.inf)
                rec['page_hit1'] = bool((sp > sp[y]).sum() == 0)
                z = sp[onA]; z = z - z.max()
                rec['page_logp'] = float(sp[y] - sp[onA].max() - np.log(np.exp(z).sum()))
                rec['page_n'] = int(onA.sum())
            if onH[y]:
                sh = np.where(onH, s_rank[d] + tb * 1e-6, -np.inf)
                rh = int((sh > sh[y]).sum())
                rec['menu_hit8'] = rh < 8; rec['menu_hit1'] = rh == 0
                z = sh[onH]; m = z.max()
                rec['menu_logp'] = float(sh[y] - m - np.log(np.exp(z - m).sum()))
                rec['menu_n'] = int(onH.sum())
        res.append(rec)
    return res


def summarize(res, nboot=2000, seed=1):
    """Means with launch-bootstrap 95% CIs (boot_mean: launches without a value are not drawn)."""
    out = {}
    launches = sorted({r['launch'] for r in res})
    by = defaultdict(list)
    for r in res:
        by[r['launch']].append(r)
    keys = {
        'key_page_hit1': lambda r: r.get('page_hit1') if r['out'] in ('key', 'wheel') else None,
        'key_page_nll': lambda r: -r['page_logp'] if r['out'] in ('key', 'wheel') and 'page_logp' in r else None,
        'key_full_hit8': lambda r: r.get('full_hit8') if r['out'] in ('key', 'wheel') else None,
        'key_full_hit1': lambda r: r.get('full_hit1') if r['out'] in ('key', 'wheel') else None,
        'menu_full_hit8': lambda r: r.get('full_hit8') if r['out'] == 'menu' else None,
        'menu_set_hit8': lambda r: r.get('menu_hit8') if r['out'] == 'menu' else None,
        'menu_set_nll': lambda r: -r['menu_logp'] if r['out'] == 'menu' and 'menu_logp' in r else None,
        'pick_nll': lambda r: -r['logp'] if r['out'] != 'nothing' and r['logp'] is not None else None,
        'all_nll': lambda r: -r['logp'] if r['logp'] is not None else None,
    }
    for k, fn in keys.items():
        per = {l: [float(v) for v in (fn(r) for r in by[l]) if v is not None] for l in launches}
        b = boot_mean(per, nboot, seed)
        if b is not None:
            out[k] = b
    return out


def paired(resA, resB, key_fn, nboot=2000, seed=2):
    """Mean difference A - B per decision, launch bootstrap CI."""
    rng = np.random.default_rng(seed)
    by = defaultdict(list)
    for a, b in zip(resA, resB):
        va, vb = key_fn(a), key_fn(b)
        if va is None or vb is None:
            continue
        by[a['launch']].append(float(va) - float(vb))
    launches = sorted(by)
    tot = [v for l in launches for v in by[l]]
    bs = []
    for _ in range(nboot):
        pick = rng.choice(len(launches), len(launches))
        vals = [v for i in pick for v in by[launches[i]]]
        bs.append(np.mean(vals))
    return float(np.mean(tot)), float(np.percentile(bs, 2.5)), float(np.percentile(bs, 97.5)), len(tot)

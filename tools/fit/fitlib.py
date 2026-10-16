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
import csv
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
def build(D, include_nothing=True, latest_caps=False, v2=None):
    needs, cols, caps, cross = D['needs'], D['cols'], D['caps'], D['cross']
    K, J = len(needs), len(cols)
    CI = {c: j for j, c in enumerate(cols)}
    xcol = np.array([CI[c] for c in cross])            # cross-features are effect columns by name
    ncap = len(caps)
    CapMat = np.zeros((ncap + 1, J))                    # last row = no cap (not in catalog)
    for g, c in enumerate(caps):
        for j, v in c:
            CapMat[g, j] = v
    # latest cap per form (0.23.34 changed the tables): optional remap
    latest = {}
    if latest_caps:
        for key, c in sorted(D['ctxs'].items(), key=lambda kv: kv[1]['utc']):
            for r in c['rows']:
                if r[CAP] >= 0 and r[UID] == 0:
                    latest[r[FORM]] = r[CAP]
    decs = [d for d in D['decs'] if d['list'] != SIMONRIM and d['C'] is not None]
    decs = [d for d in decs if d['name'] not in TRANSIENT]
    if not include_nothing:
        decs = [d for d in decs if d['out'] != 'nothing']
    decs.sort(key=lambda d: (d['launch'], d['seq']))
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
    XA = np.zeros((Dn, R, len(cross)), dtype=np.float64)
    MASK = np.zeros((Dn, R), dtype=bool)
    INHA = np.zeros((Dn, R))
    ITEM = np.zeros((Dn, R), dtype=np.int32)
    SLOTA = np.full((Dn, R), -1, dtype=np.int32)
    UTILA = np.full((Dn, R), np.nan)
    V2 = np.full((Dn, R, 4), np.nan)          # ctx corr potion fav
    CLS = np.full((Dn, R), -1, dtype=np.int32)
    Y = np.full(Dn, -1, dtype=np.int32)
    N = np.zeros((Dn, K))
    OUT = np.empty(Dn, dtype=object)
    LA = np.zeros(Dn, dtype=np.int32)
    v2ok = np.zeros(Dn, dtype=bool)
    classes = {}
    meta = []
    for di, (d, rows) in enumerate(recs):
        for k, v in d['C']['need'].items():
            N[di, k] = v
        OUT[di] = d['out']; LA[di] = LI[d['launch']]
        cands = join_v2(v2, d) if (v2 is not None and d['out'] != 'nothing') else None
        v2ok[di] = cands is not None
        for ri, (r, is_ch) in enumerate(rows):
            cid = r[CAP]
            if latest_caps and r[UID] == 0 and r[FORM] in latest:
                cid = latest[r[FORM]]
            CAPID[di, ri] = cid if cid >= 0 else ncap
            for j, v in r[X]:
                XA[di, ri, j] = v
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
                     'ctxkey': d['ctxkey'], 'build': d['build'], 'case': d['case'], 'need_ep': d['ep']['gi'] if d['ep'] else None})
    return dict(CapMat=CapMat, xcol=xcol, CAPID=CAPID, X=XA, MASK=MASK, INHA=INHA, ITEM=ITEM, SLOT=SLOTA, UTIL=UTILA,
                V2=V2, CLS=CLS, Y=Y, N=N, OUT=OUT, LA=LA, v2ok=v2ok, launches=launches, items=items,
                classes=classes, meta=meta, K=K, J=J, needs=needs, cols=cols, R=R)


def subset(S, idx):
    out = dict(S)
    for k in ('CAPID', 'X', 'MASK', 'INHA', 'ITEM', 'SLOT', 'UTIL', 'V2', 'CLS', 'Y', 'N', 'OUT', 'LA', 'v2ok'):
        out[k] = S[k][idx]
    out['meta'] = [S['meta'][i] for i in idx]
    return out


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
                 nitems=None):
        self.theta, self.item, self.kappa, self.outside = theta, item, kappa, outside
        self.feat_spec = list(feat_spec)
        self.K = S['K'] + (1 if bias_need else 0); self.J = S['J']
        self.bias_need = bias_need
        self.nitems = nitems if nitems is not None else len(S['items'])
        self.T0, self.sig_obv, self.sig_dense, self.sig_b, self.sig_u, self.sig_w = T0, sig_obv, sig_dense, sig_b, sig_u, sig_w
        self.theta0 = np.zeros((self.K, self.J)); self.sig = np.full((self.K, self.J), sig_dense)
        for (k, j), sgn in (obvious or {}).items():
            self.theta0[k, j] = sgn * T0; self.sig[k, j] = sig_obv
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
    """Means with launch-bootstrap 95% CIs."""
    rng = np.random.default_rng(seed)
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
        tot = [v for l in launches for v in per[l]]
        if not tot:
            continue
        mean = float(np.mean(tot))
        bs = []
        for _ in range(nboot):
            pick = rng.choice(len(launches), len(launches))
            vals = [v for i in pick for v in per[launches[i]]]
            if vals:
                bs.append(np.mean(vals))
        out[k] = (mean, float(np.percentile(bs, 2.5)), float(np.percentile(bs, 97.5)), len(tot))
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

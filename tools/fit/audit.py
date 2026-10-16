"""Data audit of the v3 log: counts per launch and build, the context joins, duplicates,
every need column's range and firing rate, the curve inputs (present, sentinel), and the
effect columns ever used. Writes audit.txt."""
import statistics as st
from collections import Counter, defaultdict
from common import *

D = load()
decs = D['decs']; needs = D['needs']; cols = D['cols']; caps = D['caps']
out = out_file('audit.txt')


def p(*a):
    print(*a, file=out)


P = picks(D)
p('== counts ==')
p('decisions', len(decs), Counter(d['out'] for d in decs))
p('picks', len(P), 'kind', Counter(d['kind'] for d in P), 'src', Counter(d['src'] for d in P))
p('chars', Counter(d['char'] for d in decs))
p('\n-- per launch (build, list): key wheel menu nothing | equip consume | hours span')
bl = defaultdict(Counter)
span = {}
for d in decs:
    k = (d['launch'], d['build'], d['list'])
    bl[k][d['out']] += 1
    bl[k][d['kind'] or '-'] += 1
    span.setdefault(k, [d['utc'], d['utc']]); span[k][1] = d['utc']
tot_h = 0
for k in sorted(bl):
    c = bl[k]; h = (T(span[k][1]) - T(span[k][0])).total_seconds() / 3600; tot_h += h
    p(f"{k[0]} {k[1]:22} {k[2]:20} key {c['key']:4} wheel {c['wheel']:4} menu {c['menu']:4} nothing {c['nothing']:5} | eq {c['equip']:4} cons {c['consume']:4} | {h:4.1f}h")
p('total wall hours (first to last decision per launch)', round(tot_h, 1))
p('\n-- per build: picks by src')
bb = defaultdict(Counter)
for d in P:
    bb[d['build']][d['src']] += 1
for b in sorted(bb):
    p(b, dict(bb[b]))
p('\nvia', Counter('key' if d['via'].startswith('key') else d['via'] for d in P))
p('case (menu)', Counter(d['case'] for d in P if d['out'] == 'menu'))
p('learned/skip', Counter((d['out'], d['learned'], d['skip']) for d in P))
p('repeat', Counter((d['out'], d['repeat']) for d in P))
p('chosen kind', Counter((d['out'], d['chosen'][KIND] if d['chosen'] else None) for d in P))

p('\n== context join quality ==')
p('dec with no ctx', sum(1 for d in decs if d['C'] is None), 'ctx id missing in file', sum(1 for d in decs if d['ctxkey'] and d['C'] is None))
p('picks: chosen row None', sum(1 for d in P if d['chosen'] is None))
p('picks: chosen addedAtPick (missing from ctx)', Counter(d['out'] for d in P if d['chosen'] and d['chosen'][FLAGS] & F['addedAtPick']))
for o in ('key', 'wheel', 'menu', 'nothing'):
    ages = [d['ctxAgeMs'] for d in decs if d['out'] == o]
    if ages:
        q = st.quantiles(ages, n=20)
        p(f'ctxAgeMs {o}: n {len(ages)} median {st.median(ages):.0f} p90 {q[17]:.0f} p95 {q[18]:.0f} max {max(ages)}  >2s {sum(a > 2000 for a in ages)}  >10s {sum(a > 10000 for a in ages)}  >60s {sum(a > 60000 for a in ages)}')
p('ctx why by out', Counter((d['out'], d['C']['why'], d['C'].get('menu')) for d in decs if d['C']))
p('menu picks: via x ctx.why', Counter((d['via'], d['C']['why']) for d in P if d['out'] == 'menu' and d['C']))
p('pipe.ok=0 by out', Counter(d['out'] for d in decs if d['C'] and not d['C']['pipe']['ok']))
p('heldFull=0 by out', Counter(d['out'] for d in decs if d['C'] and not d['C']['heldFull']))
pa = [d['C']['pipe']['ageMs'] for d in P if d['C'] and d['C']['pipe']['ok']]
p('pipe.ageMs picks: median', st.median(pa), 'p95', st.quantiles(pa, n=20)[18], 'max', max(pa), '>1s', sum(a > 1000 for a in pa), '>5s', sum(a > 5000 for a in pa))
ks = Counter()
for d in P:
    c = d['chosen']
    if not c:
        continue
    ks[(d['out'], 'shown' if c[FLAGS] & F['shown'] else 'notshown', 'preEq' if d['preEquipped'] else '')] += 1
p('chosen shown?', ks)
hs = []; inHA = Counter()
for d in P:
    if d['out'] != 'menu' or not d['C']:
        continue
    rows = d['rows']; ch = d['chosen']
    HA = [r for r in rows if r[FLAGS] & F['held'] and not r[FLAGS] & F['shown'] and not (r[FLAGS] & F['equipped'] and r is not ch)]
    hs.append(len(HA))
    if ch in HA:
        lab = 'chosen in H\\A'
    elif ch and ch[FLAGS] & F['shown']:
        lab = 'chosen shown'
    else:
        lab = 'chosen not held' if ch and not ch[FLAGS] & F['held'] else 'other'
    inHA[(lab, d['C']['heldFull'], ch[KIND] if ch else None)] += 1
if hs:
    p('menu H\\A size: median', st.median(hs), 'min', min(hs), 'max', max(hs))
    p('menu chosen membership', inHA)
rc = [len(d['C']['rows']) for d in P if d['C']]
p('rows per pick ctx: median', st.median(rc), 'max', max(rc))
p('shown per pick ctx', Counter(sum(1 for r in d['C']['rows'] if r[FLAGS] & F['shown']) for d in P if d['C']))
p('key picks: slot of chosen', Counter(d['chosen'][SLOT] for d in P if d['out'] == 'key' and d['chosen']))
p('key picks: chosen flags wildcard/override/remembered', Counter((bool(d['chosen'][FLAGS] & 32), bool(d['chosen'][FLAGS] & 64), bool(d['chosen'][FLAGS] & 128)) for d in P if d['out'] in ('key', 'wheel') and d['chosen']))

p('\n== duplicates ==')
seen = Counter((d['launch'], d['seq']) for d in decs)
p('dup (launch,seq)', sum(v - 1 for v in seen.values() if v > 1))
seen2 = Counter((d['launch'], d['utc'], d['form'], d['out']) for d in P)
p('dup (launch,utc,form,out)', sum(v - 1 for v in seen2.values() if v > 1))
close = Counter(); prev = {}
for d in sorted(P, key=lambda d: d['utc']):
    k = (d['launch'], d['form'])
    if k in prev and (T(d['utc']) - T(prev[k][0])).total_seconds() < 5:
        close[(prev[k][1], d['out'], d['kind'])] += 1
    prev[k] = (d['utc'], d['out'])
p('same item picked again within 5 s (prev out, out, kind)', close)
cs = Counter(d['ctxkey'] for d in P)
p('picks sharing a ctx: ctxs with >1 pick', sum(1 for v in cs.values() if v > 1), 'picks in them', sum(v for v in cs.values() if v > 1),
  'by out', Counter(d['out'] for d in P if cs[d['ctxkey']] > 1))
# nothing records sharing ctx
cn = Counter(d['ctxkey'] for d in decs if d['out'] == 'nothing')
p('nothing: distinct ctxs', len(cn), 'records', sum(cn.values()))
p('nothing by need (top 30)', Counter(needs[d['ep']['gi']] for d in decs if d['out'] == 'nothing').most_common(30))
p('nothing durSec median', st.median([d['ep']['durSec'] for d in decs if d['out'] == 'nothing']))

p('\n== need columns ==')
allctx = list(D['ctxs'].values())
pickctx = {d['ctxkey'] for d in P}


def needstats(ctxlist, label):
    p(f'-- {label}: {len(ctxlist)} contexts; per need: frac>0.01, frac>=0.5, max, distinct values')
    rows = []
    for k, n in enumerate(needs):
        vals = [c['need'].get(k, 0.0) for c in ctxlist]
        nz = sum(v > 0.01 for v in vals) / len(vals); hi = sum(v >= 0.5 for v in vals) / len(vals)
        rows.append((n, nz, hi, max(vals), len(set(round(v, 3) for v in vals))))
    for n, nz, hi, mx, dv in rows:
        p(f'  {n:24} >0.01 {nz:6.3f}  >=0.5 {hi:6.3f}  max {mx:7.4f}  distinct {dv}')
    return rows


rall = needstats(allctx, 'all contexts')
rpick = needstats([D['ctxs'][k] for k in pickctx if k in D['ctxs']], 'pick contexts')
p('\nNEVER >= 0.5 in any context:', [n for n, nz, hi, mx, dv in rall if hi == 0])
p('NEVER > 0.01:', [n for n, nz, hi, mx, dv in rall if nz == 0])
p('ALWAYS >= 0.5 (>99%):', [n for n, nz, hi, mx, dv in rall if hi > 0.99])
p('\n-- curve inputs: share of contexts where input present; sentinel 1e6/4096 share')
for k, n in enumerate(needs):
    vals = [c['in'].get(k) for c in allctx]
    pres = [v for v in vals if v is not None]
    if not pres:
        p(f'  {n:24} input never logged (non-zero)'); continue
    sent = sum(1 for v in pres if v in (1e6, 4096))
    nan = sum(1 for v in pres if v != v)
    p(f'  {n:24} present {len(pres) / len(vals):.3f} sentinel {sent / len(pres):.3f} nan {nan} min {min(pres):.4g} max {max(pres):.4g}')

p('\n== effect columns ==')


def capcols(rowsel):
    cnt = Counter(); n = 0
    for r in rowsel:
        n += 1
        if r[CAP] >= 0:
            for j, v in caps[r[CAP]]:
                cnt[j] += 1
        for j, v in r[X]:
            cnt[('x', j)] += 1
    return cnt, n


chosen_rows = [d['chosen'] for d in P if d['chosen']]
shown_rows = [r for d in P if d['C'] for r in d['C']['rows'] if r[FLAGS] & F['shown']]
held_rows = [r for d in P if d['C'] for r in d['C']['rows'] if r[FLAGS] & F['held']]
cc, nc = capcols(chosen_rows); cs_, ns = capcols(shown_rows); ch_, nh = capcols(held_rows)
p(f'chosen rows {nc}, shown rows {ns}, held rows {nh}')
p('chosen rows with cap -1 (not in catalog):', sum(1 for r in chosen_rows if r[CAP] < 0), Counter(r[KIND] for r in chosen_rows if r[CAP] < 0))
p('  names:', Counter(d['name'] for d in P if d['chosen'] and d['chosen'][CAP] < 0).most_common(20))
p('chosen rows with empty cap:', sum(1 for r in chosen_rows if r[CAP] >= 0 and not caps[r[CAP]]), Counter(d['name'] for d in P if d['chosen'] and d['chosen'][CAP] >= 0 and not caps[d['chosen'][CAP]]).most_common(10))
p('shown rows with cap -1:', sum(1 for r in shown_rows if r[CAP] < 0), 'of', ns)
p('effect columns ever non-zero: chosen', len([k for k in cc if not isinstance(k, tuple)]), 'shown', len([k for k in cs_ if not isinstance(k, tuple)]), 'held', len([k for k in ch_ if not isinstance(k, tuple)]), 'of', len(cols))
p('top chosen columns:', [(cols[j], v) for j, v in cc.most_common(70) if not isinstance(j, tuple)])
p('cross-features on chosen:', [(D['cross'][j[1]], v) for j, v in cc.items() if isinstance(j, tuple)])
p('cross-features on shown:', [(D['cross'][j[1]], v) for j, v in cs_.items() if isinstance(j, tuple)])
p('cross-features on held:', [(D['cross'][j[1]], v) for j, v in ch_.items() if isinstance(j, tuple)])
everall = Counter()
for c in allctx:
    for r in c['rows']:
        if r[CAP] >= 0:
            for j, v in caps[r[CAP]]:
                everall[j] += 1
p('columns never non-zero in any row of any context:', len([j for j in range(len(cols)) if everall[j] == 0]))
p('  ', [cols[j] for j in range(len(cols)) if everall[j] == 0])
out.close()

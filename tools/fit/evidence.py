"""Evidence per need (picks only): how many picks had the need active and an answering item."""
import numpy as np
from collections import Counter, defaultdict
from fitlib import *
D = load(); v2 = load_v2()
out = out_file('evidence.txt')
def p_(*a):
    print(*a, file=out, flush=True)
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

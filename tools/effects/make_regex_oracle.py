"""tests/core/fixtures/regex_oracle.csv: Python `re` results for every rule-table pattern
on a corpus of real effect names, normalised descriptions and keyword lists (from the
effect fixtures). usage: python -I make_regex_oracle.py <repo> <patterns.txt>"""
import sys, csv, re, random
repo, pat_file = sys.argv[1], sys.argv[2]
random.seed(7)
patterns = [l.rstrip('\n') for l in open(pat_file, encoding='utf-8') if l.strip()]


def norm(text):
    t = text.lower()
    t = re.sub(r'<[^>]*>', 'N', t)
    t = re.sub(r'\b\d+(\.\d+)?', 'N', t)
    return t


corpus = set()
for lst in ('vanilla', 'simonrim', 'lorerim'):
    for r in csv.DictReader(open(f'{repo}/tests/core/fixtures/effects_{lst}.csv', encoding='utf-8')):
        corpus.add(r['effectName'].lower())
        if r.get('effectDescription'):
            corpus.add(norm(r['effectDescription']))
        corpus.add(r['effectKeywords'])
        corpus.add(r['name'].lower())
corpus = sorted(t for t in corpus if t.isascii() and len(t) < 400)
rows = []
for p in patterns:
    rx = re.compile(p)
    hits, misses = [], []
    for t in corpus:
        (hits if rx.search(t) else misses).append(t)
    random.shuffle(hits); random.shuffle(misses)
    for t in hits[:12] + misses[:4]:
        m = rx.search(t)
        g1 = ''
        if m and m.re.groups >= 1 and m.group(1) is not None:
            g1 = m.group(1)
        rows.append({'pattern': p, 'text': t, 'matched': 1 if m else 0,
                     'start': m.start() if m else -1, 'end': m.end() if m else -1, 'group1': g1})
with open(f'{repo}/tests/core/fixtures/regex_oracle.csv', 'w', newline='', encoding='utf-8') as f:
    w = csv.DictWriter(f, fieldnames=['pattern', 'text', 'matched', 'start', 'end', 'group1'])
    w.writeheader()
    w.writerows(rows)
print(len(patterns), 'patterns', len(corpus), 'texts', len(rows), 'rows',
      sum(r['matched'] for r in rows), 'matches')

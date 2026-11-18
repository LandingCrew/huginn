"""Churn per press from the [Soak] heartbeat: the R9 comparison against the R8 session.

R9 (the Bayesian challenger rule, roadmap) is judged in game on churn per press: the visible
slot changes the player did not cause, over the slot presses, in a session on R9 against one on
R8. 0.24.2's heartbeat prints it per window (`| perPress churn=N presses=M churn/press=X
bounce10s=B (hold=H)`), but a session's figure is the SUM over the sum, not a mean of the window
ratios, and R8's heartbeat has no such field. Both carry what it is made of -- the per-cause
slotChurn breakdown and presses= -- so this script computes it the same way for any build:

    churn = every visible change but a page switch, a Used backfill, a Remembrance hold and an
            override (the player's own causes and the safety layer; trace-04's "without player
            or safety causes")
    churn/press = sum(churn) / sum(presses)

It also sums, where the build prints them: the hold's causes (expired + unheld, the changes the
challenger rule governs) per press, the ratio and dsd buckets, under= and the bounces.

usage:  python -I tools/soak/churn_per_press.py <log> [<log> ...]
        (each log on its own: give the R8 session's log and the R9 session's log side by side;
        a launch boundary -- up= going backwards -- splits a file into launches)

Standard library only.
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

PLAYER_CAUSES = ('page', 'used', 'remembrance', 'override')
HOLD_CAUSES = ('expired', 'unheld')

SOAK = re.compile(r'\[Soak\] up=(\d+)h(\d+)m(\d+)s')
CHURN = re.compile(r'slotChurn=(\d+)(?: peak5s=\S+ \(([^)]*)\))?')
PRESSES = re.compile(r'\bpresses=(\d+)')
GROUP = r'\b{}\(([^)]*)\)'
UNDER = re.compile(r'\) under=(\d+)')
BOUNCE = re.compile(r'bounce10s=(\d+) \(hold=(\d+)\)')
PAIR = re.compile(r'(\S+)=(\d+)(?=\s|$)')   # greedy key: '>=1.5=3' is key '>=1.5'


def pairs(text):
    return {k: int(v) for k, v in PAIR.findall(text or '')}


class Tally:
    def __init__(self):
        self.windows = 0
        self.churn = 0
        self.presses = 0
        self.hold = 0
        self.causes = Counter()
        self.ratio = Counter()
        self.dsd = Counter()
        self.under = 0
        self.bounce = 0
        self.bounce_hold = 0
        self.has_r9 = False

    def add_line(self, line):
        m = CHURN.search(line)
        p = PRESSES.search(line)
        if not m or not p:
            return False
        total = int(m.group(1))
        causes = pairs(m.group(2))
        if total and not causes:
            raise ValueError('slotChurn=%d with no cause breakdown: %s' % (total, line.strip()[:200]))
        if causes and sum(causes.values()) != total:
            raise ValueError('slotChurn=%d but its causes sum to %d' % (total, sum(causes.values())))
        self.windows += 1
        self.causes.update(causes)
        self.churn += total - sum(causes.get(c, 0) for c in PLAYER_CAUSES)
        self.hold += sum(causes.get(c, 0) for c in HOLD_CAUSES)
        self.presses += int(p.group(1))
        for name, counter in (('ratio', self.ratio), ('dsd', self.dsd)):
            g = re.search(GROUP.format(name), line)
            if g:
                counter.update(pairs(g.group(1)))
        u = UNDER.search(line)
        if u:
            self.under += int(u.group(1))
        b = BOUNCE.search(line)
        if b:
            self.has_r9 = True
            self.bounce += int(b.group(1))
            self.bounce_hold += int(b.group(2))
        return True

    def per_press(self, n):
        return '%.2f' % (n / self.presses) if self.presses else 'n/a'

    def report(self, label):
        out = ['%s: %d windows, churn %d over %d presses: churn/press %s (hold causes %d: %s/press)'
               % (label, self.windows, self.churn, self.presses, self.per_press(self.churn), self.hold,
                  self.per_press(self.hold))]
        if self.causes:
            out.append('  causes ' + ' '.join('%s=%d' % kv for kv in sorted(self.causes.items())))
        if self.ratio:
            out.append('  ratio  ' + ' '.join('%s=%d' % kv for kv in self.ratio.items()))
        if self.dsd:
            out.append('  dsd    ' + ' '.join('%s=%d' % kv for kv in self.dsd.items()) + ' under=%d' % self.under)
        if self.has_r9:
            out.append('  bounce10s %d (hold %d): %s/press' % (self.bounce, self.bounce_hold,
                                                              self.per_press(self.bounce)))
        return '\n'.join(out)


def read(lines):
    """Tallies per launch (up= going backwards starts one) and for the whole input."""
    launches, whole = [], Tally()
    last_up = None
    for line in lines:
        s = SOAK.search(line)
        if not s:
            continue
        up = int(s.group(1)) * 3600 + int(s.group(2)) * 60 + int(s.group(3))
        if last_up is None or up < last_up:
            launches.append(Tally())
        last_up = up
        if launches[-1].add_line(line):
            whole.add_line(line)
    return launches, whole


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('logs', nargs='+', type=Path)
    args = ap.parse_args(argv)
    for path in args.logs:
        with open(path, encoding='utf-8', errors='replace') as f:
            launches, whole = read(f)
        if not whole.windows:
            print('%s: no [Soak] heartbeat' % path)
            continue
        print(whole.report(str(path)))
        if len(launches) > 1:
            for i, t in enumerate(launches, 1):
                print(t.report('  launch %d' % i).replace('\n', '\n  '))
    return 0


if __name__ == '__main__':
    sys.exit(main())

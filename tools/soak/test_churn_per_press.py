"""Self-test for churn_per_press.py: an R8-format heartbeat and an R9-format one.

usage:  python -I tools/soak/test_churn_per_press.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import churn_per_press as cpp  # noqa: E402

# R8 (0.24.1) shape: no dsd(), no perPress section.
R8 = ('[2026-10-10 18:48:37.321][SoakMetrics.cpp :329 ][I]: [Soak] up=0h05m00s | equips hit=0 near=0 '
      'miss=0 novel=0 new=0 accept=n/a skipped=0 | recompute=194/2575 ticks override=0 pageBail=0 | '
      'slotChurn=10 peak5s=1@slot0 (fill=1 clear=0 dedup=0 override=1 wildcard=2 expired=4 used=1 page=0 '
      'unheld=0 remembrance=1 seated=0 upsert=0 unshowable=0) ratio(gone=1 <1=0 <1.1=0 <1.25=0 <1.5=0 '
      '>=1.5=3) tenure(<1s=0 1-3s=1 3-10s=0 10-30s=1 30s-2m=2 >=2m=1) | learn items=80 trains=525 '
      'forgot=0 | tick avg=1.154 peak=9.384 ms | goals reachIns=0 (candidate=0) presses=4 (regular=4 '
      'labeled=0 offPage=0) pageFlips=0 | fight=0 | choice off | rate runs/s=0.50 wildcard=0% wheeler/s=0.00')
QUIET = ('[x][I]: [Soak] up=0h10m00s | equips hit=0 | slotChurn=0 | learn items=1 | goals reachIns=0 '
         '(candidate=0) presses=2 (regular=2 labeled=0 offPage=0) pageFlips=0')
# R9 (0.24.2) shape.
R9 = ('[x][I]: [Soak] up=0h05m00s | equips hit=0 | slotChurn=6 peak5s=1@slot2 (fill=0 clear=0 dedup=0 '
      'override=0 wildcard=1 expired=3 used=0 page=2 unheld=0 remembrance=0 seated=0 upsert=0 '
      'unshowable=0) ratio(gone=0 <1=1 <1.1=0 <1.25=0 <1.5=0 >=1.5=2) dsd(gone=0 <0=1 <0.5=0 <1=0 <2=1 '
      '<4=1 >=4=0 nosd=0) under=1 tenure(<1s=0) | learn items=1 | goals reachIns=0 (candidate=0) '
      'presses=8 (regular=8 labeled=0 offPage=0) pageFlips=0 | choice theta=1/2 | rate runs/s=1.00 | '
      'perPress churn=4 presses=8 churn/press=0.50 bounce10s=1 (hold=1)')


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def main():
    launches, whole = cpp.read([R8, 'noise', QUIET])
    check(len(launches) == 1 and whole.windows == 2, 'two windows, one launch')
    # 10 changes less override 1, used 1, remembrance 1, page 0.
    check(whole.churn == 7, 'R8 churn %d' % whole.churn)
    check(whole.presses == 6, 'presses %d' % whole.presses)
    check(whole.hold == 4, 'hold causes %d' % whole.hold)
    check(whole.per_press(whole.churn) == '1.17', whole.per_press(whole.churn))
    check(whole.ratio['>=1.5'] == 3 and not whole.dsd and not whole.has_r9, 'R8 groups')

    launches, whole = cpp.read([R9])
    # 6 changes less page 2: the same number the build printed (perPress churn=4).
    check(whole.churn == 4 and whole.presses == 8, 'R9 churn %d presses %d' % (whole.churn, whole.presses))
    check(whole.dsd['<0'] == 1 and whole.dsd['<4'] == 1 and whole.under == 1, 'R9 dsd')
    check(whole.has_r9 and whole.bounce == 1 and whole.bounce_hold == 1, 'R9 bounce')
    check(whole.per_press(whole.churn) == '0.50', 'R9 per press')

    # A launch boundary: up= goes backwards.
    launches, whole = cpp.read([R8, QUIET, R9])
    check(len(launches) == 2 and whole.windows == 3, 'two launches')
    check(launches[1].churn == 4 and whole.churn == 11, 'per launch')

    # A malformed breakdown is an error, not a silent miscount.
    try:
        cpp.read([R8.replace('expired=4', 'expired=5')])
    except ValueError:
        pass
    else:
        raise AssertionError('a breakdown that does not sum must raise')

    # No presses: n/a, not a division by zero.
    t = cpp.Tally()
    check(t.per_press(3) == 'n/a', 'no presses')
    print('test_churn_per_press: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

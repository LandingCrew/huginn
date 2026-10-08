"""Compare a `hg dump races` file's huginnReading column with the race map.

    python -I tools/races/check_race_reading.py <Huginn_Races.csv> [race_map.csv]

The dump comes from the game (Debug build, `hg dump races`, written next to the
SKSE log). Rows are joined on plugin + editorID: a runtime formID moves when the
load order changes (an ESL record's FE-prefixed ID shifts with its slot). The
formID is the fallback for a race with no editorID. A race the map lists but
the dump lacks is reported; races the dump has and the map does not are
ignored.

Expected reading per map row (the same rule as race_reading_host_check.cpp):
a row flagged in today_mismatch reads its primary family folded onto today's
six types; every other row reads the map's `today`. Exit code 1 on any
mismatch.
"""
import csv
import os
import sys

FOLD = {
    'humanoid': 'Humanoid', 'none': 'Humanoid', 'undead': 'Undead', 'daedra': 'Daedra',
    'dragon': 'Dragon', 'construct': 'Construct', 'animal': 'Beast', 'arthropod': 'Beast',
    'troll': 'Beast', 'giant': 'Beast', 'werebeast': 'Beast', 'monster': 'Beast',
}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    map_path = argv[2] if len(argv) > 2 else os.path.join(
        here, '..', '..', 'docs', 'architecture', '9-data', 'race_map.csv')
    with open(argv[1], encoding='utf-8-sig', errors='replace', newline='') as f:
        dump_rows = list(csv.DictReader(f))
    if dump_rows and 'huginnReading' not in dump_rows[0]:
        print('dump has no huginnReading column: made by a build before 0.23.8')
        return 2
    by_key = {(r['plugin'], r['editorID']): r for r in dump_rows if r['editorID']}
    by_form = {r['formID']: r for r in dump_rows}
    with open(map_path, encoding='utf-8', newline='') as f:
        rows = list(csv.DictReader(f))

    bad = 0
    for r in rows:
        d = by_key.get((r['plugin'], r['editorID'])) if r['editorID'] else None
        if d is None:
            d = by_form.get(r['formID'])
        if d is None:
            print(f"MISSING  {r['formID']} {r['editorID']}")
            bad += 1
            continue
        expected = FOLD[r['family']] if r['today_mismatch'] else r['today']
        if d['huginnReading'] != expected:
            bad += 1
            print(f"MISMATCH {r['formID']} {r['editorID']}: read {d['huginnReading']}, expected {expected} "
                  f"(family {r['family']}, today {r['today']}, flag '{r['today_mismatch']}')")
    print(f"rows {len(rows)}  matched {len(rows) - bad}  mismatched {bad}")
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))

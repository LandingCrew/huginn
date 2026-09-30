#!/usr/bin/env python3
"""Reference reader for Huginn decision-log files (docs/architecture/9-telemetry.md).

Reads one or more *.jsonl files, validates the schema, joins every reward
("rew") to the impression ("imp") it follows, and prints per-item exposure and
reward counts.

    python tools/telemetry_aggregate.py Huginn_Telemetry*.jsonl [--top 30]

Standard library only. Records belong to the most recent "session" header above
them in the same file; impression ids are unique per session id ("sid").
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter, defaultdict

SUPPORTED_SCHEMAS = {1, 2, 3}  # v2 only adds fields (fit, eqk, dlv/skill, fit+cap cfg); v3 adds the Cold/Hungry reasons


def read_records(path):
    """Yield (sid, record) pairs from one file, skipping unparseable lines."""
    sid = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                print(f"{path}:{lineno}: unparseable line skipped (truncated write?)", file=sys.stderr)
                continue
            if rec.get("t") == "session":
                schema = rec.get("schema")
                if schema not in SUPPORTED_SCHEMAS:
                    print(f"{path}:{lineno}: schema {schema} not supported "
                          f"(know {sorted(SUPPORTED_SCHEMAS)}); session skipped", file=sys.stderr)
                    sid = None
                    continue
                sid = rec.get("sid")
            if sid is None:
                continue
            yield sid, rec


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="decision-log .jsonl files")
    ap.add_argument("--top", type=int, default=30, help="items to list (default 30)")
    args = ap.parse_args(argv)

    impressions = {}                 # (sid, id) -> set of displayed item keys
    shown = Counter()                # item key -> impressions it was displayed in
    shown_wildcard = Counter()       # item key -> impressions it was displayed as a wildcard
    rewards = Counter()              # item key -> trained reward events
    reward_sum = defaultdict(float)  # item key -> summed applied reward
    by_source = Counter()
    joined = unjoined = shown_at_reward = 0
    dropped = 0
    sessions = set()
    counts = Counter()

    for path in args.files:
        for sid, rec in read_records(path):
            t = rec.get("t")
            counts[t] += 1
            if t == "session":
                sessions.add(sid)
            elif t == "drop":
                dropped += int(rec.get("count", 0))
            elif t == "imp":
                cands = rec.get("cands", [])
                keys = set()
                for slot in rec.get("slots", []):
                    c = slot.get("c", -1)
                    if 0 <= c < len(cands):
                        key = cands[c].get("k")
                        keys.add(key)
                        shown[key] += 1
                        if slot.get("as") == "Wildcard":
                            shown_wildcard[key] += 1
                impressions[(sid, rec.get("id"))] = keys
            elif t == "rew":
                key = rec.get("k")
                by_source[rec.get("src")] += 1
                imp = rec.get("imp")
                if imp is not None and (sid, imp) in impressions:
                    joined += 1
                    if key in impressions[(sid, imp)]:
                        shown_at_reward += 1
                else:
                    unjoined += 1
                if rec.get("trained"):
                    rewards[key] += 1
                    reward_sum[key] += float(rec.get("r") or 0.0)

    print(f"sessions: {len(sessions)}   records: {dict(counts)}   dropped: {dropped}")
    print(f"reward events by source: {dict(by_source)}")
    print(f"rewards joined to an impression: {joined}  (item on that impression: {shown_at_reward}), "
          f"unjoined: {unjoined}")
    print()
    keys = set(shown) | set(rewards)
    rows = sorted(keys, key=lambda k: (-shown[k], -rewards[k], k or ""))[: args.top]
    width = max((len(k or "") for k in rows), default=10)
    print(f"{'item key':<{width}}  {'shown':>7}  {'as wc':>6}  {'trained':>7}  {'sum r':>8}")
    for k in rows:
        print(f"{(k or '?'):<{width}}  {shown[k]:>7}  {shown_wildcard[k]:>6}  {rewards[k]:>7}  {reward_sum[k]:>8.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

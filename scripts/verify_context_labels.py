#!/usr/bin/env python3
"""Verify Huginn's context-reason labels from a play-session log.

The subtext label a player sees is derived from ContextRuleEngine's weight map
(architecture-critique #10). Two log lines make that surface machine-checkable:

    [Context] Reason: Low HP -> Critical HP | hp=12% mp=100% sp=64% charge=n/a
    [Subtext] page 0 | 0=Critical HP(Potion of Restore Health) 3=Low Ammo(Iron Arrow)

This script asserts the invariants that must hold between them, so a soak run is
checked by a command instead of by reading labels off the wheel.

Usage:
    python scripts/verify_context_labels.py "<path to Huginn.log>"
    python scripts/verify_context_labels.py <log> --quiet   # only failures

Exit codes: 0 = all invariants held, 1 = violation, 2 = no data to check.
"""

import argparse
import re
import sys

# Vital percentages the continuous rules invert (State::VitalThreshold).
CRITICAL_PCT = 15.0
LOW_PCT = 30.0
CHARGE_LOW_PCT = 25.0

# Float-formatted percentages are rounded to whole numbers in the log, so allow
# a hair of slack at the boundary rather than failing on 30% vs 30.4%.
EPSILON = 1.0

REASON_RE = re.compile(
    r"\[Context\] Reason: (?P<prev>.+?) → (?P<now>.+?) \| "
    r"hp=(?P<hp>-?\d+)% mp=(?P<mp>-?\d+)% sp=(?P<sp>-?\d+)% charge=(?P<charge>n/a|-?\d+%)"
)
SUBTEXT_RE = re.compile(r"\[Subtext\] page (?P<page>\d+) \| (?P<labels>.+)$")
SUBTEXT_EMPTY_RE = re.compile(r"\[Subtext\] page (?P<page>\d+) — no labels")
SLOT_RE = re.compile(r"(?P<slot>\d+)=(?P<label>[^(]+)\((?P<item>[^)]*)\)")
BUILD_RE = re.compile(r"Huginn (?P<version>v[\d.]+) \((?P<hash>[0-9a-f]+)\) \[(?P<config>\w+)\]")

# Reasons OverrideManager stamps onto the candidate it surfaces. These may
# appear on a slot regardless of the ambient reason.
OVERRIDE_REASONS = {
    "Critical HP",
    "Low MP",
    "Low SP",
    "Low Ammo",
    "Underwater",
    "Low Charge",
}


# Labels that are claims about a number on the same line. Everything else is a
# binary rule with no numeric counterpart to cross-check here.
NUMERIC_CLAIMS = {
    "Critical HP": ("hp", lambda v: v <= CRITICAL_PCT + EPSILON),
    "Low HP": ("hp", lambda v: v <= LOW_PCT + EPSILON),
    "Low MP": ("mp", lambda v: v <= LOW_PCT + EPSILON),
    "Low SP": ("sp", lambda v: v <= LOW_PCT + EPSILON),
}


def check(path, quiet=False):
    violations = []
    reasons = []
    subtexts = []
    build = None
    current_reason = "(none)"

    with open(path, encoding="utf-8", errors="replace") as handle:
        for lineno, line in enumerate(handle, 1):
            if build is None:
                found = BUILD_RE.search(line)
                if found:
                    build = found.groupdict()

            match = REASON_RE.search(line)
            if match:
                data = match.groupdict()
                reasons.append(data)

                # Invariant 1: a numeric label must agree with the numbers on
                # its own line — this is the curve inversion, checked directly.
                claim = NUMERIC_CLAIMS.get(data["now"])
                if claim:
                    field, holds = claim
                    value = float(data[field])
                    if not holds(value):
                        violations.append(
                            f"{path}:{lineno}: reported '{data['now']}' at {field}={value:.0f}%"
                        )

                # Invariant 2: "Low Charge" requires an enchanted weapon below
                # its threshold; 'n/a' means no enchanted weapon at all.
                if data["now"] == "Low Charge":
                    if data["charge"] == "n/a":
                        violations.append(
                            f"{path}:{lineno}: reported 'Low Charge' with no enchanted weapon"
                        )
                    elif float(data["charge"].rstrip("%")) > CHARGE_LOW_PCT + EPSILON:
                        violations.append(
                            f"{path}:{lineno}: reported 'Low Charge' at charge={data['charge']}"
                        )

                # Invariant 3: transitions must chain — a gap means a reason
                # change went unlogged (dedup state lost, or a skipped run).
                if data["prev"] != current_reason:
                    violations.append(
                        f"{path}:{lineno}: transition starts at '{data['prev']}' "
                        f"but last known reason was '{current_reason}'"
                    )
                current_reason = data["now"]
                continue

            match = SUBTEXT_RE.search(line)
            if match:
                slots = SLOT_RE.findall(match.group("labels"))
                subtexts.append((lineno, match.group("page"), slots))

                # Invariant 4: every non-override slot showing a label must be
                # showing THIS tick's reason (or "Favorite", the fallback).
                for _slot, label, _item in slots:
                    label = label.strip()
                    if label in (current_reason, "Favorite"):
                        continue
                    # An override stamps its own reason, which legitimately
                    # differs from the ambient one — only flag labels that are
                    # neither the tick's reason, the favourite fallback, nor a
                    # known override reason.
                    if label in OVERRIDE_REASONS:
                        continue
                    violations.append(
                        f"{path}:{lineno}: slot label '{label}' matches neither the "
                        f"active reason '{current_reason}' nor an override reason"
                    )
                continue

            match = SUBTEXT_EMPTY_RE.search(line)
            if match:
                subtexts.append((lineno, match.group("page"), []))

    if not quiet:
        if build:
            print(f"build: {build['version']} ({build['hash']}) [{build['config']}]")
        print(f"reason transitions: {len(reasons)}")
        print(f"subtext changes:    {len(subtexts)}")
        if reasons:
            seen = []
            for entry in reasons:
                if entry["now"] not in seen:
                    seen.append(entry["now"])
            print(f"reasons observed:   {', '.join(seen)}")

    if not reasons and not subtexts:
        print(
            "NO DATA: no [Context]/[Subtext] lines found. Either the log predates "
            "the reason logging, or the pipeline never ran.",
            file=sys.stderr,
        )
        return 2

    if violations:
        print(f"\nFAIL: {len(violations)} invariant violation(s)", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    if not quiet:
        print("\nPASS: all invariants held")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", help="path to Huginn.log or _Huginn_Debug.log")
    parser.add_argument("--quiet", action="store_true", help="print only failures")
    args = parser.parse_args()
    sys.exit(check(args.log, args.quiet))


if __name__ == "__main__":
    main()

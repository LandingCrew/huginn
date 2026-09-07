#!/bin/bash
# ──────────────────────────────────────────────
# Intuition Widget Build Script
# Requires: swfmill, mtasc (both on PATH)
# ──────────────────────────────────────────────

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

OUT="Intuition.swf"

echo "=== Step 1: swfmill — XML -> SWF shell ==="
swfmill simple intuition.xml "$OUT"
echo "    Created $OUT"

echo "=== Step 2: mtasc — Compile AS2 into SWF ==="
mtasc -cp . -swf "$OUT" -main Intuition.as
echo "    Injected Intuition.as (with -main entry point)"

# The shipped artifact. This copy used to be a printed instruction, and the
# manual step is exactly how Data/Interface/Huginn/Intuition.swf drifted a whole
# feature behind this source — it shipped a widget with no per-slot visual
# states, silently dropping setSlot's 6th argument.
SHIPPED="$SCRIPT_DIR/../../Data/Interface/Huginn/Intuition.swf"
echo "=== Step 3: install to Data/Interface/Huginn ==="
cp "$OUT" "$SHIPPED"
echo "    Copied to $SHIPPED"

echo "=== Done ==="
echo "Output: $SCRIPT_DIR/$OUT"
echo ""
echo "Next steps:"
echo "  1. Open $OUT in JPEXS to inspect"
echo "  2. Deploy to your test modlist's Interface/Huginn/ if it overrides Data/"

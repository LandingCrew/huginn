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

echo "=== Done ==="
echo "Output: $SCRIPT_DIR/$OUT"
echo ""
echo "Next steps:"
echo "  1. Open $OUT in JPEXS to inspect"
echo "  2. Copy to Data/Interface/Huginn/ for in-game testing"

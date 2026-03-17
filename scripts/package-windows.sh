#!/bin/bash
# package-windows.sh - Collect Windows DLL dependencies (MSYS2 UCRT64 environment)
#
# Usage: bash scripts/package-windows.sh <dist_dir>
# dist_dir structure:
#   dist/eyeq.exe
#   dist/*.dll  (populated by this script)

set -euo pipefail

DIST_DIR="${1:?Usage: $0 <dist_dir>}"
BINARY="$DIST_DIR/eyeq.exe"

if [ ! -f "$BINARY" ]; then
    echo "Error: cannot find $BINARY"
    exit 1
fi

echo "=== Collecting DLL dependencies ==="
ntldd -R "$BINARY" | grep 'ucrt64' | awk '{print $3}' | sed 's/\\/\//g' | sort -u | while read -r dep; do
    basename=$(basename "$dep")
    if [ ! -f "$DIST_DIR/$basename" ]; then
        echo "  Collecting: $basename"
        cp "$dep" "$DIST_DIR/"
    fi
done

echo ""
echo "=== Package contents ==="
ls -la "$DIST_DIR/"

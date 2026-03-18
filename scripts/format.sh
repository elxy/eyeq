#!/usr/bin/env bash
# Format C/C++/ObjC++ source files using clang-format.
# Usage:
#   scripts/format.sh          # format in-place
#   scripts/format.sh --check  # dry-run check (for CI)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Collect source files, excluding third-party headers (CLI11.hpp)
FILES=$(find "$ROOT_DIR/src" \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' -o -name '*.mm' \) \
    ! -name 'CLI11.hpp')

if [ -z "$FILES" ]; then
    echo "No source files found."
    exit 0
fi

COUNT=$(echo "$FILES" | wc -l | tr -d ' ')

if [ "${1:-}" = "--check" ]; then
    echo "Checking format of $COUNT files..."
    echo "$FILES" | xargs clang-format --dry-run --Werror
    echo "All files are properly formatted."
else
    echo "Formatting $COUNT files..."
    echo "$FILES" | xargs clang-format -i
    echo "Done."
fi

#!/bin/bash
# package-linux.sh - Linux AppImage packaging helper script
#
# This script prepares additional files needed for AppImage and performs validation.
# The actual AppImage creation is done by linuxdeploy.
#
# Usage: bash scripts/package-linux.sh <appdir>

set -euo pipefail

APPDIR="${1:?Usage: $0 <appdir>}"

echo "=== Validating AppDir structure ==="

# Check required files
if [ ! -f "$APPDIR/usr/bin/eyeq" ]; then
    echo "Error: $APPDIR/usr/bin/eyeq does not exist"
    exit 1
fi

echo "  Binary: $APPDIR/usr/bin/eyeq"

# List installed libraries
if [ -d "$APPDIR/usr/lib" ]; then
    echo "  Libraries:"
    ls "$APPDIR/usr/lib/" | head -20
fi

# If using Linuxbrew, supplement with Linuxbrew shared libraries
if command -v brew &>/dev/null; then
    BREW_LIB="$(brew --prefix)/lib"
    if [ -d "$BREW_LIB" ]; then
        echo ""
        echo "=== Supplementing Linuxbrew libraries into AppDir ==="
        mkdir -p "$APPDIR/usr/lib"

        # Use ldd to find all Linuxbrew dependencies
        BREW_DEPS=$(ldd "$APPDIR/usr/bin/eyeq" 2>/dev/null \
            | grep "$BREW_LIB" \
            | awk '{print $3}' || true)

        for dep in $BREW_DEPS; do
            basename=$(basename "$dep")
            if [ ! -f "$APPDIR/usr/lib/$basename" ]; then
                echo "  Supplementing: $basename"
                cp -L "$dep" "$APPDIR/usr/lib/$basename"
            fi
        done
    fi
fi

echo ""
echo "=== AppDir preparation complete ==="

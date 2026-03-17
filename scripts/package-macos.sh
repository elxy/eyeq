#!/bin/bash
# package-macos.sh - Collect macOS dynamic library dependencies and fix rpaths
#
# Usage: bash scripts/package-macos.sh <dist_dir>
# dist_dir structure:
#   dist/bin/eyeq
#   dist/lib/  (populated by this script)

set -euo pipefail

DIST_DIR="${1:?Usage: $0 <dist_dir>}"
BINARY="$DIST_DIR/bin/eyeq"
LIB_DIR="$DIST_DIR/lib"

if [ ! -f "$BINARY" ]; then
    echo "Error: binary not found at $BINARY"
    exit 1
fi

mkdir -p "$LIB_DIR"

BREW_PREFIX="$(brew --prefix)"

# System framework and library prefixes - these do not need to be bundled
SYSTEM_PREFIXES=(
    "/usr/lib/"
    "/System/"
    "/Library/Frameworks/"
)

is_system_lib() {
    local lib="$1"
    for prefix in "${SYSTEM_PREFIXES[@]}"; do
        if [[ "$lib" == "$prefix"* ]]; then
            return 0
        fi
    done
    return 1
}

# Recursively collect all non-system dynamic library dependencies
collect_deps() {
    local target="$1"
    local deps

    deps=$(otool -L "$target" | tail -n +2 | awk '{print $1}')

    for dep in $deps; do
        # Skip self-references
        if [[ "$dep" == "@"* ]]; then
            continue
        fi

        # Skip system libraries
        if is_system_lib "$dep"; then
            continue
        fi

        local basename
        basename=$(basename "$dep")

        # Skip already collected libraries
        if [ -f "$LIB_DIR/$basename" ]; then
            continue
        fi

        # Copy library file
        if [ -f "$dep" ]; then
            echo "  Collecting: $basename (from $dep)"
            cp -L "$dep" "$LIB_DIR/$basename"
            chmod 755 "$LIB_DIR/$basename"
            # Recursively collect dependencies of this library
            collect_deps "$LIB_DIR/$basename"
        else
            echo "  Warning: cannot find $dep"
        fi
    done
}

echo "=== Collecting dynamic library dependencies ==="
collect_deps "$BINARY"

echo ""
echo "=== Fixing rpaths ==="

# Fix dynamic library reference paths in binaries
fix_rpaths() {
    local target="$1"
    local target_dir="$2"  # "bin" or "lib"
    local deps

    deps=$(otool -L "$target" | tail -n +2 | awk '{print $1}')

    for dep in $deps; do
        if [[ "$dep" == "@"* ]] || is_system_lib "$dep"; then
            continue
        fi

        local basename
        basename=$(basename "$dep")

        if [ -f "$LIB_DIR/$basename" ]; then
            local new_path
            if [ "$target_dir" = "bin" ]; then
                new_path="@executable_path/../lib/$basename"
            else
                new_path="@loader_path/$basename"
            fi
            echo "  $basename -> $new_path (in $(basename "$target"))"
            install_name_tool -change "$dep" "$new_path" "$target"
        fi
    done
}

# Fix main binary
fix_rpaths "$BINARY" "bin"

# Fix all collected dynamic libraries
for lib in "$LIB_DIR"/*.dylib; do
    if [ -f "$lib" ]; then
        # First fix the install name
        local_name=$(basename "$lib")
        install_name_tool -id "@loader_path/$local_name" "$lib" 2>/dev/null || true
        # Then fix dependency references
        fix_rpaths "$lib" "lib"
    fi
done

echo ""
echo "=== Package contents ==="
echo "Binary: $BINARY"
echo "Dynamic libraries:"
ls -la "$LIB_DIR/"

echo ""
echo "=== Verification ==="
echo "otool -L $BINARY:"
otool -L "$BINARY"

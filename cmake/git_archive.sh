#!/bin/bash
# git_archive.sh - Create a clean Git archive without altering the working tree

set -e  # Exit immediately on error

# Default parameters
ARCHIVE_FORMAT="tar.gz"
GIT_REF="HEAD"  # Default to current HEAD
INCLUDE_DIR="."  # Default to entire repository
PREFIX=""

# Show usage help
show_help() {
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -o, --output FILE     Specify output filename (default: based on tag or commit SHA)"
    echo "  -f, --format FORMAT   Specify archive format (default: $ARCHIVE_FORMAT)"
    echo "                        Supported formats: tar, tar.gz, tgz, zip"
    echo "  -r, --ref REF         Git reference to archive (default: $GIT_REF)"
    echo "  -p, --prefix PREFIX   Add prefix to paths in the archive"
    echo "  -d, --dir DIR         Archive a subdirectory of the repository (default: $INCLUDE_DIR)"
    echo "  -h, --help            Show this help message"
    echo
    echo "Examples:"
    echo "  $0 -f zip"
    echo "  $0 -r v1.0.0"
    echo "  $0 -d src/lib"
}

# Parse arguments
OUTPUT_FILE_SPECIFIED=0
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -o|--output)
            OUTPUT_FILE="$2"
            OUTPUT_FILE_SPECIFIED=1
            shift 2
            ;;
        -f|--format)
            ARCHIVE_FORMAT="$2"
            shift 2
            ;;
        -r|--ref)
            GIT_REF="$2"
            shift 2
            ;;
        -p|--prefix)
            PREFIX="--prefix=$2"
            shift 2
            ;;
        -d|--dir)
            INCLUDE_DIR="$2"
            shift 2
            ;;
        *)
            echo "Error: unknown argument $1"
            show_help
            exit 1
            ;;
    esac
done

# Check if current directory is inside a Git repository
if ! git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    echo "Error: current directory is not inside a Git repository"
    exit 1
fi

echo "Preparing archive..."

# Get current Git state
echo "- Saving current working state"
GIT_STATUS=$(git status --porcelain)
GIT_CURRENT_BRANCH=$(git symbolic-ref --short HEAD 2>/dev/null || echo "")
HAS_CHANGES=0

if [[ -n "$GIT_STATUS" ]]; then
    HAS_CHANGES=1
    # Stash uncommitted changes
    git stash push -u -m "Temporary stash for git archive script" > /dev/null
    echo "  Stashed uncommitted changes"
fi

# Ensure we can resolve the reference to archive
echo "- Checking reference: $GIT_REF"
if ! git rev-parse --verify "$GIT_REF" > /dev/null 2>&1; then
    echo "Error: Git reference '$GIT_REF' does not exist"
    # Restore stashed changes if any
    if [[ $HAS_CHANGES -eq 1 ]]; then
        git stash pop > /dev/null
    fi
    exit 1
fi

# If output filename not specified, auto-generate based on tag or commit SHA
if [[ $OUTPUT_FILE_SPECIFIED -eq 0 ]]; then
    # Get the commit object for the reference
    COMMIT_SHA=$(git rev-parse "$GIT_REF")
    # Try to get the nearest tag pointing to this commit
    NEAREST_TAG=$(git describe --tags --exact-match "$COMMIT_SHA" 2>/dev/null || echo "")
    
    if [[ -n "$NEAREST_TAG" ]]; then
        # Use tag as filename
        TAG_NAME=$(echo "$NEAREST_TAG")
        NAME_BASE="$TAG_NAME"
        echo "- Using tag for naming: $NEAREST_TAG"
    else
        # If no tag, use short SHA
        SHORT_SHA=$(git rev-parse --short "$COMMIT_SHA")
        NAME_BASE="$SHORT_SHA"
        echo "- Using short commit SHA for naming: $SHORT_SHA"
    fi

    # Get project name (from git config or directory name)
    PROJECT_NAME=$(basename "$(git rev-parse --show-toplevel)")

    # Set file extension based on archive format
    case "$ARCHIVE_FORMAT" in
        tar) FILE_EXT="tar" ;;
        tar.gz|tgz) FILE_EXT="tar.gz" ;;
        zip) FILE_EXT="zip" ;;
    esac

    OUTPUT_FILE="${PROJECT_NAME}-${NAME_BASE}.${FILE_EXT}"
    echo "- Auto-generated output filename: $OUTPUT_FILE"
fi

# Determine the correct archive format argument
case "$ARCHIVE_FORMAT" in
    tar)
        FORMAT_ARG="--format=tar"
        ;;
    tar.gz|tgz)
        FORMAT_ARG="--format=tar.gz"
        ;;
    zip)
        FORMAT_ARG="--format=zip"
        ;;
    *)
        echo "Error: unsupported archive format '$ARCHIVE_FORMAT'"
        echo "Supported formats: tar, tar.gz, tgz, zip"
        # Restore stashed changes if any
        if [[ $HAS_CHANGES -eq 1 ]]; then
            git stash pop > /dev/null
        fi
        exit 1
        ;;
esac

# Execute archiving
echo "- Creating archive: $OUTPUT_FILE"
if git archive $FORMAT_ARG $PREFIX -o "$OUTPUT_FILE" "$GIT_REF" -- "$INCLUDE_DIR"; then
    echo "  Archive created successfully"
    sha256 "$OUTPUT_FILE"
else
    echo "Error: failed to create archive"
    # Restore stashed changes if any
    if [[ $HAS_CHANGES -eq 1 ]]; then
        git stash pop > /dev/null
    fi
    exit 1
fi

# Restore working tree state
if [[ $HAS_CHANGES -eq 1 ]]; then
    echo "- Restoring working tree state"
    git stash pop > /dev/null
    echo "  Restored uncommitted changes"
fi

echo "Done! Archive created: $OUTPUT_FILE"
exit 0

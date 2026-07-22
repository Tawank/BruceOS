#!/bin/bash
# Apply local patches to managed components before building.
# Run this after idf.py reconfigure (so components are downloaded) and before
# idf.py build.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$REPO_ROOT/patches"
ELF_LOADER_DIR="$REPO_ROOT/managed_components/espressif__elf_loader"

if [ -d "$ELF_LOADER_DIR" ]; then
    if patch -d "$ELF_LOADER_DIR" -p1 --dry-run --quiet < "$PATCH_DIR/elf_loader-v1.3.1-idf-v6.patch" 2>/dev/null; then
        echo "Applying elf_loader v1.3.1 ESP-IDF v6 patch..."
        patch -d "$ELF_LOADER_DIR" -p1 < "$PATCH_DIR/elf_loader-v1.3.1-idf-v6.patch"
    else
        echo "elf_loader patch already applied or cannot be applied; skipping"
    fi
else
    echo "espressif__elf_loader not found; run idf.py reconfigure first"
    exit 1
fi

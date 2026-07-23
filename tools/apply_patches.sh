#!/usr/bin/env bash
# Apply project-specific patches to managed components.
# This script is run after `idf.py reconfigure` as described in migration_BruceIDF.md.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Remove newlib symbol exports that the ESP-IDF v6 toolchain no longer exposes.
# Bruce supplies its own symbol allowlist via elf_set_symbol_resolver().
PATCH_TARGET="$PROJECT_ROOT/managed_components/espressif__elf_loader/src/esp_elf_symbol.c"

if [ -f "$PATCH_TARGET" ]; then
    if grep -q "ESP_ELFSYM_EXPORT(__errno)" "$PATCH_TARGET"; then
        awk '
            /\/\* newlib \*\// { skip=1; next }
            skip && /\/\* math \*\// { skip=0; print; next }
            !skip { print }
        ' "$PATCH_TARGET" > "$PATCH_TARGET.tmp"
        mv "$PATCH_TARGET.tmp" "$PATCH_TARGET"
        echo "Removed newlib symbol exports from $PATCH_TARGET"
    else
        echo "Newlib symbol exports already removed from $PATCH_TARGET"
    fi
else
    echo "Patch target not found: $PATCH_TARGET"
    exit 1
fi

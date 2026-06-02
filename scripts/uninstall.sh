#!/usr/bin/env bash
# ============================================================
# C<< (cshift) uninstaller
#
# Usage:
#   ./uninstall.sh                  # removes from /usr/local
#   ./uninstall.sh ~/opt/cshift     # removes from custom prefix
#   sudo ./uninstall.sh /usr/local
# ============================================================

set -euo pipefail

INSTALL_PREFIX="${1:-/usr/local}"

_green()  { printf '\033[32m%s\033[0m\n' "$*"; }
_yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
_red()    { printf '\033[31m%s\033[0m\n' "$*"; }
_bold()   { printf '\033[1m%s\033[0m\n'  "$*"; }

_bold "============================================"
_bold " C<< (cshift) Uninstaller"
_bold "============================================"
echo  " Prefix: $INSTALL_PREFIX"
echo  ""

if [ ! -f "$INSTALL_PREFIX/bin/cshift" ] && \
   [ ! -f "$INSTALL_PREFIX/bin/cshift.exe" ] && \
   [ ! -d "$INSTALL_PREFIX/share/cshift" ]; then
    _yellow "cshift does not appear to be installed at $INSTALL_PREFIX"
    exit 0
fi

MANIFEST="$INSTALL_PREFIX/share/cshift/.install_manifest"
removed=0
missing=0

if [ -f "$MANIFEST" ]; then
    _bold "Removing files listed in manifest..."
    while IFS= read -r file; do
        [ -z "$file" ] && continue
        if [ -f "$file" ] || [ -L "$file" ]; then
            rm -f "$file"
            echo  "  removed: $file"
            ((removed++)) || true
        else
            ((missing++)) || true
        fi
    done < "$MANIFEST"
    rm -f "$MANIFEST"
else
    _yellow "No manifest found — removing known installation paths..."
    for f in \
        "$INSTALL_PREFIX/bin/cshift" \
        "$INSTALL_PREFIX/bin/cshift.exe"; do
        if [ -f "$f" ]; then
            rm -f "$f"; echo "  removed: $f"; ((removed++)) || true
        fi
    done
    for d in \
        "$INSTALL_PREFIX/share/cshift" \
        "$INSTALL_PREFIX/lib/cshift"; do
        if [ -d "$d" ]; then
            rm -rf "$d"; echo "  removed: $d/"; ((removed++)) || true
        fi
    done
fi

# Prune empty parent directories (don't remove prefix itself)
for d in \
    "$INSTALL_PREFIX/lib/cshift" \
    "$INSTALL_PREFIX/lib" \
    "$INSTALL_PREFIX/share/cshift" \
    "$INSTALL_PREFIX/share" \
    "$INSTALL_PREFIX/bin"; do
    [ -d "$d" ] && [ -z "$(ls -A "$d" 2>/dev/null)" ] && \
        rmdir "$d" 2>/dev/null && echo "  pruned:  $d" || true
done

# ── Clean shell rc files ──────────────────────────────────────────────────────

for RC in "$HOME/.bashrc" "$HOME/.bash_profile" "$HOME/.zshrc" "$HOME/.profile"; do
    [ -f "$RC" ] || continue
    if grep -q 'cshift' "$RC" 2>/dev/null; then
        sed -i.bak \
            -e '/# >>> cshift >>>/,/# <<< cshift <<</d' \
            -e '/CSHIFT_STD_PATH/d' \
            -e '/cshift\/bin/d' \
            "$RC" 2>/dev/null || true
        _yellow "Cleaned cshift entries from $RC"
    fi
done

echo  ""
_bold "============================================"
_green " Uninstall complete  (${removed} item(s) removed, ${missing} already absent)"
_bold "============================================"
echo  ""

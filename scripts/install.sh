#!/usr/bin/env bash
# ============================================================
# C<< (cshift) installer
#
# Usage:
#   ./install.sh                  # installs to /usr/local (needs sudo)
#   ./install.sh ~/opt/cshift     # user-local install, no sudo needed
#   sudo ./install.sh /usr/local
#
# What it does:
#   1. Detects and removes any previous cshift installation at the target.
#   2. Copies bin/, share/, lib/ from the package into the prefix.
#   3. Adds CSHIFT_STD_PATH and PATH entries to the user's shell rc file.
# ============================================================

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────

INSTALL_PREFIX="${1:-/usr/local}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Colour helpers (safe to redirect)
_green()  { printf '\033[32m%s\033[0m\n' "$*"; }
_yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
_red()    { printf '\033[31m%s\033[0m\n' "$*"; }
_bold()   { printf '\033[1m%s\033[0m\n'  "$*"; }

_bold "============================================"
_bold " C<< (cshift) Installer"
_bold "============================================"
echo  " Install prefix : $INSTALL_PREFIX"
echo  " Package source : $SCRIPT_DIR"
echo  ""

# ── Permission check ──────────────────────────────────────────────────────────

if [ ! -w "$INSTALL_PREFIX" ] && [ "$EUID" -ne 0 ]; then
    _red "ERROR: $INSTALL_PREFIX is not writable."
    echo "       Run with sudo, or choose a writable prefix:"
    echo "       $0 \$HOME/.local"
    exit 1
fi

# ── Remove previous installation ──────────────────────────────────────────────

MANIFEST="$INSTALL_PREFIX/share/cshift/.install_manifest"

if [ -f "$MANIFEST" ]; then
    _yellow "Removing previous cshift installation..."
    while IFS= read -r file; do
        [ -z "$file" ] && continue
        if [ -f "$file" ] || [ -L "$file" ]; then
            rm -f "$file"
        fi
    done < "$MANIFEST"
    # Prune empty dirs (deepest first)
    sort -r "$MANIFEST" | while IFS= read -r file; do
        dir="$(dirname "$file")"
        [ -d "$dir" ] && [ -z "$(ls -A "$dir" 2>/dev/null)" ] && rmdir "$dir" 2>/dev/null || true
    done
    rm -f "$MANIFEST"
    _green "Previous installation removed."
elif [ -f "$INSTALL_PREFIX/bin/cshift" ]; then
    # Legacy install without manifest — remove known files only
    _yellow "Removing legacy cshift files (no manifest found)..."
    rm -f  "$INSTALL_PREFIX/bin/cshift"
    rm -rf "$INSTALL_PREFIX/share/cshift"
    rm -rf "$INSTALL_PREFIX/lib/cshift"
    _green "Legacy installation removed."
fi

# ── Install new files ─────────────────────────────────────────────────────────

_bold "Installing..."
NEW_MANIFEST=()

install_file() {
    local src="$1" dst_dir="$2"
    mkdir -p "$dst_dir"
    cp -f "$src" "$dst_dir/"
    dst="$dst_dir/$(basename "$src")"
    NEW_MANIFEST+=("$dst")
    echo "  install: $dst"
}

install_tree() {
    local src_dir="$1" dst_dir="$2"
    if [ -d "$src_dir" ]; then
        mkdir -p "$dst_dir"
        # Use find + install_file so we track every file
        while IFS= read -r f; do
            rel="${f#$src_dir/}"
            d="$dst_dir/$(dirname "$rel")"
            install_file "$f" "$d"
        done < <(find "$src_dir" -type f)
    fi
}

# bin/
if [ -d "$SCRIPT_DIR/bin" ]; then
    for f in "$SCRIPT_DIR/bin/"*; do
        [ -f "$f" ] || continue
        install_file "$f" "$INSTALL_PREFIX/bin"
        chmod +x "$INSTALL_PREFIX/bin/$(basename "$f")"
    done
fi

# share/
install_tree "$SCRIPT_DIR/share" "$INSTALL_PREFIX/share"

# lib/ (bundled libclang etc.)
install_tree "$SCRIPT_DIR/lib"   "$INSTALL_PREFIX/lib"

# Write manifest for future uninstall
mkdir -p "$INSTALL_PREFIX/share/cshift"
printf '%s\n' "${NEW_MANIFEST[@]}" > "$MANIFEST"
echo  ""
_green "Installed ${#NEW_MANIFEST[@]} files."

# ── Shell environment ─────────────────────────────────────────────────────────

CSHIFT_STD_PATH="$INSTALL_PREFIX/share/cshift"
CSHIFT_BIN="$INSTALL_PREFIX/bin"

# Detect the user's shell rc file
if   [ -n "${BASH_VERSION:-}" ] && [ -f "$HOME/.bashrc" ];  then RC="$HOME/.bashrc"
elif [ -n "${ZSH_VERSION:-}"  ] && [ -f "$HOME/.zshrc"  ];  then RC="$HOME/.zshrc"
elif [ -f "$HOME/.zshrc"  ];                                      then RC="$HOME/.zshrc"
elif [ -f "$HOME/.bashrc" ];                                      then RC="$HOME/.bashrc"
elif [ -f "$HOME/.bash_profile" ];                                then RC="$HOME/.bash_profile"
else                                                               RC="$HOME/.profile"
fi

# Remove stale cshift lines from existing rc
if [ -f "$RC" ]; then
    sed -i.bak \
        -e '/# >>> cshift >>>/,/# <<< cshift <<</d' \
        -e '/CSHIFT_STD_PATH/d' \
        -e '/cshift\/bin/d' \
        "$RC" 2>/dev/null || true
fi

# Append new block only if this isn't a system-wide install (where PATH is
# usually set by other means) or if the binary dir isn't already in PATH.
if [[ ":$PATH:" != *":$CSHIFT_BIN:"* ]] || [ ! -v CSHIFT_STD_PATH ]; then
    cat >> "$RC" << ENVBLOCK
# >>> cshift >>>
export CSHIFT_STD_PATH="$CSHIFT_STD_PATH"
export PATH="\$PATH:$CSHIFT_BIN"
# <<< cshift <<<
ENVBLOCK
    echo ""
    _yellow "Added to $RC:"
    echo   "  export CSHIFT_STD_PATH=\"$CSHIFT_STD_PATH\""
    echo   "  export PATH=\"\$PATH:$CSHIFT_BIN\""
    echo   ""
    echo   "Reload your shell or run:"
    _bold  "  source $RC"
fi

echo  ""
_bold "============================================"
_green " cshift installed successfully!"
_bold "============================================"
echo  ""

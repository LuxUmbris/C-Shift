#!/usr/bin/env bash
# ============================================================
# C<< (cshift) installer
#
# Usage:
#   ./install.sh                  # installs to /usr/local (needs sudo)
#   ./install.sh ~/opt/cshift     # user-local install, no sudo needed
#   sudo ./install.sh /usr/local
#
# Flags:
#   --no-deps        skip automatic dependency installation
#   --with-raylib    also install raylib's runtime dependencies
#                     (GL/X11/Wayland dev headers) for compiling examples
#   --jobs=N         parallel build jobs (default: nproc)
#
# What it does:
#   1. Detects the OS package manager and installs build dependencies
#      (cmake, a C/C++ compiler, LLVM + libclang dev packages) if missing.
#   2. Configures and builds cshift from source via CMake.
#   3. Runs `cmake --install` to place files under the chosen prefix.
#   4. Adds CSHIFT_STD_PATH and PATH entries to the user's shell rc file.
# ============================================================

set -euo pipefail

# ── Argument parsing ───────────────────────────────────────────────────────

INSTALL_PREFIX="/usr/local"
SKIP_DEPS=0
WITH_RAYLIB=0
JOBS="$( (command -v nproc >/dev/null 2>&1 && nproc) || echo 4)"

for arg in "$@"; do
    case "$arg" in
        --no-deps)     SKIP_DEPS=1 ;;
        --with-raylib) WITH_RAYLIB=1 ;;
        --jobs=*)      JOBS="${arg#--jobs=}" ;;
        --help|-h)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *) INSTALL_PREFIX="$arg" ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colour helpers (safe to redirect)
_green()  { printf '\033[32m%s\033[0m\n' "$*"; }
_yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
_red()    { printf '\033[31m%s\033[0m\n' "$*"; }
_bold()   { printf '\033[1m%s\033[0m\n'  "$*"; }

_bold "============================================"
_bold " C<< (cshift) Installer"
_bold "============================================"
echo  " Install prefix : $INSTALL_PREFIX"
echo  " Repo source     : $REPO_ROOT"
echo  " Skip deps       : $([ $SKIP_DEPS -eq 1 ] && echo yes || echo no)"
echo  " Install raylib  : $([ $WITH_RAYLIB -eq 1 ] && echo yes || echo no)"
echo  ""

# ── Step 1: Dependency detection & installation ────────────────────────────

_have() { command -v "$1" >/dev/null 2>&1; }

# Use sudo automatically when not root and sudo is available; otherwise
# fall back to running the package-manager command directly (works in
# containers running as root, or if the user already has write access).
SUDO=""
if [ "$EUID" -ne 0 ]; then
    if _have sudo; then SUDO="sudo"; fi
fi

PKG_MANAGER=""
if   _have apt-get; then PKG_MANAGER="apt"
elif _have dnf;     then PKG_MANAGER="dnf"
elif _have yum;     then PKG_MANAGER="yum"
elif _have pacman;  then PKG_MANAGER="pacman"
elif _have apk;     then PKG_MANAGER="apk"
elif _have brew;    then PKG_MANAGER="brew"
elif _have zypper;  then PKG_MANAGER="zypper"
fi

# Returns 0 (true) if something is missing, 1 (false) if everything's there.
deps_missing() {
    local missing=1
    _have cmake               || { _yellow "  missing: cmake";          missing=0; }
    { _have gcc || _have cc; } || { _yellow "  missing: C compiler";     missing=0; }
    { _have g++ || _have clang++; } || { _yellow "  missing: C++ compiler"; missing=0; }
    { _have llvm-config || _have llvm-config-18 || _have llvm-config-17 || _have llvm-config-16; } \
        || { _yellow "  missing: llvm-config (LLVM dev package)"; missing=0; }
    if ! find /usr/include /usr/local/include -maxdepth 3 -iname 'Index.h' -path '*clang-c*' 2>/dev/null | grep -q .; then
        _yellow "  missing: libclang headers (clang-c/Index.h)"
        missing=0
    fi
    [ "$missing" -eq 0 ]
}

install_deps_apt() {
    _bold "Installing dependencies via apt..."
    $SUDO apt-get update -qq
    # Try the versioned LLVM/clang packages first (Ubuntu/Debian ship these
    # as llvm-18, clang-18 etc.); fall back to the generic meta-packages.
    if ! $SUDO apt-get install -y -qq llvm-18 llvm-18-dev libclang-18-dev clang-18 2>/dev/null; then
        _yellow "  versioned LLVM 18 packages unavailable, trying generic packages..."
        $SUDO apt-get install -y -qq llvm llvm-dev libclang-dev clang
    fi
    $SUDO apt-get install -y -qq build-essential cmake pkg-config git zip unzip
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        _bold "Installing raylib build dependencies..."
        $SUDO apt-get install -y -qq \
            libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev \
            libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev
    fi
}

install_deps_dnf() {
    _bold "Installing dependencies via dnf..."
    $SUDO dnf install -y cmake gcc gcc-c++ make llvm llvm-devel clang clang-devel pkgconf git zip unzip
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        $SUDO dnf install -y mesa-libGL-devel libX11-devel libXrandr-devel \
            libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel
    fi
}

install_deps_yum() {
    _bold "Installing dependencies via yum..."
    $SUDO yum install -y cmake gcc gcc-c++ make llvm llvm-devel clang clang-devel pkgconfig git zip unzip
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        $SUDO yum install -y mesa-libGL-devel libX11-devel libXrandr-devel \
            libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel
    fi
}

install_deps_pacman() {
    _bold "Installing dependencies via pacman..."
    $SUDO pacman -Sy --noconfirm --needed cmake gcc make llvm clang pkgconf git zip unzip
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        $SUDO pacman -S --noconfirm --needed mesa libx11 libxrandr libxinerama \
            libxcursor libxi wayland libxkbcommon
    fi
}

install_deps_apk() {
    _bold "Installing dependencies via apk..."
    $SUDO apk add --no-cache cmake build-base llvm-dev clang-dev pkgconf git zip unzip
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        $SUDO apk add --no-cache mesa-dev libx11-dev libxrandr-dev libxinerama-dev \
            libxcursor-dev libxi-dev wayland-dev libxkbcommon-dev
    fi
}

install_deps_brew() {
    _bold "Installing dependencies via Homebrew..."
    brew install cmake llvm pkg-config git
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        _yellow "  raylib on macOS uses system frameworks (Cocoa/OpenGL/IOKit)"
        _yellow "  which are bundled with Xcode — no extra packages needed."
    fi
}

install_deps_zypper() {
    _bold "Installing dependencies via zypper..."
    $SUDO zypper install -y cmake gcc gcc-c++ make llvm llvm-devel clang clang-devel pkg-config git zip unzip
    if [ "$WITH_RAYLIB" -eq 1 ]; then
        $SUDO zypper install -y Mesa-libGL-devel libX11-devel libXrandr-devel \
            libXinerama-devel libXcursor-devel libXi-devel wayland-devel libxkbcommon-devel
    fi
}

if [ "$SKIP_DEPS" -eq 1 ]; then
    _yellow "Skipping dependency installation (--no-deps given)."
else
    _bold "Checking dependencies..."
    if deps_missing; then
        if [ -z "$PKG_MANAGER" ]; then
            _red "ERROR: could not detect a supported package manager"
            _red "       (looked for apt-get, dnf, yum, pacman, apk, brew, zypper)."
            echo  "       Install cmake, a C/C++ compiler, and LLVM+libclang dev"
            echo  "       packages manually, then re-run with --no-deps."
            exit 1
        fi
        _yellow "Some dependencies are missing — installing via $PKG_MANAGER..."
        "install_deps_$PKG_MANAGER"
        echo ""
        _green "Dependencies installed."
    else
        _green "All dependencies already present."
    fi
fi
echo ""

# ── Step 2: Configure & build from source ──────────────────────────────────

_bold "Building cshift (this can take a minute)..."
BUILD_DIR="$REPO_ROOT/build"
mkdir -p "$BUILD_DIR"

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    > "$BUILD_DIR/cmake_configure.log" 2>&1 \
    || { _red "CMake configure failed — see $BUILD_DIR/cmake_configure.log"; tail -n 40 "$BUILD_DIR/cmake_configure.log"; exit 1; }

cmake --build "$BUILD_DIR" -j"$JOBS" \
    > "$BUILD_DIR/cmake_build.log" 2>&1 \
    || { _red "Build failed — see $BUILD_DIR/cmake_build.log"; tail -n 60 "$BUILD_DIR/cmake_build.log"; exit 1; }

_green "Build succeeded."
echo ""

# ── Step 3: Install ─────────────────────────────────────────────────────────

_bold "Installing to $INSTALL_PREFIX ..."

NEED_SUDO_INSTALL=0
if [ -d "$INSTALL_PREFIX" ]; then
    [ ! -w "$INSTALL_PREFIX" ] && [ "$EUID" -ne 0 ] && NEED_SUDO_INSTALL=1
else
    parent="$(dirname "$INSTALL_PREFIX")"
    [ ! -w "$parent" ] && [ "$EUID" -ne 0 ] && NEED_SUDO_INSTALL=1
fi

if [ "$NEED_SUDO_INSTALL" -eq 1 ] && _have sudo; then
    sudo cmake --install "$BUILD_DIR"
elif [ "$NEED_SUDO_INSTALL" -eq 1 ]; then
    _red "ERROR: $INSTALL_PREFIX is not writable and sudo is unavailable."
    echo  "       Re-run with a writable prefix, e.g.:"
    echo  "       $0 \$HOME/.local"
    exit 1
else
    cmake --install "$BUILD_DIR"
fi

echo ""
_green "Installed cshift to $INSTALL_PREFIX."
echo ""

# ── Step 4: Shell environment ───────────────────────────────────────────────

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

echo  ""
_bold "============================================"
_green " cshift installed successfully!"
_bold "============================================"
echo  ""
echo  "Try it:"
echo  "  source $RC"
echo  "  echo 'entry { puts(\"Hello, C<<!\"); }' > hello.cll"
echo  "  cshift hello.cll -c -o hello.o"
echo  "  cc hello.o \$CSHIFT_STD_PATH/frt/native/frt.o -o hello && ./hello"
echo  ""
if [ "$WITH_RAYLIB" -eq 0 ]; then
    echo  "Note: raylib dev headers were not installed (use --with-raylib"
    echo  "      to compile the raylib examples in examples/)."
fi

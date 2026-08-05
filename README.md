# C<< (C-Shift)

![C<< Logo](https://raw.githubusercontent.com/LuxUmbris/C-Shift/main/cll_logo.svg)

[![Build](https://github.com/LuxUmbris/C-Shift/actions/workflows/build.yml/badge.svg)](https://github.com/LuxUmbris/C-Shift/actions/workflows/build.yml)

An LLVM-based compiler for a safe, arena-oriented systems language.
C<< focuses on deterministic, zero-runtime behaviour and direct C-ABI interoperability.

> Disclaimer: This project was made with some help of AI, and is only a syntax experiment for future projects (e. g. [Shaft](https://github.com/LuxUmbris/Shaft/syntax.md))

---

## Quick Start

```bash
# Download and install
gh repo clone LuxUmbris/C-Shift && cd C-Shift/scripts
sudo ./install.sh              # system-wide (/usr/local)
# or:
./install.sh ~/.local          # user-local, no sudo needed
```

```cll
// hello.cll
import std;

entry
{
    puts("Hello, C<< world!");
}
```

```bash
cshift hello.cll -o hello
./hello
```

---

## Building from Source

### Prerequisites

| Platform | Required |
|----------|----------|
| Linux    | `llvm-18-dev`, `libclang-18-dev`, `cmake ≥ 3.16`, `ninja-build` |
| macOS    | `brew install llvm cmake ninja` |
| Windows  | msys2 ucrt64: `mingw-w64-ucrt-x86_64-llvm`, `cmake`, `ninja` |

### Linux / macOS

```bash
./rebuild.sh            # Release build → build/cshift
./rebuild.sh debug      # Debug build
./rebuild.sh clean      # Wipe build dir and rebuild
```

Or manually:

```bash
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCSHIFT_LINK_STATIC=ON          # statically link LLVM (default)
cmake --build build
```

### Windows (msys2 / ucrt64)

Open the **UCRT64** msys2 shell:

```bash
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,llvm,llvm-libs,clang,clang-libs,zlib,zstd}

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
```

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `CSHIFT_LINK_STATIC` | `ON` | Link LLVM + libstdc++ statically — portable binary |
| `CSHIFT_BUNDLE_LIBCLANG` | `ON` | Copy `libclang.so` into the install tree |
| `CSHIFT_PRECOMPILE_FRT` | `ON` | Pre-compile `frt.c` for every available cross-target |
| `CSHIFT_BUNDLE_FRT` | `ON` | Install precompiled `frt.o` blobs |

---

## Compiler Flags

```
Usage: cshift <source.cll> [options]

Options:
  -o <output>          Output file
  -O0 / -O1 / -O2 / -O3   Optimization level (default: -O0)
  -Os / -Oz            Optimize for size / aggressively for size
  --mcpu <cpu>         Target CPU  (e.g. skylake, cortex-a72)
  --mattr <features>   CPU feature flags  (e.g. +avx2,+bmi2)
  --emit-llvm          Emit LLVM IR (.ll)
  --emit-asm           Emit assembly (.s)
  -c                   Compile to object file (.o), skip link
  --target <triple>    Cross-compile target triple
  --no-frt             Skip frt.o linking
  --check-only         Lex + parse + type-check only, no codegen
  -l<lib>              Pass -l<lib> to the linker  (e.g. -lm)
  -Wl,<flag>           Pass raw flag to the linker
  --link-flag <f>      Pass raw linker flag
  --verbose / -v       Print resolution steps to stderr
```

---

## Module System

Modules are real separately-parsed translation units, not text copy-paste.
Each file is parsed exactly once per compilation; duplicate imports are silently deduplicated.

```cll
import std;                   // standard library module
import "path/to/mylib.cll";   // file import (relative to source)
import io::file;              // namespace::module path
```

### C-Header Import

Direct import of C headers via libclang — no hand-written wrapper needed:

```cll
import <stdio.h>;             // system header (angle brackets)
import <math.h>;
import "vendor/mylib.h";      // local header (quoted)
```

This parses the header with libclang and generates the appropriate `import` declarations automatically. Requires `libclang` at build time; falls back to a runtime warning if absent.

### Manual C-ABI Import

For one-off declarations without a header:

```cll
import int32  printf(string fmt, ...);
import voided free(voided* ptr);
import voided* malloc(uint64 size);
```

---

## Standard Library

`import std;` provides 16+ generic containers — all monomorphized at compile time.

| Container | Description |
|-----------|-------------|
| `Vector<T>` | Dynamic array (chunk-based, zero-copy allocation) |
| `HashMap<K,V>` | Hash table with chaining |
| `LinkedList<T>` | Doubly-linked list |
| `Set<T>` | Hash set |
| `Deque<T>` | Double-ended queue (ring buffer) |
| `RingBuffer<T>` | Fixed-size circular buffer |
| `Pool<T>` | Object pool for allocation-free patterns |
| `Pair<A,B>` | Simple tuple |
| `Lazy<T>` | Compute-on-demand caching |
| `BitSet` | Compact bitfield |
| `Guard` | RAII scope guard |
| `StringBuilder` | Chunked string building |
| `Buffer<T>` | Typed buffer with capacity |
| `SortedVec<T>` | Sorted container with custom comparator |

The parsed standard library is cached as `~/.cache/cshift/bc/std.bc` — subsequent builds skip re-parsing.

---

## Install & Uninstall

### Tarball (Linux / macOS)

```bash
# Install (default: /usr/local)
sudo ./install.sh
# or user-local:
./install.sh ~/.local

# Uninstall
sudo ./uninstall.sh
# or:
./uninstall.sh ~/.local
```

The installer:
- Detects and removes any previous cshift installation at the target prefix.
- Records an install manifest (`share/cshift/.install_manifest`) for clean removal.
- Adds `CSHIFT_STD_PATH` and `PATH` entries to your shell rc file.

The uninstaller reads the manifest and removes every installed file, then prunes empty directories, and strips the shell rc entries.

### Windows

```powershell
# User-local (no elevation needed):
.\install.ps1

# System-wide (run PowerShell as Administrator):
.\install.ps1 -InstallDir "C:\Program Files\cshift"

# Uninstall (auto-detects install dir):
.\uninstall.ps1
```

### Debian / Ubuntu package

```bash
# Build the .deb (from the build directory after cmake):
cpack -G DEB

# Install (removes any old version first via preinst):
sudo dpkg -i cshift-*.deb

# Remove:
sudo dpkg -r cshift

# Purge (also removes config and cache):
sudo dpkg --purge cshift
```

### cmake uninstall target

```bash
# After cmake --install:
cmake --build build --target uninstall
```

---

## Language Overview

See [`Syntax_spec.md`](Syntax_spec.md) for the full specification.

### Hello World

```cll
import std;

entry
{
    puts("Hello, C<< world!");
}
```

### Functions and Tunnels

Functions have no return type. Values are passed back via *tunnels*:

```cll
def add(int32 x, int32 y)
{
    tunnel x + y -> int32 result;
}

entry
{
    reserve int32 result = add(5, 7);
    printf("%d\n", result);
}
```

### Structs

```cll
struct Player
{
    int32   id;
    float32 health;
    string  name;
}
```

### Templates

```cll
template<typename T>
struct Pair
{
    T first;
    T second;
}

template<typename T>
def pair_sum(Pair<T>* p)
{
    tunnel p->first + p->second -> T result;
}
```

### VOP (Vertical Ownership Programming)

C<< uses arena-based memory instead of GC or borrow checker:

1. A pointer must only point to a variable whose arena depth ≤ the pointer's depth.
2. Each `{ }` control-flow block is its own arena; when it exits, its variables are destroyed.
3. Functions use tunnels instead of return values.
4. `reset` clears the current arena (forbidden if child arenas hold pointers into it).
5. `move` transfers ownership; the source becomes *voided*. Accessing a potentially-voided variable without a `switch` guard is a compile-time error.

```cll
entry
{
    int32  x = 42;
    int32* p = &x;
    move x;           // x is now voided
    switch (p)
    {
        case valid   { printf("value: %d\n", *p); }
        case voided  { puts("pointer is voided"); }
    }
}
```

### Control Flow

`if` / `else`, `while`, `for`, `foreach`, `switch` / `case` / `default`, `break`, `continue`.

### Raw Strings

```cll
// Delimiter form:
string banner = raw<until "END">
###################
#  Hello, C<< !  #
###################
END

// Line-count form:
puts(raw<2>
Line one — backslashes \n and nulls \0 are literal here.
Line two.
);
```

---

## Philosophy

C<< is designed to be safe without fighting the compiler, and fast without runtime abstractions:

- No garbage collector, no borrow checker — arena-based deterministic memory.
- No exceptions, no virtual dispatch, no hidden allocations.
- Full C-ABI interop: call any C library with zero overhead.
- Cross-compilation to any LLVM-supported target.
- Optimization levels from `-O0` (debug) to `-O3` / `-Os` / `-Oz`.

---

## License

[Apache 2.0](LICENSE)

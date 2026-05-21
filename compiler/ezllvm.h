/*
 * ezllvm.h  —  Easy LLVM, a beginner-friendly wrapper around the LLVM C API
 *              with built-in cross-compilation and linking via lld / ld / cc.
 *
 * Single-header library. Just #include "ezllvm.h" and you're done.
 *
 * Install deps (Ubuntu/Debian):
 *   sudo apt install llvm-dev lld
 *   sudo apt install gcc-aarch64-linux-gnu   # ARM64 cross-linker sysroot
 *   sudo apt install gcc-riscv64-linux-gnu   # RISC-V cross-linker sysroot
 *
 * Install deps (macOS):
 *   brew install llvm                        # lld is bundled
 *
 * Compile your compiler:
 *   cc mycompiler.c $(llvm-config --cflags --ldflags \
 *       --libs core analysis target x86 aarch64 riscv arm) -o mycompiler
 *
 * Or: make example   (see the Makefile)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * MENTAL MODEL
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  EzModule  →  one translation unit / one .ll file
 *  EzFunc    →  a function inside a module
 *  EzBlock   →  a basic block (straight-line code + one terminator)
 *  EzVal     →  any SSA value: constant, function param, or instruction result
 *  EzType    →  a type  (i32, f64, pointer, struct, …)
 *  EzTarget  →  a cross-compilation target  (arch + OS + ABI)
 *
 *  Native build:
 *    EzModule *mod = ez_module("mymod");
 *    // ... build IR ...
 *    ez_compile(mod, "out.o");            // native .o
 *    ez_link_exe(mod, "out.o", "myapp");  // executable via lld/cc
 *    ez_free(mod);
 *
 *  Cross-compilation:
 *    EzTarget *tgt = ez_target_aarch64_linux();
 *    EzModule *mod = ez_module_for_target("mymod", tgt);
 *    // ... build IR exactly the same way ...
 *    ez_compile_for(mod, tgt, "out.o");
 *    ez_link_exe_for(tgt, "out.o", NULL, 0, "myapp");
 *    ez_target_free(tgt);
 *    ez_free(mod);
 *
 * ─────────────────────────────────────────────────────────────────────────────
 */

#ifndef EZLLVM_H
#define EZLLVM_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <llvm-c/Core.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/BitWriter.h>

/* ─── Public type aliases ──────────────────────────────────────────────────── */

/** An LLVM module — one compilation unit. */
typedef struct EzModule  EzModule;

/** An LLVM function inside a module. */
typedef struct EzFunc    EzFunc;

/** A basic block inside a function.
 *  All instructions must end with exactly one terminator (ez_ret, ez_br, …). */
typedef struct EzBlock   EzBlock;

/** Any SSA value: a constant, a function parameter, or an instruction result. */
typedef LLVMValueRef     EzVal;

/** An LLVM type (i32, f64, pointer, struct, …). */
typedef LLVMTypeRef      EzType;

/**
 * EzTarget — describes a cross-compilation target.
 *
 * Fields:
 *   triple      LLVM target triple, e.g. "aarch64-linux-gnu"
 *   cpu         CPU name, e.g. "generic", "cortex-a72", "sifive-u74"
 *   features    CPU feature string, e.g. "+neon" or "" for none
 *   linker      Preferred linker binary name (e.g. "ld.lld", "aarch64-linux-gnu-ld")
 *               Set to "" to auto-detect.
 *   sysroot     Path to target sysroot for cross-linking.
 *               Set to "" for native or bare-metal (no libc).
 *   dynamic_linker
 *               Path to ld-linux / ld.so on the target.
 *               Set to "" for static or bare-metal builds.
 */
typedef struct EzTarget {
    char triple[128];
    char cpu[64];
    char features[256];
    char linker[128];
    char sysroot[512];
    char dynamic_linker[256];
} EzTarget;

/* ─── Internal structs ─────────────────────────────────────────────────────── */

struct EzModule {
    LLVMContextRef  ctx;
    LLVMModuleRef   mod;
    LLVMBuilderRef  builder;
};

struct EzFunc {
    EzModule       *owner;
    LLVMValueRef    fn;
};

struct EzBlock {
    EzFunc         *owner;
    LLVMBasicBlockRef bb;
};

/* ═══════════════════════════════════════════════════════════════════════════
   TARGET PRESETS
   Build an EzTarget for common platforms with one call.
   You can also fill in the struct manually for anything exotic.
   ═══════════════════════════════════════════════════════════════════════════ */

/** Populate a target struct — low-level helper used by the presets below. */
static inline void ez__fill_target(EzTarget *t,
                                    const char *triple,
                                    const char *cpu,
                                    const char *features,
                                    const char *linker,
                                    const char *sysroot,
                                    const char *dynlinker) {
    memset(t, 0, sizeof(*t));
    strncpy(t->triple,         triple,     sizeof(t->triple)     - 1);
    strncpy(t->cpu,            cpu,        sizeof(t->cpu)        - 1);
    strncpy(t->features,       features,   sizeof(t->features)   - 1);
    strncpy(t->linker,         linker,     sizeof(t->linker)     - 1);
    strncpy(t->sysroot,        sysroot,    sizeof(t->sysroot)    - 1);
    strncpy(t->dynamic_linker, dynlinker,  sizeof(t->dynamic_linker) - 1);
}

/**
 * ez_target_native — host machine target (auto-detected).
 * Equivalent to not specifying a target at all.
 * Use this when you want cross-compilation infra but for the local machine.
 */
static inline EzTarget *ez_target_native(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    char *triple = LLVMGetDefaultTargetTriple();
    ez__fill_target(t, triple, "generic", "", "ld.lld", "", "");
    LLVMDisposeMessage(triple);
    return t;
}

/** x86-64 Linux (glibc) — e.g. most desktop/server Linux. */
static inline EzTarget *ez_target_x86_64_linux(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "x86_64-linux-gnu", "x86-64", "",
        "ld.lld",
        "/usr/x86_64-linux-gnu",
        "/lib64/ld-linux-x86-64.so.2");
    return t;
}

/** ARM64 / AArch64 Linux (glibc) — Raspberry Pi 4+, AWS Graviton, etc.
 *  Install cross toolchain:  sudo apt install gcc-aarch64-linux-gnu */
static inline EzTarget *ez_target_aarch64_linux(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "aarch64-linux-gnu", "generic", "",
        "ld.lld",
        "/usr/aarch64-linux-gnu",
        "/lib/ld-linux-aarch64.so.1");
    return t;
}

/** ARM 32-bit Linux hard-float (gnueabihf) — Raspberry Pi 1-3, old phones.
 *  Install cross toolchain:  sudo apt install gcc-arm-linux-gnueabihf */
static inline EzTarget *ez_target_arm32_linux(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "armv7-linux-gnueabihf", "generic", "+vfp3,+neon",
        "ld.lld",
        "/usr/arm-linux-gnueabihf",
        "/lib/ld-linux-armhf.so.3");
    return t;
}

/** RISC-V 64-bit Linux (glibc) — SiFive boards, QEMU, etc.
 *  Install cross toolchain:  sudo apt install gcc-riscv64-linux-gnu */
static inline EzTarget *ez_target_riscv64_linux(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "riscv64-linux-gnu", "generic-rv64", "+m,+a,+f,+d,+c",
        "ld.lld",
        "/usr/riscv64-linux-gnu",
        "/lib/ld-linux-riscv64-lp64d.so.1");
    return t;
}

/** x86-64 macOS (arm64 also runs x86 via Rosetta).
 *  Linking macOS binaries from Linux requires an osxcross sysroot. */
static inline EzTarget *ez_target_x86_64_macos(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "x86_64-apple-macosx10.15.0", "x86-64", "",
        "ld64.lld",
        "",   /* set t->sysroot to your osxcross SDK path */
        "");
    return t;
}

/** ARM64 macOS (Apple Silicon — M1/M2/M3).
 *  Linking from Linux requires an osxcross sysroot. */
static inline EzTarget *ez_target_aarch64_macos(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "aarch64-apple-macosx11.0.0", "apple-a14", "",
        "ld64.lld",
        "",   /* set t->sysroot to your osxcross SDK path */
        "");
    return t;
}

/** x86-64 Windows (MSVC ABI).
 *  Use lld-link or cross-compile with a mingw sysroot. */
static inline EzTarget *ez_target_x86_64_windows(void) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t,
        "x86_64-pc-windows-msvc", "x86-64", "",
        "lld-link",
        "",   /* set t->sysroot to Windows SDK path, or use mingw */
        "");
    return t;
}

/**
 * ez_target_bare_metal — freestanding (no OS, no libc).
 * Good for kernels, bootloaders, microcontrollers.
 *
 * @param triple  e.g. "thumbv7em-none-eabi" for ARM Cortex-M4
 *                     "riscv32-unknown-elf"  for embedded RISC-V
 *                     "x86_64-unknown-none"  for a 64-bit kernel
 */
static inline EzTarget *ez_target_bare_metal(const char *triple,
                                              const char *cpu,
                                              const char *features) {
    EzTarget *t = (EzTarget *)malloc(sizeof(EzTarget));
    assert(t);
    ez__fill_target(t, triple, cpu, features, "ld.lld", "", "");
    return t;
}

/** Free an EzTarget created by any ez_target_* function. */
static inline void ez_target_free(EzTarget *t) { free(t); }

/* ═══════════════════════════════════════════════════════════════════════════
   MODULE
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * ez_module — create a new module for the host (native) target.
 */
static inline EzModule *ez_module(const char *name) {
    EzModule *m = (EzModule *)malloc(sizeof(EzModule));
    assert(m);
    m->ctx     = LLVMContextCreate();
    m->mod     = LLVMModuleCreateWithNameInContext(name, m->ctx);
    m->builder = LLVMCreateBuilderInContext(m->ctx);
    return m;
}

/**
 * ez_module_for_target — create a module pre-configured for a cross target.
 * Sets the target triple and data layout so codegen is correct for that arch.
 * Use this instead of ez_module() when cross-compiling.
 */
static inline EzModule *ez_module_for_target(const char *name, EzTarget *tgt) {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();

    EzModule *m = (EzModule *)malloc(sizeof(EzModule));
    assert(m);
    m->ctx     = LLVMContextCreate();
    m->mod     = LLVMModuleCreateWithNameInContext(name, m->ctx);
    m->builder = LLVMCreateBuilderInContext(m->ctx);

    /* Set triple so LLVM knows pointer sizes, calling conventions, etc. */
    LLVMSetTarget(m->mod, tgt->triple);

    /* Compute the data layout for this triple and bake it in */
    LLVMTargetRef target_ref;
    char *err = NULL;
    if (LLVMGetTargetFromTriple(tgt->triple, &target_ref, &err) == 0) {
        LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
            target_ref, tgt->triple, tgt->cpu, tgt->features,
            LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
        LLVMTargetDataRef dl = LLVMCreateTargetDataLayout(tm);
        LLVMSetModuleDataLayout(m->mod, dl);
        LLVMDisposeTargetMachine(tm);
    } else {
        fprintf(stderr, "ez_module_for_target: unknown triple '%s': %s\n",
                tgt->triple, err ? err : "");
        LLVMDisposeMessage(err);
    }
    return m;
}

/**
 * ez_free — destroy a module and release all LLVM resources.
 */
static inline void ez_free(EzModule *m) {
    LLVMDisposeBuilder(m->builder);
    LLVMDisposeModule(m->mod);
    LLVMContextDispose(m->ctx);
    free(m);
}

/**
 * ez_dump — print the LLVM IR to stdout (great for debugging).
 */
static inline void ez_dump(EzModule *m) {
    LLVMDumpModule(m->mod);
}

/**
 * ez_to_file — write human-readable LLVM IR (.ll) to a file.
 * @return 0 on success, non-zero on error.
 */
static inline int ez_to_file(EzModule *m, const char *path) {
    char *err = NULL;
    if (LLVMPrintModuleToFile(m->mod, path, &err) != 0) {
        fprintf(stderr, "ez_to_file: %s\n", err ? err : "(unknown)");
        LLVMDisposeMessage(err);
        return 1;
    }
    return 0;
}

/**
 * ez_verify — check the IR for consistency errors.
 * Prints problems to stderr. Returns 0 if valid, 1 if broken.
 */
static inline int ez_verify(EzModule *m) {
    char *err = NULL;
    int bad = LLVMVerifyModule(m->mod, LLVMPrintMessageAction, &err);
    if (bad && err) { fprintf(stderr, "ez_verify: %s\n", err); LLVMDisposeMessage(err); }
    return bad;
}

/* ─── Internal: build a TargetMachine from an EzTarget ─────────────────────── */
static inline LLVMTargetMachineRef ez__make_tm(EzTarget *tgt) {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();

    LLVMTargetRef target_ref;
    char *err = NULL;
    if (LLVMGetTargetFromTriple(tgt->triple, &target_ref, &err) != 0) {
        fprintf(stderr, "ez: can't find target '%s': %s\n", tgt->triple, err ? err : "");
        LLVMDisposeMessage(err);
        return NULL;
    }
    return LLVMCreateTargetMachine(
        target_ref, tgt->triple, tgt->cpu, tgt->features,
        LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
}

/* ═══════════════════════════════════════════════════════════════════════════
   COMPILATION  (module → object file)
   ═══════════════════════════════════════════════════════════════════════════ */

/**
 * ez_compile — compile to a native object file for the host machine.
 * @param path  Output path, e.g. "out.o".
 * @return      0 on success, non-zero on error.
 */
static inline int ez_compile(EzModule *m, const char *path) {
    EzTarget *native = ez_target_native();
    LLVMTargetMachineRef tm = ez__make_tm(native);
    ez_target_free(native);
    if (!tm) return 1;

    LLVMSetModuleDataLayout(m->mod, LLVMCreateTargetDataLayout(tm));
    char *err = NULL;
    int rc = LLVMTargetMachineEmitToFile(tm, m->mod, path, LLVMObjectFile, &err);
    if (rc) { fprintf(stderr, "ez_compile: %s\n", err ? err : ""); LLVMDisposeMessage(err); }
    LLVMDisposeTargetMachine(tm);
    return rc;
}

/**
 * ez_compile_for — compile to an object file for a specific target.
 * This is the cross-compilation variant of ez_compile().
 *
 * @param tgt   Target descriptor (from ez_target_aarch64_linux(), etc.).
 * @param path  Output path, e.g. "out-arm64.o".
 * @return      0 on success, non-zero on error.
 *
 * Example:
 *   EzTarget *arm = ez_target_aarch64_linux();
 *   EzModule *mod = ez_module_for_target("app", arm);
 *   // ... build IR ...
 *   ez_compile_for(mod, arm, "app-arm64.o");
 */
static inline int ez_compile_for(EzModule *m, EzTarget *tgt, const char *path) {
    LLVMTargetMachineRef tm = ez__make_tm(tgt);
    if (!tm) return 1;
    LLVMSetTarget(m->mod, tgt->triple);
    LLVMSetModuleDataLayout(m->mod, LLVMCreateTargetDataLayout(tm));
    char *err = NULL;
    int rc = LLVMTargetMachineEmitToFile(tm, m->mod, path, LLVMObjectFile, &err);
    if (rc) { fprintf(stderr, "ez_compile_for: %s\n", err ? err : ""); LLVMDisposeMessage(err); }
    LLVMDisposeTargetMachine(tm);
    return rc;
}

/**
 * ez_compile_asm — emit assembly text instead of an object file.
 * Writes a .s file you can inspect or feed to an assembler.
 * @return 0 on success.
 */
static inline int ez_compile_asm(EzModule *m, const char *path) {
    EzTarget *native = ez_target_native();
    LLVMTargetMachineRef tm = ez__make_tm(native);
    ez_target_free(native);
    if (!tm) return 1;
    char *err = NULL;
    int rc = LLVMTargetMachineEmitToFile(tm, m->mod, path, LLVMAssemblyFile, &err);
    if (rc) { fprintf(stderr, "ez_compile_asm: %s\n", err ? err : ""); LLVMDisposeMessage(err); }
    LLVMDisposeTargetMachine(tm);
    return rc;
}

/**
 * ez_compile_asm_for — emit assembly for a specific target.
 */
static inline int ez_compile_asm_for(EzModule *m, EzTarget *tgt, const char *path) {
    LLVMTargetMachineRef tm = ez__make_tm(tgt);
    if (!tm) return 1;
    LLVMSetTarget(m->mod, tgt->triple);
    LLVMSetModuleDataLayout(m->mod, LLVMCreateTargetDataLayout(tm));
    char *err = NULL;
    int rc = LLVMTargetMachineEmitToFile(tm, m->mod, path, LLVMAssemblyFile, &err);
    if (rc) { fprintf(stderr, "ez_compile_asm_for: %s\n", err ? err : ""); LLVMDisposeMessage(err); }
    LLVMDisposeTargetMachine(tm);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
   LINKING  (object files → executable or shared library)
   Uses lld (via subprocess) so no C++ linker API headaches.
   Falls back to cc if lld is not installed.
   ═══════════════════════════════════════════════════════════════════════════ */

/*
 * WHY SUBPROCESS INSTEAD OF liblld C++ API?
 *
 * lld has no stable C API — only a C++ API that changes every LLVM release.
 * Clang, rustc, and swiftc all invoke the linker as a child process too.
 * Subprocess is the correct, portable approach:
 *   - Works with any installed lld version
 *   - Works with system ld, gold, mold, or cc as fallback
 *   - Zero C++ dependency in ezllvm.h
 *   - Easy to debug: the exact command is printed on error
 */

/* ─── Internal: run a shell command, print it on failure ────────────────────── */
static inline int ez__run(const char *cmd) {
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "ez: linker command failed (exit %d):\n  %s\n", rc, cmd);
    return rc;
}

/* ─── Internal: find a working linker binary ────────────────────────────────── */
static inline int ez__find_linker(const char *preferred, char *out, size_t outsz) {
    /* candidates in preference order */
    const char *candidates[] = {
        preferred[0] ? preferred : NULL,
        "ld.lld", "lld", "ld", "cc", NULL
    };
    char probe[256];
    for (int i = 0; candidates[i]; i++) {
        if (!candidates[i] || !candidates[i][0]) continue;
        snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", candidates[i]);
        if (system(probe) == 0) {
            strncpy(out, candidates[i], outsz - 1);
            out[outsz - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

/**
 * ez_link_exe — link one or more object files into a native executable.
 *
 * Uses lld (ld.lld) if available, falls back to cc.
 * Links against the host libc automatically.
 *
 * @param obj_files  Array of object file paths, e.g. { "main.o", "util.o" }.
 * @param nobj       Number of object files.
 * @param out_path   Output executable path, e.g. "myapp".
 * @param extra_libs Extra -l flags, e.g. { "-lm", "-lpthread" }. May be NULL.
 * @param nlibs      Number of extra libs.
 * @return           0 on success, non-zero on error.
 *
 * Simple example (single object file):
 *   const char *objs[] = { "out.o" };
 *   ez_link_exe(objs, 1, "myapp", NULL, 0);
 *
 * With extra libraries:
 *   const char *objs[]  = { "main.o", "math_utils.o" };
 *   const char *libs[]  = { "-lm" };
 *   ez_link_exe(objs, 2, "myapp", libs, 1);
 */
static inline int ez_link_exe(const char **obj_files, int nobj,
                               const char *out_path,
                               const char **extra_libs, int nlibs) {
    char linker[128];
    if (!ez__find_linker("ld.lld", linker, sizeof(linker))) {
        fprintf(stderr, "ez_link_exe: no linker found. Install lld:  sudo apt install lld\n");
        return 1;
    }

    /* build command string */
    char cmd[4096] = {0};
    int using_cc = (strcmp(linker, "cc") == 0 || strcmp(linker, "gcc") == 0);

    if (using_cc) {
        /* cc handles crt*.o and dynamic linker automatically */
        snprintf(cmd, sizeof(cmd), "%s", linker);
    } else {
        /* lld / ld: we need to specify crt files and dynamic linker ourselves.
         * We use $(cc --print-file-name=...) to locate them portably. */
        char crt1[512], crti[512], crtn[512], libgcc[512], libgcc_s[512];
        FILE *f;
        #define EZ__FIND_CRT(var, name) \
            f = popen("cc --print-file-name=" name " 2>/dev/null", "r"); \
            if (f) { if (!fgets(var, sizeof(var), f)) var[0]='\0'; \
                     size_t _l = strlen(var); if (_l && var[_l-1]=='\n') var[_l-1]='\0'; \
                     pclose(f); } else { var[0]='\0'; }
        EZ__FIND_CRT(crt1,    "crt1.o")
        EZ__FIND_CRT(crti,    "crti.o")
        EZ__FIND_CRT(crtn,    "crtn.o")
        EZ__FIND_CRT(libgcc,  "libgcc.a")
        EZ__FIND_CRT(libgcc_s,"libgcc_s.so.1")
        #undef EZ__FIND_CRT

        char dynld[256] = "/lib64/ld-linux-x86-64.so.2"; /* safe default */
        /* try to detect dynamically */
        FILE *ld = popen("cc -Wl,--verbose 2>&1 | grep 'ld-linux\\|ld-musl' | head -1", "r");
        if (ld) {
            char line[512] = {0};
            if (fgets(line, sizeof(line), ld)) {
                char *s = strstr(line, "/");
                if (s) {
                    char *e = strchr(s, '"');
                    if (!e) e = strchr(s, ' ');
                    if (!e) e = s + strlen(s);
                    size_t len = (size_t)(e - s);
                    if (len > 0 && len < sizeof(dynld)-1) {
                        memcpy(dynld, s, len);
                        dynld[len] = '\0';
                    }
                }
            }
            pclose(ld);
        }

        snprintf(cmd, sizeof(cmd),
            "%s -dynamic-linker %s %s %s",
            linker, dynld, crt1[0] ? crt1 : "", crti[0] ? crti : "");
    }

    /* append object files */
    for (int i = 0; i < nobj; i++) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, obj_files[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    /* append output */
    strncat(cmd, " -o ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, out_path, sizeof(cmd) - strlen(cmd) - 1);

    /* extra libs */
    for (int i = 0; i < nlibs; i++) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, extra_libs[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    /* libc + libgcc always needed for typical programs */
    if (!using_cc) {
        strncat(cmd, " -lc", sizeof(cmd) - strlen(cmd) - 1);
    }

    return ez__run(cmd);
}

/**
 * ez_link_exe_for — cross-link object files into an executable for a target.
 *
 * Uses lld which supports all targets natively — no cross-gcc needed for
 * the linker step. You DO still need the target's sysroot (libc headers/libs)
 * for libc linking. Set tgt->sysroot to your sysroot path.
 *
 * For bare-metal (no libc), set tgt->sysroot = "" and pass a linker script
 * via extra_flags, e.g. extra_flags = { "-T", "linker.ld" }.
 *
 * @param tgt         Target descriptor.
 * @param obj_files   Array of .o file paths.
 * @param nobj        Number of object files.
 * @param extra_flags Extra flags (linker scripts, -T, --entry, etc.). May be NULL.
 * @param nflags      Number of extra flags.
 * @param out_path    Output executable path.
 * @return            0 on success.
 *
 * Examples:
 *
 *   // ARM64 Linux executable:
 *   EzTarget *arm = ez_target_aarch64_linux();
 *   const char *objs[] = { "app-arm64.o" };
 *   ez_link_exe_for(arm, objs, 1, NULL, 0, "app-arm64");
 *
 *   // Bare-metal RISC-V with custom linker script:
 *   EzTarget *rv = ez_target_bare_metal("riscv32-unknown-elf", "generic-rv32", "+m,+a");
 *   const char *flags[] = { "-T", "flash.ld", "--entry", "reset_handler" };
 *   ez_link_exe_for(rv, objs, 1, flags, 4, "firmware.elf");
 */
static inline int ez_link_exe_for(EzTarget *tgt,
                                   const char **obj_files, int nobj,
                                   const char **extra_flags, int nflags,
                                   const char *out_path) {
    /* pick the right lld frontend for the target OS */
    char linker[128];
    const char *preferred = tgt->linker[0] ? tgt->linker : "ld.lld";

    /* map common lld frontends */
    const char *lld_candidates[] = {
        preferred, "ld.lld", "lld", NULL
    };
    int found = 0;
    for (int i = 0; lld_candidates[i]; i++) {
        char probe[256];
        snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", lld_candidates[i]);
        if (system(probe) == 0) {
            strncpy(linker, lld_candidates[i], sizeof(linker)-1);
            found = 1;
            break;
        }
    }
    if (!found) {
        fprintf(stderr,
            "ez_link_exe_for: lld not found. Install with:\n"
            "  sudo apt install lld\n");
        return 1;
    }

    char cmd[8192] = {0};

    /* ld64.lld (macOS/MachO) has a different CLI than ELF lld */
    int is_macho = (strstr(tgt->triple, "apple") != NULL ||
                    strstr(tgt->triple, "macos") != NULL ||
                    strstr(tgt->triple, "darwin") != NULL);
    int is_windows = (strstr(tgt->triple, "windows") != NULL ||
                      strstr(tgt->triple, "msvc")    != NULL);

    if (is_macho) {
        snprintf(cmd, sizeof(cmd), "%s -arch %s",
                 strcmp(linker, "ld.lld") == 0 ? "ld64.lld" : linker,
                 strstr(tgt->triple, "aarch64") ? "arm64" : "x86_64");
        if (tgt->sysroot[0])
            snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                     " -syslibroot %s", tgt->sysroot);
        strncat(cmd, " -lSystem", sizeof(cmd) - strlen(cmd) - 1);

    } else if (is_windows) {
        snprintf(cmd, sizeof(cmd), "lld-link");
        if (tgt->sysroot[0])
            snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                     " /libpath:%s/lib", tgt->sysroot);
        for (int i = 0; i < nobj; i++) {
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, obj_files[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                 " /out:%s", out_path);
        for (int i = 0; i < nflags; i++) {
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
            strncat(cmd, extra_flags[i], sizeof(cmd) - strlen(cmd) - 1);
        }
        return ez__run(cmd);

    } else {
        /* ELF target (Linux, bare-metal) */
        int bare = (tgt->sysroot[0] == '\0' && tgt->dynamic_linker[0] == '\0');

        snprintf(cmd, sizeof(cmd), "%s --target=%s", linker, tgt->triple);

        if (!bare) {
            /* dynamic Linux binary */
            if (tgt->sysroot[0])
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                         " --sysroot=%s", tgt->sysroot);
            if (tgt->dynamic_linker[0])
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                         " -dynamic-linker %s", tgt->dynamic_linker);

            /* crt files from sysroot */
            if (tgt->sysroot[0]) {
                /* try common sysroot layout */
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                    " %s/usr/lib/crt1.o"
                    " %s/usr/lib/crti.o",
                    tgt->sysroot, tgt->sysroot);
            }
        }
    }

    /* append object files */
    for (int i = 0; i < nobj; i++) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, obj_files[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    /* output */
    strncat(cmd, " -o ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, out_path, sizeof(cmd) - strlen(cmd) - 1);

    /* extra flags (linker scripts, --entry, etc.) */
    for (int i = 0; i < nflags; i++) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, extra_flags[i], sizeof(cmd) - strlen(cmd) - 1);
    }

    /* libc for non-bare-metal ELF */
    if (!is_macho && !is_windows) {
        int bare2 = (tgt->sysroot[0] == '\0' && tgt->dynamic_linker[0] == '\0');
        if (!bare2) {
            if (tgt->sysroot[0])
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                    " -L%s/usr/lib -L%s/lib", tgt->sysroot, tgt->sysroot);
            strncat(cmd, " -lc", sizeof(cmd) - strlen(cmd) - 1);
            if (tgt->sysroot[0])
                snprintf(cmd + strlen(cmd), sizeof(cmd) - strlen(cmd),
                    " %s/usr/lib/crtn.o", tgt->sysroot);
        }
    }

    return ez__run(cmd);
}

/**
 * ez_link_shared — link object files into a shared library (.so / .dylib).
 * Native target only. For cross-compilation, use ez_link_exe_for with
 * extra_flags = { "-shared" }.
 *
 * @param obj_files  Object file paths.
 * @param nobj       Number of object files.
 * @param out_path   Output path, e.g. "libmylib.so".
 * @return           0 on success.
 */
static inline int ez_link_shared(const char **obj_files, int nobj,
                                  const char *out_path) {
    char linker[128];
    if (!ez__find_linker("ld.lld", linker, sizeof(linker))) {
        fprintf(stderr, "ez_link_shared: no linker found.\n");
        return 1;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "%s -shared", linker);
    for (int i = 0; i < nobj; i++) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, obj_files[i], sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, " -o ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, out_path, sizeof(cmd) - strlen(cmd) - 1);
    return ez__run(cmd);
}

/**
 * ez_link_static — link object files into a static library (.a) using ar.
 *
 * @param obj_files  Object file paths.
 * @param nobj       Number of object files.
 * @param out_path   Output path, e.g. "libmylib.a".
 * @return           0 on success.
 */
static inline int ez_link_static(const char **obj_files, int nobj,
                                  const char *out_path) {
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "ar rcs %s", out_path);
    for (int i = 0; i < nobj; i++) {
        strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, obj_files[i], sizeof(cmd) - strlen(cmd) - 1);
    }
    return ez__run(cmd);
}

/**
 * ez_build_exe — convenience: compile + link in one call (native target).
 *
 * Compiles the module to a temp object file, links it, then deletes the .o.
 * For a single-module program this is all you need:
 *
 *   ez_build_exe(mod, "myapp", NULL, 0);
 *
 * @param m          Module to compile.
 * @param out_path   Output executable path.
 * @param extra_libs Extra -l flags. May be NULL.
 * @param nlibs      Number of extra libs.
 * @return           0 on success.
 */
static inline int ez_build_exe(EzModule *m, const char *out_path,
                                const char **extra_libs, int nlibs) {
    char obj[512];
    snprintf(obj, sizeof(obj), "%s.ez_tmp.o", out_path);
    if (ez_compile(m, obj) != 0) return 1;
    const char *objs[] = { obj };
    int rc = ez_link_exe(objs, 1, out_path, extra_libs, nlibs);
    remove(obj);
    return rc;
}

/**
 * ez_build_exe_for — convenience: compile + cross-link in one call.
 *
 *   EzTarget *arm = ez_target_aarch64_linux();
 *   EzModule *mod = ez_module_for_target("app", arm);
 *   // ... build IR ...
 *   ez_build_exe_for(mod, arm, "app-arm64", NULL, 0);
 *   ez_target_free(arm);
 *
 * @param m          Module (should have been created with ez_module_for_target).
 * @param tgt        Target.
 * @param out_path   Output executable path.
 * @param extra_flags Extra linker flags. May be NULL.
 * @param nflags     Number of extra flags.
 * @return           0 on success.
 */
static inline int ez_build_exe_for(EzModule *m, EzTarget *tgt,
                                    const char *out_path,
                                    const char **extra_flags, int nflags) {
    char obj[512];
    snprintf(obj, sizeof(obj), "%s.ez_tmp.o", out_path);
    if (ez_compile_for(m, tgt, obj) != 0) return 1;
    const char *objs[] = { obj };
    int rc = ez_link_exe_for(tgt, objs, 1, extra_flags, nflags, out_path);
    remove(obj);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TYPES
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzType *ez_i1(void)   { return LLVMInt1Type();  }
static inline EzType *ez_i8(void)   { return LLVMInt8Type();  }
static inline EzType *ez_i16(void)  { return LLVMInt16Type(); }
static inline EzType *ez_i32(void)  { return LLVMInt32Type(); }
static inline EzType *ez_i64(void)  { return LLVMInt64Type(); }
static inline EzType *ez_f32(void)  { return LLVMFloatType();  }
static inline EzType *ez_f64(void)  { return LLVMDoubleType(); }
static inline EzType *ez_void(void) { return LLVMVoidType(); }
static inline EzType *ez_ptr(void)  { return LLVMPointerType(LLVMInt8Type(), 0); }
static inline EzType *ez_ptr_to(EzType *elem)                         { return LLVMPointerType(elem, 0); }
static inline EzType *ez_array(EzType *elem, unsigned count)          { return LLVMArrayType(elem, count); }
static inline EzType *ez_struct(EzType **fields, unsigned count)      { return LLVMStructType(fields, count, 0); }
static inline EzType *ez_struct_named(EzModule *m, const char *name)  { return LLVMStructCreateNamed(m->ctx, name); }
static inline void    ez_struct_body(EzType *s, EzType **f, unsigned n){ LLVMStructSetBody(s, f, n, 0); }
static inline EzType *ez_func_type(EzType *ret, EzType **p, unsigned n, int v){ return LLVMFunctionType(ret, p, n, v); }

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_const_int(EzType *ty, long long v)            { return LLVMConstInt(ty, (unsigned long long)v, 1); }
static inline EzVal ez_const_uint(EzType *ty, unsigned long long v)  { return LLVMConstInt(ty, v, 0); }
static inline EzVal ez_const_float(EzType *ty, double v)             { return LLVMConstReal(ty, v); }
static inline EzVal ez_const_null(EzType *ty)                        { return LLVMConstNull(ty); }
static inline EzVal ez_const_bool(int v)                             { return LLVMConstInt(LLVMInt1Type(), v ? 1 : 0, 0); }
static inline EzVal ez_global_string(EzModule *m, const char *s, const char *n) {
    return LLVMBuildGlobalStringPtr(m->builder, s, n);
}

/* ═══════════════════════════════════════════════════════════════════════════
   FUNCTIONS & BLOCKS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzFunc *ez_func(EzModule *m, const char *name,
                               EzType *ret, EzType **params, unsigned nparams, int vararg) {
    EzFunc *f = (EzFunc *)malloc(sizeof(EzFunc));
    assert(f);
    f->owner = m;
    f->fn    = LLVMAddFunction(m->mod, name, LLVMFunctionType(ret, params, nparams, vararg));
    return f;
}

static inline EzFunc *ez_extern(EzModule *m, const char *name,
                                 EzType *ret, EzType **params, unsigned nparams, int vararg) {
    EzFunc *f = ez_func(m, name, ret, params, nparams, vararg);
    LLVMSetLinkage(f->fn, LLVMExternalLinkage);
    return f;
}

static inline EzVal  ez_param(EzFunc *f, unsigned i)                   { return LLVMGetParam(f->fn, i); }
static inline void   ez_set_param_name(EzFunc *f, unsigned i, const char *n) {
    LLVMSetValueName2(LLVMGetParam(f->fn, i), n, strlen(n));
}

static inline EzBlock *ez_block(EzFunc *f, const char *name) {
    EzBlock *b = (EzBlock *)malloc(sizeof(EzBlock));
    assert(b);
    b->owner = f;
    b->bb    = LLVMAppendBasicBlockInContext(f->owner->ctx, f->fn, name);
    return b;
}

static inline void ez_use(EzBlock *b) {
    LLVMPositionBuilderAtEnd(b->owner->owner->builder, b->bb);
}

/* ═══════════════════════════════════════════════════════════════════════════
   ARITHMETIC
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_add(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildAdd(m->builder, a, b, n); }
static inline EzVal ez_sub(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildSub(m->builder, a, b, n); }
static inline EzVal ez_mul(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildMul(m->builder, a, b, n); }
static inline EzVal ez_sdiv(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildSDiv(m->builder, a, b, n); }
static inline EzVal ez_udiv(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildUDiv(m->builder, a, b, n); }
static inline EzVal ez_srem(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildSRem(m->builder, a, b, n); }
static inline EzVal ez_urem(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildURem(m->builder, a, b, n); }
static inline EzVal ez_neg(EzModule *m,  EzVal a,          const char *n) { return LLVMBuildNeg(m->builder, a, n); }
static inline EzVal ez_and(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildAnd(m->builder, a, b, n); }
static inline EzVal ez_or(EzModule *m,   EzVal a, EzVal b, const char *n) { return LLVMBuildOr(m->builder, a, b, n); }
static inline EzVal ez_xor(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildXor(m->builder, a, b, n); }
static inline EzVal ez_not(EzModule *m,  EzVal a,          const char *n) { return LLVMBuildNot(m->builder, a, n); }
static inline EzVal ez_shl(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildShl(m->builder, a, b, n); }
static inline EzVal ez_ashr(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildAShr(m->builder, a, b, n); }
static inline EzVal ez_lshr(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildLShr(m->builder, a, b, n); }
static inline EzVal ez_fadd(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildFAdd(m->builder, a, b, n); }
static inline EzVal ez_fsub(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildFSub(m->builder, a, b, n); }
static inline EzVal ez_fmul(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildFMul(m->builder, a, b, n); }
static inline EzVal ez_fdiv(EzModule *m, EzVal a, EzVal b, const char *n) { return LLVMBuildFDiv(m->builder, a, b, n); }
static inline EzVal ez_fneg(EzModule *m, EzVal a,          const char *n) { return LLVMBuildFNeg(m->builder, a, n); }

/* ═══════════════════════════════════════════════════════════════════════════
   COMPARISONS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_eq(EzModule *m,   EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntEQ,  a, b, n); }
static inline EzVal ez_ne(EzModule *m,   EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntNE,  a, b, n); }
static inline EzVal ez_slt(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntSLT, a, b, n); }
static inline EzVal ez_sle(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntSLE, a, b, n); }
static inline EzVal ez_sgt(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntSGT, a, b, n); }
static inline EzVal ez_sge(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntSGE, a, b, n); }
static inline EzVal ez_ult(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntULT, a, b, n); }
static inline EzVal ez_ugt(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildICmp(m->builder, LLVMIntUGT, a, b, n); }
static inline EzVal ez_feq(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildFCmp(m->builder, LLVMRealOEQ, a, b, n); }
static inline EzVal ez_flt(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildFCmp(m->builder, LLVMRealOLT, a, b, n); }
static inline EzVal ez_fgt(EzModule *m,  EzVal a, EzVal b, const char *n) { return LLVMBuildFCmp(m->builder, LLVMRealOGT, a, b, n); }

/* ═══════════════════════════════════════════════════════════════════════════
   CONTROL FLOW
   ═══════════════════════════════════════════════════════════════════════════ */

static inline void ez_ret(EzModule *m, EzVal v)                            { LLVMBuildRet(m->builder, v); }
static inline void ez_ret_void(EzModule *m)                                { LLVMBuildRetVoid(m->builder); }
static inline void ez_br(EzModule *m, EzBlock *dest)                      { LLVMBuildBr(m->builder, dest->bb); }
static inline void ez_cond_br(EzModule *m, EzVal cond,
                                EzBlock *then_b, EzBlock *else_b)          { LLVMBuildCondBr(m->builder, cond, then_b->bb, else_b->bb); }

/* ═══════════════════════════════════════════════════════════════════════════
   MEMORY
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_alloca(EzModule *m, EzType *ty, const char *n)      { return LLVMBuildAlloca(m->builder, ty, n); }
static inline EzVal ez_load(EzModule *m, EzType *ty, EzVal ptr, const char *n) { return LLVMBuildLoad2(m->builder, ty, ptr, n); }
static inline void  ez_store(EzModule *m, EzVal val, EzVal ptr)             { LLVMBuildStore(m->builder, val, ptr); }
static inline EzVal ez_gep(EzModule *m, EzType *ty, EzVal ptr,
                            EzVal *idx, unsigned nidx, const char *n)       { return LLVMBuildGEP2(m->builder, ty, ptr, idx, nidx, n); }

/* ═══════════════════════════════════════════════════════════════════════════
   CALLS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_call(EzModule *m, EzFunc *fn,
                             EzVal *args, unsigned nargs, const char *n) {
    return LLVMBuildCall2(m->builder, LLVMGlobalGetValueType(fn->fn), fn->fn, args, nargs, n);
}

/* ═══════════════════════════════════════════════════════════════════════════
   PHI NODES
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_phi(EzModule *m, EzType *ty, const char *n)         { return LLVMBuildPhi(m->builder, ty, n); }
static inline void  ez_phi_add(EzVal phi, EzVal *vals, EzBlock **blocks, unsigned count) {
    LLVMBasicBlockRef *bbs = (LLVMBasicBlockRef *)malloc(count * sizeof(LLVMBasicBlockRef));
    assert(bbs);
    for (unsigned i = 0; i < count; i++) bbs[i] = blocks[i]->bb;
    LLVMAddIncoming(phi, vals, bbs, count);
    free(bbs);
}

/* ═══════════════════════════════════════════════════════════════════════════
   TYPE CONVERSIONS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_sext(EzModule *m,    EzVal v, EzType *t, const char *n) { return LLVMBuildSExt(m->builder, v, t, n); }
static inline EzVal ez_zext(EzModule *m,    EzVal v, EzType *t, const char *n) { return LLVMBuildZExt(m->builder, v, t, n); }
static inline EzVal ez_trunc(EzModule *m,   EzVal v, EzType *t, const char *n) { return LLVMBuildTrunc(m->builder, v, t, n); }
static inline EzVal ez_sitofp(EzModule *m,  EzVal v, EzType *t, const char *n) { return LLVMBuildSIToFP(m->builder, v, t, n); }
static inline EzVal ez_uitofp(EzModule *m,  EzVal v, EzType *t, const char *n) { return LLVMBuildUIToFP(m->builder, v, t, n); }
static inline EzVal ez_fptosi(EzModule *m,  EzVal v, EzType *t, const char *n) { return LLVMBuildFPToSI(m->builder, v, t, n); }
static inline EzVal ez_fptoui(EzModule *m,  EzVal v, EzType *t, const char *n) { return LLVMBuildFPToUI(m->builder, v, t, n); }
static inline EzVal ez_bitcast(EzModule *m, EzVal v, EzType *t, const char *n) { return LLVMBuildBitCast(m->builder, v, t, n); }
static inline EzVal ez_ptrtoint(EzModule *m,EzVal p, EzType *t, const char *n) { return LLVMBuildPtrToInt(m->builder, p, t, n); }
static inline EzVal ez_inttoptr(EzModule *m,EzVal v, EzType *t, const char *n) { return LLVMBuildIntToPtr(m->builder, v, t, n); }

/* ═══════════════════════════════════════════════════════════════════════════
   MISC
   ═══════════════════════════════════════════════════════════════════════════ */

static inline EzVal ez_select(EzModule *m, EzVal cond, EzVal a, EzVal b, const char *n) {
    return LLVMBuildSelect(m->builder, cond, a, b, n);
}
static inline void ez_unreachable(EzModule *m) { LLVMBuildUnreachable(m->builder); }

/* Escape hatches — direct access to raw LLVM handles */
static inline LLVMValueRef      ez_raw_val(EzVal v)      { return v; }
static inline LLVMModuleRef     ez_raw_mod(EzModule *m)  { return m->mod; }
static inline LLVMContextRef    ez_raw_ctx(EzModule *m)  { return m->ctx; }
static inline LLVMBuilderRef    ez_raw_builder(EzModule *m){ return m->builder; }
static inline LLVMValueRef      ez_raw_fn(EzFunc *f)     { return f->fn; }
static inline LLVMBasicBlockRef ez_raw_bb(EzBlock *b)    { return b->bb; }

#endif /* EZLLVM_H */
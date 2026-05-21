// cshift — C<< (C-Shift) compiler driver
//
// Usage:
//   cshift <source.cll> [-o output] [--emit-llvm] [--emit-asm] [-c]
//          [--target triple] [--no-frt] [--check-only] [--verbose]
//
// Build:
//   c++ -std=c++17 main.cpp \
//       $(llvm-config --cflags --ldflags --libs core analysis target \
//           x86 aarch64 riscv arm all-targets) \
//       -o cshift
//
// Options:
//   -o <output>        Output file name.  Default: source stem + appropriate ext.
//   --emit-llvm        Write LLVM IR (.ll) and exit; skip linking.
//   --emit-asm         Write native assembly (.s) and exit; skip linking.
//   -c                 Compile to object file (.o) only; skip linking.
//   --target <triple>  Cross-compile. Accepted aliases:
//                        x86_64-linux, aarch64-linux, arm32-linux,
//                        riscv64-linux, x86_64-macos, aarch64-macos,
//                        x86_64-windows  (or any raw LLVM triple)
//   --no-frt           Skip automatic frt.o linking (advanced use).
//   --check-only       Run lexer + parser + checker only; no codegen.
//   --verbose / -v     Print internal resolution steps to stderr.

#include "compiler/codegen.hh"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <vector>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define PATH_SEP "\\"
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define PATH_SEP "/"
#endif

// ── Utilities ─────────────────────────────────────────────────────────────────

static std::string read_file(const std::string &path) {
    std::ifstream f(path);
    if (!f) { std::cerr << "[ERROR] Cannot open '" << path << "'\n"; std::exit(1); }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string dirname_of(const std::string &path) {
    auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

static void mkdirp(const std::string &path) {
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/' || path[i] == '\\') {
            MKDIR(path.substr(0, i).c_str());
        }
    }
    MKDIR(path.c_str());
}

// ── std.cll resolution ────────────────────────────────────────────────────────
//
// Search order:
//   1. $CSHIFT_STD_PATH (env override — file or directory)
//   2. Next to the source file being compiled
//   3. Next to the cshift binary
//   4. /usr/local/share/cshift/
//   5. /usr/share/cshift/
//   6. /opt/homebrew/share/cshift/    (macOS Homebrew)
//   7. C:\cshift\                     (Windows)

static std::string find_std_cll(const std::string &src_path,
                                 const std::string &exe_path) {
    const char *env = std::getenv("CSHIFT_STD_PATH");
    if (env) {
        std::string ep(env);
        if (file_exists(ep + "/std.cll")) return ep + "/std.cll";
        if (file_exists(ep))             return ep;
    }
    for (auto &d : std::vector<std::string>{
            dirname_of(src_path),
            dirname_of(exe_path),
            "/usr/local/share/cshift",
            "/usr/share/cshift",
            "/opt/homebrew/share/cshift",
            "C:\\cshift",
        }) {
        std::string c = d + "/std.cll";
        if (file_exists(c)) return c;
    }
    return "";
}

// ── frt.o resolution ──────────────────────────────────────────────────────────
//
// Prebuilt blobs (from cmake install):
//   <exe_dir>/../share/cshift/frt/<triple>/frt.o
//   <exe_dir>/frt/<triple>/frt.o          (dev / portable tree)
//
// On-the-fly compile cache:
//   Linux  : $XDG_CACHE_HOME/cshift/frt/<triple>/frt.o
//   macOS  : ~/Library/Caches/cshift/frt/<triple>/frt.o
//   Windows: %LOCALAPPDATA%\cshift\frt\<triple>\frt.o

static std::string get_frt_cache_base() {
#ifdef _WIN32
    const char *la = std::getenv("LOCALAPPDATA");
    return std::string(la ? la : "C:") + "\\cshift\\frt";
#elif defined(__APPLE__)
    const char *h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/Library/Caches/cshift/frt";
#else
    const char *xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg) return std::string(xdg) + "/cshift/frt";
    const char *h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.cache/cshift/frt";
#endif
}

static std::string compile_frt_to_cache(const std::string &frt_c,
                                         const std::string &triple,
                                         bool verbose) {
    std::string cache_dir  = get_frt_cache_base() + "/" + triple;
    std::string cache_path = cache_dir + "/frt.o";
    if (file_exists(cache_path)) {
        if (verbose) std::cerr << "[frt] using cached " << cache_path << "\n";
        return cache_path;
    }
    mkdirp(cache_dir);

    std::vector<std::string> compilers;
    const char *cc_env = std::getenv("CC");
    if (cc_env) compilers.push_back(cc_env);
    if (!triple.empty() && triple != "native") {
        compilers.push_back(triple + "-gcc");
        compilers.push_back(triple + "-clang");
    }
    for (auto &c : std::vector<std::string>{"cc", "gcc", "clang"})
        compilers.push_back(c);

    for (auto &cc : compilers) {
        std::string cmd = cc + " -O2 -c \"" + frt_c + "\" -o \"" + cache_path + "\" 2>/dev/null";
        if (verbose) std::cerr << "[frt] trying: " << cmd << "\n";
        if (std::system(cmd.c_str()) == 0 && file_exists(cache_path)) {
            if (verbose) std::cerr << "[frt] compiled and cached at " << cache_path << "\n";
            return cache_path;
        }
    }
    return "";
}

static std::string find_frt_o(const std::string &exe_path,
                                const std::string &target_triple,
                                bool verbose, bool no_frt) {
    if (no_frt) return "";
    std::string triple = target_triple.empty() ? "native" : target_triple;

    // 1. Prebuilt blobs (cmake install layout or dev tree)
    for (auto &base : std::vector<std::string>{
            dirname_of(exe_path) + "/../share/cshift/frt",
            dirname_of(exe_path) + "/frt",
        }) {
        std::string p = base + "/" + triple + "/frt.o";
        if (file_exists(p)) {
            if (verbose) std::cerr << "[frt] prebuilt: " << p << "\n";
            return p;
        }
    }

    // 2. Compile from frt.c and cache the result
    for (auto &d : std::vector<std::string>{
            dirname_of(exe_path),
            dirname_of(exe_path) + "/../share/cshift",
            "/usr/local/share/cshift",
            "/usr/share/cshift",
            "/opt/homebrew/share/cshift",
            "C:\\cshift",
        }) {
        std::string fc = d + "/frt.c";
        if (file_exists(fc)) {
            std::string r = compile_frt_to_cache(fc, triple, verbose);
            if (!r.empty()) return r;
        }
    }

    std::cerr << "[WARNING] frt.o not found and could not be compiled.\n"
              << "          Install a C compiler (cc/gcc/clang) to auto-build it,\n"
              << "          or pass --no-frt to suppress this warning.\n";
    return "";
}

// ── import std; expansion ─────────────────────────────────────────────────────
//
// `import std;` is resolved text-level before the lexer runs.
// Duplicate occurrences in the same translation unit are silently dropped.

static std::string expand_std_import(const std::string &source,
                                      const std::string &std_cll_path,
                                      bool verbose) {
    std::string out;
    out.reserve(source.size());
    std::istringstream stream(source);
    std::string line;
    bool inlined = false;

    while (std::getline(stream, line)) {
        // Trim leading whitespace for comparison
        std::string trimmed = line;
        auto first = trimmed.find_first_not_of(" \t\r");
        if (first != std::string::npos) trimmed = trimmed.substr(first);

        if (trimmed == "import std;") {
            if (!inlined) {
                inlined = true;
                if (std_cll_path.empty()) {
                    std::cerr << "[ERROR] 'import std;' used but std.cll was not found.\n"
                              << "        Set CSHIFT_STD_PATH or place std.cll next to your source.\n";
                    std::exit(1);
                }
                if (verbose) std::cerr << "[std] inlining " << std_cll_path << "\n";
                out += "// --- begin import std ---\n";
                out += read_file(std_cll_path);
                out += "\n// --- end import std ---\n";
            }
            // else: duplicate import std; — silently drop
        } else {
            out += line + "\n";
        }
    }
    return out;
}

// ── Target selection ─────────────────────────────────────────────────────────
//
// Accepts friendly aliases as well as raw LLVM triples.

static EzTarget *make_target(const std::string &triple) {
    if (triple == "x86_64-linux"  || triple == "x86_64-linux-gnu")
        return ez_target_x86_64_linux();
    if (triple == "aarch64-linux" || triple == "aarch64-linux-gnu")
        return ez_target_aarch64_linux();
    if (triple == "arm32-linux"   || triple == "arm-linux-gnueabihf")
        return ez_target_arm32_linux();
    if (triple == "riscv64-linux" || triple == "riscv64-linux-gnu")
        return ez_target_riscv64_linux();
    if (triple == "x86_64-macos"  || triple == "x86_64-apple-darwin")
        return ez_target_x86_64_macos();
    if (triple == "aarch64-macos" || triple == "arm64-apple-darwin" || triple == "aarch64-apple-darwin")
        return ez_target_aarch64_macos();
    if (triple == "x86_64-windows"|| triple == "x86_64-w64-mingw32")
        return ez_target_x86_64_windows();

    // Raw triple: use bare-metal preset (no sysroot assumed)
    return ez_target_bare_metal(triple.c_str(), "generic", "");
}

// ── Usage ────────────────────────────────────────────────────────────────────

static void usage(const char *argv0) {
    std::cerr
        << "Usage: " << argv0 << " <source.cll> [options]\n\n"
        << "Options:\n"
        << "  -o <output>        Output file (default: source stem + ext)\n"
        << "  --emit-llvm        Emit LLVM IR (.ll); skip link\n"
        << "  --emit-asm         Emit assembly (.s); skip link\n"
        << "  -c                 Compile to object (.o); skip link\n"
        << "  --target <triple>  Cross-compile target triple or alias\n"
        << "  --no-frt           Skip frt.o linking\n"
        << "  --check-only       Lex + parse + type-check only; no codegen\n"
        << "  --verbose / -v     Verbose output\n";
    std::exit(1);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 2) usage(argv[0]);

    std::string exe_path    = argv[0];
    std::string src_path, out_path, target_triple;
    bool emit_llvm  = false;
    bool emit_asm   = false;
    bool no_link    = false;
    bool no_frt     = false;
    bool check_only = false;
    bool verbose    = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-o"           && i+1 < argc) out_path      = argv[++i];
        else if (a == "--target"     && i+1 < argc) target_triple = argv[++i];
        else if (a == "--emit-llvm")                emit_llvm     = true;
        else if (a == "--emit-asm")                 emit_asm      = true;
        else if (a == "-c")                         no_link       = true;
        else if (a == "--no-frt")                   no_frt        = true;
        else if (a == "--check-only")               check_only    = true;
        else if (a == "--verbose" || a == "-v")     verbose       = true;
        else if (a[0] != '-') {
            if (!src_path.empty()) {
                std::cerr << "[ERROR] Multiple source files not yet supported.\n";
                usage(argv[0]);
            }
            src_path = a;
        } else {
            std::cerr << "[ERROR] Unknown option: " << a << "\n";
            usage(argv[0]);
        }
    }
    if (src_path.empty()) usage(argv[0]);

    // Warn if not a .cll file
    if (src_path.size() < 4 ||
        src_path.compare(src_path.size() - 4, 4, ".cll") != 0) {
        std::cerr << "[WARNING] Source file does not have a .cll extension: "
                  << src_path << "\n";
    }

    // Derive default output path from source stem
    if (out_path.empty()) {
        out_path = src_path;
        auto dot = out_path.rfind('.');
        if (dot != std::string::npos) out_path = out_path.substr(0, dot);
        if      (emit_llvm) out_path += ".ll";
        else if (emit_asm)  out_path += ".s";
        else if (no_link)   out_path += ".o";
        // else: plain executable (no extension on Linux/macOS)
    }

    // ── Front-end ────────────────────────────────────────────────────────────

    std::string std_cll_path = find_std_cll(src_path, exe_path);
    std::string source = expand_std_import(read_file(src_path), std_cll_path, verbose);

    Lexer lexer(source);
    std::vector<Lexer::Token> tokens;
    try { tokens = lexer.tokenize(); }
    catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }

    Parser parser(tokens);
    std::vector<Parser::ASTNode*> ast;
    try { ast = parser.parse_program(); }
    catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }

    Checker checker;
    bool ok = checker.check(ast);
    checker.print_issues(std::cerr);
    if (!ok) {
        for (auto *n : ast) delete n;
        return 1;
    }

    if (check_only) {
        std::cout << "[OK] Semantic check passed.\n";
        for (auto *n : ast) delete n;
        return 0;
    }

    // ── Codegen ──────────────────────────────────────────────────────────────

    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeAllAsmPrinters();

    EzTarget *tgt = nullptr;
    EzModule *mod = nullptr;

    if (!target_triple.empty()) {
        tgt = make_target(target_triple);
        if (!tgt) {
            std::cerr << "[ERROR] Could not create target for: " << target_triple << "\n";
            for (auto *n : ast) delete n;
            return 1;
        }
        mod = ez_module_for_target("cshift", tgt);
    } else {
        mod = ez_module("cshift");
    }

    {
        Codegen cg(mod);
        try { cg.generate(ast); }
        catch (const std::exception &e) {
            std::cerr << "[CODEGEN ERROR] " << e.what() << "\n";
            ez_free(mod); ez_target_free(tgt);
            for (auto *n : ast) delete n;
            return 1;
        }
    }
    for (auto *n : ast) delete n;

    // Verify the generated IR
    char *err_msg = nullptr;
    if (LLVMVerifyModule(mod->module, LLVMReturnStatusAction, &err_msg) != 0) {
        std::cerr << "[IR VERIFY ERROR] " << (err_msg ? err_msg : "unknown") << "\n";
        LLVMDisposeMessage(err_msg);
        ez_free(mod); ez_target_free(tgt);
        return 1;
    }
    LLVMDisposeMessage(err_msg);

    // ── Output ────────────────────────────────────────────────────────────────

    int rc = 0;

    if (emit_llvm) {
        // Write human-readable LLVM IR
        rc = ez_to_file(mod, out_path.c_str());

    } else if (emit_asm) {
        rc = tgt ? ez_compile_asm_for(mod, tgt, out_path.c_str())
                 : ez_compile_asm(mod, out_path.c_str());

    } else if (no_link) {
        rc = tgt ? ez_compile_for(mod, tgt, out_path.c_str())
                 : ez_compile(mod, out_path.c_str());

    } else {
        // Full pipeline: compile → temp .o → link with frt.o → executable
        std::string tmp_obj = out_path + ".ez_tmp.o";

        rc = tgt ? ez_compile_for(mod, tgt, tmp_obj.c_str())
                 : ez_compile(mod, tmp_obj.c_str());

        if (rc == 0) {
            std::string frt_o = find_frt_o(exe_path, target_triple, verbose, no_frt);

            std::vector<const char*> objs;
            objs.push_back(tmp_obj.c_str());
            if (!frt_o.empty()) objs.push_back(frt_o.c_str());

            rc = tgt
                ? ez_link_exe_for(tgt, objs.data(), (int)objs.size(),
                                  nullptr, 0, out_path.c_str())
                : ez_link_exe(objs.data(), (int)objs.size(),
                              out_path.c_str(), nullptr, 0);
        }
        std::remove(tmp_obj.c_str());
    }

    ez_free(mod);
    ez_target_free(tgt);
    return rc;
}

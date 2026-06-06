// cshift — C<< (C-Shift) compiler driver
//
// Usage:
//   cshift <source.cll> [-o output] [--emit-llvm] [--emit-asm] [-c]
//          [--target triple] [--no-frt] [--check-only] [--verbose]
//
// Build:
//   Use CMake (see CMakeLists.txt) or rebuild.sh.
//   Direct: c++ -std=c++17 main.cpp
//     $(llvm-config --cflags --ldflags --libs core analysis target
//       x86 aarch64 riscv arm all-targets passes)
//     [-lclang-18]   # optional - enables C header import
//     -o cshift
//
// Options:
//   -o <output>        Output file name.  Default: source stem + appropriate
//   ext.
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
//   -O0/-O1/-O2/-O3    Optimization level (default: -O0).
//   -Os/-Oz            Optimize for size / aggressively for size.
//   --mcpu <cpu>       Target CPU (e.g. "skylake", "cortex-a72").
//   --mattr <feat>     Target CPU features (e.g. "+avx2,+bmi2").

#include "compiler/codegen.hh"
#include "compiler/module.hh" // must come before codegen.hh

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP "\\"
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#define PATH_SEP "/"
#endif

// ── Utilities
// ─────────────────────────────────────────────────────────────────

static std::string read_file(const std::string &path)
{
  std::ifstream f(path);
  if (!f)
  {
    std::cerr << "[ERROR] Cannot open '" << path << "'\n";
    std::exit(1);
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static bool file_exists(const std::string &path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string dirname_of(const std::string &path)
{
  auto slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

static void mkdirp(const std::string &path)
{
  for (size_t i = 1; i < path.size(); ++i)
  {
    if (path[i] == '/' || path[i] == '\\')
    {
      MKDIR(path.substr(0, i).c_str());
    }
  }
  MKDIR(path.c_str());
}

// ── std.cll resolution
// ────────────────────────────────────────────────────────
//
// Search order:
//   1. $CSHIFT_STD_PATH (env override — file or directory)
//   2. Next to the source file being compiled
//   3. Next to the cshift binary
//   4. /usr/local/share/cshift/
//   5. /usr/share/cshift/
//   6. /opt/homebrew/share/cshift/    (macOS Homebrew)
//   7. C:\cshift\                     (Windows)

static std::string find_std_cll(const std::string &src_path, const std::string &exe_path)
{
  const char *env = std::getenv("CSHIFT_STD_PATH");
  if (env && env[0] != '\0')
  {
    std::string ep(env);

    // Strip trailing slashes so the checks below are uniform.
    while (ep.size() > 1 && (ep.back() == '/' || ep.back() == '\\'))
      ep.pop_back();

    // Case 1: env var points directly to std.cll (any filename is accepted,
    // not just one ending in "std.cll", so the user can rename it).
    if (file_exists(ep))
    {
      // Is it a regular file?
      struct stat st;
      if (stat(ep.c_str(), &st) == 0 && S_ISREG(st.st_mode))
        return ep;

      // Case 2: env var points to a directory — look for std.cll inside it.
      std::string candidate = ep + "/std.cll";
      if (file_exists(candidate))
        return candidate;
    }
    else
    {
      // Path does not exist at all — warn so the user knows the override
      // was seen but is broken, rather than silently falling through.
      std::cerr << "[WARNING] CSHIFT_STD_PATH='" << ep
                << "' does not exist; falling back to default search.\n";
    }
  }

  for (auto &d : std::vector<std::string>{
           dirname_of(src_path),
           dirname_of(exe_path),
           "/usr/local/share/cshift",
           "/usr/share/cshift",
           "/opt/homebrew/share/cshift",
           "C:\\cshift",
       })
  {
    std::string c = d + "/std.cll";
    if (file_exists(c))
      return c;
  }
  return "";
}

// ── frt.o resolution
// ──────────────────────────────────────────────────────────
//
// Prebuilt blobs (from cmake install):
//   <exe_dir>/../share/cshift/frt/<triple>/frt.o
//   <exe_dir>/frt/<triple>/frt.o          (dev / portable tree)
//
// On-the-fly compile cache:
//   Linux  : $XDG_CACHE_HOME/cshift/frt/<triple>/frt.o
//   macOS  : ~/Library/Caches/cshift/frt/<triple>/frt.o
//   Windows: %LOCALAPPDATA%\cshift\frt\<triple>\frt.o

static std::string get_frt_cache_base()
{
#ifdef _WIN32
  const char *la = std::getenv("LOCALAPPDATA");
  return std::string(la ? la : "C:") + "\\cshift\\frt";
#elif defined(__APPLE__)
  const char *h = std::getenv("HOME");
  return std::string(h ? h : "/tmp") + "/Library/Caches/cshift/frt";
#else
  const char *xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg)
    return std::string(xdg) + "/cshift/frt";
  const char *h = std::getenv("HOME");
  return std::string(h ? h : "/tmp") + "/.cache/cshift/frt";
#endif
}

static std::string compile_frt_to_cache(const std::string &frt_c, const std::string &triple,
                                        const std::string &opt_flag, bool verbose)
{
  std::string cache_dir = get_frt_cache_base() + "/" + triple;
  std::string cache_path = cache_dir + "/frt.o";
  if (file_exists(cache_path))
  {
    if (verbose)
      std::cerr << "[frt] using cached " << cache_path << "\n";
    return cache_path;
  }
  mkdirp(cache_dir);

  std::vector<std::string> compilers;
  const char *cc_env = std::getenv("CC");
  if (cc_env)
    compilers.push_back(cc_env);
  if (!triple.empty() && triple != "native")
  {
    compilers.push_back(triple + "-gcc");
    compilers.push_back(triple + "-clang");
  }
  for (auto &c : std::vector<std::string>{"cc", "gcc", "clang"})
    compilers.push_back(c);

  // Shell-safe: wrap path in single quotes for POSIX shell.
  // Embedded single-quotes are replaced with '''
  auto sh_quote = [](const std::string &p) -> std::string
  {
    std::string r(1, char(39));
    for (char c : p)
    {
      if (c == char(39))
      {
        r += char(39);
        r += char(92);
        r += char(39);
        r += char(39);
      }
      else
        r += c;
    }
    r += char(39);
    return r;
  };
  for (auto &cc : compilers)
  {
    // Use the caller's opt level for frt so debug/release builds stay consistent
    std::string cmd = cc + " " + opt_flag + " -c " + sh_quote(frt_c) + " -o " +
                      sh_quote(cache_path) + " 2>/dev/null";
    if (verbose)
      std::cerr << "[frt] trying: " << cmd << "\n";
    if (std::system(cmd.c_str()) == 0 && file_exists(cache_path))
    {
      if (verbose)
        std::cerr << "[frt] compiled and cached at " << cache_path << "\n";
      return cache_path;
    }
  }
  return "";
}

static std::string find_frt_o(const std::string &exe_path, const std::string &target_triple,
                              const std::string &opt_flag, bool verbose, bool no_frt)
{
  if (no_frt)
    return "";
  std::string triple = target_triple.empty() ? "native" : target_triple;

  // 1. Prebuilt blobs (cmake install layout or dev tree)
  for (auto &base : std::vector<std::string>{
           dirname_of(exe_path) + "/../share/cshift/frt",
           dirname_of(exe_path) + "/frt",
       })
  {
    std::string p = base + "/" + triple + "/frt.o";
    if (file_exists(p))
    {
      if (verbose)
        std::cerr << "[frt] prebuilt: " << p << "\n";
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
       })
  {
    std::string fc = d + "/frt.c";
    if (file_exists(fc))
    {
      std::string r = compile_frt_to_cache(fc, triple, opt_flag, verbose);
      if (!r.empty())
        return r;
    }
  }

  std::cerr << "[WARNING] frt.o not found and could not be compiled.\n"
            << "          Install a C compiler (cc/gcc/clang) to auto-build it,\n"
            << "          or pass --no-frt to suppress this warning.\n";
  return "";
}

// expand_std_import() removed — module.hh handles all imports now

// ── Target selection ─────────────────────────────────────────────────────────
//
// Accepts friendly aliases as well as raw LLVM triples.

static EzTarget *make_target(const std::string &triple)
{
  if (triple == "x86_64-linux" || triple == "x86_64-linux-gnu")
    return ez_target_x86_64_linux();
  if (triple == "aarch64-linux" || triple == "aarch64-linux-gnu")
    return ez_target_aarch64_linux();
  if (triple == "arm32-linux" || triple == "arm-linux-gnueabihf")
    return ez_target_arm32_linux();
  if (triple == "riscv64-linux" || triple == "riscv64-linux-gnu")
    return ez_target_riscv64_linux();
  if (triple == "x86_64-macos" || triple == "x86_64-apple-darwin")
    return ez_target_x86_64_macos();
  if (triple == "aarch64-macos" || triple == "arm64-apple-darwin" ||
      triple == "aarch64-apple-darwin")
    return ez_target_aarch64_macos();
  if (triple == "x86_64-windows" || triple == "x86_64-w64-mingw32")
    return ez_target_x86_64_windows();

  // Raw triple: use bare-metal preset (no sysroot assumed)
  return ez_target_bare_metal(triple.c_str(), "generic", "");
}

// ── Usage ────────────────────────────────────────────────────────────────────

static void usage(const char *argv0)
{
  std::cerr << "Usage: " << argv0 << " <source.cll> [options]\n\n"
            << "Options:\n"
            << "  -o <output>        Output file (default: source stem + ext)\n"
            << "  --emit-llvm        Emit LLVM IR (.ll); skip link\n"
            << "  --emit-asm         Emit assembly (.s); skip link\n"
            << "  -c                 Compile to object (.o); skip link\n"
            << "  --target <triple>  Cross-compile target triple or alias\n"
            << "  --no-frt           Skip frt.o linking\n"
            << "  --check-only       Lex + parse + type-check only; no codegen\n"
            << "  --verbose / -v     Verbose output\n"
            << "  -O0/-O1/-O2/-O3    Optimization level (default: -O0)\n"
            << "  -Os                Optimize for size\n"
            << "  -Oz                Optimize aggressively for size\n"
            << "  --mcpu <cpu>       Target CPU name (e.g. skylake, cortex-a72)\n"
            << "  --mattr <feat>     CPU feature flags (e.g. +avx2,+bmi2)\n"
            << "  -l<lib>            Pass -l<lib> to the linker (e.g. -lm)\n"
            << "  -Wl,<flag>         Pass <flag> through to the linker\n"
            << "  --link-flag <f>    Pass raw linker flag <f>\n";
  std::exit(1);
}

// ── main
// ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
  if (argc < 2)
    usage(argv[0]);

  std::string exe_path = argv[0];
  std::string src_path, out_path, target_triple;
  std::string mcpu, mattr;
  bool emit_llvm = false;
  bool emit_asm = false;
  bool no_link = false;
  bool no_frt = false;
  bool check_only = false;
  bool verbose = false;
  std::vector<std::string> linker_flags_storage;
  std::vector<std::string> extra_include_dirs_storage;

  // Optimization level: 0=none(debug), 1=less, 2=default, 3=aggressive
  // Stored as the LLVM pipeline string for LLVMRunPasses.
  // Also controls the TargetMachine CodeGenOptLevel.
  int opt_int = 0;                                   // numeric level (0-3)
  bool opt_size = false;                             // -Os
  bool opt_size_z = false;                           // -Oz
  LLVMCodeGenOptLevel tm_opt = LLVMCodeGenLevelNone; // for TargetMachine

  for (int i = 1; i < argc; ++i)
  {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc)
      out_path = argv[++i];
    else if (a == "--target" && i + 1 < argc)
      target_triple = argv[++i];
    else if (a == "--emit-llvm")
      emit_llvm = true;
    else if (a == "--emit-asm")
      emit_asm = true;
    else if (a == "-c")
      no_link = true;
    else if (a == "--no-frt")
      no_frt = true;
    else if (a == "--check-only")
      check_only = true;
    else if (a == "--verbose" || a == "-v")
      verbose = true;
    else if (a == "-O0")
    {
      opt_int = 0;
      tm_opt = LLVMCodeGenLevelNone;
      opt_size = opt_size_z = false;
    }
    else if (a == "-O1")
    {
      opt_int = 1;
      tm_opt = LLVMCodeGenLevelLess;
      opt_size = opt_size_z = false;
    }
    else if (a == "-O2")
    {
      opt_int = 2;
      tm_opt = LLVMCodeGenLevelDefault;
      opt_size = opt_size_z = false;
    }
    else if (a == "-O3")
    {
      opt_int = 3;
      tm_opt = LLVMCodeGenLevelAggressive;
      opt_size = opt_size_z = false;
    }
    else if (a == "-Os")
    {
      opt_int = 2;
      tm_opt = LLVMCodeGenLevelDefault;
      opt_size = true;
      opt_size_z = false;
    }
    else if (a == "-Oz")
    {
      opt_int = 2;
      tm_opt = LLVMCodeGenLevelDefault;
      opt_size_z = true;
      opt_size = false;
    }
    else if (a == "--mcpu" && i + 1 < argc)
      mcpu = argv[++i];
    else if (a.size() > 7 && a.substr(0, 7) == "--mcpu=")
      mcpu = a.substr(7);
    else if (a == "--mattr" && i + 1 < argc)
      mattr = argv[++i];
    else if (a.size() > 8 && a.substr(0, 8) == "--mattr=")
      mattr = a.substr(8);
    else if (a.size() >= 2 && a[0] == '-' && a[1] == 'l')
    {
      // -lname  or  -l name
      if (a.size() > 2)
        linker_flags_storage.push_back(a);
      else if (i + 1 < argc)
        linker_flags_storage.push_back("-l" + std::string(argv[++i]));
    }
    else if (a.size() >= 4 && a.substr(0, 4) == "-Wl,")
      linker_flags_storage.push_back(a);
    else if (a == "--link-flag" && i + 1 < argc)
      linker_flags_storage.push_back(argv[++i]);
    else if (a.size() >= 2 && a[0] == '-' && a[1] == 'I')
    {
      if (a.size() > 2)
        extra_include_dirs_storage.push_back(a.substr(2));
      else if (i + 1 < argc)
        extra_include_dirs_storage.push_back(argv[++i]);
    }
    else if (a[0] != '-')
    {
      if (!src_path.empty())
      {
        std::cerr << "[ERROR] Multiple source files not yet supported.\n";
        usage(argv[0]);
      }
      src_path = a;
    }
    else
    {
      std::cerr << "[ERROR] Unknown option: " << a << "\n";
      usage(argv[0]);
    }
  }
  if (src_path.empty())
    usage(argv[0]);

  // Build the compiler opt flag string for frt.c compilation
  std::string opt_flag;
  if (opt_size_z)
    opt_flag = "-Oz";
  else if (opt_size)
    opt_flag = "-Os";
  else if (opt_int == 3)
    opt_flag = "-O3";
  else if (opt_int == 2)
    opt_flag = "-O2";
  else if (opt_int == 1)
    opt_flag = "-O1";
  else
    opt_flag = "-O0";

  // Warn if not a .cll file
  if (src_path.size() < 4 || src_path.compare(src_path.size() - 4, 4, ".cll") != 0)
  {
    std::cerr << "[WARNING] Source file does not have a .cll extension: " << src_path << "\n";
  }

  // Derive default output path from source stem
  if (out_path.empty())
  {
    out_path = src_path;
    auto dot = out_path.rfind('.');
    if (dot != std::string::npos)
      out_path = out_path.substr(0, dot);
    if (emit_llvm)
      out_path += ".ll";
    else if (emit_asm)
      out_path += ".s";
    else if (no_link)
      out_path += ".o";
    // else: plain executable (no extension on Linux/macOS)
  }

  // ── Front-end ────────────────────────────────────────────────────────────

  // ── Module loader setup ─────────────────────────────────────────────────
  ModuleLoader loader;
  loader.verbose = verbose;
  loader.std_cll_override = find_std_cll(src_path, exe_path);
  if (verbose && !loader.std_cll_override.empty())
    std::cerr << "[std] resolved std.cll -> " << loader.std_cll_override << "\n";
  else if (verbose)
    std::cerr << "[std] std.cll not found in any search path\n";
  // Search directories: next to source, next to binary, system paths
  for (auto &d : std::vector<std::string>{
           dirname_of(src_path),
           dirname_of(exe_path),
           "/usr/local/share/cshift",
           "/usr/share/cshift",
           "/opt/homebrew/share/cshift",
       })
    loader.search_dirs.push_back(d);
  loader.extra_include_dirs = extra_include_dirs_storage;

  // ── Lex + parse the main source ─────────────────────────────────────────
  Lexer lexer(read_file(src_path));
  std::vector<Lexer::Token> tokens;
  try
  {
    tokens = lexer.tokenize();
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << "\n";
    return 1;
  }

  Parser parser(tokens);
  std::vector<Parser::ASTNode *> ast;
  try
  {
    ast = parser.parse_program();
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << "\n";
    return 1;
  }

  // ── Resolve all imports (modules, files, C headers) ─────────────────────
  // This replaces the old text-level expand_std_import() hack.
  // Each module is parsed exactly once; duplicate imports are silently deduped.
  //
  // OWNERSHIP: resolved.owned = nodes from this file (caller deletes)
  //            resolved.borrowed = nodes from modules (loader deletes at scope end)
  //            resolved.all = combined view for checker/codegen
  ResolvedAST resolved = resolve_all_imports(ast, loader, src_path, verbose);
  // From here on use resolved.all for processing and resolved.owned for cleanup.
  auto &ast_view = resolved.all;

  Checker checker;
  bool ok = checker.check(ast_view);
  checker.print_issues(std::cerr);
  if (!ok)
  {
    for (auto *n : resolved.owned)
      delete n;
    return 1;
  }

  if (check_only)
  {
    std::cout << "[OK] Semantic check passed.\n";
    for (auto *n : resolved.owned)
      delete n;
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

  if (!target_triple.empty())
  {
    tgt = make_target(target_triple);
    if (!tgt)
    {
      std::cerr << "[ERROR] Could not create target for: " << target_triple << "\n";
      for (auto *n : resolved.owned)
        delete n;
      return 1;
    }
    // Apply --mcpu / --mattr overrides
    if (!mcpu.empty())
      snprintf(tgt->cpu, sizeof(tgt->cpu), "%s", mcpu.c_str());
    if (!mattr.empty())
      snprintf(tgt->features, sizeof(tgt->features), "%s", mattr.c_str());
    mod = ez_module_for_target("cshift", tgt);
  }
  else
  {
    // Native target — apply mcpu/mattr if given
    if (!mcpu.empty() || !mattr.empty())
    {
      tgt = ez_target_native();
      if (!mcpu.empty())
        snprintf(tgt->cpu, sizeof(tgt->cpu), "%s", mcpu.c_str());
      if (!mattr.empty())
        snprintf(tgt->features, sizeof(tgt->features), "%s", mattr.c_str());
      mod = ez_module_for_target("cshift", tgt);
    }
    else
    {
      mod = ez_module("cshift");
    }
  }

  {
    Codegen cg(mod);
    try
    {
      cg.generate(ast_view);
    }
    catch (const std::exception &e)
    {
      std::cerr << "[CODEGEN ERROR] " << e.what() << "\n";
      ez_free(mod);
      ez_target_free(tgt);
      for (auto *n : resolved.owned)
        delete n;
      return 1;
    }
  }
  for (auto *n : resolved.owned)
    delete n;

  // ── Optimization pass ───────────────────────────────────────────────────
  // Build the pass pipeline string based on -O flags and run it.
  // We always run at least -O0 (which still canonicalizes the IR).
  {
    std::string pipeline;
    if (opt_size_z)
      pipeline = "default<Oz>";
    else if (opt_size)
      pipeline = "default<Os>";
    else if (opt_int == 3)
      pipeline = "default<O3>";
    else if (opt_int == 2)
      pipeline = "default<O2>";
    else if (opt_int == 1)
      pipeline = "default<O1>";
    else
      pipeline = "default<O0>";

    // Build a temporary TM just for the pass manager
    // (needed for target-specific passes like loop-vectorizer cost models)
    EzTarget *opt_tgt = tgt ? tgt : ez_target_native();
    LLVMTargetMachineRef opt_tm = ez__make_tm(opt_tgt, tm_opt);
    if (opt_tm)
    {
      if (verbose)
        std::cerr << "[opt] running " << pipeline << "\n";
      if (ez_optimize(mod, opt_tm, pipeline.c_str()) != 0)
        std::cerr << "[WARNING] optimization pass failed (continuing)\n";
      LLVMDisposeTargetMachine(opt_tm);
    }
    if (!tgt)
      ez_target_free(opt_tgt);
  }

  // Verify the generated IR
  char *err_msg = nullptr;
  if (LLVMVerifyModule(mod->mod, LLVMReturnStatusAction, &err_msg) != 0)
  {
    std::cerr << "[IR VERIFY ERROR] " << (err_msg ? err_msg : "unknown") << "\n";
    LLVMDisposeMessage(err_msg);
    ez_free(mod);
    ez_target_free(tgt);
    return 1;
  }
  LLVMDisposeMessage(err_msg);

  // ── std.bc cache write ────────────────────────────────────────────────────
  // If this compilation imported std (or any other module that has a bc_cache
  // path), write the compiled LLVM IR for that module to disk so the next
  // compilation can skip re-parsing.  We write from the already-compiled main
  // module, but only the nodes that originated from the std module are
  // represented there — since all modules are merged into one LLVM module we
  // instead cache the whole TU only when it *is* the std module (i.e. when
  // compiling std.cll directly).  For the common case of caching parsed+checked
  // AST nodes, the module loader already avoids re-parsing within one
  // invocation; the bitcode cache speeds up *cross-invocation* std loading by
  // pre-compiling std into a linkable .bc file that the linker can consume.
  {
    std::string std_path = loader.std_cll_override.empty() ? loader.resolve_module_name("std")
                                                           : loader.std_cll_override;
    if (!std_path.empty() && loader.is_loaded(std_path))
    {
      std::string bcp = ModuleLoader::bc_path_for(std_path);
      // Only (re)write if stale or missing
      if (!ModuleLoader::bc_cache_fresh(std_path))
      {
#ifdef CSHIFT_HAVE_LLVM_BITCODE
        // Write the full module — std symbols are embedded in it
        ModuleLoader::ensure_bc_dir();
        if (LLVMWriteBitcodeToFile(mod->mod, bcp.c_str()) == 0)
        {
          if (verbose)
            std::cerr << "[cache] wrote std.bc -> " << bcp << "\n";
        }
        else if (verbose)
          std::cerr << "[cache] WARNING: could not write std.bc cache\n";
#endif
      }
      else if (verbose)
        std::cerr << "[cache] std.bc is fresh: " << bcp << "\n";
    }
  }

  // ── Output ────────────────────────────────────────────────────────────────

  int rc = 0;

  if (emit_llvm)
  {
    // Write human-readable LLVM IR
    rc = ez_to_file(mod, out_path.c_str());
  }
  else if (emit_asm)
  {
    rc = tgt ? ez_compile_asm_for(mod, tgt, out_path.c_str(), tm_opt)
             : ez_compile_asm(mod, out_path.c_str(), tm_opt);
  }
  else if (no_link)
  {
    rc = tgt ? ez_compile_for(mod, tgt, out_path.c_str(), tm_opt)
             : ez_compile(mod, out_path.c_str(), tm_opt);
  }
  else
  {
    // Full pipeline: compile → temp .o → link with frt.o → executable
    std::string tmp_obj = out_path + ".ez_tmp.o";

    rc = tgt ? ez_compile_for(mod, tgt, tmp_obj.c_str(), tm_opt)
             : ez_compile(mod, tmp_obj.c_str(), tm_opt);

    if (rc == 0)
    {
      std::string frt_o = find_frt_o(exe_path, target_triple, opt_flag, verbose, no_frt);

      std::vector<const char *> objs;
      objs.push_back(tmp_obj.c_str());
      if (!frt_o.empty())
        objs.push_back(frt_o.c_str());

      std::vector<const char *> lflags;
      for (auto &f : linker_flags_storage)
        lflags.push_back(f.c_str());

      rc = tgt ? ez_link_exe_for(tgt, objs.data(), (int)objs.size(),
                                 lflags.empty() ? nullptr : lflags.data(), (int)lflags.size(),
                                 out_path.c_str())
               : ez_link_exe(objs.data(), (int)objs.size(), out_path.c_str(),
                             lflags.empty() ? nullptr : lflags.data(), (int)lflags.size());
    }
    std::remove(tmp_obj.c_str());
  }

  ez_free(mod);
  ez_target_free(tgt);
  return rc;
}

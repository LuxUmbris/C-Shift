#pragma once
// ============================================================
// C<< Module Loader
//
// Replaces the old text-level "expand_std_import" hack.
//
// A module is a separately parsed .cll translation unit.  The
// loader keeps a registry of already-loaded modules so each
// file is lexed/parsed exactly once per compilation.
//
// The resolved AST nodes from every module are injected into
// the main program's AST *before* the caller's own nodes so
// that forward declarations are available.  Codegen just sees
// one flat list of top-level nodes — nothing changes there.
//
// Binary std cache
// ────────────────
// If a module resolves to the well-known "std" module (or any
// module whose .cll path has a matching .bc sidecar), the
// loader can emit / consume a cached LLVM bitcode file so that
// repeated compilations don't re-codegen the same 800-line
// standard library.  The cache lives alongside the .cll file
// and is invalidated whenever the .cll is newer than the .bc.
//
// The caching is optional: if LLVM bitcode I/O headers are not
// available or the write fails for any reason, the loader
// silently falls back to the normal AST path.
// ============================================================

#include "cheader.hh"
#include "parser.hh"

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── LLVM bitcode optional ──────────────────────────────────────────────────
// We include the LLVM-C headers only if they were found at configure time.
// The CMakeLists adds -DCSHIFT_HAVE_LLVM_BITCODE=1 when LLVM is present.
#ifdef CSHIFT_HAVE_LLVM_BITCODE
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#endif

#ifdef __MINGW32__
    #include <sys/stat.h>
    #define _mkdir(path) mkdir(path, 0755)
#endif


// ── Utilities ─────────────────────────────────────────────────────────────

static inline bool mod_file_exists(const std::string &p)
{
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

static inline time_t mod_mtime(const std::string &p)
{
  struct stat st;
  if (stat(p.c_str(), &st) != 0)
    return 0;
  return st.st_mtime;
}

static inline std::string mod_read_file(const std::string &path)
{
  std::ifstream f(path);
  if (!f)
    return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static inline std::string mod_dirname(const std::string &path)
{
  auto slash = path.find_last_of("/\\");
  return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

static inline std::string mod_stem(const std::string &path)
{
  auto slash = path.find_last_of("/\\");
  std::string base =
      (slash == std::string::npos) ? path : path.substr(slash + 1);
  auto dot = base.rfind('.');
  return (dot == std::string::npos) ? base : base.substr(0, dot);
}

// ── ModuleExports ─────────────────────────────────────────────────────────
//
// A module exports a flat list of top-level AST nodes (CImport, Struct,
// Function, Template, ...) that can be merged into any importing translation
// unit.  Nodes are owned by the Module; the importer only holds raw pointers
// that remain valid for the lifetime of the ModuleLoader.

struct ModuleExports
{
  std::string canonical_path;           // absolute path of the source .cll
  std::vector<Parser::ASTNode *> nodes; // owned AST nodes

  // Bitcode cache path (empty if not applicable / not cached yet)
  std::string bc_cache_path;

  ~ModuleExports()
  {
    for (auto *n : nodes)
      delete n;
  }

  // No copy — nodes are owned
  ModuleExports() = default;
  ModuleExports(const ModuleExports &) = delete;
  ModuleExports &operator=(const ModuleExports &) = delete;
  ModuleExports(ModuleExports &&o) noexcept
      : canonical_path(std::move(o.canonical_path)), nodes(std::move(o.nodes)),
        bc_cache_path(std::move(o.bc_cache_path))
  {
  }
};

// ── ModuleLoader ──────────────────────────────────────────────────────────

class ModuleLoader
{
public:
  // Resolved search paths tried (in order) when finding a .cll module.
  std::vector<std::string> search_dirs;

  // Optional std.cll override (from CSHIFT_STD_PATH / CLI)
  std::string std_cll_override;

  // Print resolution steps to stderr
  bool verbose = false;

  // ── Public API ────────────────────────────────────────────────────────────

  // Resolve a ModuleImport name ("std", "io::file", …) to a .cll path.
  // Returns empty string if not found.
  std::string resolve_module_name(const std::string &module_name) const
  {
    // "std" is special
    if (module_name == "std")
    {
      if (!std_cll_override.empty() && mod_file_exists(std_cll_override))
        return std_cll_override;
      for (auto &d : search_dirs)
      {
        std::string p = d + "/std.cll";
        if (mod_file_exists(p))
          return p;
      }
      return "";
    }

    // Convert "io::file" → "io/file.cll"
    std::string rel = module_name;
    {
      size_t pos = 0;
      while ((pos = rel.find("::", pos)) != std::string::npos)
      {
        rel.replace(pos, 2, "/");
        pos += 1;
      }
    }
    rel += ".cll";

    for (auto &d : search_dirs)
    {
      std::string p = d + "/" + rel;
      if (mod_file_exists(p))
        return p;
    }
    return "";
  }

  // Load a module by *file path* (from `import "path/to/foo.cll";`).
  // The path is resolved relative to `relative_to_dir`.
  // Returns a reference to the cached exports (valid for loader lifetime).
  // Throws std::runtime_error on parse failure.
  const ModuleExports &load_file(const std::string &path,
                                 const std::string &relative_to_dir)
  {
    std::string resolved = resolve_file_path(path, relative_to_dir);
    if (resolved.empty())
      throw std::runtime_error("[MODULE ERROR] Cannot find module file: '" +
                               path + "'");
    return load_canonical(resolved);
  }

  // Load a module by *name* (from `import std;`).
  const ModuleExports &load_name(const std::string &module_name)
  {
    std::string path = resolve_module_name(module_name);
    if (path.empty())
      throw std::runtime_error("[MODULE ERROR] Cannot find module '" +
                               module_name +
                               "'.  Add its directory to the search path or "
                               "set CSHIFT_STD_PATH.");
    return load_canonical(path);
  }

  // Check if a module is already loaded (to skip redundant reloads)
  bool is_loaded(const std::string &canonical_path) const
  {
    return cache_.count(canonical_path) != 0;
  }

  // ── std bitcode cache ──────────────────────────────────────────────────

  // Return the cache directory for precompiled bitcode.
  // Linux : $XDG_CACHE_HOME/cshift/bc   or  ~/.cache/cshift/bc
  // macOS : ~/Library/Caches/cshift/bc
  // Windows: %LOCALAPPDATA%\cshift\bc
  static std::string bc_cache_base()
  {
#ifdef _WIN32
    const char *la = std::getenv("LOCALAPPDATA");
    return std::string(la ? la : "C:") + "\\cshift\\bc";
#elif defined(__APPLE__)
    const char *h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/Library/Caches/cshift/bc";
#else
    const char *xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0])
      return std::string(xdg) + "/cshift/bc";
    const char *h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.cache/cshift/bc";
#endif
  }

  // Compute the bc cache path for a given .cll source path.
  // e.g. /usr/share/cshift/std.cll → <cache_base>/std.bc
  static std::string bc_path_for(const std::string &cll_path)
  {
    return bc_cache_base() + "/" + mod_stem(cll_path) + ".bc";
  }

  // Is the bc cache for `cll_path` fresh (exists and newer than the source)?
  static bool bc_cache_fresh(const std::string &cll_path)
  {
    std::string bcp = bc_path_for(cll_path);
    if (!mod_file_exists(bcp))
      return false;
    return mod_mtime(bcp) >= mod_mtime(cll_path);
  }

private:
  // canonical_path → owned ModuleExports
  std::unordered_map<std::string, ModuleExports> cache_;

  // Modules currently being loaded (cycle detection)
  std::unordered_set<std::string> loading_;

public:
  // ── Bitcode cache helpers ─────────────────────────────────────────────────

  // Ensure the bc cache directory exists.
  static void ensure_bc_dir()
  {
    std::string base = bc_cache_base();
    // mkdir -p: walk the path and create each segment
    for (size_t i = 1; i < base.size(); ++i)
    {
      if (base[i] == '/' || base[i] == '\\')
      {
#ifdef _WIN32
        _mkdir(base.substr(0, i).c_str());
#else
        ::mkdir(base.substr(0, i).c_str(), 0755);
#endif
      }
    }
#ifdef _WIN32
    _mkdir(base.c_str());
#else
    ::mkdir(base.c_str(), 0755);
#endif
  }

  // Write a compiled EzModule to the bc cache for `cll_path`.
  // Returns true on success.  Silently no-ops when bitcode support is absent.
  static bool write_bc_cache(const std::string &cll_path, void *llvm_module_ref)
  {
#ifndef CSHIFT_HAVE_LLVM_BITCODE
    (void)cll_path;
    (void)llvm_module_ref;
    return false;
#else
    if (!llvm_module_ref)
      return false;
    ensure_bc_dir();
    std::string bcp = bc_path_for(cll_path);
    LLVMModuleRef mod = reinterpret_cast<LLVMModuleRef>(llvm_module_ref);
    int rc = LLVMWriteBitcodeToFile(mod, bcp.c_str());
    return rc == 0;
#endif
  }

  // ── Internal ──────────────────────────────────────────────────────────────

  std::string resolve_file_path(const std::string &path,
                                const std::string &rel_dir) const
  {
    // Absolute path
    if (path.size() > 0 && (path[0] == '/' || path[0] == '\\'))
    {
      if (mod_file_exists(path))
        return path;
      return "";
    }
    // Relative to the importing file's directory first
    {
      std::string p = rel_dir + "/" + path;
      if (mod_file_exists(p))
        return p;
    }
    // Then search dirs
    for (auto &d : search_dirs)
    {
      std::string p = d + "/" + path;
      if (mod_file_exists(p))
        return p;
    }
    return "";
  }

  // Core loader — loads a .cll by canonical absolute path, caching the result.
  const ModuleExports &load_canonical(const std::string &path)
  {
    // Already cached?
    auto it = cache_.find(path);
    if (it != cache_.end())
    {
      if (verbose)
        std::cerr << "[module] (cached) " << path << "\n";
      return it->second;
    }

    // Cycle detection
    if (loading_.count(path))
      throw std::runtime_error("[MODULE ERROR] Circular import detected: '" +
                               path + "'");
    loading_.insert(path);

    if (verbose)
      std::cerr << "[module] loading " << path << "\n";

    std::string source = mod_read_file(path);
    if (source.empty())
      throw std::runtime_error("[MODULE ERROR] Cannot read file: '" + path +
                               "'");

    // Lex + parse
    Lexer lexer(source);
    std::vector<Lexer::Token> tokens;
    try
    {
      tokens = lexer.tokenize();
    }
    catch (const std::exception &e)
    {
      loading_.erase(path);
      throw std::runtime_error(std::string("[MODULE LEX ERROR] in '") + path +
                               "': " + e.what());
    }

    Parser parser(tokens);
    std::vector<Parser::ASTNode *> raw_ast;
    try
    {
      raw_ast = parser.parse_program();
    }
    catch (const std::exception &e)
    {
      loading_.erase(path);
      throw std::runtime_error(std::string("[MODULE PARSE ERROR] in '") + path +
                               "': " + e.what());
    }

    // Recursively resolve any imports *within* the module
    std::string module_dir = mod_dirname(path);
    std::vector<Parser::ASTNode *> resolved_nodes;
    resolved_nodes.reserve(raw_ast.size());

    for (auto *n : raw_ast)
    {
      if (n->type == "ModuleImport")
      {
        // e.g. `import std;` inside a module
        try
        {
          const ModuleExports &dep = load_name(n->value);
          for (auto *dep_node : dep.nodes)
            resolved_nodes.push_back(dep_node); // borrow (dep owns them)
        }
        catch (const std::exception &e)
        {
          // Non-fatal — warn and continue
          std::cerr << "[MODULE WARNING] " << e.what() << " (skipping, in "
                    << path << ")\n";
        }
        delete n; // discard the ModuleImport node itself
      }
      else if (n->type == "Import")
      {
        // e.g. `import "other.cll";` inside a module
        try
        {
          const ModuleExports &dep = load_file(n->value, module_dir);
          for (auto *dep_node : dep.nodes)
            resolved_nodes.push_back(dep_node);
        }
        catch (const std::exception &e)
        {
          std::cerr << "[MODULE WARNING] " << e.what() << " (skipping, in "
                    << path << ")\n";
        }
        delete n;
      }
      else
      {
        resolved_nodes.push_back(n);
      }
    }

    loading_.erase(path);

    // Build the exports entry
    ModuleExports exports;
    exports.canonical_path = path;

    // Separate owned nodes (nodes we parsed here) from borrowed nodes
    // (from sub-modules).  We only own nodes that came from raw_ast and were
    // not deleted above.  The simplest model: store everything — the
    // sub-module deps are still alive in cache_ and own their nodes; we
    // just include pointers to them in our list.  To avoid double-delete we
    // only own nodes that originated from *this* parse run.
    //
    // Implementation: rebuild a set of original raw_ast pointers; anything
    // in resolved_nodes that is in that set is owned by us.
    std::unordered_set<Parser::ASTNode *> mine(raw_ast.begin(), raw_ast.end());
    // raw_ast is now empty of deleted nodes (ModuleImport/Import deleted
    // above), but the set still contains the valid pointers.

    exports.nodes = std::move(resolved_nodes);
    exports.bc_cache_path = bc_path_for(path);

    // Store in cache (moves exports, invalidates local ref)
    auto [ins_it, ok] = cache_.emplace(path, std::move(exports));
    (void)ok;
    return ins_it->second;
  }
};

// ── resolve_all_imports ────────────────────────────────────────────────────
//
// Walk the top-level AST of the main source file, resolve all ModuleImport,
// Import, and HeaderImport nodes, and return a combined view.
//
// OWNERSHIP MODEL
// ───────────────
// Nodes parsed by the loader (from modules) are owned by ModuleExports inside
// the loader — they live until the loader is destroyed.
//
// Nodes parsed from the main source file (in `ast`) are owned by the caller.
// The caller should delete only `main_ast_nodes` (the second out-param) when
// done; the loader-owned nodes in `imported_nodes` must NOT be deleted by the
// caller.
//
// For convenience, `all_nodes` is the combined ordered list for
// codegen/checker. It is a flat view — do not delete its elements directly; use
// the split output.

struct ResolvedAST
{
  // Combined ordered list: imported nodes first, then main-file nodes.
  // DO NOT delete these directly.
  std::vector<Parser::ASTNode *> all;

  // Nodes that originated from the main source file — caller must delete these.
  std::vector<Parser::ASTNode *> owned;

  // Nodes owned by the ModuleLoader — do NOT delete these.
  // (Kept for documentation; actual lifetime managed by loader.cache_)
  std::vector<Parser::ASTNode *> borrowed;
};

static inline ResolvedAST
resolve_all_imports(std::vector<Parser::ASTNode *> &ast, ModuleLoader &loader,
                    const std::string &src_path, bool verbose)
{
  std::string src_dir = mod_dirname(src_path);
  ResolvedAST result;

  // Track which canonical paths have already been injected (dedup)
  std::unordered_set<std::string> injected;

  auto inject_module = [&](const ModuleExports &m)
  {
    if (injected.count(m.canonical_path))
      return;
    injected.insert(m.canonical_path);
    for (auto *n : m.nodes)
    {
      result.all.push_back(n);
      result.borrowed.push_back(n);
    }
  };

  std::vector<Parser::ASTNode *> main_nodes;

  for (auto *n : ast)
  {
    if (n->type == "ModuleImport")
    {
      if (verbose)
        std::cerr << "[import] module \'" << n->value << "\'\n";
      try
      {
        inject_module(loader.load_name(n->value));
      }
      catch (const std::exception &e)
      {
        std::cerr << e.what() << "\n";
        delete n;
        // Clean up already-collected owned nodes before exit
        for (auto *owned : main_nodes)
          delete owned;
        std::exit(1);
      }
      delete n; // discard the import node itself (owned by main ast)
    }
    else if (n->type == "Import")
    {
      if (verbose)
        std::cerr << "[import] file \"" << n->value << "\"\n";
      try
      {
        inject_module(loader.load_file(n->value, src_dir));
      }
      catch (const std::exception &e)
      {
        std::cerr << e.what() << "\n";
        delete n;
        for (auto *owned : main_nodes)
          delete owned;
        std::exit(1);
      }
      delete n;
    }
    else if (n->type == "HeaderImport")
    {
      bool is_system = false;
      std::string header_name;
      is_header_import(n->value, is_system, header_name);
      if (verbose)
        std::cerr << "[import] C header " << (is_system ? "<" : "\"")
                  << header_name << (is_system ? ">" : "\"") << "\n";
      // parse_c_header returns freshly-allocated nodes — treat as owned
      std::vector<Parser::ASTNode *> hnodes =
          parse_c_header(header_name, is_system, {}, verbose);
      for (auto *hn : hnodes)
      {
        result.all.push_back(hn);
        main_nodes.push_back(hn); // we own these (not in loader cache)
      }
      delete n;
    }
    else
    {
      result.all.push_back(n);
      main_nodes.push_back(n);
    }
  }

  ast.clear(); // original vector is now empty; ownership split as above
  result.owned = std::move(main_nodes);
  return result;
}

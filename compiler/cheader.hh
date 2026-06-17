#pragma once
// ============================================================
// C<< C-Header Parser  —  compiler/cheader.hh
//
// Parses C headers via libclang and emits CImport AST nodes
// so users can write:
//
//   import <stdio.h>;
//   import <math.h>;
//   import "mylib.h";
//
// instead of hand-writing every `import int32 printf(...)`.
//
// Design
// ──────
// • Uses the libclang C API (clang-c/Index.h) which is stable
//   across LLVM versions and doesn't require C++17 clang libs.
// • Only function declarations visible at file scope are
//   emitted; macros, typedefs, struct definitions, etc. are
//   currently ignored (structs can be declared manually in
//   .cll as before).
// • The type mapping is conservative: unknown C types fall
//   back to `voided*` (opaque pointer), which is safe for
//   most OS/libc handle types.
// • When libclang is not available (-DCSHIFT_HAVE_CHEADER=0)
//   the whole file compiles to a stub that returns an empty
//   list and prints a warning.
// ============================================================

#include "parser.hh"

#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef CSHIFT_HAVE_CHEADER
// clang-c headers may be under a versioned path — CMake adds the right -I
#include <clang-c/Index.h>
#endif

// ── Type mapping ──────────────────────────────────────────────────────────
//
// Maps a CXType (from libclang) to a C<< type string.
// The mapping is intentionally lossy; C<< types are simpler than C types.

#ifdef CSHIFT_HAVE_CHEADER

static std::string clang_type_to_cshift(CXType t)
{
  // Strip canonical (resolves typedefs)
  CXType canon = clang_getCanonicalType(t);

  switch (canon.kind)
  {
  case CXType_Void:
    return "voided";
  case CXType_Bool:
    return "bool";
  case CXType_Char_S:
  case CXType_Char_U:
    return "int8";
  case CXType_SChar:
    return "int8";
  case CXType_UChar:
    return "uint8";
  case CXType_Short:
    return "int16";
  case CXType_UShort:
    return "uint16";
  case CXType_Int:
    return "int32";
  case CXType_UInt:
    return "uint32";
  case CXType_Long:
    return "int64"; // assume 64-bit LP64
  case CXType_ULong:
    return "uint64";
  case CXType_LongLong:
    return "int64";
  case CXType_ULongLong:
    return "uint64";
  case CXType_Float:
    return "float32";
  case CXType_Double:
    return "float64";
  case CXType_LongDouble:
    return "float64"; // best approximation

  case CXType_Pointer:
  {
    CXType pointee = clang_getPointeeType(canon);
    // char* / const char* → string
    CXType pcanon = clang_getCanonicalType(pointee);
    if (pcanon.kind == CXType_Char_S || pcanon.kind == CXType_Char_U || pcanon.kind == CXType_SChar)
      return "string"; // i8* in C<<
    if (pcanon.kind == CXType_Void)
      return "voided*";
    // pointer to known type → type*
    std::string inner = clang_type_to_cshift(pointee);
    return inner + "*";
  }

  case CXType_ConstantArray:
  case CXType_IncompleteArray:
  {
    // Decay to pointer
    CXType elem = clang_getArrayElementType(canon);
    std::string inner = clang_type_to_cshift(elem);
    return inner + "*";
  }

  case CXType_Record:
  {
    // Struct by-value: expand to flat list of primitive field types.
    struct FlatFields { std::vector<std::string> fields; bool ok = true; };
    FlatFields fc;
    clang_Type_visitFields(canon,
      [](CXCursor c, CXClientData data) -> CXVisitorResult {
        auto *fc = reinterpret_cast<FlatFields *>(data);
        if (fc->fields.size() >= 8) { fc->ok = false; return CXVisit_Break; }
        // Recursively get the field type
        CXType ft = clang_getCursorType(c);
        CXType ft_canon = clang_getCanonicalType(ft);
        // Only handle primitive fields — nested structs stay as voided*
        std::string mapped;
        switch ((int)ft_canon.kind) {
          case 3:  mapped = "bool";    break; // _Bool
          case 4:  mapped = "int8";    break; // Char_U
          case 5:  mapped = "uint8";   break; // UChar
          case 6:  mapped = "uint16";  break; // UShort
          case 7:  mapped = "uint32";  break; // UInt
          case 8:  mapped = "uint64";  break; // ULong
          case 10: mapped = "uint64";  break; // ULongLong
          case 13: mapped = "int8";    break; // Char_S
          case 14: mapped = "int8";    break; // SChar
          case 15: mapped = "int16";   break; // Short
          case 17: mapped = "int32";   break; // Int
          case 18: mapped = "int64";   break; // Long
          case 20: mapped = "int64";   break; // LongLong
          case 21: mapped = "float32"; break; // Float
          case 22: mapped = "float64"; break; // Double
          case 101: mapped = "voided*"; break; // Pointer — allow ptrs in structs
          default: fc->ok = false; return CXVisit_Break;
        }
        fc->fields.push_back(mapped);
        return CXVisit_Continue;
      }, &fc);
    if (fc.ok && !fc.fields.empty()) {
      std::string result = "flat:";
      for (size_t i = 0; i < fc.fields.size(); ++i) { if (i) result += ","; result += fc.fields[i]; }
      return result;
    }
    return "voided*"; // large or complex struct
  }

  case CXType_Elaborated:
  {
    // Elaborated type (typedef'd struct, union) — get the named type and recurse.
    CXType named = clang_Type_getNamedType(canon);
    CXType named_canon = clang_getCanonicalType(named);
    if (named_canon.kind == CXType_Record)
      return clang_type_to_cshift(named_canon); // delegate to Record case above
    // Anonymous or opaque
    return "voided*";
  }

  case CXType_FunctionProto:
  case CXType_FunctionNoProto:
    return "voided*"; // function pointer → opaque

  default:
  {
    // Last resort: map common C type spellings to C<< primitives,
    // then fall back to voided* for anything else.
    CXString spelling = clang_getTypeSpelling(canon);
    std::string s = clang_getCString(spelling);
    clang_disposeString(spelling);
    // Strip leading "const " / "volatile "
    for (const char *pfx : {"const ", "volatile ", "signed ", "unsigned "})
    {
      while (s.rfind(pfx, 0) == 0)
        s = s.substr(strlen(pfx));
    }
    if (s == "int" || s == "int32_t")   return "int32";
    if (s == "unsigned int" || s == "uint32_t") return "uint32";
    if (s == "short" || s == "int16_t") return "int16";
    if (s == "unsigned short" || s == "uint16_t") return "uint16";
    if (s == "long" || s == "long int" || s == "int64_t") return "int64";
    if (s == "unsigned long" || s == "unsigned long int" || s == "uint64_t") return "uint64";
    if (s == "long long" || s == "long long int") return "int64";
    if (s == "unsigned long long") return "uint64";
    if (s == "char" || s == "int8_t") return "int8";
    if (s == "unsigned char" || s == "uint8_t") return "uint8";
    if (s == "float")  return "float32";
    if (s == "double") return "float64";
    if (s == "bool" || s == "_Bool") return "bool";
    if (s == "void")   return "voided";
    if (s == "size_t" || s == "uintptr_t" || s == "ptrdiff_t") return "uint64";
    if (s == "ssize_t" || s == "intptr_t") return "int64";
    return "voided*";
  }
  }
}

// ── Visitor context ───────────────────────────────────────────────────────

struct HeaderVisitorCtx
{
  std::vector<Parser::ASTNode *> *nodes;
  std::unordered_set<std::string> *seen; // dedup by function name
  size_t line_offset;                    // for ASTNode line numbers
  bool verbose;
};

static CXChildVisitResult header_visitor(CXCursor cursor, CXCursor /*parent*/,
                                         CXClientData client_data)
{
  HeaderVisitorCtx *ctx = reinterpret_cast<HeaderVisitorCtx *>(client_data);

  // Only top-level function declarations
  if (clang_getCursorKind(cursor) != CXCursor_FunctionDecl)
    return CXChildVisit_Continue;

  // Skip declarations from included sub-headers? No — we want everything
  // that the header transitively exposes (matches what #include does).

  CXString cx_name = clang_getCursorSpelling(cursor);
  std::string fname = clang_getCString(cx_name);
  clang_disposeString(cx_name);

  if (fname.empty())
    return CXChildVisit_Continue;

  // Deduplicate (headers often declare the same function multiple times)
  if (ctx->seen->count(fname))
    return CXChildVisit_Continue;
  ctx->seen->insert(fname);

  CXType func_type = clang_getCursorType(cursor);
  CXType ret_type = clang_getResultType(func_type);

  std::string ret_s = clang_type_to_cshift(ret_type);

  // Build ASTNode value:  "rettype funcname"
  auto *node = new Parser::ASTNode("CImport", ret_s + " " + fname, ctx->line_offset, 0);

  // Build CParams child
  auto *cparams = new Parser::ASTNode("CParams", "", ctx->line_offset, 0);

  int nargs = clang_Cursor_getNumArguments(cursor);
  bool is_vararg = clang_isFunctionTypeVariadic(func_type) != 0;

  for (int i = 0; i < nargs; ++i)
  {
    CXCursor arg_cursor = clang_Cursor_getArgument(cursor, i);
    CXType arg_type = clang_getCursorType(arg_cursor);
    CXString arg_name_cx = clang_getCursorSpelling(arg_cursor);
    std::string arg_name = clang_getCString(arg_name_cx);
    clang_disposeString(arg_name_cx);

    std::string arg_type_s = clang_type_to_cshift(arg_type);
    std::string param_val = arg_name.empty() ? arg_type_s : (arg_type_s + " " + arg_name);

    cparams->children.push_back(new Parser::ASTNode("CParam", param_val, ctx->line_offset, 0));
  }

  if (is_vararg)
    cparams->children.push_back(new Parser::ASTNode("Variadic", "...", ctx->line_offset, 0));

  node->children.push_back(cparams);
  ctx->nodes->push_back(node);

  if (ctx->verbose)
  {
    std::string params_s;
    for (auto *p : cparams->children)
    {
      if (!params_s.empty()) params_s += ", ";
      params_s += p->value;
    }
    if (is_vararg) params_s += params_s.empty() ? "..." : ", ...";
    std::cerr << "[cheader] imported: " << ret_s << " " << fname
              << "(" << params_s << ")\n";
  }

  return CXChildVisit_Continue;
}

#endif // CSHIFT_HAVE_CHEADER

// ── Public API ────────────────────────────────────────────────────────────

// Parse a C header and return a list of CImport ASTNodes.
//
// `header`   — either a bare system header name like "stdio.h" (for
//              angle-bracket includes) or a file path for quoted includes.
// `is_system`— true  → emit  #include <header>
//              false → emit  #include "header"
// `extra_include_dirs` — additional -I paths passed to clang
// `verbose`  — print imported symbols to stderr
//
// Returns an empty list (with a warning) when libclang is not available.

static inline std::vector<Parser::ASTNode *>
parse_c_header(const std::string &header, bool is_system,
               const std::vector<std::string> &extra_include_dirs = {}, bool verbose = false)
{
  std::vector<Parser::ASTNode *> nodes;

#ifndef CSHIFT_HAVE_CHEADER
  (void)header;
  (void)is_system;
  (void)extra_include_dirs;
  (void)verbose;
  std::cerr << "[cheader] WARNING: C-header import not supported "
               "(cshift was built without libclang).\n"
               "          Re-build with libclang-dev installed.\n";
  return nodes;
#else

  // Build a minimal in-memory translation unit that just includes the header
  std::string tu_source =
      is_system ? ("#include <" + header + ">\n") : ("#include \"" + header + "\"\n");

  // Build clang args
  std::vector<const char *> clang_args;
  clang_args.push_back("-x");
  clang_args.push_back("c");
  clang_args.push_back("-std=c11");
  // Keep ownership of the strings
  std::vector<std::string> iflags;
  for (auto &d : extra_include_dirs)
  {
    iflags.push_back("-I" + d);
    clang_args.push_back(iflags.back().c_str());
  }

  CXIndex index = clang_createIndex(0 /*excludeDecls*/, 0 /*displayDiags*/);

  // Use an unsaved file so we don't need to write to disk
  CXUnsavedFile unsaved;
  unsaved.Filename = "<cshift_header_import>";
  unsaved.Contents = tu_source.c_str();
  unsaved.Length = (unsigned long)tu_source.size();

  CXTranslationUnit tu = clang_parseTranslationUnit(
      index, unsaved.Filename, clang_args.data(), (int)clang_args.size(), &unsaved, 1,
      CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_DetailedPreprocessingRecord);

  if (!tu)
  {
    clang_disposeIndex(index);
    std::cerr << "[cheader] ERROR: clang failed to parse header '" << header << "'\n";
    return nodes;
  }

  // Print diagnostics (errors only, not notes/warnings for system headers)
  unsigned ndiag = clang_getNumDiagnostics(tu);
  for (unsigned i = 0; i < ndiag; ++i)
  {
    CXDiagnostic diag = clang_getDiagnostic(tu, i);
    CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
    if (sev >= CXDiagnostic_Error)
    {
      CXString msg = clang_getDiagnosticSpelling(diag);
      std::cerr << "[cheader] clang: " << clang_getCString(msg) << "\n";
      clang_disposeString(msg);
    }
    clang_disposeDiagnostic(diag);
  }

  // Walk the AST
  std::unordered_set<std::string> seen;
  HeaderVisitorCtx ctx{&nodes, &seen, 1, verbose};
  clang_visitChildren(clang_getTranslationUnitCursor(tu), header_visitor, &ctx);

  clang_disposeTranslationUnit(tu);
  clang_disposeIndex(index);

  if (verbose)
    std::cerr << "[cheader] " << nodes.size() << " functions imported from <" << header << ">\n";

  return nodes;
#endif
}

// ── Header import from parser AST ─────────────────────────────────────────
//
// A `HeaderImport` AST node is produced by the parser when it sees:
//
//   import <stdio.h>;
//   import <math.h>;
//   import "mylib.h";
//
// This function resolves such nodes to real CImport nodes.
// It is called from resolve_all_imports() in module.hh.

static inline bool is_header_import(const std::string &module_name, bool &out_is_system,
                                    std::string &out_header)
{
  // The parser stores the raw import value as-is.
  // Angle-bracket form is stored as "<stdio.h>" (with brackets).
  // Quoted form is just the bare path stored in Import node value.
  if (module_name.size() >= 2 && module_name.front() == '<' && module_name.back() == '>')
  {
    out_is_system = true;
    out_header = module_name.substr(1, module_name.size() - 2);
    return true;
  }
  // .h extension → treat as header file (quoted include)
  if (module_name.size() > 2 && module_name.substr(module_name.size() - 2) == ".h")
  {
    out_is_system = false;
    out_header = module_name;
    return true;
  }
  return false;
}

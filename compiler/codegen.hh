#pragma once
#include "checker.hh" // pulls in parser.hh → lexer.hh
#include "ezllvm.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================
// C<< Codegen  —  AST → LLVM IR via ezllvm.h
//
// Covers:
//   • CImport / ModuleImport / Import (file)
//   • entry  → main()
//   • def    → named function
//   • Declarations / Const / reserve
//   • Assignments
//   • CallStatement / Expression calls
//   • tunnel  → store into caller-alloca'd slot
//   • if / else / while / for / foreach
//   • switch  (including valid/voided guards)
//   • move   (marks voided — no runtime action)
//   • Arithmetic, comparisons, casts in expressions
//   • Structs (field access via GEP)
//   • string literals → global i8*
// ============================================================

class Codegen
{
  // ── Types ────────────────────────────────────────────────────────────────

  EzModule *mod;
  EzFunc *current_func = nullptr;
  EzBlock *current_block = nullptr;

  // name → alloca (local variables in the current function)
  using VarMap = std::unordered_map<std::string, EzVal /*alloca ptr*/>;
  std::vector<VarMap> var_scopes;

  // name → EzFunc* (for call generation)
  std::unordered_map<std::string, EzFunc *> func_map;

  // Tunnel output info for C<< functions: name → ordered list of (type, name)
  // pairs. These become hidden pointer parameters appended after the normal
  // params.
  struct TunnelParam
  {
    std::string type;
    std::string name;
  };
  std::unordered_map<std::string, std::vector<TunnelParam>> func_tunnels;

  // Loop/Switch context for break/continue
  struct LoopContext
  {
    EzBlock *loop_end;  // where break jumps to
    EzBlock *loop_cond; // where continue jumps to (nullptr for do-while semantics)
    bool is_switch;     // true if this is a switch, false if loop
  };
  std::vector<LoopContext> loop_stack;

  // Collect all unique tunnel targets from a function body (recursive)
  void collect_tunnels(Parser::ASTNode *node, std::vector<TunnelParam> &out)
  {
    if (!node)
      return;
    if (node->type == "Tunnel" && node->children.size() >= 2)
    {
      auto *target = node->children[1];
      auto sp = target->value.find(' ');
      std::string ttype = target->value.substr(0, sp);
      std::string tname = target->value.substr(sp + 1);
      // Deduplicate
      bool found = false;
      for (auto &tp : out)
        if (tp.name == tname && tp.type == ttype)
        {
          found = true;
          break;
        }
      if (!found)
        out.push_back({ttype, tname});
    }
    for (auto *child : node->children)
      collect_tunnels(child, out);
  }

  // name → struct type (for field GEP)
  struct StructLayout
  {
    EzType llvm_type;
    std::vector<std::string> field_names;
    std::vector<std::string> field_types;
  };
  std::unordered_map<std::string, StructLayout> struct_map;

  // tunnel slots: in a function, "tunnel expr -> type name" writes into
  // a pointer that was passed in from the caller's alloca.
  // We model this by pre-allocating a local alloca for each tunnel target
  // and storing its address in a side-table keyed by target name.
  std::unordered_map<std::string, EzVal /*alloca*/> tunnel_slots;

  // ── Helpers ──────────────────────────────────────────────────────────────

  // ── Arena-based scope model ───────────────────────────────────────────────
  // Every scope that may heap-allocate owns a cshift_arena_t stack slot.
  // All managed allocations (Vector, T[], etc.) register their pointer with
  // the arena via cshift_arena_push(). On scope exit ONE call to
  // cshift_arena_free_all() releases everything.  `reset;` calls
  // cshift_arena_reset() which frees data but keeps the arena alive.

  struct ArenaScope
  {
    EzVal arena_slot; /* alloca ptr, or nullptr if no allocs yet */
  };
  std::vector<ArenaScope> arena_stack;

  EzVal get_or_create_scope_arena()
  {
    if (arena_stack.empty())
      return nullptr;
    ArenaScope &as = arena_stack.back();
    if (as.arena_slot)
      return as.arena_slot;
    // Represent cshift_arena_t as [3 x i64] — opaque, passed by pointer.
    EzType arena_ty = LLVMArrayType(ez_i64(), 3);
    as.arena_slot = alloca_in_entry(current_func, arena_ty, "__arena");
    ensure_arena_fns();
    EzVal init_args[] = {as.arena_slot};
    ez_call(mod, func_map["__cshift_arena_init"], init_args, 1, "");
    return as.arena_slot;
  }

  // Wrap a heap pointer with the arena tracker; returns the same pointer.
  EzVal arena_track(EzVal ptr)
  {
    if (!ptr)
      return ptr;
    EzVal arena = get_or_create_scope_arena();
    if (!arena)
      return ptr;
    ensure_arena_fns();
    EzVal push_args[] = {arena, ptr};
    return ez_call(mod, func_map["__cshift_arena_push"], push_args, 2, "tracked");
  }

  void emit_arena_free()
  {
    if (arena_stack.empty())
      return;
    EzVal slot = arena_stack.back().arena_slot;
    if (!slot)
      return;
    ensure_arena_fns();
    EzVal args[] = {slot};
    ez_call(mod, func_map["__cshift_arena_free_all"], args, 1, "");
  }

  void emit_arena_reset_stmt()
  {
    EzVal arena = get_or_create_scope_arena();
    if (!arena)
      return;
    ensure_arena_fns();
    EzVal args[] = {arena};
    ez_call(mod, func_map["__cshift_arena_reset"], args, 1, "");
  }

  void ensure_arena_fns()
  {
    auto ensure = [&](const char *name, EzType ret, std::initializer_list<EzType> params)
    {
      if (func_map.count(name))
        return;
      std::vector<EzType> pv(params);
      func_map[name] = ez_extern(mod, name, ret, pv.data(), (unsigned)pv.size(), 0);
    };
    ensure("__cshift_arena_init", ez_void(), {ez_ptr()});
    ensure("__cshift_arena_push", ez_ptr(), {ez_ptr(), ez_ptr()});
    ensure("__cshift_arena_free_all", ez_void(), {ez_ptr()});
    ensure("__cshift_arena_reset", ez_void(), {ez_ptr()});
  }

  // ── Named color constants (raylib + general) ────────────────────────────
  // These expand to flat {r,g,b,a} i8 values when passed as Color arguments.
  static const std::unordered_map<std::string, std::array<uint8_t, 4>> &color_constants()
  {
    using A4 = std::array<uint8_t, 4>;
    static const std::unordered_map<std::string, std::array<uint8_t, 4>> m = {
        {"LIGHTGRAY", A4{200, 200, 200, 255}}, {"GRAY", A4{130, 130, 130, 255}},
        {"DARKGRAY", A4{80, 80, 80, 255}},     {"YELLOW", A4{253, 249, 0, 255}},
        {"GOLD", A4{255, 203, 0, 255}},        {"ORANGE", A4{255, 161, 0, 255}},
        {"PINK", A4{255, 109, 194, 255}},      {"RED", A4{230, 41, 55, 255}},
        {"MAROON", A4{190, 33, 55, 255}},      {"GREEN", A4{0, 228, 48, 255}},
        {"LIME", A4{0, 158, 47, 255}},         {"DARKGREEN", A4{0, 117, 44, 255}},
        {"SKYBLUE", A4{102, 191, 255, 255}},   {"BLUE", A4{0, 121, 241, 255}},
        {"DARKBLUE", A4{0, 82, 172, 255}},     {"PURPLE", A4{200, 122, 255, 255}},
        {"VIOLET", A4{135, 60, 190, 255}},     {"DARKPURPLE", A4{112, 31, 126, 255}},
        {"BEIGE", A4{211, 176, 131, 255}},     {"BROWN", A4{127, 106, 79, 255}},
        {"DARKBROWN", A4{76, 63, 47, 255}},    {"WHITE", A4{255, 255, 255, 255}},
        {"BLACK", A4{0, 0, 0, 255}},           {"BLANK", A4{0, 0, 0, 0}},
        {"MAGENTA", A4{255, 0, 255, 255}},     {"RAYWHITE", A4{245, 245, 245, 255}},
    };
    return m;
  }

  // Check if a token is a named color constant.
  static bool is_color_constant(const std::string &name)
  {
    return color_constants().count(name) > 0;
  }

  // Expand a named color into 4 i8 LLVM constants.
  std::array<EzVal, 4> expand_color(const std::string &name)
  {
    auto it = color_constants().find(name);
    if (it == color_constants().end())
      return {nullptr, nullptr, nullptr, nullptr};
    auto &clr = it->second;
    return std::array<EzVal, 4>{
        LLVMConstInt(ez_i8(), clr[0], 0),
        LLVMConstInt(ez_i8(), clr[1], 0),
        LLVMConstInt(ez_i8(), clr[2], 0),
        LLVMConstInt(ez_i8(), clr[3], 0),
    };
  }

  // Build arg list for a call, expanding flat: struct params and color constants.
  // `param_types` is the declared LLVM param type array (may be longer than user args
  // because flat structs count as multiple params).
  void collect_call_args(const std::vector<std::vector<Parser::ASTNode *>> &arg_groups,
                         const std::vector<LLVMTypeRef> &param_types, unsigned regular_params,
                         bool is_vararg, std::vector<EzVal> &out_args)
  {
    unsigned param_idx = 0; // tracks position in declared param list

    for (auto &grp : arg_groups)
    {
      // Single-token color constant: WHITE, RED, ...
      if (grp.size() == 1 && grp[0]->type == "Token" &&
          grp[0]->token_type == Lexer::TokenType::IDENTIFIER && is_color_constant(grp[0]->value))
      {
        // How many i8 params does the function expect starting at param_idx?
        unsigned n_bytes = 0;
        for (unsigned k = param_idx; k < regular_params; ++k)
        {
          LLVMTypeKind kk = LLVMGetTypeKind(param_types[k]);
          if (kk == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(param_types[k]) == 8)
            n_bytes++;
          else
            break;
        }
        if (n_bytes >= 3) // at least r,g,b
        {
          auto rgba = expand_color(grp[0]->value);
          for (unsigned k = 0; k < n_bytes && k < 4; ++k)
          {
            out_args.push_back(rgba[k]);
            param_idx++;
          }
          continue;
        }
      }

      // Normal arg
      std::string hint =
          (param_idx < regular_params) ? hint_from_llvm_type(param_types[param_idx]) : "";
      EzVal v = eval_expr_children(grp, hint);
      if (v)
      {
        if (!is_vararg && param_idx < regular_params)
          v = coerce_to_param(v, param_types[param_idx]);
        else if (is_vararg && LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMFloatTypeKind)
          v = LLVMBuildFPExt(mod->builder, v, ez_f64(), "va_fprom");
        out_args.push_back(v);
        param_idx++;
      }
      else if (!is_vararg && param_idx < regular_params)
      {
        out_args.push_back(LLVMConstNull(param_types[param_idx]));
        param_idx++;
      }
    }
  }
  // (needed for correct &v semantics — load the ptr rather than addr-of slot).
  static const std::unordered_map<std::string, std::string> &managed_free_fns()
  {
    static const std::unordered_map<std::string, std::string> m = {
        {"Vector", "vec_free"},    {"HashMap", "map_free"},    {"LinkedList", "list_free"},
        {"Set", "set_free"},       {"Deque", "deque_free"},    {"RingBuffer", "ring_free"},
        {"Pool", "pool_free"},     {"SortedVec", "svec_free"}, {"StringBuilder", "sb_free"},
        {"BitSet", "bitset_free"},
    };
    return m;
  }

  void push_scope()
  {
    var_scopes.push_back({});
    arena_stack.push_back({nullptr});
  }

  void pop_scope()
  {
    emit_arena_free(); // one call frees all managed allocs in this scope
    var_scopes.pop_back();
    arena_stack.pop_back();
  }

  // name → declared C<< type string (for struct GEP resolution)
  std::unordered_map<std::string, std::string> var_type_map;

  void declare_var(const std::string &name, EzVal alloca_ptr, const std::string &type_s = "")
  {
    if (!var_scopes.empty())
    {
      var_scopes.back()[name] = alloca_ptr;
      if (!type_s.empty())
        var_type_map[name] = type_s;
    }
  }

  EzVal lookup_var(const std::string &name)
  {
    for (int i = (int)var_scopes.size() - 1; i >= 0; --i)
    {
      auto it = var_scopes[i].find(name);
      if (it != var_scopes[i].end())
        return it->second;
    }
    return nullptr;
  }

  // Map C<< type string → LLVM EzType
  EzType cshift_type(const std::string &t)
  {
    std::string base = t;
    // strip pointer/array decorators to find base
    bool is_ptr = (t.find('*') != std::string::npos);
    bool is_slice = (t.find('[') != std::string::npos);
    // strip template parameters first: "Vector<T>" -> "Vector"
    {
      auto lt = base.find('<');
      if (lt != std::string::npos)
        base = base.substr(0, lt);
    }
    // strip all remaining decorators
    for (char c : std::string("*[]:|"))
    {
      base.erase(std::remove(base.begin(), base.end(), c), base.end());
    }

    EzType elem = nullptr;
    if (base == "int8" || base == "char")
      elem = ez_i8();
    else if (base == "int16")
      elem = ez_i16();
    else if (base == "int32")
      elem = ez_i32();
    else if (base == "int64")
      elem = ez_i64();
    else if (base == "uint8")
      elem = ez_i8();
    else if (base == "uint16")
      elem = ez_i16();
    else if (base == "uint32")
      elem = ez_i32();
    else if (base == "uint64")
      elem = ez_i64();
    else if (base == "float32")
      elem = ez_f32();
    else if (base == "float64")
      elem = ez_f64();
    else if (base == "bool")
      elem = ez_i1();
    else if (base == "string")
      elem = ez_ptr(); // i8*
    else if (base == "voided" || base == "void")
      elem = ez_void();
    else
    {
      // user-defined struct?
      auto it = struct_map.find(base);
      if (it != struct_map.end())
        elem = it->second.llvm_type;
      else
        elem = ez_i32(); // fallback: treat unknown as i32
    }

    // Special-case: pointer-to-void/voided should map to i8* (ez_ptr()),
    // not a pointer to LLVM void type which is invalid.
    if (is_ptr || is_slice)
    {
      if (base == "voided" || base == "void")
        return ez_ptr();
      return ez_ptr_to(elem);
    }
    return elem;
  }

  bool is_float_type(const std::string &t) { return t == "float32" || t == "float64"; }
  bool is_unsigned_type(const std::string &t) { return t.substr(0, 4) == "uint"; }

  // ── Entry-point alloca builder for the first block ────────────────────────
  // We position the builder at the very start of the entry block for allocas.
  EzVal alloca_in_entry(EzFunc *fn, EzType ty, const std::string &name)
  {
    // Temporarily move builder to the first instruction of the entry block
    LLVMBasicBlockRef entry_bb = LLVMGetEntryBasicBlock(fn->fn);
    if (!entry_bb)
    {
      // ensure an entry block exists (some functions may not have one yet)
      entry_bb = LLVMAppendBasicBlockInContext(fn->owner->ctx, fn->fn, "entry");
    }
    LLVMValueRef first = LLVMGetFirstInstruction(entry_bb);
    if (first)
      LLVMPositionBuilderBefore(mod->builder, first);
    else
      LLVMPositionBuilderAtEnd(mod->builder, entry_bb);
    EzVal slot = ez_alloca(mod, ty, name.c_str());
    // Restore builder to current block
    ez_use(current_block);
    return slot;
  }

  // Returns true if the current basic block already ends with a terminator.
  // Emitting a second terminator (e.g. an unconditional br after a while-true
  // loop) produces invalid IR and can crash LLVM's optimizer.
  bool current_block_has_terminator() const
  {
    if (!current_block)
      return false;
    LLVMValueRef last = LLVMGetLastInstruction(current_block->bb);
    return last && LLVMIsATerminatorInst(last);
  }

public:
  // ── Public interface ──────────────────────────────────────────────────────

  explicit Codegen(EzModule *m) : mod(m) {}

  // Generate IR for a full program AST.
  void generate(const std::vector<Parser::ASTNode *> &ast)
  {
    // Pass 1: forward-declare all structs, functions, and C imports
    for (auto *n : ast)
      forward_declare(n);
    // Pass 2: emit bodies
    for (auto *n : ast)
      emit_top(n);
  }

private:
  // ── Forward declarations ──────────────────────────────────────────────────

  void forward_declare(Parser::ASTNode *n)
  {
    if (!n)
      return;
    if (n->type == "Struct")
      forward_struct(n);
    else if (n->type == "CImport")
      forward_c_import(n);
    else if (n->type == "Function")
      forward_function(n);
    else if (n->type == "FuncDecl")
      forward_func_decl(n);
    else if (n->type == "Class")
      forward_class(n);
    else if (n->type == "Entry")
      forward_entry(n);
    else if (n->type == "Template")
    {
      // Extract and forward-declare the templated definition
      if (n->children.size() >= 2)
        forward_declare(n->children[1]);
    }
    else if (n->type == "Namespace")
      for (auto *c : n->children)
        forward_declare(c);
  }

  // dec name(params) [-> type t1, type t2];
  // Forward-declares the LLVM function signature (with hidden tunnel-output
  // pointer params) WITHOUT a body. The matching `def name(params) { ... }`
  // later in the file reuses this declaration (forward_function detects an
  // existing func_map entry and skips re-declaring).
  void forward_func_decl(Parser::ASTNode *n)
  {
    const std::string &name = n->value;
    std::vector<EzType> params;
    std::vector<std::string> pnames;

    if (!n->children.empty() && n->children[0]->type == "Parameters")
    {
      for (auto *p : n->children[0]->children)
      {
        auto sp = p->value.find(' ');
        params.push_back(cshift_type(p->value.substr(0, sp)));
        pnames.push_back(p->value.substr(sp + 1));
      }
    }

    // Tunnel outputs declared in the `dec` signature
    std::vector<TunnelParam> tunnels;
    if (n->children.size() >= 2 && n->children[1]->type == "DeclTunnels")
    {
      for (auto *t : n->children[1]->children)
      {
        auto sp = t->value.find(' ');
        tunnels.push_back({t->value.substr(0, sp), t->value.substr(sp + 1)});
      }
    }
    func_tunnels[name] = tunnels;

    for (auto &tp : tunnels)
    {
      params.push_back(ez_ptr());
      pnames.push_back("__tunnel_" + tp.name);
    }

    if (func_map.count(name))
      return; // already declared (e.g. duplicate dec, or def came first)

    EzFunc *f = ez_func(mod, name.c_str(), ez_void(), params.data(), (unsigned)params.size(), 0);
    for (unsigned i = 0; i < pnames.size(); ++i)
      ez_set_param_name(f, i, pnames[i].c_str());
    func_map[name] = f;
    decl_only_funcs.insert(name); // mark as declaration-only until a def appears
  }

  // Names declared via `dec` but not yet defined via `def`.
  // forward_function() removes the name from this set once it provides a body.
  std::unordered_set<std::string> decl_only_funcs;

  // Maps "ClassName_methodName" → synthetic Function AST node (emitted in
  // emit_top when the Class node is visited during pass 2).
  std::unordered_map<std::string, Parser::ASTNode *> class_method_nodes;
  // Maps ClassName → list of "ClassName_methodName" in declaration order
  std::unordered_map<std::string, std::vector<std::string>> class_methods;

  // class Name { fields...; def method(params) { body } }
  //   → struct Name { fields }
  //   + def Name_method(Name* self, params) { body }   (internal linkage)
  // Method bodies reference `self.field` via the normal pointer-field-access
  // codegen path (self is Name*, so self.field auto-derefs).
  void forward_class(Parser::ASTNode *n)
  {
    const std::string &cls_name = n->value;
    Parser::ASTNode *fields_node = nullptr;
    Parser::ASTNode *methods_node = nullptr;
    for (auto *c : n->children)
    {
      if (c->type == "Fields")
        fields_node = c;
      if (c->type == "Methods")
        methods_node = c;
    }

    // 1. Forward-declare the struct from Fields
    if (fields_node)
    {
      auto *struct_node = new Parser::ASTNode("Struct", cls_name, n->line, n->depth);
      for (auto *f : fields_node->children)
        struct_node->children.push_back(f); // reuse Field nodes ("type name")
      forward_struct(struct_node);
    }

    // 2. For each method, build a synthetic Function node:
    //    def ClassName_method(ClassName* self, <original params>) { body }
    if (methods_node)
    {
      auto &order = class_methods[cls_name];
      for (auto *m : methods_node->children)
      {
        std::string fn_name = cls_name + "_" + m->value;
        auto *fn_node = new Parser::ASTNode("Function", fn_name, m->line, m->depth);

        Parser::ASTNode *orig_params = nullptr;
        Parser::ASTNode *body = nullptr;
        for (auto *c : m->children)
        {
          if (c->type == "Parameters")
            orig_params = c;
          else if (c->type == "Block")
            body = c; // last Block wins (the real body)
          // DeclTunnels (-> type tname) is documentary only — actual tunnel
          // statements live inside Block and are found by collect_tunnels.
        }
        auto *new_params = new Parser::ASTNode("Parameters", "", m->line, m->depth);
        new_params->children.push_back(
            new Parser::ASTNode("Param", cls_name + "* self", m->line, m->depth));
        if (orig_params)
          for (auto *p : orig_params->children)
            new_params->children.push_back(p);

        fn_node->children.push_back(new_params);
        fn_node->children.push_back(body ? body
                                         : new Parser::ASTNode("Block", "", m->line, m->depth));

        forward_function(fn_node);
        class_method_nodes[fn_name] = fn_node;
        order.push_back(fn_name);
      }
    }
  }

  void forward_struct(Parser::ASTNode *n)
  {
    const std::string &name = n->value;
    StructLayout layout;
    layout.llvm_type = ez_struct_named(mod, name.c_str());

    std::vector<EzType> fields;
    for (auto *f : n->children)
    {
      if (!f)
        continue;
      auto sp = f->value.find(' ');
      std::string ftype = f->value.substr(0, sp);
      std::string fname = f->value.substr(sp + 1);
      layout.field_names.push_back(fname);
      layout.field_types.push_back(ftype);
      fields.push_back(cshift_type(ftype));
    }
    ez_struct_body(layout.llvm_type, fields.data(), (unsigned)fields.size());
    struct_map[name] = layout;
  }

  void forward_c_import(Parser::ASTNode *n)
  {
    // value = "rettype funcname"
    auto sp = n->value.find(' ');
    std::string ret_s = n->value.substr(0, sp);
    std::string fname = n->value.substr(sp + 1);

    EzType ret = (ret_s == "voided" || ret_s == "void") ? ez_void() : cshift_type(ret_s);

    std::vector<EzType> params;
    bool vararg = false;
    if (!n->children.empty() && n->children[0]->type == "CParams")
    {
      for (auto *p : n->children[0]->children)
      {
        if (p->type == "Variadic")
        {
          vararg = true;
          continue;
        }
        auto psp = p->value.find(' ');
        std::string ptype = p->value.substr(0, psp);

        // flat:<t0>,<t1>,... — expand struct-by-value to individual params
        if (ptype.rfind("flat:", 0) == 0)
        {
          std::string fields = ptype.substr(5); // after "flat:"
          std::string cur;
          for (char ch : fields + ",")
          {
            if (ch == ',')
            {
              if (!cur.empty())
              {
                params.push_back(cshift_type(cur));
                cur.clear();
              }
            }
            else
              cur += ch;
          }
          continue;
        }

        params.push_back(cshift_type(ptype));
      }
    }

    EzFunc *f =
        ez_extern(mod, fname.c_str(), ret, params.data(), (unsigned)params.size(), vararg ? 1 : 0);
    func_map[fname] = f;
  }

  void forward_function(Parser::ASTNode *n)
  {
    const std::string &name = n->value;

    // Scan the function body for tunnel targets and add them as hidden
    // pointer parameters after the regular params.  This is how C<< VOP
    // tunnel outputs are implemented: the caller passes the address of its
    // reserved slot, and the function writes into it directly.
    std::vector<TunnelParam> tunnels;
    if (n->children.size() >= 2)
      collect_tunnels(n->children[1], tunnels);

    // If a matching `dec` already forward-declared this function, reuse the
    // existing LLVM function — do NOT create a second declaration (that
    // would be a duplicate-symbol error in LLVM).
    auto existing = func_map.find(name);
    if (existing != func_map.end() && decl_only_funcs.count(name))
    {
      // Validate tunnel signature consistency (best-effort — mismatches
      // would already break at the call sites via param count).
      func_tunnels[name] = tunnels;
      decl_only_funcs.erase(name); // now defined
      EzFunc *f = existing->second;
      if (n->meta == "export")
        LLVMSetLinkage(f->fn, LLVMExternalLinkage);
      else
        LLVMSetLinkage(f->fn, LLVMInternalLinkage);
      return;
    }

    EzType ret = ez_void();
    std::vector<EzType> params;
    std::vector<std::string> pnames;

    if (!n->children.empty() && n->children[0]->type == "Parameters")
    {
      for (auto *p : n->children[0]->children)
      {
        auto sp = p->value.find(' ');
        params.push_back(cshift_type(p->value.substr(0, sp)));
        pnames.push_back(p->value.substr(sp + 1));
      }
    }

    func_tunnels[name] = tunnels;
    for (auto &tp : tunnels)
    {
      params.push_back(ez_ptr()); // pointer to caller's slot
      pnames.push_back("__tunnel_" + tp.name);
    }

    EzFunc *f = ez_func(mod, name.c_str(), ret, params.data(), (unsigned)params.size(), 0);
    for (unsigned i = 0; i < pnames.size(); ++i)
      ez_set_param_name(f, i, pnames[i].c_str());
    // Non-exported functions get internal linkage — invisible to the C linker.
    // export def → ExternalLinkage (visible/callable from C).
    if (n->meta == "export")
      LLVMSetLinkage(f->fn, LLVMExternalLinkage);
    else
      LLVMSetLinkage(f->fn, LLVMInternalLinkage);
    func_map[name] = f;
  }

  void forward_entry(Parser::ASTNode *)
  {
    // main() : i32
    EzFunc *f = ez_func(mod, "main", ez_i32(), nullptr, 0, 0);
    func_map["__entry__"] = f;
  }

  // ── Top-level emitters ────────────────────────────────────────────────────

  void emit_top(Parser::ASTNode *n)
  {
    if (!n)
      return;
    if (n->type == "Function")
      emit_function(n);
    else if (n->type == "Entry")
      emit_entry(n);
    else if (n->type == "Struct")
    { /* body already set in forward pass */
    }
    else if (n->type == "Class")
    {
      // Emit each desugared method as a regular function
      auto it = class_methods.find(n->value);
      if (it != class_methods.end())
        for (auto &fn_name : it->second)
        {
          auto fit = class_method_nodes.find(fn_name);
          if (fit != class_method_nodes.end())
            emit_function(fit->second);
        }
    }
    else if (n->type == "Template")
    {
      // Emit the templated definition
      if (n->children.size() >= 2)
        emit_top(n->children[1]);
    }
    else if (n->type == "Namespace")
      for (auto *c : n->children)
        emit_top(c);
    // CImport / ModuleImport / Import / HeaderImport: nothing to emit
  }

  void emit_entry(Parser::ASTNode *n)
  {
    EzFunc *f = func_map["__entry__"];
    current_func = f;
    EzBlock *entry_b = ez_block(f, "entry");
    current_block = entry_b;
    ez_use(entry_b);

    push_scope();
    tunnel_slots.clear();
    if (!n->children.empty())
      emit_block_body(n->children[0]);
    pop_scope();

    // return 0 — but only if the block doesn't already have a terminator
    if (!current_block_has_terminator())
      ez_ret(mod, ez_const_int(ez_i32(), 0));
    current_func = nullptr;
  }

  void emit_function(Parser::ASTNode *n)
  {
    const std::string &name = n->value;
    EzFunc *f = func_map[name];
    current_func = f;
    EzBlock *entry_b = ez_block(f, "entry");
    current_block = entry_b;
    ez_use(entry_b);

    push_scope();
    tunnel_slots.clear();

    // Bind regular parameters to allocas
    unsigned param_idx = 0;
    if (!n->children.empty() && n->children[0]->type == "Parameters")
    {
      for (auto *p : n->children[0]->children)
      {
        auto sp = p->value.find(' ');
        std::string ptype = p->value.substr(0, sp);
        std::string pname = p->value.substr(sp + 1);
        EzType ty = cshift_type(ptype);
        EzVal slot = alloca_in_entry(f, ty, pname.c_str());
        ez_store(mod, ez_param(f, param_idx++), slot);
        declare_var(pname, slot, ptype);

        // If this is a "ClassName* self" parameter, register aliases for any
        // T[] fields so that self.field[i] and self.field.len() work inside
        // the method body using the existing arena-array machinery.
        if (pname == "self" && !ptype.empty() && ptype.back() == '*')
        {
          std::string cls = ptype.substr(0, ptype.size() - 1);
          auto sit = struct_map.find(cls);
          if (sit != struct_map.end())
          {
            auto &layout = sit->second;
            for (size_t fi = 0; fi < layout.field_names.size(); fi++)
            {
              const std::string &ftype = layout.field_types[fi];
              const std::string &fname = layout.field_names[fi];
              if (ftype.size() >= 2 && ftype.substr(ftype.size() - 2) == "[]")
              {
                // Compute GEP to the array-metadata struct in self
                // self is stored in slot (alloca ptr to ptr)
                // We need: GEP(load(slot), fi) = pointer to the T[] metadata
                EzType struct_ty = layout.llvm_type;
                EzVal self_ptr = ez_load(mod, ez_ptr(), slot, "self_ptr");
                EzVal zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
                EzVal fidx = LLVMConstInt(LLVMInt32Type(), (unsigned)fi, 0);
                EzVal gep_args[] = {zero, fidx};
                EzVal field_gep = LLVMBuildGEP2(mod->builder, struct_ty, self_ptr, gep_args, 2,
                                                (fname + "_gep").c_str());
                // Register under "self.fname" AND under fname alone (for local use)
                std::string alias = "self." + fname;
                declare_var(alias, field_gep, ftype);
                declare_var(fname, field_gep, ftype);

                // Register in arena_array_elem_type so subscript/len work
                std::string elem_type = ftype.substr(0, ftype.size() - 2);
                arena_array_elem_type[alias] = elem_type;
                arena_array_elem_type[fname] = elem_type;
              }
            }
          }
        }
      }
    }

    // Bind hidden tunnel pointer params: each tunnel target is passed as a
    // ptr by the caller. Register them directly in tunnel_slots and var scope
    // so that emit_tunnel writes through to the caller's alloca.
    auto tit = func_tunnels.find(name);
    if (tit != func_tunnels.end())
    {
      for (auto &tp : tit->second)
      {
        // The param is already a pointer to the caller's slot
        EzVal caller_ptr = ez_param(f, param_idx++);
        // Store the param ptr itself into a local alloca so lookup_var works
        EzVal ptr_slot = alloca_in_entry(f, ez_ptr(), ("__tptr_" + tp.name).c_str());
        ez_store(mod, caller_ptr, ptr_slot);
        tunnel_slots[tp.name] = ptr_slot; // slot holding the ptr
        declare_var(tp.name, ptr_slot, tp.type);
      }
    }

    if (n->children.size() >= 2)
      emit_block_body(n->children[1]);
    pop_scope();

    // implicit void return — guarded against double terminator
    EzType ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(f->fn));
    if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind && !current_block_has_terminator())
      ez_ret_void(mod);

    current_func = nullptr;
  }

  // ── Block / statement dispatch ────────────────────────────────────────────

  void emit_block_body(Parser::ASTNode *block)
  {
    if (!block)
      return;
    push_scope();
    for (auto *stmt : block->children)
      emit_stmt(stmt);
    pop_scope();
  }

  void emit_stmt(Parser::ASTNode *n)
  {
    if (!n)
      return;
    const std::string &t = n->type;
    if (t == "Declaration")
      emit_declaration(n);
    else if (t == "Const")
      emit_const(n);
    else if (t == "Reserve")
      emit_reserve(n);
    else if (t == "Assignment")
      emit_assignment(n);
    else if (t == "IndexAssignment")
      emit_index_assignment(n);
    else if (t == "CallStatement")
      emit_call_stmt(n);
    else if (t == "Tunnel")
      emit_tunnel(n);
    else if (t == "Move")
    {
      // Set the runtime validity flag to false so that a switch guard with
      // meta=="unknown" can branch correctly at runtime.
      const std::string &vname = n->value;
      EzVal vslot = lookup_var("__track_validity_" + vname);
      if (vslot)
        ez_store(mod, LLVMConstInt(ez_i1(), 0, 0), vslot);
      // (voided-state tracking for static paths is done in the checker)
    }
    else if (t == "Reset")
    {
      emit_arena_reset_stmt();
    }
    else if (t == "Break")
      emit_break(n);
    else if (t == "Continue")
      emit_continue(n);
    else if (t == "If")
      emit_if(n);
    else if (t == "While")
      emit_while(n);
    else if (t == "For")
      emit_for(n);
    else if (t == "Foreach")
      emit_foreach(n);
    else if (t == "Switch")
      emit_switch(n);
    else if (t == "ArrayAppend")
      emit_array_append(n);
    else if (t == "Block")
      emit_block_body(n);
    else if (t == "Expression")
      emit_expression(n); // expression-statement
  }

  // ── Variables ─────────────────────────────────────────────────────────────

  // name → element type for arena arrays (T[])
  std::unordered_map<std::string, std::string> arena_array_elem_type;

  // Ensure malloc/realloc/free are declared (for arena arrays)
  void ensure_alloc_fns()
  {
    if (!func_map.count("malloc"))
    {
      EzType params[] = {ez_i64()};
      func_map["malloc"] = ez_extern(mod, "malloc", ez_ptr(), params, 1, 0);
    }
    if (!func_map.count("realloc"))
    {
      EzType params[] = {ez_ptr(), ez_i64()};
      func_map["realloc"] = ez_extern(mod, "realloc", ez_ptr(), params, 2, 0);
    }
    if (!func_map.count("free"))
    {
      EzType params[] = {ez_ptr()};
      func_map["free"] = ez_extern(mod, "free", ez_void(), params, 1, 0);
    }
  }

  // Arena array layout: for variable "arr" of type T[]:
  //   arr_data  : ptr  (T*)
  //   arr_len   : i64
  //   arr_cap   : i64
  EzVal get_arena_len_ptr(const std::string &name) { return lookup_var(name + "__len"); }
  EzVal get_arena_cap_ptr(const std::string &name) { return lookup_var(name + "__cap"); }
  EzVal get_arena_data_ptr(const std::string &name) { return lookup_var(name + "__data"); }

  void emit_arena_array_declaration(const std::string &name, const std::string &elem_type_s)
  {
    ensure_alloc_fns();
    arena_array_elem_type[name] = elem_type_s;
    // __data pointer is tracked by arena_track() in emit_array_append — no manual cleanup needed.
    EzType i64 = ez_i64();

    // Allocate len, cap, data slots
    EzVal len_slot = alloca_in_entry(current_func, i64, (name + "__len").c_str());
    EzVal cap_slot = alloca_in_entry(current_func, i64, (name + "__cap").c_str());
    EzVal data_slot = alloca_in_entry(current_func, ez_ptr(), (name + "__data").c_str());

    ez_store(mod, ez_const_int(i64, 0), len_slot);
    ez_store(mod, ez_const_int(i64, 0), cap_slot);
    ez_store(mod, LLVMConstNull(ez_ptr()), data_slot);

    declare_var(name + "__len", len_slot, "int64");
    declare_var(name + "__cap", cap_slot, "int64");
    declare_var(name + "__data", data_slot, "voided*");
    // Register the variable name itself for .len access
    declare_var(name, len_slot, "int64"); // main slot = len for .len reads
    var_type_map[name] = "int64";         // arr.len loads from len_slot
  }

  void emit_array_append(Parser::ASTNode *n)
  {
    // n->value = array variable name, n->children[0] = value expr
    const std::string &arr = n->value;
    EzVal len_ptr = get_arena_len_ptr(arr);
    EzVal cap_ptr = get_arena_cap_ptr(arr);
    EzVal data_ptr = get_arena_data_ptr(arr);
    if (!len_ptr || !cap_ptr || !data_ptr)
      return; // not an arena array

    std::string elem_s = "int32";
    auto eit = arena_array_elem_type.find(arr);
    if (eit != arena_array_elem_type.end())
      elem_s = eit->second;
    EzType elem_ty = cshift_type(elem_s);
    EzType i64 = ez_i64();

    EzVal val = emit_expression_val(n->children.empty() ? nullptr : n->children[0], elem_s);
    if (!val)
      return;

    EzVal len = ez_load(mod, i64, len_ptr, "len");
    EzVal cap = ez_load(mod, i64, cap_ptr, "cap");

    // if (len >= cap) { cap = cap ? cap*2 : 8; data = realloc(data, cap *
    // sizeof(T)) }
    EzBlock *grow_b = ez_block(current_func, "arr.grow");
    EzBlock *store_b = ez_block(current_func, "arr.store");

    EzVal need_grow = ez_sge(mod, len, cap, "need_grow");
    ez_cond_br(mod, need_grow, grow_b, store_b);

    // grow block
    current_block = grow_b;
    ez_use(grow_b);
    EzVal cap_zero = ez_eq(mod, cap, ez_const_int(i64, 0), "cap_zero");
    EzVal new_cap = LLVMBuildSelect(mod->builder, cap_zero, ez_const_int(i64, 8),
                                    ez_mul(mod, cap, ez_const_int(i64, 2), "cap2"), "new_cap");
    ez_store(mod, new_cap, cap_ptr);
    // elem size via LLVM
    unsigned elem_bits = LLVMSizeOfTypeInBits(LLVMGetModuleDataLayout(mod->mod), elem_ty);
    EzVal elem_size = ez_const_int(i64, elem_bits / 8);
    EzVal new_bytes = ez_mul(mod, new_cap, elem_size, "new_bytes");
    EzVal old_data = ez_load(mod, ez_ptr(), data_ptr, "old_data");
    EzFunc *realloc_fn = func_map["realloc"];
    EzVal rargs[] = {old_data, new_bytes};
    EzVal new_data = ez_call(mod, realloc_fn, rargs, 2, "new_data");
    // Track the new allocation with the scope arena (replaces old pointer).
    // arena_track is a no-op if no arena exists yet — harmless.
    EzVal tracked_data = arena_track(new_data);
    ez_store(mod, tracked_data, data_ptr);
    ez_br(mod, store_b);

    // store block
    current_block = store_b;
    ez_use(store_b);
    // Reload len/data (may have changed)
    EzVal len2 = ez_load(mod, i64, len_ptr, "len2");
    EzVal data2 = ez_load(mod, ez_ptr(), data_ptr, "data2");
    EzVal gep_idx[] = {len2};
    EzVal elem_ptr = ez_gep(mod, elem_ty, data2, gep_idx, 1, "elem_ptr");
    ez_store(mod, val, elem_ptr);
    EzVal new_len = ez_add(mod, len2, ez_const_int(i64, 1), "new_len");
    ez_store(mod, new_len, len_ptr);
    // Also update the "name" slot (= len slot for .len reads)
    EzVal main_slot = lookup_var(arr);
    if (main_slot && main_slot != len_ptr)
      ez_store(mod, new_len, main_slot);
  }

  void emit_declaration(Parser::ASTNode *n)
  {
    auto sp = n->value.find(' ');
    std::string type_s = n->value.substr(0, sp);
    std::string vname = n->value.substr(sp + 1);

    // Arena array: T[]
    if (type_s.size() > 2 && type_s.substr(type_s.size() - 2) == "[]")
    {
      std::string elem = type_s.substr(0, type_s.size() - 2);
      emit_arena_array_declaration(vname, elem);
      return;
    }

    // Strip template params to get base type name: "Vector<int32>" → "Vector"
    std::string base_type = type_s;
    {
      auto lt = base_type.find('<');
      if (lt != std::string::npos)
        base_type = base_type.substr(0, lt);
    }

    // Check if this is a managed container type (Vector, HashMap, …)
    const auto &mfns = managed_free_fns();
    auto mfit = mfns.find(base_type);
    bool is_managed = (mfit != mfns.end());

    if (is_managed)
    {
      // Managed types are heap-allocated: the variable holds a *pointer*
      // to the heap object returned by vec_new / map_new / etc.
      // We store the pointer in a ptr-sized alloca and register a cleanup.
      EzVal slot = alloca_in_entry(current_func, ez_ptr(), vname.c_str());
      ez_store(mod, LLVMConstNull(ez_ptr()), slot); // init to null
      declare_var(vname, slot, type_s);

      if (!n->children.empty())
      {
        EzVal init = emit_expression_val(n->children[0], type_s);
        if (init)
        {
          // Track heap pointer with scope arena — freed on scope exit.
          EzVal tracked = arena_track(init);
          ez_store(mod, tracked, slot);
        }
      }
      return;
    }

    EzType ty = cshift_type(type_s);
    EzVal slot = alloca_in_entry(current_func, ty, vname.c_str());
    declare_var(vname, slot, type_s);

    // Hidden runtime-validity flag: __track_validity_<name>.
    // Starts true. Set to false by `move`. Used by switch guards when the
    // checker cannot statically determine voided state (meta == "unknown").
    {
      std::string vflag = "__track_validity_" + vname;
      EzVal vslot = alloca_in_entry(current_func, ez_i1(), vflag.c_str());
      ez_store(mod, LLVMConstInt(ez_i1(), 1, 0), vslot);
      declare_var(vflag, vslot, "bool");
    }

    if (!n->children.empty())
    {
      EzVal init = emit_expression_val(n->children[0], type_s);
      if (init)
      {
        // Auto-coerce numeric types (e.g. i64 → i32 from strlen on 64-bit).
        // If the coercion is lossy (wider → narrower) it still happens but
        // the checker would have emitted a warning; we just do the trunc/sext.
        LLVMTypeRef init_ty = LLVMTypeOf(init);
        if (init_ty != ty)
        {
          LLVMTypeKind init_kind = LLVMGetTypeKind(init_ty);
          LLVMTypeKind dest_kind = LLVMGetTypeKind(ty);
          bool init_int = (init_kind == LLVMIntegerTypeKind);
          bool dest_int = (dest_kind == LLVMIntegerTypeKind);
          bool init_fp = (init_kind == LLVMDoubleTypeKind || init_kind == LLVMFloatTypeKind);
          bool dest_fp = (dest_kind == LLVMDoubleTypeKind || dest_kind == LLVMFloatTypeKind);

          if (init_int && dest_int)
          {
            unsigned iw = LLVMGetIntTypeWidth(init_ty);
            unsigned dw = LLVMGetIntTypeWidth(ty);
            if (dw > iw)
              init = LLVMBuildSExt(mod->builder, init, ty, "decl_widen");
            else
              init = LLVMBuildTrunc(mod->builder, init, ty, "decl_trunc");
          }
          else if (init_fp && dest_fp)
          {
            if (dest_kind == LLVMDoubleTypeKind)
              init = LLVMBuildFPExt(mod->builder, init, ty, "decl_fpext");
            else
              init = LLVMBuildFPTrunc(mod->builder, init, ty, "decl_fptrunc");
          }
          else if (init_int && dest_fp)
            init = LLVMBuildSIToFP(mod->builder, init, ty, "decl_itof");
          else if (init_fp && dest_int)
            init = LLVMBuildFPToSI(mod->builder, init, ty, "decl_ftoi");
          // ptr ↔ ptr: compatible in opaque-ptr LLVM — no cast needed
        }
        ez_store(mod, init, slot);
      }
    }
  }

  void emit_const(Parser::ASTNode *n)
  {
    // Same as declaration (constness enforced by checker, not codegen)
    emit_declaration(n);
  }

  void emit_reserve(Parser::ASTNode *n)
  {
    auto sp = n->value.find(' ');
    std::string type_s = n->value.substr(0, sp);
    std::string vname = n->value.substr(sp + 1);

    // ── Determine `start` (skip Shared/TunnelBind) up-front so both the
    // __infer__ resolution and the main body can use it consistently. ──────
    size_t start0 = 0;
    if (!n->children.empty() && n->children[0]->type == "Shared")
      start0 = 1;
    std::string tunnel_bind_name;
    if (n->children.size() > start0 && n->children[start0]->type == "TunnelBind")
    {
      tunnel_bind_name = n->children[start0]->value;
      start0++;
    }

    // Type-inferred reserve: resolve type from the callee's tunnel(s)
    if (type_s == "__infer__")
    {
      if (n->children.size() > start0)
      {
        auto *init_expr = n->children[start0];

        bool plain_call = (init_expr->type == "Expression" && init_expr->children.size() >= 2 &&
                           init_expr->children[0]->token_type == Lexer::TokenType::IDENTIFIER &&
                           init_expr->children[1]->value == "(");
        bool method_call = (init_expr->type == "Expression" && init_expr->children.size() >= 4 &&
                            init_expr->children[0]->token_type == Lexer::TokenType::IDENTIFIER &&
                            init_expr->children[1]->value == "." &&
                            init_expr->children[2]->token_type == Lexer::TokenType::IDENTIFIER &&
                            init_expr->children[3]->value == "(");

        std::string call_fname;
        if (plain_call)
          call_fname = init_expr->children[0]->value;
        else if (method_call)
        {
          std::string obj_name = init_expr->children[0]->value;
          std::string method_name = init_expr->children[2]->value;
          auto vit = var_type_map.find(obj_name);
          std::string base_type = vit != var_type_map.end() ? vit->second : "";
          auto lt = base_type.find('<');
          if (lt != std::string::npos)
            base_type = base_type.substr(0, lt);
          call_fname = base_type + "_" + method_name;
        }

        if (plain_call || method_call)
        {
          auto tit = func_tunnels.find(call_fname);
          if (tit != func_tunnels.end() && !tunnel_bind_name.empty())
          {
            bool found = false;
            for (auto &tp : tit->second)
              if (tp.name == tunnel_bind_name)
              {
                type_s = tp.type;
                found = true;
                break;
              }
            if (!found)
            {
              fprintf(stderr, "[ERROR] reserve: '%s' has no tunnel output named '%s'.\n",
                      call_fname.c_str(), tunnel_bind_name.c_str());
              return;
            }
          }
          else if (tit != func_tunnels.end() && tit->second.size() == 1)
          {
            type_s = tit->second[0].type;
          }
          else if (tit != func_tunnels.end() && tit->second.size() > 1)
          {
            fprintf(stderr,
                    "[ERROR] reserve without type: '%s' has multiple tunnels — "
                    "cannot infer type; specify it explicitly or use "
                    "'reserve name << tunnel_name = %s();'.\n",
                    call_fname.c_str(), call_fname.c_str());
            return;
          }
          else
          {
            fprintf(stderr, "[ERROR] reserve without type: '%s' has no tunnel outputs.\n",
                    call_fname.c_str());
            return;
          }
        }
        else
        {
          fprintf(stderr, "[ERROR] reserve without type requires an initializer that is "
                          "a single function call.\n");
          return;
        }
      }
      else
      {
        fprintf(stderr, "[ERROR] reserve without type requires an initializer.\n");
        return;
      }
    }

    EzType ty = cshift_type(type_s);

    EzVal slot = alloca_in_entry(current_func, ty, vname.c_str());
    declare_var(vname, slot, type_s);

    size_t start = start0;

    // Extended syntax: reserve [type] name << tunnel_name [= call();]
    // TunnelBind explicitly names which tunnel output this slot receives,
    // bypassing positional/type inference.
    std::string explicit_tunnel_name = tunnel_bind_name;

    if (n->children.size() > start)
    {
      auto *init_expr = n->children[start];
      // Detect inline function call: reserve int32 x = fn(args)
      // parse_expression puts tokens [fn, (, args, )] into the Expression.
      // emit_expression_val → eval_call_expr handles this and returns nullptr
      // for void C<< functions (they tunnel instead of returning).
      // So: try the normal expression path first; if it returns a value, store
      // it. If the call is to a void (tunnel-based) C<< function, the call
      // itself writes the tunnel slot we just declared above — nothing to
      // store.
      bool is_call = (init_expr->type == "Expression" && init_expr->children.size() >= 2 &&
                      init_expr->children[0]->token_type == Lexer::TokenType::IDENTIFIER &&
                      init_expr->children[1]->value == "(");

      // Method-call initializer: reserve int32 x = obj.method(args)
      // tokens: [obj, ., method, (, ...]
      bool is_method_call = (init_expr->type == "Expression" && init_expr->children.size() >= 4 &&
                             init_expr->children[0]->token_type == Lexer::TokenType::IDENTIFIER &&
                             init_expr->children[1]->value == "." &&
                             init_expr->children[2]->token_type == Lexer::TokenType::IDENTIFIER &&
                             init_expr->children[3]->value == "(");

      if (is_method_call)
      {
        // Resolve class_fn = "ClassName_method" so we can look up func_tunnels
        std::string obj_name = init_expr->children[0]->value;
        std::string method_name = init_expr->children[2]->value;
        auto vit = var_type_map.find(obj_name);
        std::string base_type = vit != var_type_map.end() ? vit->second : "";
        auto lt = base_type.find('<');
        if (lt != std::string::npos)
          base_type = base_type.substr(0, lt);
        std::string class_fn = base_type + "_" + method_name;

        auto tit = func_tunnels.find(class_fn);
        if (tit != func_tunnels.end())
        {
          push_scope();
          if (!explicit_tunnel_name.empty())
          {
            // Bind only to the named tunnel output
            declare_var(explicit_tunnel_name, slot, type_s);
          }
          else
          {
            for (auto &tp : tit->second)
              declare_var(tp.name, slot, type_s);
          }
          EzVal call_result = emit_expression_val(init_expr, type_s);
          pop_scope();
          if (call_result)
          {
            EzType res_ty = LLVMTypeOf(call_result);
            if (LLVMGetTypeKind(res_ty) != LLVMVoidTypeKind)
              ez_store(mod, call_result, slot);
          }
          return;
        }
        // No tunnel outputs for this method — fall through to normal eval below.
      }

      if (is_call)
      {
        // For C<< tunnel functions, pre-register this slot under every
        // tunnel target name so emit_call_val passes the right pointer.
        // This handles: reserve int32 x = fn(args)  where fn tunnels
        // a variable named differently (e.g. "result").
        std::string call_fname = init_expr->children[0]->value;
        auto tit = func_tunnels.find(call_fname);
        std::vector<std::string> injected_names;
        if (tit != func_tunnels.end())
        {
          push_scope();
          if (!explicit_tunnel_name.empty())
          {
            // Bind only to the named tunnel output — verify it exists
            bool found = false;
            for (auto &tp : tit->second)
              if (tp.name == explicit_tunnel_name)
              {
                found = true;
                break;
              }
            if (!found)
            {
              fprintf(stderr, "[ERROR] reserve: '%s' has no tunnel output named '%s'.\n",
                      call_fname.c_str(), explicit_tunnel_name.c_str());
            }
            declare_var(explicit_tunnel_name, slot, type_s);
            injected_names.push_back(explicit_tunnel_name);
          }
          else
          {
            for (auto &tp : tit->second)
            {
              declare_var(tp.name, slot, type_s);
              injected_names.push_back(tp.name);
            }
          }
        }
        EzVal call_result = emit_expression_val(init_expr, type_s);
        if (tit != func_tunnels.end())
        {
          pop_scope();
        }
        // Only store if call_result is a real (non-void) value.
        // For C<< void/tunnel functions, ez_call still returns the call
        // instruction (non-null) but it has void type — storing it crashes
        // LLVM.
        if (call_result)
        {
          EzType res_ty = LLVMTypeOf(call_result);
          if (LLVMGetTypeKind(res_ty) != LLVMVoidTypeKind)
            ez_store(mod, call_result, slot);
        }
        // For C<< tunnel functions the slot is now written by the callee
      }
      else
      {
        EzVal init = emit_expression_val(init_expr, type_s);
        if (init)
          ez_store(mod, init, slot);
      }
    }
  }

  // arr[idx] = val  (also +=, -=, etc.)
  void emit_index_assignment(Parser::ASTNode *n)
  {
    // value = "arrname op"  children[0] = idx_expr, children[1] = rhs_expr
    auto sp = n->value.rfind(' ');
    std::string arr = n->value.substr(0, sp);
    std::string op = n->value.substr(sp + 1);

    if (n->children.size() < 2)
      return;

    auto eit = arena_array_elem_type.find(arr);
    if (eit == arena_array_elem_type.end())
    {
      // Not an arena array — could be a pointer subscript
      EzVal base = lookup_var(arr);
      if (!base)
        return;
      auto vit = var_type_map.find(arr);
      std::string elem_type = vit != var_type_map.end() ? vit->second : "int32";
      // strip one pointer level
      if (!elem_type.empty() && elem_type.back() == '*')
        elem_type = elem_type.substr(0, elem_type.size() - 1);
      EzType ty = cshift_type(elem_type);
      EzVal ptr_val = ez_load(mod, ez_ptr(), base, "base_ptr");
      EzVal idx = eval_expr_children(n->children[0]->children, "int64");
      if (!idx)
        return;
      if (LLVMGetIntTypeWidth(LLVMTypeOf(idx)) < 64)
        idx = LLVMBuildSExt(mod->builder, idx, ez_i64(), "idx64");
      EzVal gep_args[] = {idx};
      EzVal elem_ptr = ez_gep(mod, ty, ptr_val, gep_args, 1, "elem_ptr");
      EzVal rhs = eval_expr_children(n->children[1]->children, elem_type);
      if (!rhs)
        return;
      rhs = coerce_to_param(rhs, ty);
      if (op == "=")
      {
        ez_store(mod, rhs, elem_ptr);
        return;
      }
      EzVal lhs = ez_load(mod, ty, elem_ptr, "lhs");
      EzVal res = nullptr;
      if (op == "+=")
        res = LLVMBuildAdd(mod->builder, lhs, rhs, "add");
      else if (op == "-=")
        res = LLVMBuildSub(mod->builder, lhs, rhs, "sub");
      else if (op == "*=")
        res = LLVMBuildMul(mod->builder, lhs, rhs, "mul");
      else if (op == "/=")
        res = LLVMBuildSDiv(mod->builder, lhs, rhs, "div");
      if (res)
        ez_store(mod, res, elem_ptr);
      return;
    }

    // Arena array: compute element pointer via data+index
    std::string elem_type = eit->second;
    EzType elem_ty = cshift_type(elem_type);

    EzVal data_ptr = get_arena_data_ptr(arr);
    if (!data_ptr)
      return;
    EzVal data = ez_load(mod, ez_ptr(), data_ptr, "data");

    EzVal idx = eval_expr_children(n->children[0]->children, "int64");
    if (!idx)
      return;
    if (LLVMGetIntTypeWidth(LLVMTypeOf(idx)) < 64)
      idx = LLVMBuildSExt(mod->builder, idx, ez_i64(), "idx64");
    EzVal gep_args[] = {idx};
    EzVal elem_ptr = ez_gep(mod, elem_ty, data, gep_args, 1, "elem_ptr");

    EzVal rhs = eval_expr_children(n->children[1]->children, elem_type);
    if (!rhs)
      return;
    rhs = coerce_to_param(rhs, elem_ty);

    if (op == "=")
    {
      ez_store(mod, rhs, elem_ptr);
      return;
    }
    EzVal lhs = ez_load(mod, elem_ty, elem_ptr, "lhs");
    EzVal res = nullptr;
    if (op == "+=")
      res = LLVMBuildAdd(mod->builder, lhs, rhs, "add");
    else if (op == "-=")
      res = LLVMBuildSub(mod->builder, lhs, rhs, "sub");
    else if (op == "*=")
      res = LLVMBuildMul(mod->builder, lhs, rhs, "mul");
    else if (op == "/=")
      res = LLVMBuildSDiv(mod->builder, lhs, rhs, "div");
    else if (op == "%=")
      res = LLVMBuildSRem(mod->builder, lhs, rhs, "rem");
    if (res)
      ez_store(mod, res, elem_ptr);
  }

  void emit_assignment(Parser::ASTNode *n)
  {
    // value = "target op"
    auto sp = n->value.rfind(' ');
    std::string target = n->value.substr(0, sp);
    std::string op = n->value.substr(sp + 1);

    // Field access: "a.b"
    auto dot = target.find('.');
    EzVal ptr = nullptr;
    std::string load_type;

    if (dot != std::string::npos)
    {
      std::string base = target.substr(0, dot);
      std::string field = target.substr(dot + 1);
      EzVal base_ptr = lookup_var(base);
      if (!base_ptr)
        return;
      auto vit2 = var_type_map.find(base);
      bool is_ptr_var2 =
          (vit2 != var_type_map.end() && !vit2->second.empty() && vit2->second.back() == '*');
      EzVal gep_base2 = base_ptr;
      if (is_ptr_var2)
      {
        std::string pointed = vit2->second.substr(0, vit2->second.size() - 1);
        auto sit = struct_map.find(pointed);
        EzType struct_ty = (sit != struct_map.end()) ? sit->second.llvm_type : ez_i8();
        gep_base2 = ez_load(mod, ez_ptr_to(struct_ty), base_ptr, (base + "_deref").c_str());
      }
      ptr = gep_field(gep_base2, base, field);
      load_type = field_type_of(base, field);
    }
    else
    {
      ptr = lookup_var(target);
      // Resolve the real declared type from var_type_map.
      // Falling back to "int32" here miscompiles string/pointer variables:
      // a string alloca holds an i8* (8 bytes on x86_64) but an i32 load
      // reads only 4 bytes, producing a garbage pointer → segfault.
      auto vit = var_type_map.find(target);
      load_type = (vit != var_type_map.end()) ? vit->second : "int32";
    }
    if (!ptr)
      return;

    EzVal rhs = emit_expression_val(n->children.empty() ? nullptr : n->children[0], load_type);
    if (!rhs)
      return;

    if (op == "=")
    {
      ez_store(mod, rhs, ptr);
    }
    else
    {
      // Compound assign: load, operate, store
      EzType ty = cshift_type(load_type);
      EzVal lhs = ez_load(mod, ty, ptr, "lhs");
      EzVal result = nullptr;
      if (op == "+=")
        result =
            is_float_type(load_type) ? ez_fadd(mod, lhs, rhs, "add") : ez_add(mod, lhs, rhs, "add");
      else if (op == "-=")
        result =
            is_float_type(load_type) ? ez_fsub(mod, lhs, rhs, "sub") : ez_sub(mod, lhs, rhs, "sub");
      else if (op == "*=")
        result =
            is_float_type(load_type) ? ez_fmul(mod, lhs, rhs, "mul") : ez_mul(mod, lhs, rhs, "mul");
      else if (op == "/=")
        result = is_float_type(load_type) ? ez_fdiv(mod, lhs, rhs, "div")
                                          : ez_sdiv(mod, lhs, rhs, "div");
      else if (op == "%=")
        result = ez_srem(mod, lhs, rhs, "rem");
      if (result)
        ez_store(mod, result, ptr);
    }
  }

  // ── Call statement ────────────────────────────────────────────────────────

  void emit_call_stmt(Parser::ASTNode *n) { emit_call_val(n, /*result_name=*/""); }

  // Split a flat token vector on top-level commas (used by method-call dispatch)
  static std::vector<std::vector<Parser::ASTNode *>>
  split_args_by_comma_vec(const std::vector<Parser::ASTNode *> &tokens)
  {
    std::vector<std::vector<Parser::ASTNode *>> groups;
    std::vector<Parser::ASTNode *> cur;
    int depth = 0;
    for (auto *tok : tokens)
    {
      if (tok->value == "(" || tok->value == "[")
        depth++;
      else if (tok->value == ")" || tok->value == "]")
        depth--;
      if (tok->value == "," && depth == 0)
      {
        if (!cur.empty())
          groups.push_back(cur);
        cur.clear();
      }
      else
        cur.push_back(tok);
    }
    if (!cur.empty())
      groups.push_back(cur);
    return groups;
  }

  static std::vector<std::vector<Parser::ASTNode *>> split_args_by_comma(Parser::ASTNode *args_node)
  {
    std::vector<std::vector<Parser::ASTNode *>> groups;
    if (!args_node)
      return groups;

    // Collect the flat token list: either from the single Expression child
    // (parse_call_statement path) or directly from the node's children
    // (already-split path from eval_call_expr).
    const std::vector<Parser::ASTNode *> *flat = nullptr;
    std::vector<Parser::ASTNode *> tmp;

    if (args_node->children.size() == 1 && args_node->children[0]->type == "Expression")
    {
      // Standard path: one Expression containing all tokens+commas
      flat = &args_node->children[0]->children;
    }
    else
    {
      // Already split: each child is a separate Expression
      // Flatten into token groups directly
      for (auto *child : args_node->children)
      {
        if (child->type == "Expression")
          groups.push_back(child->children);
        else
          groups.push_back({child});
      }
      return groups;
    }

    // Split flat token list on "," at paren depth 0
    std::vector<Parser::ASTNode *> cur;
    int depth = 0;
    for (auto *tok : *flat)
    {
      if (tok->value == "(")
      {
        depth++;
        cur.push_back(tok);
      }
      else if (tok->value == ")")
      {
        depth--;
        cur.push_back(tok);
      }
      else if (tok->value == "," && depth == 0)
      {
        groups.push_back(cur);
        cur.clear();
      }
      else
      {
        cur.push_back(tok);
      }
    }
    if (!cur.empty())
      groups.push_back(cur);
    return groups;
  }

  // Helper: derive a hint string from a declared LLVM param type
  static std::string hint_from_llvm_type(LLVMTypeRef ty)
  {
    LLVMTypeKind kind = LLVMGetTypeKind(ty);
    switch (kind)
    {
    case LLVMIntegerTypeKind:
    {
      unsigned w = LLVMGetIntTypeWidth(ty);
      if (w == 64)
        return "int64";
      else if (w == 32)
        return "int32";
      else if (w == 16)
        return "int16";
      else
        return "int8";
    }
    case LLVMDoubleTypeKind:
      return "float64";
    case LLVMFloatTypeKind:
      return "float32";
    case LLVMPointerTypeKind:
      return "string";
    default:
      return "";
    }
  }

  // Coerce a value to match the declared parameter type.
  // Handles integer widening (i32 → i64), truncation (i64 → i32),
  // and int↔ptr no-ops (both are opaque ptr in LLVM opaque-ptr mode).
  EzVal coerce_to_param(EzVal v, LLVMTypeRef expected_ty)
  {
    if (!v || !expected_ty)
      return v;
    LLVMTypeRef actual_ty = LLVMTypeOf(v);
    if (actual_ty == expected_ty)
      return v;

    LLVMTypeKind exp_kind = LLVMGetTypeKind(expected_ty);
    LLVMTypeKind act_kind = LLVMGetTypeKind(actual_ty);

    // Integer ↔ integer: widen or truncate
    if (exp_kind == LLVMIntegerTypeKind && act_kind == LLVMIntegerTypeKind)
    {
      unsigned exp_w = LLVMGetIntTypeWidth(expected_ty);
      unsigned act_w = LLVMGetIntTypeWidth(actual_ty);
      if (exp_w > act_w)
        return LLVMBuildSExt(mod->builder, v, expected_ty, "coerce_widen");
      else if (exp_w < act_w)
        return LLVMBuildTrunc(mod->builder, v, expected_ty, "coerce_trunc");
    }
    // Float ↔ float
    if (exp_kind == LLVMDoubleTypeKind && act_kind == LLVMFloatTypeKind)
      return LLVMBuildFPExt(mod->builder, v, expected_ty, "coerce_fpext");
    if (exp_kind == LLVMFloatTypeKind && act_kind == LLVMDoubleTypeKind)
      return LLVMBuildFPTrunc(mod->builder, v, expected_ty, "coerce_fptrunc");
    // int → float
    if ((exp_kind == LLVMDoubleTypeKind || exp_kind == LLVMFloatTypeKind) &&
        act_kind == LLVMIntegerTypeKind)
      return LLVMBuildSIToFP(mod->builder, v, expected_ty, "coerce_itof");
    // float → int
    if (exp_kind == LLVMIntegerTypeKind &&
        (act_kind == LLVMDoubleTypeKind || act_kind == LLVMFloatTypeKind))
      return LLVMBuildFPToSI(mod->builder, v, expected_ty, "coerce_ftoi");

    // Otherwise: bitcast (handles ptr↔ptr variations etc.)
    // Only bitcast if same size; otherwise leave as-is and let verifier catch it.
    return v;
  }

  EzVal emit_call_val(Parser::ASTNode *n, const std::string &result_name)
  {
    std::string fname = n->value;

    // ── Method-call desugar: "v.push" or "self.data.push" ───────────────────
    {
      auto dot = fname.find('.');
      if (dot != std::string::npos)
      {
        // Split into parts: "self.data.push" → ["self","data","push"]
        std::vector<std::string> parts;
        std::string tmp = fname;
        while (true)
        {
          auto d = tmp.find('.');
          if (d == std::string::npos)
          {
            parts.push_back(tmp);
            break;
          }
          parts.push_back(tmp.substr(0, d));
          tmp = tmp.substr(d + 1);
        }

        std::string obj;
        std::string method;
        // Resolve the actual object and method:
        // "v.push" → obj=v, method=push (with managed type lookup)
        // "self.data.push" → load self→struct, get field "data", call method on it
        // We flatten: find the last part as method, everything before as obj chain
        method = parts.back();
        // Build resolved object variable name
        // For "self.data", we need to look up the type of "data" field on self
        EzVal resolved_obj_slot = nullptr;
        std::string resolved_type;

        if (parts.size() == 2)
        {
          obj = parts[0];
          resolved_obj_slot = lookup_var(obj);
          auto vit2 = var_type_map.find(obj);
          resolved_type = vit2 != var_type_map.end() ? vit2->second : "";
        }
        else if (parts.size() == 3)
        {
          // "self.field.method" — look up the field on self
          std::string self_var = parts[0];  // "self"
          std::string field_var = parts[1]; // "data"
          // Find type of self
          auto vit2 = var_type_map.find(self_var);
          std::string self_type = vit2 != var_type_map.end() ? vit2->second : "";
          if (!self_type.empty() && self_type.back() == '*')
            self_type = self_type.substr(0, self_type.size() - 1);
          auto sit = struct_map.find(self_type);
          if (sit != struct_map.end())
          {
            auto &layout = sit->second;
            for (size_t fi = 0; fi < layout.field_names.size(); fi++)
            {
              if (layout.field_names[fi] == field_var)
              {
                resolved_type = layout.field_types[fi];
                // GEP to the field slot within self
                EzVal self_slot = lookup_var(self_var);
                EzVal self_ptr =
                    self_slot ? ez_load(mod, ez_ptr(), self_slot, "self_ptr") : nullptr;
                if (self_ptr)
                {
                  EzVal zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
                  EzVal fidx = LLVMConstInt(LLVMInt32Type(), (unsigned)fi, 0);
                  EzVal gep_args[] = {zero, fidx};
                  resolved_obj_slot = LLVMBuildGEP2(mod->builder, layout.llvm_type, self_ptr,
                                                    gep_args, 2, "field_slot");
                }
                break;
              }
            }
          }
          if (!resolved_obj_slot)
          {
            // fall back: look for "self.field" as a declared alias
            resolved_obj_slot = lookup_var(self_var + "." + field_var);
            if (resolved_obj_slot)
            {
              auto vit3 = var_type_map.find(self_var + "." + field_var);
              if (vit3 != var_type_map.end())
                resolved_type = vit3->second;
            }
          }
        }

        if (!resolved_obj_slot)
          goto skip_method_desugar;

        {
          // Strip template args from type
          std::string base_type = resolved_type;
          {
            auto lt = base_type.find('<');
            if (lt != std::string::npos)
              base_type = base_type.substr(0, lt);
          }
          // Strip trailing *
          while (!base_type.empty() && base_type.back() == '*')
            base_type.pop_back();

          static const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
              method_map = {
                  {"Vector",
                   {{"push", "vec_push"},
                    {"get", "vec_get"},
                    {"len", "vec_len"},
                    {"pop", "vec_pop"},
                    {"clear", "vec_clear"},
                    {"set", "vec_set"},
                    {"contains", "vec_contains"},
                    {"remove", "vec_remove"}}},
                  {"HashMap",
                   {{"set", "map_set"},
                    {"get", "map_get"},
                    {"has", "map_has"},
                    {"insert", "map_insert"},
                    {"remove", "map_remove"},
                    {"len", "map_len"},
                    {"clear", "map_clear"},
                    {"contains", "map_contains"}}},
                  {"SortedVec",
                   {{"push", "svec_push"},
                    {"get", "svec_get"},
                    {"len", "svec_len"},
                    {"find", "svec_find"},
                    {"remove", "svec_remove"}}},
                  {"StringBuilder",
                   {{"append", "sb_append"},
                    {"append_char", "sb_append_char"},
                    {"append_int", "sb_append_int"},
                    {"append_float", "sb_append_float"},
                    {"build", "sb_build"},
                    {"clear", "sb_clear"},
                    {"len", "sb_len"}}},
                  {"LinkedList",
                   {{"push", "list_push"},
                    {"pop", "list_pop"},
                    {"len", "list_len"},
                    {"get", "list_get"}}},
                  {"Set",
                   {{"insert", "set_insert"},
                    {"contains", "set_contains"},
                    {"remove", "set_remove"},
                    {"len", "set_len"}}},
                  {"BitSet",
                   {{"set", "bitset_set"}, {"get", "bitset_get"}, {"clear", "bitset_clear"}}},
              };

          auto cit = method_map.find(base_type);
          if (cit != method_map.end())
          {
            auto fit2 = cit->second.find(method);
            if (fit2 != cit->second.end())
            {
              fname = fit2->second;
              auto fit3 = func_map.find(fname);
              if (fit3 == func_map.end())
              {
                fprintf(stderr,
                        "[CODEGEN ERROR] method '%s' on '%s' not declared"
                        " — add 'import ... %s(...)' to your imports\n",
                        method.c_str(), base_type.c_str(), fname.c_str());
                goto skip_method_desugar;
              }

              EzFunc *f = fit3->second;
              EzType fn_ty = LLVMGlobalGetValueType(f->fn);
              unsigned np = LLVMCountParamTypes(fn_ty);
              std::vector<LLVMTypeRef> ptypes(np);
              if (np > 0)
                LLVMGetParamTypes(fn_ty, ptypes.data());
              bool va = LLVMIsFunctionVarArg(fn_ty) != 0;

              std::vector<EzVal> call_args;
              // First arg: the managed heap ptr from the slot
              EzVal heap_ptr =
                  ez_load(mod, ez_ptr(), resolved_obj_slot, (method + "_objptr").c_str());
              call_args.push_back(heap_ptr);

              if (!n->children.empty() && n->children[0]->type == "Args")
              {
                auto arg_groups2 = split_args_by_comma(n->children[0]);
                for (auto &grp : arg_groups2)
                {
                  unsigned idx = (unsigned)call_args.size();
                  std::string ah = (idx < np) ? hint_from_llvm_type(ptypes[idx]) : "";
                  EzVal v2 = eval_expr_children(grp, ah);
                  if (v2)
                  {
                    if (!va && idx < np)
                      v2 = coerce_to_param(v2, ptypes[idx]);
                    call_args.push_back(v2);
                  }
                  else if (!va && idx < np)
                    call_args.push_back(LLVMConstNull(ptypes[idx]));
                }
              }

              // Append tunnel pointer args
              {
                auto tit = func_tunnels.find(fname);
                if (tit != func_tunnels.end())
                  for (auto &tp : tit->second)
                  {
                    EzVal tslot = lookup_var(tp.name);
                    if (!tslot)
                    {
                      EzType tty = cshift_type(tp.type);
                      tslot = alloca_in_entry(current_func, tty, tp.name.c_str());
                      declare_var(tp.name, tslot, tp.type);
                    }
                    call_args.push_back(tslot);
                  }
              }

              EzType ret_ty = LLVMGetReturnType(fn_ty);
              bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
              return ez_call(mod, f, call_args.data(), (unsigned)call_args.size(),
                             is_void ? "" : result_name.c_str());
            }
          }

          // Check zero-cost class methods for the resolved type
          {
            auto cmit = class_methods.find(base_type);
            if (cmit != class_methods.end())
            {
              std::string class_fn = base_type + "_" + method;
              auto fit_cls = func_map.find(class_fn);
              if (fit_cls != func_map.end())
              {
                EzFunc *f = fit_cls->second;
                EzType fn_ty = LLVMGlobalGetValueType(f->fn);
                unsigned np = LLVMCountParamTypes(fn_ty);
                std::vector<LLVMTypeRef> ptypes(np);
                if (np > 0)
                  LLVMGetParamTypes(fn_ty, ptypes.data());
                bool va = LLVMIsFunctionVarArg(fn_ty) != 0;

                std::vector<EzVal> call_args;
                call_args.push_back(resolved_obj_slot); // self ptr

                if (!n->children.empty() && n->children[0]->type == "Args")
                {
                  auto arg_groups3 = split_args_by_comma(n->children[0]);
                  for (auto &grp : arg_groups3)
                  {
                    unsigned idx = (unsigned)call_args.size();
                    std::string ah = (idx < np) ? hint_from_llvm_type(ptypes[idx]) : "";
                    EzVal v3 = eval_expr_children(grp, ah);
                    if (v3)
                    {
                      if (!va && idx < np)
                        v3 = coerce_to_param(v3, ptypes[idx]);
                      call_args.push_back(v3);
                    }
                    else if (!va && idx < np)
                      call_args.push_back(LLVMConstNull(ptypes[idx]));
                  }
                }

                {
                  auto tit = func_tunnels.find(class_fn);
                  if (tit != func_tunnels.end())
                    for (auto &tp : tit->second)
                    {
                      EzVal ts = lookup_var(tp.name);
                      if (!ts)
                      {
                        ts = alloca_in_entry(current_func, cshift_type(tp.type), tp.name.c_str());
                        declare_var(tp.name, ts, tp.type);
                      }
                      call_args.push_back(ts);
                    }
                }

                EzType ret_ty = LLVMGetReturnType(fn_ty);
                bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
                return ez_call(mod, f, call_args.data(), (unsigned)call_args.size(),
                               is_void ? "" : result_name.c_str());
              }
            }
          }
        }
      skip_method_desugar:;
      }
    }

    auto it = func_map.find(fname);
    if (it == func_map.end())
    {
      // Function was called but never declared via import or def.
      // Auto-declare as a variadic C extern returning void so the call
      // is emitted rather than silently dropped.  The checker already
      // warned about this; here we make it link-time rather than
      // compile-time-silent.
      EzFunc *f_auto = ez_extern(mod, fname.c_str(), ez_void(), nullptr, 0, /*vararg=*/1);
      func_map[fname] = f_auto;
      it = func_map.find(fname);
    }
    EzFunc *f = it->second;

    std::vector<EzVal> args;

    if (!n->children.empty())
    {
      // n->children[0] is the Args wrapper node from parse_call_statement
      auto *args_node = n->children[0];

      // Split args on commas (parse_expression doesn't stop at commas,
      // so all tokens land in one flat Expression inside the Args node)
      auto arg_groups = split_args_by_comma(args_node);

      // Only use the declared non-tunnel param types for hints
      auto tit = func_tunnels.find(fname);
      unsigned n_tunnels = tit != func_tunnels.end() ? (unsigned)tit->second.size() : 0;
      EzType fn_type = LLVMGlobalGetValueType(f->fn);
      unsigned total_params = LLVMCountParamTypes(fn_type);
      unsigned regular_params = total_params - n_tunnels;
      std::vector<LLVMTypeRef> param_types(total_params);
      if (total_params > 0)
        LLVMGetParamTypes(fn_type, param_types.data());

      bool is_vararg_fn = LLVMIsFunctionVarArg(fn_type) != 0;
      collect_call_args(arg_groups, param_types, regular_params, is_vararg_fn, args);
    }

    // Append hidden tunnel pointer args unconditionally: pass address of
    // caller's reserved slot. If a slot doesn't exist yet, create one.
    {
      auto tit = func_tunnels.find(fname);
      if (tit != func_tunnels.end())
      {
        for (auto &tp : tit->second)
        {
          EzVal slot = lookup_var(tp.name);
          if (!slot)
          {
            EzType ty = cshift_type(tp.type);
            slot = alloca_in_entry(current_func, ty, tp.name.c_str());
            declare_var(tp.name, slot, tp.type);
          }
          // Pass the alloca pointer directly (it IS the address)
          args.push_back(slot);
        }
      }
    }

    // ── Guard: ensure non-vararg calls have exactly the right arg count ──────
    {
      EzType fn_type2 = LLVMGlobalGetValueType(f->fn);
      bool va2 = LLVMIsFunctionVarArg(fn_type2) != 0;
      unsigned total_params2 = LLVMCountParamTypes(fn_type2);
      auto tit2 = func_tunnels.find(fname);
      unsigned n_tunnels2 = tit2 != func_tunnels.end() ? (unsigned)tit2->second.size() : 0;
      unsigned regular2 = total_params2 - n_tunnels2;

      if (!va2)
      {
        // args = [user_args... , tunnel_args...]
        // The tunnel args were just appended, so user_args_count = args.size() - n_tunnels2
        unsigned user_args_count =
            (unsigned)args.size() > n_tunnels2 ? (unsigned)args.size() - n_tunnels2 : 0;
        if (user_args_count < regular2)
        {
          std::vector<LLVMTypeRef> all_ptypes(total_params2);
          if (total_params2 > 0)
            LLVMGetParamTypes(fn_type2, all_ptypes.data());
          for (unsigned i = user_args_count; i < regular2; ++i)
            args.insert(args.begin() + i, LLVMConstNull(all_ptypes[i]));
        }
        else if (user_args_count > regular2)
        {
          unsigned excess = user_args_count - regular2;
          args.erase(args.begin() + regular2, args.begin() + regular2 + excess);
        }
      }
    }

    EzType ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(f->fn));
    bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
    return ez_call(mod, f, args.data(), (unsigned)args.size(), is_void ? "" : result_name.c_str());
  }

  // ── Tunnel ────────────────────────────────────────────────────────────────
  // tunnel expr -> type name
  // Inside a function:  evaluate expr, store into a pre-alloca'd slot
  //                     that represents the output (caller reads it).
  // Inside entry/block: evaluate expr, store into the named variable.

  void emit_tunnel(Parser::ASTNode *n)
  {
    if (n->children.size() < 2)
      return;
    auto *expr = n->children[0];
    auto *target = n->children[1];

    auto sp = target->value.find(' ');
    std::string ttype = target->value.substr(0, sp);
    std::string tname = target->value.substr(sp + 1);

    EzVal rhs = emit_expression_val(expr, ttype);
    if (!rhs)
      return;

    EzType ty = cshift_type(ttype);

    // Coerce rhs to the tunnel's declared type. Without this, a method call
    // whose return type doesn't exactly match the tunnel type (e.g. vec_get
    // returns int64 but the tunnel is declared int32) would store the wrong
    // width into the caller's slot — corrupting adjacent stack memory.
    {
      LLVMTypeRef rhs_ty = LLVMTypeOf(rhs);
      if (rhs_ty != ty)
      {
        LLVMTypeKind rk = LLVMGetTypeKind(rhs_ty);
        LLVMTypeKind tk = LLVMGetTypeKind(ty);
        bool r_fp = (rk == LLVMFloatTypeKind || rk == LLVMDoubleTypeKind);
        bool t_fp = (tk == LLVMFloatTypeKind || tk == LLVMDoubleTypeKind);
        if (rk == LLVMIntegerTypeKind && tk == LLVMIntegerTypeKind)
        {
          unsigned rw = LLVMGetIntTypeWidth(rhs_ty), tw = LLVMGetIntTypeWidth(ty);
          if (rw > tw)
            rhs = LLVMBuildTrunc(mod->builder, rhs, ty, "tunnel_trunc");
          else if (rw < tw)
            rhs = LLVMBuildSExt(mod->builder, rhs, ty, "tunnel_sext");
        }
        else if (r_fp && t_fp)
          rhs = (rk == LLVMDoubleTypeKind && tk == LLVMFloatTypeKind)
                    ? LLVMBuildFPTrunc(mod->builder, rhs, ty, "tunnel_fptrunc")
                    : LLVMBuildFPExt(mod->builder, rhs, ty, "tunnel_fpext");
        else if (rk == LLVMIntegerTypeKind && t_fp)
          rhs = LLVMBuildSIToFP(mod->builder, rhs, ty, "tunnel_itof");
        else if (r_fp && tk == LLVMIntegerTypeKind)
          rhs = LLVMBuildFPToSI(mod->builder, rhs, ty, "tunnel_ftoi");
      }
    }

    // Check if this tunnel target was registered as a hidden pointer param
    auto tit = tunnel_slots.find(tname);
    if (tit != tunnel_slots.end())
    {
      // tunnel_slots[name] holds a ptr-to-ptr (local alloca storing the
      // caller's pointer).  Load the caller's pointer then store rhs into it.
      EzVal caller_ptr = ez_load(mod, ez_ptr(), tit->second, "tptr");
      ez_store(mod, rhs, caller_ptr);
      return;
    }

    // Entry/block scope tunnel: write directly into the named variable
    EzVal ptr = lookup_var(tname);
    if (!ptr)
    {
      ptr = alloca_in_entry(current_func, ty, tname.c_str());
      declare_var(tname, ptr, ttype);
      tunnel_slots[tname] = ptr;
    }
    ez_store(mod, rhs, ptr);
  }

  void emit_break(Parser::ASTNode *)
  {
    if (!loop_stack.empty())
    {
      EzBlock *exit_b = loop_stack.back().loop_end;
      ez_br(mod, exit_b);
      // Current block is now terminated; any subsequent statements are
      // unreachable but must still be processed by the parser/checker
      EzBlock *unreachable_b = ez_block(current_func, "break.unreachable");
      current_block = unreachable_b;
      ez_use(unreachable_b);
    }
  }

  void emit_continue(Parser::ASTNode *)
  {
    if (!loop_stack.empty() && !loop_stack.back().is_switch)
    {
      EzBlock *cont_b = loop_stack.back().loop_cond;
      if (cont_b)
      {
        ez_br(mod, cont_b);
        // Current block is now terminated
        EzBlock *unreachable_b = ez_block(current_func, "continue.unreachable");
        current_block = unreachable_b;
        ez_use(unreachable_b);
      }
    }
  }

  void emit_template(Parser::ASTNode *)
  {
    // Templates don't emit code themselves; their templated definitions
    // (struct or function) are processed during forward_declare and emit_top
  }

  // ── Control flow ──────────────────────────────────────────────────────────

  void emit_if(Parser::ASTNode *n)
  {
    if (n->children.empty())
      return;

    // Checker annotated this If with a compile-time result — emit only the
    // live branch directly, with no branch instruction at all.
    if (n->meta == "always_true")
    {
      push_scope();
      if (n->children.size() > 1)
        emit_block_body(n->children[1]);
      pop_scope();
      return;
    }
    if (n->meta == "always_false")
    {
      if (n->children.size() > 2)
      {
        push_scope();
        auto *el = n->children[2];
        if (el->type == "If")
          emit_if(el);
        else
          emit_block_body(el);
        pop_scope();
      }
      return;
    }

    EzVal cond = emit_condition(n->children[0]);

    EzBlock *then_b = ez_block(current_func, "if.then");
    EzBlock *else_b = ez_block(current_func, "if.else");
    EzBlock *end_b = ez_block(current_func, "if.end");

    ez_cond_br(mod, cond, then_b, else_b);

    // then
    current_block = then_b;
    ez_use(then_b);
    if (n->children.size() > 1)
      emit_block_body(n->children[1]);
    if (!current_block_has_terminator())
      ez_br(mod, end_b);

    // else
    current_block = else_b;
    ez_use(else_b);
    if (n->children.size() > 2)
    {
      auto *el = n->children[2];
      if (el->type == "If")
        emit_if(el);
      else
        emit_block_body(el);
    }
    if (!current_block_has_terminator())
      ez_br(mod, end_b);

    current_block = end_b;
    ez_use(end_b);
  }

  void emit_while(Parser::ASTNode *n)
  {
    EzBlock *cond_b = ez_block(current_func, "while.cond");
    EzBlock *body_b = ez_block(current_func, "while.body");
    EzBlock *end_b = ez_block(current_func, "while.end");

    ez_br(mod, cond_b);

    current_block = cond_b;
    ez_use(cond_b);
    EzVal cond = (n->children.size() > 0) ? emit_condition(n->children[0]) : ez_const_bool(1);
    ez_cond_br(mod, cond, body_b, end_b);

    loop_stack.push_back({end_b, cond_b, false});
    current_block = body_b;
    ez_use(body_b);
    if (n->children.size() > 1)
      emit_block_body(n->children[1]);
    // Guard against double terminator (break already added one)
    if (!current_block_has_terminator())
      ez_br(mod, cond_b);
    loop_stack.pop_back();

    current_block = end_b;
    ez_use(end_b);
  }

  void emit_for(Parser::ASTNode *n)
  {
    // children: [init_decl, cond_expr, incr_expr, body_block]
    push_scope();
    if (n->children.size() > 0)
      emit_declaration(n->children[0]);

    EzBlock *cond_b = ez_block(current_func, "for.cond");
    EzBlock *body_b = ez_block(current_func, "for.body");
    EzBlock *incr_b = ez_block(current_func, "for.incr");
    EzBlock *end_b = ez_block(current_func, "for.end");

    ez_br(mod, cond_b);

    current_block = cond_b;
    ez_use(cond_b);
    EzVal cond = (n->children.size() > 1) ? emit_condition(n->children[1]) : ez_const_bool(1);
    ez_cond_br(mod, cond, body_b, end_b);

    loop_stack.push_back({end_b, incr_b, false});
    current_block = body_b;
    ez_use(body_b);
    if (n->children.size() > 3)
      emit_block_body(n->children[3]);
    // Guard against double terminator (break already added one)
    if (!current_block_has_terminator())
      ez_br(mod, incr_b);
    loop_stack.pop_back();

    current_block = incr_b;
    ez_use(incr_b);
    if (n->children.size() > 2)
      emit_expression(n->children[2]);
    ez_br(mod, cond_b);

    current_block = end_b;
    ez_use(end_b);
    pop_scope();
  }

  void emit_foreach(Parser::ASTNode *n)
  {
    if (!n || n->children.size() < 2)
      return;

    auto sp = n->value.find(' ');
    std::string item_type = (sp != std::string::npos) ? n->value.substr(0, sp) : "int32";
    std::string item_name = (sp != std::string::npos) ? n->value.substr(sp + 1) : n->value;

    EzVal collection = emit_expression_val(n->children[0], item_type + "*");
    if (!collection)
      return;

    EzType i32_ty = ez_i32();
    EzVal idx_slot = alloca_in_entry(current_func, i32_ty, "__foreach_idx");
    ez_store(mod, ez_const_int(i32_ty, 0), idx_slot);

    EzType elem_ty = cshift_type(item_type);
    EzVal item_slot = alloca_in_entry(current_func, elem_ty, item_name.c_str());

    EzBlock *cond_b = ez_block(current_func, "foreach.cond");
    EzBlock *body_b = ez_block(current_func, "foreach.body");
    EzBlock *end_b = ez_block(current_func, "foreach.end");

    ez_br(mod, cond_b);

    current_block = cond_b;
    ez_use(cond_b);
    EzVal idx_val = ez_load(mod, i32_ty, idx_slot, "idx");
    EzVal limit = ez_const_int(i32_ty, 0x7FFFFFFF);
    EzVal cond = ez_slt(mod, idx_val, limit, "foreach.cond");
    ez_cond_br(mod, cond, body_b, end_b);

    loop_stack.push_back({end_b, cond_b, false});
    current_block = body_b;
    ez_use(body_b);
    push_scope();
    declare_var(item_name, item_slot, item_type);

    EzVal gep_idx[1] = {idx_val};
    EzVal elem_ptr = ez_gep(mod, elem_ty, collection, gep_idx, 1, "elem_ptr");
    EzVal elem_val = ez_load(mod, elem_ty, elem_ptr, item_name.c_str());
    ez_store(mod, elem_val, item_slot);

    emit_block_body(n->children[1]);
    pop_scope();

    // Guard against double terminator (break already added one)
    if (!current_block_has_terminator())
    {
      EzVal idx_next = ez_add(mod, idx_val, ez_const_int(i32_ty, 1), "idx.next");
      ez_store(mod, idx_next, idx_slot);
      ez_br(mod, cond_b);
    }
    loop_stack.pop_back();

    current_block = end_b;
    ez_use(end_b);
  }

  void emit_switch(Parser::ASTNode *n)
  {
    if (n->children.empty())
      return;

    // ── Voided-state guard: fully static, zero runtime cost ─────────────────
    // The checker has already resolved whether the switched variable was
    // voided or valid at this point and stored the result in n->meta:
    //   "voided" → unconditional br to the 'case voided' block
    //   "valid"  → unconditional br to the 'case valid' block
    //   ""       → regular numeric switch (handled below)
    if (!n->meta.empty())
    {
      // ── meta == "unknown": runtime branch on __track_validity_<var> ──────
      if (n->meta == "unknown")
      {
        // Get the switched variable name from the first expression token
        std::string switched_var;
        if (!n->children[0]->children.empty())
          switched_var = n->children[0]->children[0]->value;

        // Load the runtime validity flag
        EzVal vflag_slot =
            switched_var.empty() ? nullptr : lookup_var("__track_validity_" + switched_var);
        EzVal is_valid = vflag_slot ? ez_load(mod, ez_i1(), vflag_slot, "is_valid")
                                    : LLVMConstInt(ez_i1(), 1, 0); // no flag → assume valid

        Parser::ASTNode *valid_case = nullptr, *voided_case = nullptr;
        for (size_t i = 1; i < n->children.size(); ++i)
        {
          auto *c = n->children[i];
          if (c->type == "Case" && c->value == "valid")
            valid_case = c;
          if (c->type == "Case" && c->value == "voided")
            voided_case = c;
        }

        EzBlock *end_b = ez_block(current_func, "sw.end");
        EzBlock *valid_b = valid_case ? ez_block(current_func, "sw.valid") : end_b;
        EzBlock *voided_b = voided_case ? ez_block(current_func, "sw.voided") : end_b;

        // i1 true → valid, false → voided
        LLVMBuildCondBr(mod->builder, is_valid, valid_b->bb, voided_b->bb);

        loop_stack.push_back({end_b, nullptr, true});

        auto emit_arm = [&](EzBlock *blk, Parser::ASTNode *arm)
        {
          if (!arm)
            return;
          current_block = blk;
          ez_use(blk);
          for (auto *stmt : arm->children)
            emit_stmt(stmt);
          if (!current_block_has_terminator())
            ez_br(mod, end_b);
        };
        emit_arm(valid_b, valid_case);
        emit_arm(voided_b, voided_case);

        loop_stack.pop_back();
        current_block = end_b;
        ez_use(end_b);
        return;
      }

      // ── meta == "voided" or "valid": static, zero-cost ───────────────────
      bool take_voided = (n->meta == "voided");

      // Find the target case body
      Parser::ASTNode *target_case = nullptr;
      std::string target_val = take_voided ? "voided" : "valid";
      for (size_t i = 1; i < n->children.size(); ++i)
      {
        auto *c = n->children[i];
        if (c->type == "Case" && c->value == target_val)
        {
          target_case = c;
          break;
        }
      }
      // If the expected arm is missing, emit nothing (checker already warned)
      if (!target_case)
        return;

      // Emit the body inline — no branch overhead at all
      EzBlock *end_b = ez_block(current_func, "sw.end");
      loop_stack.push_back({end_b, nullptr, true});

      for (auto *stmt : target_case->children)
        emit_stmt(stmt);
      if (!current_block_has_terminator())
        ez_br(mod, end_b);

      loop_stack.pop_back();
      current_block = end_b;
      ez_use(end_b);
      return;
    }

    // ── Regular numeric switch ───────────────────────────────────────────────
    EzVal switched = emit_expression_val(n->children[0], "int32");

    std::vector<Parser::ASTNode *> cases;
    Parser::ASTNode *default_node = nullptr;
    for (size_t i = 1; i < n->children.size(); ++i)
    {
      auto *c = n->children[i];
      if (c->type == "Case")
        cases.push_back(c);
      if (c->type == "Default")
        default_node = c;
    }

    EzBlock *end_b = ez_block(current_func, "sw.end");
    std::vector<EzBlock *> case_blocks;
    for (auto *c : cases)
      case_blocks.push_back(ez_block(current_func, ("sw.case." + c->value).c_str()));
    EzBlock *default_b = default_node ? ez_block(current_func, "sw.default") : end_b;

    LLVMValueRef sw =
        LLVMBuildSwitch(mod->builder, switched, default_b->bb, (unsigned)cases.size());
    for (size_t i = 0; i < cases.size(); ++i)
    {
      const std::string &val_s = cases[i]->value;
      long long ival = 0;
      try
      {
        ival = std::stoll(val_s);
      }
      catch (...)
      {
      }
      LLVMAddCase(sw, LLVMConstInt(ez_i32(), (unsigned long long)ival, 1), case_blocks[i]->bb);
    }

    loop_stack.push_back({end_b, nullptr, true});

    for (size_t i = 0; i < cases.size(); ++i)
    {
      current_block = case_blocks[i];
      ez_use(case_blocks[i]);
      for (auto *stmt : cases[i]->children)
        emit_stmt(stmt);
      if (!current_block_has_terminator())
        ez_br(mod, end_b);
    }
    if (default_node)
    {
      current_block = default_b;
      ez_use(default_b);
      for (auto *stmt : default_node->children)
        emit_stmt(stmt);
      if (!current_block_has_terminator())
        ez_br(mod, end_b);
    }

    loop_stack.pop_back();
    current_block = end_b;
    ez_use(end_b);
  }

  // ── Expression evaluation ─────────────────────────────────────────────────
  // Returns an EzVal (SSA value). hint_type guides numeric literal sizing.

  EzVal emit_expression_val(Parser::ASTNode *n, const std::string &hint_type)
  {
    if (!n)
      return nullptr;

    // Flat expression node: walk children and fold into a value
    if (n->type == "Expression")
      return eval_expr_children(n->children, hint_type);
    else if (n->type == "Token")
      return eval_token(n, hint_type);
    return nullptr;
  }

  // Condition: evaluate and ensure i1
  EzVal emit_condition(Parser::ASTNode *n)
  {
    EzVal v = emit_expression_val(n, "");
    if (!v)
      return ez_const_bool(0);
    EzType ty = LLVMTypeOf(v);
    if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(ty) == 1)
      return v;
    // Compare != 0
    return ez_ne(mod, v, LLVMConstInt(ty, 0, 0), "tobool");
  }

  // Expression used as a statement (for incr expressions in for-loops etc.)
  void emit_expression(Parser::ASTNode *n) { emit_expression_val(n, "int32"); }

  // Walk a flat token list and build a value.
  // We handle:  literals, identifiers (load), unary -, unary !, binary ops,
  // function calls. This is a simple left-to-right evaluator for the flat
  // expression AST.
  EzVal eval_expr_children(const std::vector<Parser::ASTNode *> &tokens, const std::string &hint)
  {
    if (tokens.empty())
      return nullptr;

    // Handle 'move VAR' — just load the variable value; voiding is static
    if (tokens.size() == 2 && tokens[0]->value == "move" && tokens[1]->type == "Token")
    {
      return eval_token(tokens[1], hint);
    }

    // Detect function call: IDENT ( args... )
    // Only dispatch when the entire token sequence is a call (not call OP expr).
    // For `rand() % 10`, find_binary_op will find the `%` and handle it first.
    if (tokens.size() >= 3 && tokens[0]->type == "Token" &&
        tokens[0]->token_type == Lexer::TokenType::IDENTIFIER && tokens[1]->value == "(")
    {
      // Check that no top-level binary operator exists outside the call parens
      int op_outside = find_binary_op(tokens);
      if (op_outside <= 0)
        return eval_call_expr(tokens, hint);
      // else: fall through to binary-op handler below
    }

    // Single token
    if (tokens.size() == 1)
      return eval_token(tokens[0], hint);

    // Unary minus: - expr
    if (tokens.size() >= 2 && tokens[0]->value == "-")
    {
      std::vector<Parser::ASTNode *> inner(tokens.begin() + 1, tokens.end());
      EzVal v = eval_expr_children(inner, hint);
      if (!v)
        return nullptr;
      return is_float_type(hint) ? ez_fneg(mod, v, "neg") : ez_neg(mod, v, "neg");
    }

    if (tokens.size() >= 2 && tokens[0]->value == "!")
    {
      std::vector<Parser::ASTNode *> inner(tokens.begin() + 1, tokens.end());
      EzVal v = eval_expr_children(inner, hint);
      if (!v)
        return nullptr;
      EzType ty = LLVMTypeOf(v);
      EzVal as_bool;
      if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(ty) == 1)
        as_bool = v;
      else
        as_bool = ez_ne(mod, v, LLVMConstInt(ty, 0, 0), "tobool");
      return LLVMBuildNot(mod->builder, as_bool, "lnot");
    }

    if (tokens.size() >= 2 && tokens[0]->value == "&")
    {
      if (tokens.size() == 2 && tokens[1]->token_type == Lexer::TokenType::IDENTIFIER)
      {
        const std::string &vname = tokens[1]->value;
        EzVal slot = lookup_var(vname);
        if (slot)
        {
          // For managed container types (Vector, HashMap, …) the slot holds a
          // heap pointer. `&v` in C<< means "pass v to a function taking T*",
          // which is just the heap pointer itself — not the address of the slot.
          auto vit = var_type_map.find(vname);
          if (vit != var_type_map.end())
          {
            std::string btype = vit->second;
            auto lt = btype.find('<');
            if (lt != std::string::npos)
              btype = btype.substr(0, lt);
            if (managed_free_fns().count(btype))
              return ez_load(mod, ez_ptr(), slot, (vname + "_ptr").c_str());
          }
          return slot; // alloca pointer IS the address for regular vars
        }
      }
      if (tokens.size() == 4 && tokens[1]->token_type == Lexer::TokenType::IDENTIFIER &&
          tokens[2]->value == "." && tokens[3]->token_type == Lexer::TokenType::IDENTIFIER)
      {
        EzVal field_ptr =
            gep_field(lookup_var(tokens[1]->value), tokens[1]->value, tokens[3]->value);
        if (field_ptr)
          return field_ptr;
      }
    }

    if (tokens.size() >= 2 && tokens[0]->value == "*" &&
        (tokens.size() == 2 || tokens[1]->value != "*"))
    {
      std::vector<Parser::ASTNode *> inner(tokens.begin() + 1, tokens.end());
      EzVal ptr = eval_expr_children(inner, hint + "*");
      if (ptr)
      {
        EzType pointee = hint.empty() ? ez_i32() : cshift_type(hint);
        return ez_load(mod, pointee, ptr, "deref");
      }
    }

    // Binary expression: lhs op rhs
    // Find rightmost low-precedence operator (simple left-associative)
    int op_idx = find_binary_op(tokens);
    if (op_idx > 0)
    {
      std::vector<Parser::ASTNode *> lhs_tokens(tokens.begin(), tokens.begin() + op_idx);
      std::vector<Parser::ASTNode *> rhs_tokens(tokens.begin() + op_idx + 1, tokens.end());
      EzVal lhs = eval_expr_children(lhs_tokens, hint);
      EzVal rhs = eval_expr_children(rhs_tokens, hint);
      if (!lhs || !rhs)
        return lhs ? lhs : rhs;
      EzVal binop_result = apply_binop(tokens[op_idx]->value, lhs, rhs, hint);
      // Coerce result back to the hint type when promotion widened it.
      // e.g. float32 * int32 → double internally, but if hint=="float32", truncate.
      if (binop_result && !hint.empty())
      {
        LLVMTypeRef res_ty = LLVMTypeOf(binop_result);
        LLVMTypeRef hint_ty = cshift_type(hint);
        if (hint_ty && res_ty != hint_ty)
        {
          LLVMTypeKind rk = LLVMGetTypeKind(res_ty);
          LLVMTypeKind hk = LLVMGetTypeKind(hint_ty);
          bool r_fp = (rk == LLVMFloatTypeKind || rk == LLVMDoubleTypeKind);
          bool h_fp = (hk == LLVMFloatTypeKind || hk == LLVMDoubleTypeKind);
          if (r_fp && h_fp && rk == LLVMDoubleTypeKind && hk == LLVMFloatTypeKind)
            binop_result = LLVMBuildFPTrunc(mod->builder, binop_result, hint_ty, "res_f32");
          else if (r_fp && h_fp && rk == LLVMFloatTypeKind && hk == LLVMDoubleTypeKind)
            binop_result = LLVMBuildFPExt(mod->builder, binop_result, hint_ty, "res_f64");
          else if (rk == LLVMIntegerTypeKind && h_fp)
            binop_result = LLVMBuildSIToFP(mod->builder, binop_result, hint_ty, "itof");
        }
      }
      return binop_result;
    }

    // Strip outer parens
    if (tokens.front()->value == "(" && tokens.back()->value == ")")
    {
      std::vector<Parser::ASTNode *> inner(tokens.begin() + 1, tokens.end() - 1);
      return eval_expr_children(inner, hint);
    }

    // Array index access: IDENT [ expr ]
    if (tokens.size() >= 4 && tokens[0]->type == "Token" && tokens[1]->value == "[")
    {
      std::string arr = tokens[0]->value;
      // Find matching ]
      std::vector<Parser::ASTNode *> idx_toks;
      for (size_t i = 2; i < tokens.size() - 1; ++i)
        idx_toks.push_back(tokens[i]);

      // arr[[:]] — length-of operator
      if (idx_toks.size() == 1 && idx_toks[0]->value == "[:]")
      {
        auto eit = arena_array_elem_type.find(arr);
        if (eit != arena_array_elem_type.end())
        {
          EzVal len_ptr = get_arena_len_ptr(arr);
          if (len_ptr)
            return ez_load(mod, ez_i64(), len_ptr, "arr_len");
        }
        return nullptr;
      }

      EzVal idx = eval_expr_children(idx_toks, "int64");
      if (!idx)
        return nullptr;
      // Widen index to i64 if needed
      EzType idx_ty = LLVMTypeOf(idx);
      if (LLVMGetTypeKind(idx_ty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(idx_ty) < 64)
        idx = LLVMBuildSExt(mod->builder, idx, ez_i64(), "idx64");

      // Check if it's an arena array
      auto eit = arena_array_elem_type.find(arr);
      if (eit != arena_array_elem_type.end())
      {
        EzVal data_ptr = get_arena_data_ptr(arr);
        if (!data_ptr)
          return nullptr;
        EzType elem_ty = cshift_type(eit->second);
        EzVal data = ez_load(mod, ez_ptr(), data_ptr, "data");
        EzVal gep_idx[] = {idx};
        EzVal elem_ptr = ez_gep(mod, elem_ty, data, gep_idx, 1, "elem_ptr");
        return ez_load(mod, elem_ty, elem_ptr, "elem");
      }
    }

    // Field access: IDENT . FIELD  (or chained: a.b.c)
    // ── Chained field method: self.field.method(args) e.g. self.data.len(),
    //    self.data.get(i), self.data.push(x) ───────────────────────────────
    // Tokens: [self, ., field, ., method, (, args..., )]
    if (tokens.size() >= 7 && tokens[1]->value == "." && tokens[3]->value == "." &&
        tokens[5]->value == "(")
    {
      std::string self_var = tokens[0]->value;  // e.g. "self"
      std::string field_var = tokens[2]->value; // e.g. "data"
      std::string method = tokens[4]->value;    // e.g. "len", "get", "push"

      auto vit = var_type_map.find(self_var);
      std::string self_type = vit != var_type_map.end() ? vit->second : "";
      if (!self_type.empty() && self_type.back() == '*')
        self_type = self_type.substr(0, self_type.size() - 1);
      auto sit = struct_map.find(self_type);

      if (sit != struct_map.end())
      {
        auto &layout = sit->second;
        for (size_t fi = 0; fi < layout.field_names.size(); fi++)
        {
          if (layout.field_names[fi] != field_var)
            continue;

          std::string field_type = layout.field_types[fi];
          EzVal self_slot = lookup_var(self_var);
          if (!self_slot)
            break;
          EzVal self_ptr = ez_load(mod, ez_ptr(), self_slot, "self_ptr");
          EzVal zero = LLVMConstInt(LLVMInt32Type(), 0, 0);
          EzVal fidx = LLVMConstInt(LLVMInt32Type(), (unsigned)fi, 0);
          EzVal gep_args[] = {zero, fidx};
          EzVal field_slot =
              LLVMBuildGEP2(mod->builder, layout.llvm_type, self_ptr, gep_args, 2, "field_slot");

          // T[] arena array field — only .len() is supported (raw T[] fields
          // don't carry length metadata when embedded in a struct; prefer
          // Vector<T> for class/struct fields that need length-aware access).
          if (field_type.size() >= 2 && field_type.substr(field_type.size() - 2) == "[]")
          {
            if (method == "len")
            {
              // field_slot holds just a T* (data pointer) — no length is
              // available. This previously produced a bogus uint64 by
              // accident; surface a clear runtime-impossible case instead.
              fprintf(stderr,
                      "[CODEGEN ERROR] '%s.%s' is a T[] struct field — "
                      ".len() is not supported on raw arrays inside "
                      "structs/classes. Use Vector<T> instead.\n",
                      self_var.c_str(), field_var.c_str());
              return LLVMConstInt(ez_i64(), 0, 0);
            }
            break;
          }

          // Strip template args / pointer suffix to get the container type
          std::string base_type = field_type;
          {
            auto lt = base_type.find('<');
            if (lt != std::string::npos)
              base_type = base_type.substr(0, lt);
          }
          while (!base_type.empty() && base_type.back() == '*')
            base_type.pop_back();

          static const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
              method_map = {
                  {"Vector",
                   {{"push", "vec_push"},
                    {"get", "vec_get"},
                    {"len", "vec_len"},
                    {"pop", "vec_pop"},
                    {"clear", "vec_clear"},
                    {"set", "vec_set"},
                    {"contains", "vec_contains"},
                    {"remove", "vec_remove"}}},
                  {"HashMap",
                   {{"set", "map_set"},
                    {"get", "map_get"},
                    {"has", "map_has"},
                    {"insert", "map_insert"},
                    {"remove", "map_remove"},
                    {"len", "map_len"},
                    {"clear", "map_clear"},
                    {"contains", "map_contains"}}},
                  {"SortedVec",
                   {{"push", "svec_push"},
                    {"get", "svec_get"},
                    {"len", "svec_len"},
                    {"find", "svec_find"},
                    {"remove", "svec_remove"}}},
                  {"StringBuilder",
                   {{"append", "sb_append"},
                    {"append_char", "sb_append_char"},
                    {"append_int", "sb_append_int"},
                    {"append_float", "sb_append_float"},
                    {"build", "sb_build"},
                    {"clear", "sb_clear"},
                    {"len", "sb_len"}}},
                  {"LinkedList",
                   {{"push", "list_push"},
                    {"pop", "list_pop"},
                    {"len", "list_len"},
                    {"get", "list_get"}}},
                  {"Set",
                   {{"insert", "set_insert"},
                    {"contains", "set_contains"},
                    {"remove", "set_remove"},
                    {"len", "set_len"}}},
                  {"BitSet",
                   {{"set", "bitset_set"}, {"get", "bitset_get"}, {"clear", "bitset_clear"}}},
              };

          auto cit = method_map.find(base_type);
          if (cit == method_map.end())
            break;
          auto fit2 = cit->second.find(method);
          if (fit2 == cit->second.end())
            break;

          std::string fn_name = fit2->second;
          auto fit3 = func_map.find(fn_name);
          if (fit3 == func_map.end())
          {
            fprintf(stderr,
                    "[CODEGEN ERROR] method '%s' on '%s' not declared"
                    " — add 'import ... %s(...)' to your imports\n",
                    method.c_str(), base_type.c_str(), fn_name.c_str());
            break;
          }

          EzFunc *f = fit3->second;
          EzType fn_ty = LLVMGlobalGetValueType(f->fn);
          unsigned np = LLVMCountParamTypes(fn_ty);
          std::vector<LLVMTypeRef> ptypes(np);
          if (np > 0)
            LLVMGetParamTypes(fn_ty, ptypes.data());
          bool va = LLVMIsFunctionVarArg(fn_ty) != 0;

          std::vector<EzVal> call_args;
          EzVal heap_ptr = ez_load(mod, ez_ptr(), field_slot, (field_var + "_objptr").c_str());
          call_args.push_back(heap_ptr);

          std::vector<Parser::ASTNode *> at(tokens.begin() + 6, tokens.end() - 1);
          auto arg_groups5 = split_args_by_comma_vec(at);
          for (auto &grp : arg_groups5)
          {
            unsigned idx = (unsigned)call_args.size();
            std::string ah = (idx < np) ? hint_from_llvm_type(ptypes[idx]) : "";
            EzVal v5 = eval_expr_children(grp, ah);
            if (v5)
            {
              if (!va && idx < np)
                v5 = coerce_to_param(v5, ptypes[idx]);
              call_args.push_back(v5);
            }
            else if (!va && idx < np)
              call_args.push_back(LLVMConstNull(ptypes[idx]));
          }

          EzType ret_ty = LLVMGetReturnType(fn_ty);
          bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
          return ez_call(mod, f, call_args.data(), (unsigned)call_args.size(),
                         is_void ? "" : (method + "_result").c_str());
        }
      }
    }

    if (tokens.size() >= 3 && tokens[1]->value == ".")
    {
      std::string base = tokens[0]->value;
      std::string member = tokens[2]->value;

      // ── Method call: v.method(args...) ──────────────────────────────────
      // Tokens: [base, ., method, (, args..., )]
      // Desugars to: ctn_method(&base, args...)
      if (tokens.size() >= 5 && tokens[3]->value == "(")
      {
        // Determine the base variable's declared type
        auto vit = var_type_map.find(base);
        std::string base_type = vit != var_type_map.end() ? vit->second : "";
        // Strip template params: "Vector<int32>" → "Vector"
        std::string base_stripped = base_type;
        {
          auto lt = base_stripped.find('<');
          if (lt != std::string::npos)
            base_stripped = base_stripped.substr(0, lt);
        }

        // Method → C function name mapping
        // Managed types: Vector, HashMap, SortedVec, StringBuilder, LinkedList, ...
        // Arena arrays (T[]): .len(), .push(x)
        struct MethodEntry
        {
          std::string type_prefix;
          std::string method;
          std::string fn;
          bool needs_addr;
        };
        static const std::vector<MethodEntry> methods = {
            // Vector<T>
            {"Vector", "push", "vec_push", true},
            {"Vector", "get", "vec_get", true},
            {"Vector", "len", "vec_len", true},
            {"Vector", "pop", "vec_pop", true},
            {"Vector", "clear", "vec_clear", true},
            {"Vector", "set", "vec_set", true},
            {"Vector", "contains", "vec_contains", true},
            {"Vector", "remove", "vec_remove", true},
            // HashMap<K,V>
            {"HashMap", "set", "map_set", true},
            {"HashMap", "get", "map_get", true},
            {"HashMap", "has", "map_has", true},
            {"HashMap", "remove", "map_remove", true},
            {"HashMap", "len", "map_len", true},
            {"HashMap", "clear", "map_clear", true},
            // SortedVec<T>
            {"SortedVec", "push", "svec_push", true},
            {"SortedVec", "get", "svec_get", true},
            {"SortedVec", "len", "svec_len", true},
            {"SortedVec", "find", "svec_find", true},
            {"SortedVec", "remove", "svec_remove", true},
            // StringBuilder
            {"StringBuilder", "append", "sb_append", true},
            {"StringBuilder", "append_char", "sb_append_char", true},
            {"StringBuilder", "append_int", "sb_append_int", true},
            {"StringBuilder", "append_float", "sb_append_float", true},
            {"StringBuilder", "build", "sb_build", true},
            {"StringBuilder", "clear", "sb_clear", true},
            {"StringBuilder", "len", "sb_len", true},
            // LinkedList<T>
            {"LinkedList", "push", "list_push", true},
            {"LinkedList", "pop", "list_pop", true},
            {"LinkedList", "len", "list_len", true},
            {"LinkedList", "get", "list_get", true},
            // Set<T>
            {"Set", "insert", "set_insert", true},
            {"Set", "contains", "set_contains", true},
            {"Set", "remove", "set_remove", true},
            {"Set", "len", "set_len", true},
            // BitSet
            {"BitSet", "set", "bitset_set", true},
            {"BitSet", "get", "bitset_get", true},
            {"BitSet", "clear", "bitset_clear", true},
        };

        // Check arena arrays (T[]): base has entry in arena_array_elem_type
        if (base_stripped.empty() && arena_array_elem_type.count(base))
        {
          // T[] methods
          if (member == "len")
          {
            EzVal len_ptr = get_arena_len_ptr(base);
            if (len_ptr)
              return ez_load(mod, ez_i64(), len_ptr, "arr_len");
          }
          // .push(x) — same as <<; collect single arg and do array append
          // (complex: for now fall through to struct field handler)
        }

        // ── Zero-cost class method dispatch: obj.method(args) ──────────────
        {
          auto cmit = class_methods.find(base_stripped);
          if (cmit != class_methods.end())
          {
            std::string class_fn = base_stripped + "_" + member;
            auto fit_cls = func_map.find(class_fn);
            if (fit_cls != func_map.end())
            {
              EzVal self_slot = lookup_var(base);
              if (self_slot)
              {
                EzFunc *f = fit_cls->second;
                EzType fn_ty = LLVMGlobalGetValueType(f->fn);
                unsigned np = LLVMCountParamTypes(fn_ty);
                std::vector<LLVMTypeRef> ptypes(np);
                if (np > 0)
                  LLVMGetParamTypes(fn_ty, ptypes.data());
                bool va = LLVMIsFunctionVarArg(fn_ty) != 0;

                std::vector<EzVal> call_args;
                call_args.push_back(self_slot);

                if (tokens.size() >= 5 && tokens[3]->value == "(")
                {
                  std::vector<Parser::ASTNode *> at(tokens.begin() + 4, tokens.end() - 1);
                  auto arg_groups4 = split_args_by_comma_vec(at);
                  for (auto &grp : arg_groups4)
                  {
                    unsigned idx = (unsigned)call_args.size();
                    std::string ah = (idx < np) ? hint_from_llvm_type(ptypes[idx]) : "";
                    EzVal v4 = eval_expr_children(grp, ah);
                    if (v4)
                    {
                      if (!va && idx < np)
                        v4 = coerce_to_param(v4, ptypes[idx]);
                      call_args.push_back(v4);
                    }
                    else if (!va && idx < np)
                      call_args.push_back(LLVMConstNull(ptypes[idx]));
                  }
                }

                // Append hidden tunnel pointer args (same ABI as collect_call_args
                // for regular C<< functions with `tunnel` outputs).
                {
                  auto tit = func_tunnels.find(class_fn);
                  if (tit != func_tunnels.end())
                  {
                    for (auto &tp : tit->second)
                    {
                      EzVal tslot = lookup_var(tp.name);
                      if (!tslot)
                      {
                        EzType tty = cshift_type(tp.type);
                        tslot = alloca_in_entry(current_func, tty, tp.name.c_str());
                        declare_var(tp.name, tslot, tp.type);
                      }
                      call_args.push_back(tslot);
                    }
                  }
                }

                EzType ret_ty = LLVMGetReturnType(fn_ty);
                bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
                return ez_call(mod, f, call_args.data(), (unsigned)call_args.size(),
                               is_void ? "" : (member + "_result").c_str());
              }
            }
          }
        }

        // Look up method in table
        const MethodEntry *entry = nullptr;
        for (auto &m : methods)
          if (m.type_prefix == base_stripped && m.method == member)
          {
            entry = &m;
            break;
          }

        if (entry)
        {
          auto fit = func_map.find(entry->fn);
          if (fit != func_map.end())
          {
            // Collect args from tokens[4..n-1] (skip closing ')')
            std::vector<Parser::ASTNode *> arg_toks(tokens.begin() + 4, tokens.end() - 1);

            // Build arg list: first arg is &base (the managed ptr from its slot)
            std::vector<EzVal> args;

            // For managed types, &v = the stored heap pointer (not alloca addr)
            EzVal base_slot = lookup_var(base);
            if (!base_slot)
              goto skip_method;

            if (entry->needs_addr)
            {
              // Managed var slot holds ptr → pass it directly (it IS the *T)
              EzVal heap_ptr = ez_load(mod, ez_ptr(), base_slot, (base + "_ptr").c_str());
              args.push_back(heap_ptr);
            }

            // Parse additional args with commas as separators
            if (!arg_toks.empty())
            {
              // Split by top-level commas
              auto arg_groups = split_args_by_comma_vec(arg_toks);
              // Get function param types for coercion hints
              EzType fn_ty = LLVMGlobalGetValueType(fit->second->fn);
              unsigned np = LLVMCountParamTypes(fn_ty);
              std::vector<LLVMTypeRef> ptypes(np);
              if (np > 0)
                LLVMGetParamTypes(fn_ty, ptypes.data());
              bool va = LLVMIsFunctionVarArg(fn_ty) != 0;

              for (auto &grp : arg_groups)
              {
                unsigned idx = (unsigned)args.size();
                std::string ahint = (idx < np) ? hint_from_llvm_type(ptypes[idx]) : "";
                EzVal v = eval_expr_children(grp, ahint);
                if (v)
                {
                  if (!va && idx < np)
                    v = coerce_to_param(v, ptypes[idx]);
                  args.push_back(v);
                }
                else if (!va && idx < np)
                  args.push_back(LLVMConstNull(ptypes[idx]));
              }
            }

            {
              EzType ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(fit->second->fn));
              bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
              return ez_call(mod, fit->second, args.data(), (unsigned)args.size(),
                             is_void ? "" : (member + "_result").c_str());
            }
          }
        }
      skip_method:;
      }

      // ── Field/property access: v.field ───────────────────────────────────
      if (member == "len" && arena_array_elem_type.count(base))
      {
        EzVal len_ptr = get_arena_len_ptr(base);
        if (len_ptr)
          return ez_load(mod, ez_i64(), len_ptr, "arr_len");
      }

      EzVal base_ptr = lookup_var(base);
      if (base_ptr)
      {
        auto vit = var_type_map.find(base);
        bool is_ptr_var =
            (vit != var_type_map.end() && !vit->second.empty() && vit->second.back() == '*');
        EzVal gep_base = base_ptr;
        if (is_ptr_var)
        {
          std::string pointed = vit->second.substr(0, vit->second.size() - 1);
          auto sit = struct_map.find(pointed);
          EzType struct_ty = (sit != struct_map.end()) ? sit->second.llvm_type : ez_i8();
          gep_base = ez_load(mod, ez_ptr_to(struct_ty), base_ptr, (base + "_deref").c_str());
        }
        EzVal field_ptr = gep_field(gep_base, base, member);
        std::string ftype = field_type_of(base, member);
        EzType fty = cshift_type(ftype.empty() ? hint : ftype);
        return ez_load(mod, fty, field_ptr, member.c_str());
      }
    }

    // Fallback: evaluate first token
    return eval_token(tokens[0], hint);
  }

  // Find the index of the rightmost binary operator (lowest precedence first)
  int find_binary_op(const std::vector<Parser::ASTNode *> &tokens)
  {
    // Precedence levels (lower number = lower precedence, split last)
    static const std::vector<std::vector<std::string>> prec = {
        {"||"}, {"&&"}, {"==", "!="}, {"<", ">", "<=", ">="}, {"+", "-"}, {"*", "/", "%"}};
    int depth = 0;
    for (auto &level : prec)
    {
      depth = 0; // reset for each precedence scan
      // scan right-to-left at depth 0
      for (int i = (int)tokens.size() - 1; i >= 1; --i)
      {
        const std::string &v = tokens[i]->value;
        if (v == ")")
          depth++;
        if (v == "(")
          depth--;
        if (depth == 0)
        {
          for (auto &op : level)
            if (v == op)
              return i;
        }
      }
    }
    return -1;
  }

  EzVal apply_binop(const std::string &op, EzVal lhs, EzVal rhs, const std::string &hint)
  {
    // ── Automatic operand coercion ─────────────────────────────────────────
    // LLVM requires both operands to have identical types. We unify them here
    // so the user can write float32 + float64, int32 < uint64, float > int, etc.
    {
      LLVMTypeRef lt = LLVMTypeOf(lhs);
      LLVMTypeRef rt = LLVMTypeOf(rhs);
      if (lt != rt)
      {
        LLVMTypeKind lk = LLVMGetTypeKind(lt);
        LLVMTypeKind rk = LLVMGetTypeKind(rt);
        bool l_fp = (lk == LLVMFloatTypeKind || lk == LLVMDoubleTypeKind);
        bool r_fp = (rk == LLVMFloatTypeKind || rk == LLVMDoubleTypeKind);
        bool l_int = (lk == LLVMIntegerTypeKind);
        bool r_int = (rk == LLVMIntegerTypeKind);

        if (l_fp || r_fp)
        {
          // Promote both to the wider float (or double if mixed with int)
          LLVMTypeRef target = ez_f64();
          if (l_fp && r_fp)
            target = (lk == LLVMDoubleTypeKind || rk == LLVMDoubleTypeKind) ? ez_f64() : ez_f32();
          if (l_int)
            lhs = LLVMBuildSIToFP(mod->builder, lhs, target, "itof");
          else if (lk == LLVMFloatTypeKind && target == ez_f64())
            lhs = LLVMBuildFPExt(mod->builder, lhs, target, "fpext");
          else if (lk == LLVMDoubleTypeKind && target == ez_f32())
            lhs = LLVMBuildFPTrunc(mod->builder, lhs, target, "fptrunc");
          if (r_int)
            rhs = LLVMBuildSIToFP(mod->builder, rhs, target, "itof");
          else if (rk == LLVMFloatTypeKind && target == ez_f64())
            rhs = LLVMBuildFPExt(mod->builder, rhs, target, "fpext");
          else if (rk == LLVMDoubleTypeKind && target == ez_f32())
            rhs = LLVMBuildFPTrunc(mod->builder, rhs, target, "fptrunc");
        }
        else if (l_int && r_int)
        {
          // Promote both to the wider integer
          unsigned lw = LLVMGetIntTypeWidth(lt);
          unsigned rw = LLVMGetIntTypeWidth(rt);
          if (lw < rw)
            lhs = LLVMBuildSExt(mod->builder, lhs, rt, "widen");
          else
            rhs = LLVMBuildSExt(mod->builder, rhs, lt, "widen");
        }
        // ptr vs ptr: leave as-is (LLVM opaque ptrs are all the same)
      }
    }

    EzType lhs_ty = LLVMTypeOf(lhs);
    LLVMTypeKind kind = LLVMGetTypeKind(lhs_ty);
    bool fp = (kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind);
    bool un = !fp && is_unsigned_type(hint);

    if (op == "+")
      return fp ? ez_fadd(mod, lhs, rhs, "add") : ez_add(mod, lhs, rhs, "add");
    if (op == "-")
      return fp ? ez_fsub(mod, lhs, rhs, "sub") : ez_sub(mod, lhs, rhs, "sub");
    if (op == "*")
      return fp ? ez_fmul(mod, lhs, rhs, "mul") : ez_mul(mod, lhs, rhs, "mul");
    if (op == "/")
      return fp ? ez_fdiv(mod, lhs, rhs, "div")
                : (un ? ez_udiv(mod, lhs, rhs, "div") : ez_sdiv(mod, lhs, rhs, "div"));
    if (op == "%")
      return un ? ez_urem(mod, lhs, rhs, "rem") : ez_srem(mod, lhs, rhs, "rem");
    if (op == "==")
      return fp ? ez_feq(mod, lhs, rhs, "eq") : ez_eq(mod, lhs, rhs, "eq");
    if (op == "!=")
      return fp ? LLVMBuildFCmp(mod->builder, LLVMRealONE, lhs, rhs, "ne")
                : ez_ne(mod, lhs, rhs, "ne");
    if (op == "<")
      return fp ? ez_flt(mod, lhs, rhs, "lt")
                : (un ? ez_ult(mod, lhs, rhs, "lt") : ez_slt(mod, lhs, rhs, "lt"));
    if (op == ">")
      return fp ? ez_fgt(mod, lhs, rhs, "gt")
                : (un ? ez_ugt(mod, lhs, rhs, "gt") : ez_sgt(mod, lhs, rhs, "gt"));
    if (op == "<=")
      return fp ? LLVMBuildFCmp(mod->builder, LLVMRealOLE, lhs, rhs, "le")
                : ez_sle(mod, lhs, rhs, "le");
    if (op == ">=")
      return fp ? LLVMBuildFCmp(mod->builder, LLVMRealOGE, lhs, rhs, "ge")
                : ez_sge(mod, lhs, rhs, "ge");
    if (op == "&&")
      return ez_and(mod, lhs, rhs, "and");
    if (op == "||")
      return ez_or(mod, lhs, rhs, "or");
    return lhs;
  }

  // Evaluate a call expression: tokens = [name, "(", arg_exprs..., ")"]
  EzVal eval_call_expr(const std::vector<Parser::ASTNode *> &tokens, const std::string &hint)
  {
    (void)hint;
    std::string fname = tokens[0]->value;
    auto it = func_map.find(fname);
    if (it == func_map.end())
    {
      EzFunc *f_auto = ez_extern(mod, fname.c_str(), ez_void(), nullptr, 0, /*vararg=*/1);
      func_map[fname] = f_auto;
      it = func_map.find(fname);
    }
    EzFunc *f = it->second;

    // Collect argument token groups separated by ',' at depth 0
    std::vector<std::vector<Parser::ASTNode *>> arg_groups;
    std::vector<Parser::ASTNode *> cur_group;
    int depth = 0;
    for (size_t i = 2; i < tokens.size(); ++i)
    {
      const std::string &v = tokens[i]->value;
      if (v == "(")
      {
        depth++;
        cur_group.push_back(tokens[i]);
      }
      else if (v == ")")
      {
        if (depth == 0)
        {
          if (!cur_group.empty())
            arg_groups.push_back(cur_group);
          break;
        }
        depth--;
        cur_group.push_back(tokens[i]);
      }
      else if (v == "," && depth == 0)
      {
        arg_groups.push_back(cur_group);
        cur_group.clear();
      }
      else
      {
        cur_group.push_back(tokens[i]);
      }
    }

    // Only use the declared non-tunnel param types for hints
    auto tit2 = func_tunnels.find(fname);
    unsigned n_tunnels2 = tit2 != func_tunnels.end() ? (unsigned)tit2->second.size() : 0;
    EzType fn_type = LLVMGlobalGetValueType(f->fn);
    unsigned total_params = LLVMCountParamTypes(fn_type);
    unsigned regular_params = total_params - n_tunnels2;
    std::vector<LLVMTypeRef> param_types(total_params);
    if (total_params > 0)
      LLVMGetParamTypes(fn_type, param_types.data());

    bool is_vararg_fn2 = LLVMIsFunctionVarArg(fn_type) != 0;
    std::vector<EzVal> args;
    collect_call_args(arg_groups, param_types, regular_params, is_vararg_fn2, args);

    // Append hidden tunnel pointer args — find or create caller slots
    if (tit2 != func_tunnels.end())
    {
      for (auto &tp : tit2->second)
      {
        EzVal slot = lookup_var(tp.name);
        if (!slot)
        {
          EzType ty = cshift_type(tp.type);
          slot = alloca_in_entry(current_func, ty, tp.name.c_str());
          declare_var(tp.name, slot, tp.type);
        }
        args.push_back(slot);
      }
    }

    // ── Guard: ensure non-vararg calls have exactly the right arg count ──────
    if (!is_vararg_fn2)
    {
      // At this point args = [user_args... , tunnel_args...]
      // Expected layout:     [regular_params..., tunnel_params...]
      // If user provided fewer args than regular_params, pad before tunnels.
      unsigned user_args_now = (unsigned)args.size() - n_tunnels2;
      if (user_args_now < regular_params)
      {
        for (unsigned pi = user_args_now; pi < regular_params; ++pi)
          args.insert(args.begin() + pi, LLVMConstNull(param_types[pi]));
      }
      // If user provided too many args, trim the excess before tunnel args.
      else if (user_args_now > regular_params)
      {
        unsigned excess = user_args_now - regular_params;
        args.erase(args.begin() + regular_params, args.begin() + regular_params + excess);
      }
    }

    EzType ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(f->fn));
    bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
    EzVal call_result = ez_call(mod, f, args.data(), (unsigned)args.size(), is_void ? "" : "call");

    // If this is a single-tunnel function used as an expression value
    // (e.g. print(add(4, 8))), load the tunnel slot and return it.
    if (tit2 != func_tunnels.end() && tit2->second.size() == 1)
    {
      const auto &tp = tit2->second[0];
      EzVal slot = lookup_var(tp.name);
      if (slot)
      {
        EzType slot_ty = cshift_type(tp.type);
        return ez_load(mod, slot_ty, slot, "tunnel_val");
      }
    }
    return call_result;
  }

  EzVal eval_token(Parser::ASTNode *tok, const std::string &hint)
  {
    if (!tok)
      return nullptr;
    const std::string &v = tok->value;

    // __arena — evaluates to the current scope's arena pointer (creates it lazily)
    if (v == "__arena")
      return get_or_create_scope_arena();

    // __arena_null — a null arena pointer (opt-out of arena tracking)
    if (v == "__arena_null")
      return LLVMConstNull(ez_ptr());

    // process escape sequences the lexer left as raw text
    // before handing the string to LLVM, otherwise \n becomes \\n in the IR.
    if (tok->token_type == Lexer::TokenType::STRING)
    {
      static int __str_id = 0;
      std::string name = std::string(".str") + std::to_string(__str_id++);
      std::string processed;
      processed.reserve(v.size());
      for (size_t i = 0; i < v.size(); ++i)
      {
        if (v[i] == '\\' && i + 1 < v.size())
        {
          switch (v[i + 1])
          {
          case 'n':
            processed += '\n';
            ++i;
            break;
          case 't':
            processed += '\t';
            ++i;
            break;
          case 'r':
            processed += '\r';
            ++i;
            break;
          case '0':
            processed += '\0';
            ++i;
            break;
          case '\\':
            processed += '\\';
            ++i;
            break;
          case '"':
            processed += '"';
            ++i;
            break;
          default:
            processed += v[i];
            break;
          }
        }
        else
        {
          processed += v[i];
        }
      }
      return ez_global_string(mod, processed.c_str(), name.c_str());
    }

    // Number literal
    if (tok->token_type == Lexer::TokenType::NUMBER)
    {
      if (v.find('.') != std::string::npos)
      {
        double d = std::stod(v);
        EzType ty = (hint == "float32") ? ez_f32() : ez_f64();
        return ez_const_float(ty, d);
      }
      long long iv = 0;
      try
      {
        iv = std::stoll(v, nullptr, 0);
      }
      catch (...)
      {
      }
      EzType ty = cshift_type(hint.empty() ? "int32" : hint);
      return ez_const_int(ty, iv);
    }
    // Keywords: true / false
    if (v == "true")
      return ez_const_bool(1);
    if (v == "false")
      return ez_const_bool(0);

    // always load a variable using its declared type.
    // Never use the hint for the load type — if hint is "bool" (propagated
    // from emit_condition) we would load an int32 alloca as i1, reading only
    // the lowest bit and producing garbage comparison results.
    if (tok->token_type == Lexer::TokenType::IDENTIFIER ||
        tok->token_type == Lexer::TokenType::KEYWORD)
    {
      EzVal ptr = lookup_var(v);
      if (!ptr)
        return nullptr;
      auto vit = var_type_map.find(v);
      std::string real_type = (vit != var_type_map.end()) ? vit->second : "int32";
      EzType ty = cshift_type(real_type);
      return ez_load(mod, ty, ptr, v.c_str());
    }
    return nullptr;
  }

  // ── Struct helpers ────────────────────────────────────────────────────────

  EzVal gep_field(EzVal struct_ptr, const std::string &struct_var, const std::string &field)
  {
    // Resolve the struct type name from the variable's declared type.
    // This avoids false matches when multiple structs share a field name.
    std::string struct_type_name;
    auto it = var_type_map.find(struct_var);
    if (it != var_type_map.end())
    {
      // Strip pointer/array decorators to get the base struct name
      struct_type_name = it->second;
      while (!struct_type_name.empty() &&
             (struct_type_name.back() == '*' || struct_type_name.back() == ']' ||
              struct_type_name.back() == ':' || struct_type_name.back() == '['))
        struct_type_name.pop_back();
    }

    // If we know the struct type, look it up directly; otherwise fall back to
    // searching
    if (!struct_type_name.empty())
    {
      auto sit = struct_map.find(struct_type_name);
      if (sit != struct_map.end())
      {
        auto &layout = sit->second;
        for (unsigned i = 0; i < layout.field_names.size(); ++i)
        {
          if (layout.field_names[i] == field)
          {
            EzVal idxs[2] = {ez_const_int(ez_i32(), 0), ez_const_int(ez_i32(), (long long)i)};
            return ez_gep(mod, layout.llvm_type, struct_ptr, idxs, 2, field.c_str());
          }
        }
      }
    }
    // Fallback: search all struct layouts (ambiguous but better than nothing)
    for (auto &[sname, layout] : struct_map)
    {
      for (unsigned i = 0; i < layout.field_names.size(); ++i)
      {
        if (layout.field_names[i] == field)
        {
          EzVal idxs[2] = {ez_const_int(ez_i32(), 0), ez_const_int(ez_i32(), (long long)i)};
          return ez_gep(mod, layout.llvm_type, struct_ptr, idxs, 2, field.c_str());
        }
      }
    }
    return struct_ptr;
  }

  std::string field_type_of(const std::string &var, const std::string &field)
  {
    // Resolve via var_type_map first for accuracy
    std::string struct_type_name;
    auto it = var_type_map.find(var);
    if (it != var_type_map.end())
    {
      struct_type_name = it->second;
      while (!struct_type_name.empty() &&
             (struct_type_name.back() == '*' || struct_type_name.back() == ']' ||
              struct_type_name.back() == ':' || struct_type_name.back() == '['))
        struct_type_name.pop_back();
    }
    if (!struct_type_name.empty())
    {
      auto sit = struct_map.find(struct_type_name);
      if (sit != struct_map.end())
      {
        auto &layout = sit->second;
        for (unsigned i = 0; i < layout.field_names.size(); ++i)
          if (layout.field_names[i] == field)
            return layout.field_types[i];
      }
    }
    // Fallback: search all layouts
    for (auto &[sname, layout] : struct_map)
    {
      for (unsigned i = 0; i < layout.field_names.size(); ++i)
        if (layout.field_names[i] == field)
          return layout.field_types[i];
    }
    return "int32";
  }
};

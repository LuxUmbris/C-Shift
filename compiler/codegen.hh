#pragma once
#include "checker.hh" // pulls in parser.hh → lexer.hh
#include "ezllvm.h"
#include <cassert>
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

  void push_scope() { var_scopes.push_back({}); }
  void pop_scope() { var_scopes.pop_back(); }

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

    // Scan the function body for tunnel targets and add them as hidden
    // pointer parameters after the regular params.  This is how C<< VOP
    // tunnel outputs are implemented: the caller passes the address of its
    // reserved slot, and the function writes into it directly.
    std::vector<TunnelParam> tunnels;
    if (n->children.size() >= 2)
      collect_tunnels(n->children[1], tunnels);
    func_tunnels[name] = tunnels;
    for (auto &tp : tunnels)
    {
      params.push_back(ez_ptr()); // pointer to caller's slot
      pnames.push_back("__tunnel_" + tp.name);
    }

    EzFunc *f = ez_func(mod, name.c_str(), ret, params.data(), (unsigned)params.size(), 0);
    for (unsigned i = 0; i < pnames.size(); ++i)
      ez_set_param_name(f, i, pnames[i].c_str());
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
    else if (t == "CallStatement")
      emit_call_stmt(n);
    else if (t == "Tunnel")
      emit_tunnel(n);
    else if (t == "Move")
    { /* voided-state is static; no runtime action */
    }
    else if (t == "Reset")
    { /* no runtime action */
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
    ez_store(mod, new_data, data_ptr);
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

    EzType ty = cshift_type(type_s);

    EzVal slot = alloca_in_entry(current_func, ty, vname.c_str());
    declare_var(vname, slot, type_s);

    if (!n->children.empty())
    {
      EzVal init = emit_expression_val(n->children[0], type_s);
      if (init)
        ez_store(mod, init, slot);
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

    // Type-inferred reserve: resolve type from the single tunnel of the callee
    if (type_s == "__infer__")
    {
      size_t start = 0;
      if (!n->children.empty() && n->children[0]->type == "Shared")
        start = 1;
      if (n->children.size() > start)
      {
        auto *init_expr = n->children[start];
        if (init_expr->type == "Expression" && init_expr->children.size() >= 2 &&
            init_expr->children[0]->token_type == Lexer::TokenType::IDENTIFIER &&
            init_expr->children[1]->value == "(")
        {
          std::string call_fname = init_expr->children[0]->value;
          auto tit = func_tunnels.find(call_fname);
          if (tit != func_tunnels.end() && tit->second.size() == 1)
          {
            type_s = tit->second[0].type;
          }
          else if (tit != func_tunnels.end() && tit->second.size() > 1)
          {
            fprintf(stderr,
                    "[ERROR] reserve without type: '%s' has multiple tunnels — "
                    "cannot infer type; specify it explicitly.\n",
                    call_fname.c_str());
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

    size_t start = 0;
    if (!n->children.empty() && n->children[0]->type == "Shared")
      start = 1;
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
          for (auto &tp : tit->second)
          {
            declare_var(tp.name, slot, type_s);
            injected_names.push_back(tp.name);
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
      ptr = gep_field(base_ptr, base, field);
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

  EzVal emit_call_val(Parser::ASTNode *n, const std::string &result_name)
  {
    const std::string &fname = n->value;
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

    // FIX (Bug 1): build regular args and tunnel args in separate steps.
    // The tunnel append must happen unconditionally — outside the
    // children-present guard — so that zero-explicit-arg tunnel calls still
    // pass their hidden output pointers.

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

      for (auto &grp : arg_groups)
      {
        std::string arg_hint = "";
        unsigned arg_idx = (unsigned)args.size();
        if (arg_idx < regular_params)
          arg_hint = hint_from_llvm_type(param_types[arg_idx]);
        EzVal v = eval_expr_children(grp, arg_hint);
        if (v)
          args.push_back(v);
      }
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

    EzVal switched = emit_expression_val(n->children[0], "int32");

    // Count cases
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

    // Build case blocks
    std::vector<EzBlock *> case_blocks;
    for (auto *c : cases)
      case_blocks.push_back(ez_block(current_func, ("sw.case." + c->value).c_str()));
    EzBlock *default_b = default_node ? ez_block(current_func, "sw.default") : end_b;

    // LLVM switch instruction
    LLVMValueRef sw =
        LLVMBuildSwitch(mod->builder, switched, default_b->bb, (unsigned)cases.size());
    for (size_t i = 0; i < cases.size(); ++i)
    {
      const std::string &val_s = cases[i]->value;
      // valid/voided are symbolic; map to i32 sentinel values for now
      long long ival = 0;
      if (val_s == "valid")
        ival = 1;
      else if (val_s == "voided")
        ival = 0;
      else
      {
        try
        {
          ival = std::stoll(val_s);
        }
        catch (...)
        {
        }
      }
      LLVMAddCase(sw, LLVMConstInt(ez_i32(), (unsigned long long)ival, 1), case_blocks[i]->bb);
    }

    // Push switch context for break handling
    loop_stack.push_back({end_b, nullptr, true});

    // Emit case bodies
    for (size_t i = 0; i < cases.size(); ++i)
    {
      current_block = case_blocks[i];
      ez_use(case_blocks[i]);
      for (auto *stmt : cases[i]->children)
        emit_stmt(stmt);
      // Guard against double terminator (break already added one)
      if (!current_block_has_terminator())
        ez_br(mod, end_b);
    }
    if (default_node)
    {
      current_block = default_b;
      ez_use(default_b);
      for (auto *stmt : default_node->children)
        emit_stmt(stmt);
      // Guard against double terminator
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
    if (tokens.size() >= 3 && tokens[0]->type == "Token" &&
        tokens[0]->token_type == Lexer::TokenType::IDENTIFIER && tokens[1]->value == "(")
    {
      return eval_call_expr(tokens, hint);
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
      return apply_binop(tokens[op_idx]->value, lhs, rhs, hint);
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
    if (tokens.size() >= 3 && tokens[1]->value == ".")
    {
      std::string base = tokens[0]->value;
      std::string field = tokens[2]->value;

      // Arena array .len
      if (field == "len" && arena_array_elem_type.count(base))
      {
        EzVal len_ptr = get_arena_len_ptr(base);
        if (len_ptr)
          return ez_load(mod, ez_i64(), len_ptr, "arr_len");
      }

      EzVal base_ptr = lookup_var(base);
      if (base_ptr)
      {
        EzVal field_ptr = gep_field(base_ptr, base, field);
        std::string ftype = field_type_of(base, field);
        EzType fty = cshift_type(ftype.empty() ? hint : ftype);
        return ez_load(mod, fty, field_ptr, field.c_str());
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
    // derive signedness/floatness from the actual LLVM type of
    // lhs rather than the hint string, which may be empty or "bool" when
    // called from a comparison context.
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

    std::vector<EzVal> args;
    for (auto &grp : arg_groups)
    {
      unsigned arg_idx = (unsigned)args.size();
      std::string arg_hint =
          (arg_idx < regular_params) ? hint_from_llvm_type(param_types[arg_idx]) : "";
      EzVal v = eval_expr_children(grp, arg_hint);
      if (v)
        args.push_back(v);
    }

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

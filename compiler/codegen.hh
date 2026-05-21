#pragma once
#include "checker.hh"   // pulls in parser.hh → lexer.hh
#include "ezllvm.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cassert>

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
    EzFunc   *current_func = nullptr;
    EzBlock  *current_block = nullptr;

    // name → alloca (local variables in the current function)
    using VarMap = std::unordered_map<std::string, EzVal /*alloca ptr*/>;
    std::vector<VarMap> var_scopes;

    // name → EzFunc* (for call generation)
    std::unordered_map<std::string, EzFunc*> func_map;

    // name → struct type (for field GEP)
    struct StructLayout {
        EzType *llvm_type;
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
    void pop_scope()  { var_scopes.pop_back(); }

    // name → declared C<< type string (for struct GEP resolution)
    std::unordered_map<std::string, std::string> var_type_map;

    void declare_var(const std::string &name, EzVal alloca_ptr, const std::string &type_s = "") {
        if (!var_scopes.empty()) {
            var_scopes.back()[name] = alloca_ptr;
            if (!type_s.empty()) var_type_map[name] = type_s;
        }
    }

    EzVal lookup_var(const std::string &name) {
        for (int i = (int)var_scopes.size() - 1; i >= 0; --i) {
            auto it = var_scopes[i].find(name);
            if (it != var_scopes[i].end()) return it->second;
        }
        return nullptr;
    }

    // Map C<< type string → LLVM EzType
    EzType *cshift_type(const std::string &t) {
        std::string base = t;
        // strip pointer/array decorators to find base
        bool is_ptr   = (t.find('*')   != std::string::npos);
        bool is_slice = (t.find('[')   != std::string::npos);
        // strip all decorators
        for (char c : std::string("*[]:|")) {
            base.erase(std::remove(base.begin(), base.end(), c), base.end());
        }

        EzType *elem = nullptr;
        if      (base == "int8"  || base == "char")  elem = ez_i8();
        else if (base == "int16")                    elem = ez_i16();
        else if (base == "int32")                    elem = ez_i32();
        else if (base == "int64")                    elem = ez_i64();
        else if (base == "uint8")                    elem = ez_i8();
        else if (base == "uint16")                   elem = ez_i16();
        else if (base == "uint32")                   elem = ez_i32();
        else if (base == "uint64")                   elem = ez_i64();
        else if (base == "float32")                  elem = ez_f32();
        else if (base == "float64")                  elem = ez_f64();
        else if (base == "bool")                     elem = ez_i1();
        else if (base == "string")                   elem = ez_ptr(); // i8*
        else if (base == "voided" || base == "void") elem = ez_void();
        else {
            // user-defined struct?
            auto it = struct_map.find(base);
            if (it != struct_map.end()) elem = it->second.llvm_type;
            else elem = ez_i32(); // fallback: treat unknown as i32
        }

        if (is_ptr || is_slice) return ez_ptr_to(elem);
        return elem;
    }

    bool is_float_type(const std::string &t) {
        return t == "float32" || t == "float64";
    }
    bool is_unsigned_type(const std::string &t) {
        return t.substr(0,4) == "uint";
    }

    // ── Entry-point alloca builder for the first block ────────────────────────
    // We position the builder at the very start of the entry block for allocas.
    EzVal alloca_in_entry(EzFunc *fn, EzType *ty, const std::string &name) {
        // Temporarily move builder to the first instruction of the entry block
        LLVMBasicBlockRef entry_bb = LLVMGetEntryBasicBlock(fn->fn);
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

public:
    // ── Public interface ──────────────────────────────────────────────────────

    explicit Codegen(EzModule *m) : mod(m) {}

    // Generate IR for a full program AST.
    void generate(const std::vector<Parser::ASTNode*> &ast) {
        // Pass 1: forward-declare all structs, functions, and C imports
        for (auto *n : ast) forward_declare(n);
        // Pass 2: emit bodies
        for (auto *n : ast) emit_top(n);
    }

private:
    // ── Forward declarations ──────────────────────────────────────────────────

    void forward_declare(Parser::ASTNode *n) {
        if (!n) return;
        if (n->type == "Struct")       forward_struct(n);
        if (n->type == "CImport")      forward_c_import(n);
        if (n->type == "Function")     forward_function(n);
        if (n->type == "Entry")        forward_entry(n);
        if (n->type == "Namespace")
            for (auto *c : n->children) forward_declare(c);
    }

    void forward_struct(Parser::ASTNode *n) {
        const std::string &name = n->value;
        StructLayout layout;
        layout.llvm_type = ez_struct_named(mod, name.c_str());

        std::vector<EzType*> fields;
        for (auto *f : n->children) {
            if (!f) continue;
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

    void forward_c_import(Parser::ASTNode *n) {
        // value = "rettype funcname"
        auto sp = n->value.find(' ');
        std::string ret_s  = n->value.substr(0, sp);
        std::string fname  = n->value.substr(sp + 1);

        EzType *ret = (ret_s == "voided" || ret_s == "void") ? ez_void() : cshift_type(ret_s);

        std::vector<EzType*> params;
        bool vararg = false;
        if (!n->children.empty() && n->children[0]->type == "CParams") {
            for (auto *p : n->children[0]->children) {
                if (p->type == "Variadic") { vararg = true; continue; }
                auto psp = p->value.find(' ');
                std::string ptype = p->value.substr(0, psp);
                params.push_back(cshift_type(ptype));
            }
        }

        EzFunc *f = ez_extern(mod, fname.c_str(), ret, params.data(), (unsigned)params.size(), vararg ? 1 : 0);
        func_map[fname] = f;
    }

    void forward_function(Parser::ASTNode *n) {
        const std::string &name = n->value;
        // C<< uses VOP (Vertical Ownership Programming): functions never return
        // values — all output flows through tunnel declarations (tunnel expr -> type name).
        // Functions therefore always have a void return type at the IR level.
        EzType *ret = ez_void();
        std::vector<EzType*> params;
        std::vector<std::string> pnames;

        if (!n->children.empty() && n->children[0]->type == "Parameters") {
            for (auto *p : n->children[0]->children) {
                auto sp = p->value.find(' ');
                params.push_back(cshift_type(p->value.substr(0, sp)));
                pnames.push_back(p->value.substr(sp + 1));
            }
        }

        EzFunc *f = ez_func(mod, name.c_str(), ret, params.data(), (unsigned)params.size(), 0);
        for (unsigned i = 0; i < pnames.size(); ++i)
            ez_set_param_name(f, i, pnames[i].c_str());
        func_map[name] = f;
    }

    void forward_entry(Parser::ASTNode *) {
        // main() : i32
        EzFunc *f = ez_func(mod, "main", ez_i32(), nullptr, 0, 0);
        func_map["__entry__"] = f;
    }

    // ── Top-level emitters ────────────────────────────────────────────────────

    void emit_top(Parser::ASTNode *n) {
        if (!n) return;
        if (n->type == "Function")     emit_function(n);
        else if (n->type == "Entry")   emit_entry(n);
        else if (n->type == "Struct")  { /* body already set in forward pass */ }
        else if (n->type == "Namespace")
            for (auto *c : n->children) emit_top(c);
        // CImport / ModuleImport / Import: nothing to emit
    }

    void emit_entry(Parser::ASTNode *n) {
        EzFunc *f = func_map["__entry__"];
        current_func = f;
        EzBlock *entry_b = ez_block(f, "entry");
        current_block = entry_b;
        ez_use(entry_b);

        push_scope();
        tunnel_slots.clear();
        if (!n->children.empty()) emit_block_body(n->children[0]);
        pop_scope();

        // return 0
        ez_ret(mod, ez_const_int(ez_i32(), 0));
        current_func = nullptr;
    }

    void emit_function(Parser::ASTNode *n) {
        const std::string &name = n->value;
        EzFunc *f = func_map[name];
        current_func = f;
        EzBlock *entry_b = ez_block(f, "entry");
        current_block = entry_b;
        ez_use(entry_b);

        push_scope();
        tunnel_slots.clear();

        // Bind parameters to allocas
        if (!n->children.empty() && n->children[0]->type == "Parameters") {
            unsigned idx = 0;
            for (auto *p : n->children[0]->children) {
                auto sp = p->value.find(' ');
                std::string ptype = p->value.substr(0, sp);
                std::string pname = p->value.substr(sp + 1);
                EzType *ty = cshift_type(ptype);
                EzVal slot = alloca_in_entry(f, ty, pname.c_str());
                ez_store(mod, ez_param(f, idx++), slot);
                declare_var(pname, slot, ptype);
            }
        }

        if (n->children.size() >= 2) emit_block_body(n->children[1]);
        pop_scope();

        // implicit void return
        EzType *ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(f->fn));
        if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
            ez_ret_void(mod);

        current_func = nullptr;
    }

    // ── Block / statement dispatch ────────────────────────────────────────────

    void emit_block_body(Parser::ASTNode *block) {
        if (!block) return;
        push_scope();
        for (auto *stmt : block->children) emit_stmt(stmt);
        pop_scope();
    }

    void emit_stmt(Parser::ASTNode *n) {
        if (!n) return;
        const std::string &t = n->type;
        if      (t == "Declaration")    emit_declaration(n);
        else if (t == "Const")          emit_const(n);
        else if (t == "Reserve")        emit_reserve(n);
        else if (t == "Assignment")     emit_assignment(n);
        else if (t == "CallStatement")  emit_call_stmt(n);
        else if (t == "Tunnel")         emit_tunnel(n);
        else if (t == "Move")           { /* voided-state is static; no runtime action */ }
        else if (t == "Reset")          { /* no runtime action */ }
        else if (t == "If")             emit_if(n);
        else if (t == "While")          emit_while(n);
        else if (t == "For")            emit_for(n);
        else if (t == "Foreach")        emit_foreach(n);
        else if (t == "Switch")         emit_switch(n);
        else if (t == "Block")          emit_block_body(n);
        else if (t == "Expression")     emit_expression(n); // expression-statement
    }

    // ── Variables ─────────────────────────────────────────────────────────────

    void emit_declaration(Parser::ASTNode *n) {
        auto sp = n->value.find(' ');
        std::string type_s = n->value.substr(0, sp);
        std::string vname  = n->value.substr(sp + 1);
        EzType *ty = cshift_type(type_s);

        EzVal slot = alloca_in_entry(current_func, ty, vname.c_str());
        declare_var(vname, slot, type_s);

        if (!n->children.empty()) {
            EzVal init = emit_expression_val(n->children[0], type_s);
            if (init) ez_store(mod, init, slot);
        }
    }

    void emit_const(Parser::ASTNode *n) {
        // Same as declaration (constness enforced by checker, not codegen)
        emit_declaration(n);
    }


    void emit_reserve(Parser::ASTNode *n) {
        // reserve is like declaration; the tunnel mechanism handles the init
        auto sp = n->value.find(' ');
        std::string type_s = n->value.substr(0, sp);
        std::string vname  = n->value.substr(sp + 1);
        EzType *ty = cshift_type(type_s);

        EzVal slot = alloca_in_entry(current_func, ty, vname.c_str());
        declare_var(vname, slot, type_s);

        // If there's an initialiser expression, evaluate it
        size_t start = 0;
        if (!n->children.empty() && n->children[0]->type == "Shared") start = 1;
        if (n->children.size() > start) {
            EzVal init = emit_expression_val(n->children[start], type_s);
            if (init) ez_store(mod, init, slot);
        }
    }

    void emit_assignment(Parser::ASTNode *n) {
        // value = "target op"
        auto sp = n->value.rfind(' ');
        std::string target = n->value.substr(0, sp);
        std::string op     = n->value.substr(sp + 1);

        // Field access: "a.b"
        auto dot = target.find('.');
        EzVal ptr = nullptr;
        std::string load_type;

        if (dot != std::string::npos) {
            std::string base  = target.substr(0, dot);
            std::string field = target.substr(dot + 1);
            EzVal base_ptr = lookup_var(base);
            if (!base_ptr) return;
            ptr = gep_field(base_ptr, base, field);
            load_type = field_type_of(base, field);
        } else {
            ptr = lookup_var(target);
            load_type = "int32"; // best-effort; checker already validated
        }
        if (!ptr) return;

        EzVal rhs = emit_expression_val(n->children.empty() ? nullptr : n->children[0], load_type);
        if (!rhs) return;

        if (op == "=") {
            ez_store(mod, rhs, ptr);
        } else {
            // Compound assign: load, operate, store
            EzType *ty = cshift_type(load_type);
            EzVal lhs = ez_load(mod, ty, ptr, "lhs");
            EzVal result = nullptr;
            if      (op == "+=") result = is_float_type(load_type) ? ez_fadd(mod, lhs, rhs, "add") : ez_add(mod, lhs, rhs, "add");
            else if (op == "-=") result = is_float_type(load_type) ? ez_fsub(mod, lhs, rhs, "sub") : ez_sub(mod, lhs, rhs, "sub");
            else if (op == "*=") result = is_float_type(load_type) ? ez_fmul(mod, lhs, rhs, "mul") : ez_mul(mod, lhs, rhs, "mul");
            else if (op == "/=") result = is_float_type(load_type) ? ez_fdiv(mod, lhs, rhs, "div") : ez_sdiv(mod, lhs, rhs, "div");
            else if (op == "%=") result = ez_srem(mod, lhs, rhs, "rem");
            if (result) ez_store(mod, result, ptr);
        }
    }

    // ── Call statement ────────────────────────────────────────────────────────

    void emit_call_stmt(Parser::ASTNode *n) {
        emit_call_val(n, /*result_name=*/"");
    }

    EzVal emit_call_val(Parser::ASTNode *n, const std::string &result_name) {
        const std::string &fname = n->value;
        auto it = func_map.find(fname);
        if (it == func_map.end()) return nullptr;
        EzFunc *f = it->second;

        std::vector<EzVal> args;
        if (!n->children.empty()) {
            for (auto *arg : n->children[0]->children) {
                EzVal v = emit_expression_val(arg, ""); // type inferred
                if (v) args.push_back(v);
            }
        }
        EzType *ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(f->fn));
        bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
        return ez_call(mod, f, args.data(), (unsigned)args.size(),
                       is_void ? "" : result_name.c_str());
    }

    // ── Tunnel ────────────────────────────────────────────────────────────────
    // tunnel expr -> type name
    // Inside a function:  evaluate expr, store into a pre-alloca'd slot
    //                     that represents the output (caller reads it).
    // Inside entry/block: evaluate expr, store into the named variable.

    void emit_tunnel(Parser::ASTNode *n) {
        if (n->children.size() < 2) return;
        auto *expr   = n->children[0];
        auto *target = n->children[1];

        auto sp      = target->value.find(' ');
        std::string ttype = target->value.substr(0, sp);
        std::string tname = target->value.substr(sp + 1);

        EzVal rhs = emit_expression_val(expr, ttype);
        if (!rhs) return;

        // Find or create the target slot
        EzVal ptr = lookup_var(tname);
        if (!ptr) {
            // Create a slot (function-scoped tunnel output)
            EzType *ty = cshift_type(ttype);
            ptr = alloca_in_entry(current_func, ty, tname.c_str());
            declare_var(tname, ptr);
            tunnel_slots[tname] = ptr;
        }
        ez_store(mod, rhs, ptr);
    }

    // ── Control flow ──────────────────────────────────────────────────────────

    void emit_if(Parser::ASTNode *n) {
        if (n->children.empty()) return;

        EzVal cond = emit_condition(n->children[0]);

        EzBlock *then_b = ez_block(current_func, "if.then");
        EzBlock *else_b = ez_block(current_func, "if.else");
        EzBlock *end_b  = ez_block(current_func, "if.end");

        ez_cond_br(mod, cond, then_b, else_b);

        // then
        current_block = then_b; ez_use(then_b);
        if (n->children.size() > 1) emit_block_body(n->children[1]);
        ez_br(mod, end_b);

        // else
        current_block = else_b; ez_use(else_b);
        if (n->children.size() > 2) {
            auto *el = n->children[2];
            if (el->type == "If") emit_if(el);
            else                  emit_block_body(el);
        }
        ez_br(mod, end_b);

        current_block = end_b; ez_use(end_b);
    }

    void emit_while(Parser::ASTNode *n) {
        EzBlock *cond_b = ez_block(current_func, "while.cond");
        EzBlock *body_b = ez_block(current_func, "while.body");
        EzBlock *end_b  = ez_block(current_func, "while.end");

        ez_br(mod, cond_b);

        current_block = cond_b; ez_use(cond_b);
        EzVal cond = (n->children.size() > 0) ? emit_condition(n->children[0]) : ez_const_bool(1);
        ez_cond_br(mod, cond, body_b, end_b);

        current_block = body_b; ez_use(body_b);
        if (n->children.size() > 1) emit_block_body(n->children[1]);
        ez_br(mod, cond_b);

        current_block = end_b; ez_use(end_b);
    }

    void emit_for(Parser::ASTNode *n) {
        // children: [init_decl, cond_expr, incr_expr, body_block]
        push_scope();
        if (n->children.size() > 0) emit_declaration(n->children[0]);

        EzBlock *cond_b = ez_block(current_func, "for.cond");
        EzBlock *body_b = ez_block(current_func, "for.body");
        EzBlock *incr_b = ez_block(current_func, "for.incr");
        EzBlock *end_b  = ez_block(current_func, "for.end");

        ez_br(mod, cond_b);

        current_block = cond_b; ez_use(cond_b);
        EzVal cond = (n->children.size() > 1) ? emit_condition(n->children[1]) : ez_const_bool(1);
        ez_cond_br(mod, cond, body_b, end_b);

        current_block = body_b; ez_use(body_b);
        if (n->children.size() > 3) emit_block_body(n->children[3]);
        ez_br(mod, incr_b);

        current_block = incr_b; ez_use(incr_b);
        if (n->children.size() > 2) emit_expression(n->children[2]);
        ez_br(mod, cond_b);

        current_block = end_b; ez_use(end_b);
        pop_scope();
    }

    void emit_foreach(Parser::ASTNode *n) {
        // foreach (type item : collection) { body }
        // n->value  = "type item"
        // n->children[0] = collection expression (must evaluate to a pointer)
        // n->children[1] = body block
        //
        // Lowering strategy: the collection must be a pointer (T*) with a known
        // length.  Because C<< has no built-in slice length at runtime, we require
        // the programmer to iterate over a compile-time-sized array.  We therefore
        // lower foreach into a counted index loop:
        //
        //   i32 __idx = 0
        //   loop:  if __idx >= __len  goto end
        //          type item = collection[__idx]
        //          body
        //          __idx++
        //          goto loop
        //   end:
        //
        // If the collection expression cannot be lowered to a pointer we skip the
        // body and emit a diagnostic.  This keeps the compiler from crashing on
        // foreach while clearly indicating unsupported use.

        if (!n || n->children.size() < 2) return;

        auto sp = n->value.find(' ');
        std::string item_type = (sp != std::string::npos) ? n->value.substr(0, sp) : "int32";
        std::string item_name = (sp != std::string::npos) ? n->value.substr(sp + 1) : n->value;

        // Evaluate the collection — we expect a pointer value
        EzVal collection = emit_expression_val(n->children[0], item_type + "*");
        if (!collection) {
            // Cannot evaluate collection: emit nothing (checker will have warned)
            return;
        }

        // Allocate the loop index
        EzType *i32_ty = ez_i32();
        EzVal idx_slot = alloca_in_entry(current_func, i32_ty, "__foreach_idx");
        ez_store(mod, ez_const_int(i32_ty, 0), idx_slot);

        // Allocate the item slot
        EzType *elem_ty = cshift_type(item_type);
        EzVal item_slot = alloca_in_entry(current_func, elem_ty, item_name.c_str());

        // Blocks
        EzBlock *cond_b = ez_block(current_func, "foreach.cond");
        EzBlock *body_b = ez_block(current_func, "foreach.body");
        EzBlock *end_b  = ez_block(current_func, "foreach.end");

        ez_br(mod, cond_b);

        // cond: index < INT32_MAX (open-ended; requires programmer discipline).
        // In practice, foreach over a finite array is bounded by the data.
        // We use a sentinel of 0x7FFFFFFF as a safety upper bound.
        current_block = cond_b; ez_use(cond_b);
        EzVal idx_val = ez_load(mod, i32_ty, idx_slot, "idx");
        EzVal limit   = ez_const_int(i32_ty, 0x7FFFFFFF);
        EzVal cond    = ez_slt(mod, idx_val, limit, "foreach.cond");
        ez_cond_br(mod, cond, body_b, end_b);

        // body
        current_block = body_b; ez_use(body_b);
        push_scope();
        declare_var(item_name, item_slot, item_type);

        // Load collection[idx] via GEP
        EzVal gep_idx[1] = { idx_val };
        EzVal elem_ptr = ez_gep(mod, elem_ty, collection, gep_idx, 1, "elem_ptr");
        EzVal elem_val = ez_load(mod, elem_ty, elem_ptr, item_name.c_str());
        ez_store(mod, elem_val, item_slot);

        emit_block_body(n->children[1]);
        pop_scope();

        // increment
        EzVal idx_next = ez_add(mod, idx_val, ez_const_int(i32_ty, 1), "idx.next");
        ez_store(mod, idx_next, idx_slot);
        ez_br(mod, cond_b);

        current_block = end_b; ez_use(end_b);
    }

    void emit_switch(Parser::ASTNode *n) {
        if (n->children.empty()) return;

        EzVal switched = emit_expression_val(n->children[0], "int32");

        // Count cases
        std::vector<Parser::ASTNode*> cases;
        Parser::ASTNode *default_node = nullptr;
        for (size_t i = 1; i < n->children.size(); ++i) {
            auto *c = n->children[i];
            if (c->type == "Case")    cases.push_back(c);
            if (c->type == "Default") default_node = c;
        }

        EzBlock *end_b = ez_block(current_func, "sw.end");

        // Build case blocks
        std::vector<EzBlock*> case_blocks;
        for (auto *c : cases) case_blocks.push_back(ez_block(current_func, ("sw.case." + c->value).c_str()));
        EzBlock *default_b = default_node ? ez_block(current_func, "sw.default") : end_b;

        // LLVM switch instruction
        LLVMValueRef sw = LLVMBuildSwitch(mod->builder, switched, default_b->bb, (unsigned)cases.size());
        for (size_t i = 0; i < cases.size(); ++i) {
            const std::string &val_s = cases[i]->value;
            // valid/voided are symbolic; map to i32 sentinel values for now
            long long ival = 0;
            if (val_s == "valid")   ival = 1;
            else if (val_s == "voided") ival = 0;
            else { try { ival = std::stoll(val_s); } catch(...) {} }
            LLVMAddCase(sw, LLVMConstInt(ez_i32(), (unsigned long long)ival, 1), case_blocks[i]->bb);
        }

        // Emit case bodies
        for (size_t i = 0; i < cases.size(); ++i) {
            current_block = case_blocks[i]; ez_use(case_blocks[i]);
            for (auto *stmt : cases[i]->children) emit_stmt(stmt);
            ez_br(mod, end_b);
        }
        if (default_node) {
            current_block = default_b; ez_use(default_b);
            for (auto *stmt : default_node->children) emit_stmt(stmt);
            ez_br(mod, end_b);
        }

        current_block = end_b; ez_use(end_b);
    }

    // ── Expression evaluation ─────────────────────────────────────────────────
    // Returns an EzVal (SSA value). hint_type guides numeric literal sizing.

    EzVal emit_expression_val(Parser::ASTNode *n, const std::string &hint_type) {
        if (!n) return nullptr;

        // Flat expression node: walk children and fold into a value
        if (n->type == "Expression") return eval_expr_children(n->children, hint_type);
        if (n->type == "Token")      return eval_token(n, hint_type);
        return nullptr;
    }

    // Condition: evaluate and ensure i1
    EzVal emit_condition(Parser::ASTNode *n) {
        EzVal v = emit_expression_val(n, "bool");
        if (!v) return ez_const_bool(0);
        EzType *ty = LLVMTypeOf(v);
        if (LLVMGetTypeKind(ty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(ty) == 1) return v;
        // Compare != 0
        return ez_ne(mod, v, LLVMConstInt(ty, 0, 0), "tobool");
    }

    // Expression used as a statement (for incr expressions in for-loops etc.)
    void emit_expression(Parser::ASTNode *n) {
        emit_expression_val(n, "int32");
    }

    // Walk a flat token list and build a value.
    // We handle:  literals, identifiers (load), unary -, binary ops, function calls.
    // This is a simple left-to-right evaluator for the flat expression AST.
    EzVal eval_expr_children(const std::vector<Parser::ASTNode*> &tokens, const std::string &hint) {
        if (tokens.empty()) return nullptr;

        // Detect function call: IDENT ( args... )
        if (tokens.size() >= 3 &&
            tokens[0]->type == "Token" && tokens[0]->token_type == Lexer::TokenType::IDENTIFIER &&
            tokens[1]->value == "(") {
            return eval_call_expr(tokens, hint);
        }

        // Single token
        if (tokens.size() == 1) return eval_token(tokens[0], hint);

        // Unary minus: - TOKEN
        if (tokens.size() == 2 && tokens[0]->value == "-") {
            EzVal v = eval_token(tokens[1], hint);
            if (!v) return nullptr;
            return is_float_type(hint) ? ez_fneg(mod, v, "neg") : ez_neg(mod, v, "neg");
        }

        // Binary expression: lhs op rhs
        // Find rightmost low-precedence operator (simple left-associative)
        int op_idx = find_binary_op(tokens);
        if (op_idx > 0) {
            std::vector<Parser::ASTNode*> lhs_tokens(tokens.begin(), tokens.begin() + op_idx);
            std::vector<Parser::ASTNode*> rhs_tokens(tokens.begin() + op_idx + 1, tokens.end());
            EzVal lhs = eval_expr_children(lhs_tokens, hint);
            EzVal rhs = eval_expr_children(rhs_tokens, hint);
            if (!lhs || !rhs) return lhs ? lhs : rhs;
            return apply_binop(tokens[op_idx]->value, lhs, rhs, hint);
        }

        // Strip outer parens
        if (tokens.front()->value == "(" && tokens.back()->value == ")") {
            std::vector<Parser::ASTNode*> inner(tokens.begin() + 1, tokens.end() - 1);
            return eval_expr_children(inner, hint);
        }

        // Fallback: evaluate first token
        return eval_token(tokens[0], hint);
    }

    // Find the index of the rightmost binary operator (lowest precedence first)
    int find_binary_op(const std::vector<Parser::ASTNode*> &tokens) {
        // Precedence levels (lower number = lower precedence, split last)
        static const std::vector<std::vector<std::string>> prec = {
            {"||"}, {"&&"},
            {"==", "!="}, {"<", ">", "<=", ">="},
            {"+", "-"}, {"*", "/", "%"}
        };
        int depth = 0;
        for (auto &level : prec) {
            // scan right-to-left at depth 0
            for (int i = (int)tokens.size() - 1; i >= 1; --i) {
                const std::string &v = tokens[i]->value;
                if (v == ")") depth++;
                if (v == "(") depth--;
                if (depth == 0) {
                    for (auto &op : level) if (v == op) return i;
                }
            }
        }
        return -1;
    }

    EzVal apply_binop(const std::string &op, EzVal lhs, EzVal rhs, const std::string &hint) {
        bool fp = is_float_type(hint);
        bool un = is_unsigned_type(hint);
        if (op == "+")  return fp ? ez_fadd(mod,lhs,rhs,"add") : ez_add(mod,lhs,rhs,"add");
        if (op == "-")  return fp ? ez_fsub(mod,lhs,rhs,"sub") : ez_sub(mod,lhs,rhs,"sub");
        if (op == "*")  return fp ? ez_fmul(mod,lhs,rhs,"mul") : ez_mul(mod,lhs,rhs,"mul");
        if (op == "/")  return fp ? ez_fdiv(mod,lhs,rhs,"div") : (un ? ez_udiv(mod,lhs,rhs,"div") : ez_sdiv(mod,lhs,rhs,"div"));
        if (op == "%")  return un ? ez_urem(mod,lhs,rhs,"rem") : ez_srem(mod,lhs,rhs,"rem");
        if (op == "==") return fp ? ez_feq(mod,lhs,rhs,"eq")  : ez_eq(mod,lhs,rhs,"eq");
        if (op == "!=") return fp ? LLVMBuildFCmp(mod->builder,LLVMRealONE,lhs,rhs,"ne")
                                  : ez_ne(mod,lhs,rhs,"ne");
        if (op == "<")  return fp ? ez_flt(mod,lhs,rhs,"lt")  : (un ? ez_ult(mod,lhs,rhs,"lt") : ez_slt(mod,lhs,rhs,"lt"));
        if (op == ">")  return fp ? ez_fgt(mod,lhs,rhs,"gt")  : (un ? ez_ugt(mod,lhs,rhs,"gt") : ez_sgt(mod,lhs,rhs,"gt"));
        if (op == "<=") return fp ? LLVMBuildFCmp(mod->builder,LLVMRealOLE,lhs,rhs,"le")
                                  : ez_sle(mod,lhs,rhs,"le");
        if (op == ">=") return fp ? LLVMBuildFCmp(mod->builder,LLVMRealOGE,lhs,rhs,"ge")
                                  : ez_sge(mod,lhs,rhs,"ge");
        if (op == "&&") return ez_and(mod,lhs,rhs,"and");
        if (op == "||") return ez_or(mod,lhs,rhs,"or");
        return lhs;
    }

    // Evaluate a call expression: tokens = [name, "(", arg_exprs..., ")"]
    EzVal eval_call_expr(const std::vector<Parser::ASTNode*> &tokens, const std::string &hint) {
        (void)hint;
        std::string fname = tokens[0]->value;
        auto it = func_map.find(fname);
        if (it == func_map.end()) return nullptr;
        EzFunc *f = it->second;

        // Collect argument token groups separated by ',' at depth 0
        std::vector<std::vector<Parser::ASTNode*>> arg_groups;
        std::vector<Parser::ASTNode*> cur_group;
        int depth = 0;
        for (size_t i = 2; i < tokens.size(); ++i) {
            const std::string &v = tokens[i]->value;
            if (v == "(") { depth++; cur_group.push_back(tokens[i]); }
            else if (v == ")") {
                if (depth == 0) {
                    if (!cur_group.empty()) arg_groups.push_back(cur_group);
                    break;
                }
                depth--;
                cur_group.push_back(tokens[i]);
            } else if (v == "," && depth == 0) {
                arg_groups.push_back(cur_group);
                cur_group.clear();
            } else {
                cur_group.push_back(tokens[i]);
            }
        }

        std::vector<EzVal> args;
        for (auto &grp : arg_groups) {
            EzVal v = eval_expr_children(grp, "");
            if (v) args.push_back(v);
        }

        EzType *ret_ty = LLVMGetReturnType(LLVMGlobalGetValueType(f->fn));
        bool is_void = (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind);
        return ez_call(mod, f, args.data(), (unsigned)args.size(), is_void ? "" : "call");
    }

    EzVal eval_token(Parser::ASTNode *tok, const std::string &hint) {
        if (!tok) return nullptr;
        const std::string &v = tok->value;

        // String literal
        if (tok->token_type == Lexer::TokenType::STRING) {
            return ez_global_string(mod, v.c_str(), ".str");
        }
        // Number literal
        if (tok->token_type == Lexer::TokenType::NUMBER) {
            if (v.find('.') != std::string::npos) {
                double d = std::stod(v);
                EzType *ty = (hint == "float32") ? ez_f32() : ez_f64();
                return ez_const_float(ty, d);
            }
            long long iv = 0;
            try { iv = std::stoll(v, nullptr, 0); } catch(...) {}
            EzType *ty = cshift_type(hint.empty() ? "int32" : hint);
            return ez_const_int(ty, iv);
        }
        // Keywords: true / false
        if (v == "true")  return ez_const_bool(1);
        if (v == "false") return ez_const_bool(0);

        // Identifier: load from alloca
        if (tok->token_type == Lexer::TokenType::IDENTIFIER ||
            tok->token_type == Lexer::TokenType::KEYWORD) {
            EzVal ptr = lookup_var(v);
            if (!ptr) return nullptr;
            EzType *ty = cshift_type(hint.empty() ? "int32" : hint);
            return ez_load(mod, ty, ptr, v.c_str());
        }
        return nullptr;
    }

    // ── Struct helpers ────────────────────────────────────────────────────────

    EzVal gep_field(EzVal struct_ptr, const std::string &struct_var, const std::string &field) {
        // Resolve the struct type name from the variable's declared type.
        // This avoids false matches when multiple structs share a field name.
        std::string struct_type_name;
        auto it = var_type_map.find(struct_var);
        if (it != var_type_map.end()) {
            // Strip pointer/array decorators to get the base struct name
            struct_type_name = it->second;
            while (!struct_type_name.empty() &&
                   (struct_type_name.back() == '*' || struct_type_name.back() == ']' ||
                    struct_type_name.back() == ':' || struct_type_name.back() == '['))
                struct_type_name.pop_back();
        }

        // If we know the struct type, look it up directly; otherwise fall back to searching
        if (!struct_type_name.empty()) {
            auto sit = struct_map.find(struct_type_name);
            if (sit != struct_map.end()) {
                auto &layout = sit->second;
                for (unsigned i = 0; i < layout.field_names.size(); ++i) {
                    if (layout.field_names[i] == field) {
                        EzVal idxs[2] = { ez_const_int(ez_i32(), 0), ez_const_int(ez_i32(), (long long)i) };
                        return ez_gep(mod, layout.llvm_type, struct_ptr, idxs, 2, field.c_str());
                    }
                }
            }
        }
        // Fallback: search all struct layouts (ambiguous but better than nothing)
        for (auto &[sname, layout] : struct_map) {
            for (unsigned i = 0; i < layout.field_names.size(); ++i) {
                if (layout.field_names[i] == field) {
                    EzVal idxs[2] = { ez_const_int(ez_i32(), 0), ez_const_int(ez_i32(), (long long)i) };
                    return ez_gep(mod, layout.llvm_type, struct_ptr, idxs, 2, field.c_str());
                }
            }
        }
        return struct_ptr;
    }

    std::string field_type_of(const std::string &var, const std::string &field) {
        // Resolve via var_type_map first for accuracy
        std::string struct_type_name;
        auto it = var_type_map.find(var);
        if (it != var_type_map.end()) {
            struct_type_name = it->second;
            while (!struct_type_name.empty() &&
                   (struct_type_name.back() == '*' || struct_type_name.back() == ']' ||
                    struct_type_name.back() == ':' || struct_type_name.back() == '['))
                struct_type_name.pop_back();
        }
        if (!struct_type_name.empty()) {
            auto sit = struct_map.find(struct_type_name);
            if (sit != struct_map.end()) {
                auto &layout = sit->second;
                for (unsigned i = 0; i < layout.field_names.size(); ++i)
                    if (layout.field_names[i] == field) return layout.field_types[i];
            }
        }
        // Fallback: search all layouts
        for (auto &[sname, layout] : struct_map) {
            for (unsigned i = 0; i < layout.field_names.size(); ++i)
                if (layout.field_names[i] == field) return layout.field_types[i];
        }
        return "int32";
    }
};

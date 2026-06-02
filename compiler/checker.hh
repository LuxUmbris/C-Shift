#pragma once
#include "parser.hh"
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================
// C<< Checker — VOP Validation, Voided-State Tracking,
// Tunnel Type Checking, Arena Lifetime Enforcement
// ============================================================

class Checker
{
public:
  // Symbol entry in the scope
  struct Symbol
  {
    std::string name;
    std::string type; // C<< type string
    size_t depth;     // arena depth where declared
    bool is_voided;   // has been moved
    bool is_pointer;  // contains pointer (matters for VOP)
    bool is_const;
    bool is_shared;
  };

  // Collected tunnel info (for reserve-init validation)
  struct TunnelInfo
  {
    std::string type;
    std::string name;
  };

  // Function signature
  struct FuncSig
  {
    std::string name;
    std::vector<std::pair<std::string, std::string>> params; // {type, name}
    std::vector<TunnelInfo> tunnels; // all declared tunnels
  };

  struct CheckError
  {
    size_t line;
    std::string message;
  };

  std::vector<CheckError> errors;
  std::vector<CheckError> warnings;

private:
  // Stack of scopes. Each scope maps name -> Symbol.
  using Scope = std::unordered_map<std::string, Symbol>;
  std::vector<Scope> scope_stack;
  size_t arena_depth = 0;

  // Function table
  std::unordered_map<std::string, FuncSig> func_table;

  // Current function context (for tunnel validation)
  FuncSig *current_func = nullptr;
  bool in_function = false;

  // Loop/Switch context tracking
  int loop_depth = 0;   // for break/continue validation
  int switch_depth = 0; // for break validation

  // Struct types and template types
  std::unordered_set<std::string> struct_types;
  std::unordered_set<std::string> enum_types;
  std::unordered_set<std::string> template_types;

  void push_scope()
  {
    scope_stack.push_back({});
    arena_depth++;
  }
  void pop_scope()
  {
    scope_stack.pop_back();
    arena_depth--;
  }

  void add_error(size_t line, const std::string &msg)
  {
    errors.push_back(
        {line, "[CHECKER ERROR] Line " + std::to_string(line) + ": " + msg});
  }
  void add_warning(size_t line, const std::string &msg)
  {
    warnings.push_back(
        {line, "[CHECKER WARNING] Line " + std::to_string(line) + ": " + msg});
  }

  Symbol *lookup(const std::string &name)
  {
    for (int i = (int)scope_stack.size() - 1; i >= 0; --i)
    {
      auto it = scope_stack[i].find(name);
      if (it != scope_stack[i].end())
        return &it->second;
    }
    return nullptr;
  }

  void declare(const Symbol &sym)
  {
    if (scope_stack.empty())
      return;
    auto &scope = scope_stack.back();
    if (scope.count(sym.name))
      add_error(0, "Re-declaration of '" + sym.name + "' in same arena");
    scope[sym.name] = sym;
  }

  bool is_pointer_type(const std::string &type)
  {
    return type.find('*') != std::string::npos;
  }

  std::string extract_base_type(const std::string &decl_value)
  {
    // "int32 name" -> "int32"
    auto sp = decl_value.find(' ');
    if (sp != std::string::npos)
      return decl_value.substr(0, sp);
    return decl_value;
  }

  std::string extract_name(const std::string &decl_value)
  {
    auto sp = decl_value.find(' ');
    if (sp != std::string::npos)
      return decl_value.substr(sp + 1);
    return decl_value;
  }

  // First pass: collect function signatures for forward resolution
  void collect_signatures(const std::vector<Parser::ASTNode *> &nodes)
  {
    for (auto *n : nodes)
    {
      if (!n)
        continue;
      if (n->type == "Function")
      {
        FuncSig sig;
        sig.name = n->value;
        if (!n->children.empty() && n->children[0]->type == "Parameters")
        {
          for (auto *p : n->children[0]->children)
          {
            if (p->type == "Param")
            {
              sig.params.push_back(
                  {extract_base_type(p->value), extract_name(p->value)});
            }
          }
        }
        // Collect tunnels from body
        if (n->children.size() >= 2)
          collect_tunnels_from_block(n->children[1], sig.tunnels);
        func_table[sig.name] = sig;
      }
      if (n->type == "Struct")
        struct_types.insert(n->value);
      if (n->type == "Enum")
      {
        auto name = n->value;
        auto colon = name.find(':');
        if (colon != std::string::npos)
          name = name.substr(0, colon);
        enum_types.insert(name);
      }
      if (n->type == "Template")
      {
        // Extract the templated definition (Struct or Function)
        if (n->children.size() >= 2)
        {
          auto *def = n->children[1];
          // Recursively process the definition
          collect_signatures({def});
        }
      }
      // C-function declarations: register in func_table so calls don't warn
      if (n->type == "CImport")
      {
        FuncSig sig;
        // value is "rettype funcname"
        sig.name = extract_name(n->value);
        if (!n->children.empty() && n->children[0]->type == "CParams")
        {
          for (auto *p : n->children[0]->children)
          {
            if (p->type == "CParam")
              sig.params.push_back(
                  {extract_base_type(p->value), extract_name(p->value)});
            // Variadic: no param entry needed
          }
        }
        func_table[sig.name] = sig;
      }
      // Recurse into namespaces and blocks (namespaces wrap their contents in a
      // Block)
      if (n->type == "Namespace" || n->type == "Block")
      {
        for (auto *c : n->children)
          collect_signatures({c});
      }
    }
  }

  void collect_tunnels_from_block(Parser::ASTNode *block,
                                  std::vector<TunnelInfo> &out)
  {
    if (!block)
      return;
    for (auto *c : block->children)
    {
      if (!c)
        continue;
      if (c->type == "Tunnel" && c->children.size() >= 2)
      {
        auto *target = c->children[1];
        TunnelInfo ti;
        ti.type = extract_base_type(target->value);
        ti.name = extract_name(target->value);
        // Deduplicate: same-named tunnels across if/else/switch branches count
        // once
        bool already = false;
        for (auto &existing : out)
          if (existing.type == ti.type && existing.name == ti.name)
          {
            already = true;
            break;
          }
        if (!already)
          out.push_back(ti);
      }
      // Recurse into sub-blocks (if/while/for etc.)
      for (auto *sub : c->children)
        if (sub && sub->type == "Block")
          collect_tunnels_from_block(sub, out);
    }
  }

public:
  bool check(const std::vector<Parser::ASTNode *> &ast)
  {
    // Global scope
    push_scope();
    collect_signatures(ast);
    for (auto *node : ast)
      check_node(node);
    pop_scope();
    return errors.empty();
  }

  void print_issues(std::ostream &os) const
  {
    for (auto &w : warnings)
      os << w.message << "\n";
    for (auto &e : errors)
      os << e.message << "\n";
  }

private:
  void check_node(Parser::ASTNode *n)
  {
    if (!n)
      return;

    if (n->type == "Function")
      check_function(n);
    else if (n->type == "Entry")
      check_entry(n);
    else if (n->type == "Struct")
      check_struct(n);
    else if (n->type == "Enum")
    { /* already catalogued, values always valid */
    }
    else if (n->type == "Namespace")
      check_namespace(n);
    else if (n->type == "Declaration")
      check_declaration(n);
    else if (n->type == "Const")
      check_const(n);
    else if (n->type == "Reserve")
      check_reserve(n);
    else if (n->type == "Tunnel")
      check_tunnel(n);
    else if (n->type == "Move")
      check_move(n);
    else if (n->type == "Assignment")
      check_assignment(n);
    else if (n->type == "CallStatement")
      check_call_statement(n);
    else if (n->type == "If")
      check_if(n);
    else if (n->type == "While")
      check_while(n);
    else if (n->type == "For")
      check_for(n);
    else if (n->type == "Foreach")
      check_foreach(n);
    else if (n->type == "Switch")
      check_switch(n);
    else if (n->type == "Break")
      check_break(n);
    else if (n->type == "Continue")
      check_continue(n);
    else if (n->type == "Template")
      check_template(n);
    else if (n->type == "Block")
      check_block(n);
    else if (n->type == "Import")
    { /* file import — nothing to validate semantically */
    }
    else if (n->type == "ModuleImport")
    { /* module import — resolved by ModuleLoader before checker runs */
    }
    else if (n->type == "HeaderImport")
    { /* C-header import — resolved by ModuleLoader before checker runs */
    }
    else if (n->type == "CImport")
      check_c_import(n);
    else if (n->type == "Reset")
    { /* valid anywhere in arena */
    }
    else if (n->type == "Expression" || n->type == "Token")
    {
      // Generic expression: check identifiers for voided state
      check_expression_tokens(n);
    }
  }

  void check_block(Parser::ASTNode *n)
  {
    push_scope();
    for (auto *c : n->children)
      check_node(c);
    pop_scope();
  }

  void check_namespace(Parser::ASTNode *n)
  {
    // Namespaces are lexical — no new arena
    for (auto *c : n->children)
      check_node(c);
  }

  void check_struct(Parser::ASTNode *n)
  {
    struct_types.insert(n->value);
    for (auto *field : n->children)
    {
      if (!field)
        continue;
      std::string ftype = extract_base_type(field->value);
      if (!is_known_type(ftype))
        add_warning(field->line, "Unknown field type '" + ftype +
                                     "' in struct '" + n->value + "'");
    }
  }

  void check_function(Parser::ASTNode *n)
  {
    push_scope();
    FuncSig &sig = func_table[n->value];
    FuncSig *prev_func = current_func;
    bool prev_in_function = in_function;
    current_func = &sig;
    in_function = true;

    // Declare parameters in function scope
    if (!n->children.empty() && n->children[0]->type == "Parameters")
    {
      for (auto *p : n->children[0]->children)
      {
        std::string ptype = extract_base_type(p->value);
        std::string pname = extract_name(p->value);
        declare({pname, ptype, arena_depth, false, is_pointer_type(ptype),
                 false, false});
      }
    }
    // Check body
    if (n->children.size() >= 2)
      check_block(n->children[1]);

    in_function = false;
    current_func = prev_func;
    in_function = prev_in_function;
    pop_scope();
  }

  void check_entry(Parser::ASTNode *n)
  {
    push_scope();
    bool prev = in_function;
    in_function = false;
    if (!n->children.empty())
      check_block(n->children[0]);
    in_function = prev;
    pop_scope();
  }

  void check_declaration(Parser::ASTNode *n)
  {
    std::string type = extract_base_type(n->value);
    std::string name = extract_name(n->value);
    if (!is_known_type(type))
      add_warning(n->line,
                  "Unknown type '" + type + "' for variable '" + name + "'");
    declare(
        {name, type, arena_depth, false, is_pointer_type(type), false, false});
    if (!n->children.empty())
    {
      auto *init = n->children[0];
      // Type-mismatch check: string literal assigned to non-string type
      if (init->type == "Expression" && init->children.size() == 1)
      {
        auto *tok = init->children[0];
        if (tok->token_type == Lexer::TokenType::STRING && type != "string" &&
            type != "char*" && !is_pointer_type(type))
          add_error(n->line, "Type mismatch: string literal assigned to '" +
                                 type + " " + name + "'");
        if ((tok->token_type == Lexer::TokenType::NUMBER) && (type == "string"))
          add_error(n->line, "Type mismatch: numeric literal assigned to '" +
                                 type + " " + name + "'");
      }
      check_expression(init);
    }
  }

  void check_const(Parser::ASTNode *n)
  {
    std::string type = extract_base_type(n->value);
    std::string name = extract_name(n->value);
    declare(
        {name, type, arena_depth, false, is_pointer_type(type), true, false});
    if (!n->children.empty())
      check_expression(n->children[0]);
  }

  void check_reserve(Parser::ASTNode *n)
  {
    std::string type = extract_base_type(n->value);
    std::string name = extract_name(n->value);
    bool is_shared = false;

    size_t child_start = 0;
    if (!n->children.empty() && n->children[0]->type == "Shared")
    {
      is_shared = true;
      child_start = 1;
    }

    declare({name, type, arena_depth, false, is_pointer_type(type), false,
             is_shared});

    if (n->children.size() > child_start)
    {
      auto *init = n->children[child_start];
      // init is an Expression containing function call tokens
      // Validate: if it's a function call, check exactly one tunnel matches
      check_reserve_init(n, type, name, init);
    }
  }

  void check_reserve_init(Parser::ASTNode *reserve_node,
                          const std::string &want_type,
                          const std::string &want_name, Parser::ASTNode *expr)
  {
    if (!expr || expr->children.empty())
      return;
    // Try to detect function call: first child is IDENT, second is "("
    std::string func_name;
    bool looks_like_call = false;
    if (!expr->children.empty() && expr->children[0]->type == "Token")
    {
      func_name = expr->children[0]->value;
      if (expr->children.size() > 1 && expr->children[1]->value == "(")
        looks_like_call = true;
    }

    if (looks_like_call && func_table.count(func_name))
    {
      auto &sig = func_table[func_name];
      // Check if any tunnel matches the exact name (named tunnel call).
      // If no tunnel has this name but there is exactly one tunnel of the
      // right type, allow it as an anonymous inline call
      // (reserve int32 x = add(5,3) where add tunnels int32 result).
      // Only error if there are no matching-type tunnels at all.
      // When type is __infer__, we only need exactly one tunnel to exist.
      // Skip the type-mismatch check entirely; codegen will resolve the type.
      if (want_type == "__infer__")
      {
        if (sig.tunnels.empty())
          add_error(reserve_node->line, "reserve without type: '" + func_name +
                                            "' has no tunnel outputs.");
        else if (sig.tunnels.size() > 1)
          add_error(
              reserve_node->line,
              "reserve without type: '" + func_name +
                  "' has multiple tunnels — specify the type explicitly.");
        // single tunnel: OK
        check_expression(expr);
        return;
      }

      bool exact_match = false;
      int type_matches = 0;
      for (auto &ti : sig.tunnels)
      {
        if (ti.type == want_type && ti.name == want_name)
        {
          exact_match = true;
          break;
        }
        if (ti.type == want_type)
          type_matches++;
      }
      if (!exact_match && type_matches == 0 && !sig.tunnels.empty())
        add_error(reserve_node->line, "reserve: no tunnel of type '" +
                                          want_type + "' found in function '" +
                                          func_name + "'");
    }

    check_expression(expr);
  }

  void check_tunnel(Parser::ASTNode *n)
  {
    if (n->children.size() < 2)
      return;
    auto *expr = n->children[0];
    auto *target = n->children[1];

    std::string t_type = extract_base_type(target->value);
    std::string t_name = extract_name(target->value);

    check_expression(expr);

    if (in_function)
    {
      // Inside a function, tunnels write to the CALL scope (caller's scope).
      // The target variable is declared by the caller or is an output param —
      // we just validate the type annotation is a known type and move on.
      if (!is_known_type(t_type))
        add_error(n->line, "Tunnel: unknown type '" + t_type +
                               "' for target '" + t_name + "'");
      // VOP: warn if tunneling a pointer type out of a function
      if (is_pointer_type(t_type))
        add_warning(
            n->line,
            "VOP Tunnel Law: tunneling pointer type '" + t_type +
                "' out of function '" +
                (current_func ? current_func->name : "?") +
                "' — ensure the pointed-to arena outlives the call site");
      return;
    }

    // Block-scope tunnel: target must exist in a parent scope
    Symbol *sym = lookup(t_name);
    if (!sym)
    {
      add_error(n->line, "Tunnel target '" + t_name +
                             "' not declared in any reachable scope");
      return;
    }
    // Type must match exactly
    if (sym->type != t_type)
    {
      add_error(n->line, "Tunnel type mismatch: '" + t_name +
                             "' declared as '" + sym->type +
                             "' but tunnel specifies '" + t_type + "'");
    }
    // VOP Tunnel Law: cannot tunnel pointers to arenas that will be destroyed
    if (is_pointer_type(t_type) && sym->depth < arena_depth)
    {
      add_warning(n->line, "VOP Tunnel Law: tunneling pointer type '" + t_type +
                               "' from depth " + std::to_string(arena_depth) +
                               " to depth " + std::to_string(sym->depth) +
                               " — ensure the referenced arena outlives '" +
                               t_name + "'");
    }
  }

  void check_move(Parser::ASTNode *n)
  {
    Symbol *sym = lookup(n->value);
    if (!sym)
    {
      add_error(n->line, "move: variable '" + n->value + "' not declared");
      return;
    }
    if (sym->is_voided)
      add_error(n->line, "move: variable '" + n->value + "' is already voided");
    if (sym->is_const)
      add_error(n->line, "move: cannot move const variable '" + n->value + "'");
    sym->is_voided = true;
  }

  void check_assignment(Parser::ASTNode *n)
  {
    // value is "name op"
    auto sp = n->value.rfind(' ');
    std::string var_name = n->value.substr(0, sp);
    // Handle field access: strip to base name
    auto dot = var_name.find('.');
    std::string base_name =
        (dot != std::string::npos) ? var_name.substr(0, dot) : var_name;

    Symbol *sym = lookup(base_name);
    if (!sym)
    {
      add_error(n->line,
                "Assignment to undeclared variable '" + base_name + "'");
    }
    else
    {
      if (sym->is_voided)
        add_error(n->line, "Assignment to voided variable '" + base_name +
                               "' — guard with switch(valid/voided) first");
      if (sym->is_const)
        add_error(n->line, "Assignment to const variable '" + base_name + "'");
      if (sym->is_shared)
        add_error(n->line,
                  "Assignment to shared (read-only after init) variable '" +
                      base_name + "'");
    }
    for (auto *c : n->children)
      check_expression(c);
  }

  void check_call_statement(Parser::ASTNode *n)
  {
    if (!func_table.count(n->value))
      add_warning(n->line, "Call to undeclared function '" + n->value +
                               "' (may be external)");
    if (!n->children.empty())
      for (auto *a : n->children[0]->children)
      {
        // Handle 'move VAR' inside call arguments
        if (a->type == "Expression" && a->children.size() == 2 &&
            a->children[0]->value == "move" && a->children[1]->type == "Token")
        {
          const std::string &mname = a->children[1]->value;
          Symbol *sym = lookup(mname);
          if (!sym)
            add_error(n->line, "move: variable '" + mname + "' not declared");
          else if (sym->is_voided)
            add_error(n->line,
                      "move: variable '" + mname + "' is already voided");
          else if (sym->is_const)
            add_error(n->line,
                      "move: cannot move const variable '" + mname + "'");
          if (sym)
            sym->is_voided = true;
        }
        else
          check_expression(a);
      }
  }

  void check_c_import(Parser::ASTNode *n)
  {
    // value = "rettype funcname"
    std::string ret_type = extract_base_type(n->value);
    std::string func_name = extract_name(n->value);

    // Validate return type (void / C<< primitives are all fine; warn on
    // unknowns)
    if (!is_known_type(ret_type) && ret_type != "voided")
      add_warning(n->line, "CImport '" + func_name +
                               "': unknown return type '" + ret_type + "'");

    // Validate each parameter type
    if (!n->children.empty() && n->children[0]->type == "CParams")
    {
      for (auto *p : n->children[0]->children)
      {
        if (p->type == "Variadic")
          continue; // ... is always valid
        std::string ptype = extract_base_type(p->value);
        if (!is_known_type(ptype) && ptype != "voided")
          add_warning(p->line, "CImport '" + func_name +
                                   "': unknown param type '" + ptype + "'");
      }
    }

    // Already registered in func_table by collect_signatures — nothing else
    // needed.
  }

  void check_if(Parser::ASTNode *n)
  {
    if (!n->children.empty())
      check_expression(n->children[0]);
    if (n->children.size() > 1)
      check_block(n->children[1]);
    if (n->children.size() > 2)
    {
      auto *el = n->children[2];
      if (el->type == "If")
        check_if(el);
      else
        check_block(el);
    }
  }

  void check_while(Parser::ASTNode *n)
  {
    if (!n->children.empty())
      check_expression(n->children[0]);
    if (n->children.size() > 1)
    {
      push_scope();
      loop_depth++;
      check_block(n->children[1]);
      loop_depth--;
      pop_scope();
    }
  }

  void check_for(Parser::ASTNode *n)
  {
    push_scope();
    size_t i = 0;
    loop_depth++;
    for (auto *c : n->children)
    {
      if (i == 0)
        check_declaration(c); // init
      else if (i == 1 || i == 2)
        check_expression(c); // cond, incr
      else
        check_block(c);
      ++i;
    }
    loop_depth--;
    pop_scope();
  }

  void check_foreach(Parser::ASTNode *n)
  {
    push_scope();
    loop_depth++;
    std::string type = extract_base_type(n->value);
    std::string item = extract_name(n->value);
    declare(
        {item, type, arena_depth, false, is_pointer_type(type), false, false});
    if (!n->children.empty())
      check_expression(n->children[0]);
    if (n->children.size() > 1)
      check_block(n->children[1]);
    loop_depth--;
    pop_scope();
  }

  void check_switch(Parser::ASTNode *n)
  {
    if (n->children.empty())
      return;

    // Determine if any case is valid/voided — making this a voided-state guard
    bool is_voided_guard = false;
    bool has_valid_case = false, has_voided_case = false;
    for (size_t i = 1; i < n->children.size(); ++i)
    {
      auto *c = n->children[i];
      if (c->type == "Case" && c->value == "valid")
      {
        is_voided_guard = true;
        has_valid_case = true;
      }
      if (c->type == "Case" && c->value == "voided")
      {
        is_voided_guard = true;
        has_voided_case = true;
      }
    }

    // Get the switched variable name (first token of the switch expression)
    std::string switched_var;
    if (!n->children[0]->children.empty())
      switched_var = n->children[0]->children[0]->value;

    // For a valid/voided guard, accessing a voided variable in the switch
    // expression is legal
    bool was_voided = false;
    if (is_voided_guard && !switched_var.empty())
    {
      Symbol *sym = lookup(switched_var);
      if (sym && sym->is_voided)
      {
        was_voided = true;
        sym->is_voided = false;
      }
    }
    check_expression(n->children[0]);
    // Restore voided state for branch checking
    if (was_voided)
    {
      Symbol *sym = lookup(switched_var);
      if (sym)
        sym->is_voided = true;
    }

    // Warn if switching on a voided variable without proper guards
    if (was_voided)
    {
      if (!has_valid_case)
        add_warning(n->line, "switch on voided variable '" + switched_var +
                                 "' without 'case valid'");
      if (!has_voided_case)
        add_warning(n->line, "switch on voided variable '" + switched_var +
                                 "' without 'case voided'");
    }

    // Check each case body
    switch_depth++;
    for (size_t i = 1; i < n->children.size(); ++i)
    {
      auto *c = n->children[i];
      if (c->type == "Case")
      {
        push_scope();
        if (c->value == "valid" && was_voided)
        {
          // Inside the 'valid' branch: the variable is accessible
          Symbol *sym = lookup(switched_var);
          if (sym)
            sym->is_voided = false;
          for (auto *stmt : c->children)
            check_node(stmt);
          if (sym)
            sym->is_voided = true; // restore after branch
        }
        else if (c->value == "voided")
        {
          // Inside the 'voided' branch: variable stays voided
          for (auto *stmt : c->children)
            check_node(stmt);
        }
        else
        {
          for (auto *stmt : c->children)
            check_node(stmt);
        }
        pop_scope();
      }
      else if (c->type == "Default")
      {
        push_scope();
        for (auto *stmt : c->children)
          check_node(stmt);
        pop_scope();
      }
    }
    switch_depth--;
  }

  void check_expression(Parser::ASTNode *n)
  {
    if (!n)
      return;
    check_expression_tokens(n);
  }

  void check_expression_tokens(Parser::ASTNode *n)
  {
    if (!n)
      return;
    // Check each identifier token
    if (n->type == "Token")
    {
      auto *sym = lookup(n->value);
      if (sym && sym->is_voided)
      {
        add_error(n->line, "Use of voided variable '" + n->value +
                               "' — guard with switch(" + n->value +
                               ") { case valid / case voided }");
      }
      // VOP Lifetime Law: pointer access
      if (sym && sym->is_pointer)
      {
        // Pointer depth must be <= current depth (it was declared at its depth)
        // This is enforced by the fact that pointers can't be stored beyond
        // their arena
      }
      return;
    }
    // Handle 'move VAR' as an Expression node with two Token children
    if (n->type == "Expression" && n->children.size() == 2 &&
        n->children[0]->value == "move" && n->children[1]->type == "Token")
    {
      const std::string &mname = n->children[1]->value;
      Symbol *sym = lookup(mname);
      if (!sym)
        add_error(n->children[1]->line,
                  "move: variable '" + mname + "' not declared");
      else if (sym->is_voided)
        add_error(n->children[1]->line,
                  "move: variable '" + mname + "' is already voided");
      else if (sym->is_const)
        add_error(n->children[1]->line,
                  "move: cannot move const variable '" + mname + "'");
      if (sym)
        sym->is_voided = true;
      return;
    }
    // Handle flat token list: look for 'move' keyword token followed by IDENT
    if (n->type == "Expression")
    {
      for (size_t i = 0; i + 1 < n->children.size(); ++i)
      {
        if (n->children[i]->value == "move" &&
            n->children[i + 1]->type == "Token" &&
            n->children[i + 1]->token_type == Lexer::TokenType::IDENTIFIER)
        {
          const std::string &mname = n->children[i + 1]->value;
          Symbol *sym = lookup(mname);
          if (!sym)
            add_error(n->children[i]->line,
                      "move: variable '" + mname + "' not declared");
          else if (sym->is_voided)
            add_error(n->children[i]->line,
                      "move: variable '" + mname + "' is already voided");
          else if (sym->is_const)
            add_error(n->children[i]->line,
                      "move: cannot move const variable '" + mname + "'");
          if (sym)
            sym->is_voided = true;
          ++i; // skip the variable token
          continue;
        }
      }
    }
    for (auto *c : n->children)
      check_expression_tokens(c);
  }

  void check_break(Parser::ASTNode *n)
  {
    if (loop_depth == 0 && switch_depth == 0)
    {
      add_error(n->line, "break statement outside of loop or switch");
    }
  }

  void check_continue(Parser::ASTNode *n)
  {
    if (loop_depth == 0)
    {
      add_error(n->line, "continue statement outside of loop");
    }
  }

  void check_template(Parser::ASTNode *n)
  {
    if (n->children.empty())
    {
      add_error(n->line, "Template node has no children");
      return;
    }

    // First child should be TemplateParams
    auto *params = n->children[0];
    if (!params || params->type != "TemplateParams")
    {
      add_error(n->line, "Template missing TemplateParams");
      return;
    }

    // Collect template parameter names
    std::unordered_set<std::string> template_params;
    for (auto *p : params->children)
    {
      if (p && p->type == "TypeParam")
      {
        template_params.insert(p->value);
      }
    }

    // Second child should be the templated definition (Struct or Function)
    if (n->children.size() < 2)
    {
      add_error(n->line, "Template missing definition");
      return;
    }

    auto *def = n->children[1];
    if (!def)
    {
      add_error(n->line, "Template definition is null");
      return;
    }

    if (def->type == "Struct")
    {
      // Register templated struct type
      std::string templ_name = def->value + "<...>";
      template_types.insert(templ_name);
      check_struct(def);
    }
    else if (def->type == "Function")
    {
      // Register templated function
      std::string templ_name = def->value + "<...>";
      // Could track template functions separately if needed
      check_function(def);
    }
    else if (def->type == "CImport" || def->type == "ModuleImport" ||
             def->type == "Import")
    {
      // template<typename T> import rettype fn(params); — treat as plain
      // CImport
      check_node(def);
    }
    else if (def->type == "Template")
    {
      // nested template<K> template<V> struct/import — recurse
      check_template(def);
    }
    else
    {
      add_error(
          n->line,
          "Template can only precede struct, function, or import definitions");
    }
  }

  bool is_known_type(const std::string &t) const
  {
    static const std::unordered_set<std::string> primitives = {
        "int8",   "int16",   "int32",   "int64", "uint8", "uint16", "uint32",
        "uint64", "float32", "float64", "char",  "bool",  "string", "void"};
    std::string base = t;
    // Strip pointer/array decorators
    while (!base.empty() && (base.back() == '*' || base.back() == ']' ||
                             base.back() == ':' || base.back() == '['))
      base.pop_back();
    // Strip generic type params: Vector<T> → Vector, Foo<A,B> → Foo
    auto lt = base.find('<');
    if (lt != std::string::npos)
      base = base.substr(0, lt);
    if (primitives.count(base))
      return true;
    // treat 'voided' as a known primitive type
    if (base == "voided")
      return true;
    // Single-letter or short uppercase names are template type parameters (T,
    // K, V, etc.)
    if (!base.empty() && base.size() <= 2 &&
        std::isupper((unsigned char)base[0]) &&
        (base.size() == 1 || std::isupper((unsigned char)base[1])))
      return true;
    if (struct_types.count(base))
      return true;
    if (enum_types.count(base))
      return true;
    // Check template types (e.g., "Vector<int32>")
    if (template_types.count(base))
      return true;
    // If any struct type is registered with matching prefix (templated struct)
    for (auto &s : struct_types)
      if (s == base)
        return true;
    return false;
  }
};

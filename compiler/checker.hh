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
    std::string type;     // C<< type string
    size_t depth;         // arena depth where declared
    bool is_voided;       // definitely moved at this point in all paths
    bool is_voided_maybe; // moved on some paths but not others → needs runtime bool
    bool is_pointer;      // contains pointer (matters for VOP)
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
    std::vector<TunnelInfo> tunnels;                         // all declared tunnels
    bool is_variadic = false; // true for C-imported variadic functions (e.g. printf)
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

  // ── Compile-time constant expression evaluator ────────────────────────────
  // Returns {known, value}: known=true when the expression is a compile-time
  // constant we can fold.  Handles literals, simple comparisons, and logical
  // operators on literals.  Conservative: returns known=false for anything
  // involving variables or function calls.
  struct ConstVal
  {
    bool known;
    long long value;
  };

  ConstVal eval_const_expr(Parser::ASTNode *n) const
  {
    if (!n)
      return {false, 0};

    // Single-token: numeric literal
    if (n->type == "Token")
    {
      if (n->token_type == Lexer::TokenType::NUMBER)
      {
        try
        {
          return {true, (long long)std::stod(n->value)};
        }
        catch (...)
        {
        }
      }
      if (n->value == "true")
        return {true, 1};
      if (n->value == "false")
        return {true, 0};
      // Variable — not a constant we know
      return {false, 0};
    }

    // Expression node: collect tokens and try to evaluate
    if (n->type == "Expression")
    {
      auto &ch = n->children;
      // Single child — recurse
      if (ch.size() == 1)
        return eval_const_expr(ch[0]);

      // Binary operator pattern: [lhs_tokens...] [op] [rhs_tokens...]
      // We only handle simple 3-token case: lhs op rhs
      if (ch.size() == 3)
      {
        auto lv = eval_const_expr(ch[0]);
        auto rv = eval_const_expr(ch[2]);
        if (!lv.known || !rv.known)
          return {false, 0};
        const std::string &op = ch[1]->value;
        long long l = lv.value, r = rv.value;
        if (op == "+")
          return {true, l + r};
        if (op == "-")
          return {true, l - r};
        if (op == "*")
          return {true, l * r};
        if (op == "/")
          return {true, r != 0 ? l / r : 0};
        if (op == "%")
          return {true, r != 0 ? l % r : 0};
        if (op == "==")
          return {true, l == r ? 1 : 0};
        if (op == "!=")
          return {true, l != r ? 1 : 0};
        if (op == "<")
          return {true, l < r ? 1 : 0};
        if (op == ">")
          return {true, l > r ? 1 : 0};
        if (op == "<=")
          return {true, l <= r ? 1 : 0};
        if (op == ">=")
          return {true, l >= r ? 1 : 0};
        if (op == "&&")
          return {true, (l && r) ? 1 : 0};
        if (op == "||")
          return {true, (l || r) ? 1 : 0};
      }
      // Prefix: op rhs
      if (ch.size() == 2 && ch[0]->type == "Token")
      {
        auto rv = eval_const_expr(ch[1]);
        if (!rv.known)
          return {false, 0};
        if (ch[0]->value == "!")
          return {true, rv.value ? 0 : 1};
        if (ch[0]->value == "-")
          return {true, -rv.value};
      }
      // For multi-token expressions, try to find a binary op in the middle
      // by scanning for a known operator at the top level (depth 0)
      // This handles `1 + 2 + 3` etc. by left-fold
      ConstVal acc = eval_const_expr(ch[0]);
      for (size_t i = 1; i + 1 < ch.size(); i += 2)
      {
        if (!acc.known)
          return {false, 0};
        auto rv2 = eval_const_expr(ch[i + 1]);
        if (!rv2.known)
          return {false, 0};
        const std::string &op = ch[i]->value;
        long long l = acc.value, r = rv2.value;
        if (op == "+")
        {
          acc = {true, l + r};
          continue;
        }
        if (op == "-")
        {
          acc = {true, l - r};
          continue;
        }
        if (op == "*")
        {
          acc = {true, l * r};
          continue;
        }
        if (op == "/")
        {
          acc = {true, r ? l / r : 0};
          continue;
        }
        if (op == "==")
        {
          acc = {true, l == r ? 1 : 0};
          continue;
        }
        if (op == "!=")
        {
          acc = {true, l != r ? 1 : 0};
          continue;
        }
        if (op == "<")
        {
          acc = {true, l < r ? 1 : 0};
          continue;
        }
        if (op == ">")
        {
          acc = {true, l > r ? 1 : 0};
          continue;
        }
        if (op == "<=")
        {
          acc = {true, l <= r ? 1 : 0};
          continue;
        }
        if (op == ">=")
        {
          acc = {true, l >= r ? 1 : 0};
          continue;
        }
        if (op == "&&")
        {
          acc = {true, (l && r) ? 1 : 0};
          continue;
        }
        if (op == "||")
        {
          acc = {true, (l || r) ? 1 : 0};
          continue;
        }
        return {false, 0};
      }
      return acc;
    }
    return {false, 0};
  }

  // Convenience: evaluate if-condition as a tristate
  // Returns: -1 = definitely false, 1 = definitely true, 0 = unknown
  int eval_branch_condition(Parser::ASTNode *cond) const
  {
    if (!cond)
      return 0;
    auto cv = eval_const_expr(cond);
    if (!cv.known)
      return 0;
    return cv.value ? 1 : -1;
  }

  std::vector<Scope> scope_stack;
  size_t arena_depth = 0;

  // Function table
  std::unordered_map<std::string, FuncSig> func_table;
  // Names registered via a `dec` node (as opposed to a plain `def`).
  std::unordered_set<std::string> funcdecl_names;
  // What each `dec` actually declared (tunnel list from the dec itself, not the def body).
  std::unordered_map<std::string, FuncSig> dec_sig_table;
  // using Alias = type;
  std::unordered_map<std::string, std::string> type_aliases;
  std::unordered_set<std::string> imported_namespaces;
  std::unordered_map<std::string, std::string> ns_aliases;
  // Struct fields for class methods: synth-node-address → field list
  std::unordered_map<Parser::ASTNode *, std::vector<std::pair<std::string, std::string>>>
      class_method_fields;

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
    errors.push_back({line, "[CHECKER ERROR] Line " + std::to_string(line) + ": " + msg});
  }
  void add_warning(size_t line, const std::string &msg)
  {
    warnings.push_back({line, "[CHECKER WARNING] Line " + std::to_string(line) + ": " + msg});
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

  bool is_pointer_type(const std::string &type) { return type.find('*') != std::string::npos; }

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
              sig.params.push_back({extract_base_type(p->value), extract_name(p->value)});
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
      if (n->type == "FuncDecl")
      {
        // dec name(params) [-> type t1, type t2];
        FuncSig sig;
        sig.name = n->value;
        if (!n->children.empty() && n->children[0]->type == "Parameters")
          for (auto *p : n->children[0]->children)
            if (p->type == "Param")
              sig.params.push_back({extract_base_type(p->value), extract_name(p->value)});
        if (n->children.size() >= 2 && n->children[1]->type == "DeclTunnels")
          for (auto *t : n->children[1]->children)
            sig.tunnels.push_back({extract_base_type(t->value), extract_name(t->value)});
        func_table[sig.name] = sig;
        funcdecl_names.insert(sig.name);
        dec_sig_table[sig.name] = sig; // preserve what the dec itself declared
      }
      if (n->type == "Class")
      {
        // class Name { fields; methods }
        // → register Name as a struct type, and Name_method as functions
        struct_types.insert(n->value);
        Parser::ASTNode *fields_node = nullptr, *methods_node = nullptr;
        for (auto *c : n->children)
        {
          if (c->type == "Fields")
            fields_node = c;
          if (c->type == "Methods")
            methods_node = c;
        }
        // Register struct fields for field-access checking
        if (fields_node)
        {
          auto *struct_node = new Parser::ASTNode("Struct", n->value, n->line, n->depth);
          for (auto *f : fields_node->children)
            struct_node->children.push_back(f);
          check_struct(struct_node);
        }
        // Register each method as Name_method with self prepended
        if (methods_node)
          for (auto *m : methods_node->children)
          {
            FuncSig sig;
            sig.name = n->value + "_" + m->value;
            sig.params.push_back({n->value + "*", "self"});
            Parser::ASTNode *orig_params = nullptr;
            for (auto *c : m->children)
              if (c->type == "Parameters")
                orig_params = c;
            if (orig_params)
              for (auto *p : orig_params->children)
                if (p->type == "Param")
                  sig.params.push_back({extract_base_type(p->value), extract_name(p->value)});
            if (m->children.size() >= 2)
              collect_tunnels_from_block(m->children.back(), sig.tunnels);
            func_table[sig.name] = sig;
          }
      }
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
              sig.params.push_back({extract_base_type(p->value), extract_name(p->value)});
            else if (p->type == "Variadic")
              sig.is_variadic = true; // no fixed param entry, but marks "..." present
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

  void collect_tunnels_from_block(Parser::ASTNode *block, std::vector<TunnelInfo> &out)
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
    else if (n->type == "FuncDecl")
    {
      // Validate: if the matching def has tunnel outputs, the dec MUST declare
      // them with `-> type name` — otherwise codegen creates a wrong signature.
      // We cross-check during collect_signatures pass by comparing what the
      // dec declares vs what the def body will produce.  Since we can't run
      // the def body here, we validate the other way: if dec declares tunnels,
      // they must match the Function body that follows (checked in check_function).
      // If dec has NO tunnel declarations at all, warn now — the user must add
      // `-> type name` to make the forward declaration usable.
      // We defer the actual mismatch check to check_function below.
      (void)n; // collected in collect_signatures; cross-check done in check_function
    }
    else if (n->type == "Class")
      check_class(n);
    else if (n->type == "Entry")
      check_entry(n);
    else if (n->type == "Struct")
      check_struct(n);
    else if (n->type == "Enum")
    { /* already catalogued, values always valid */
    }
    else if (n->type == "Namespace")
      check_namespace(n);
    else if (n->type == "UsingAlias")
    {
      if (!n->children.empty())
      {
        const std::string &target = n->children[0]->value;
        type_aliases[n->value] = target;
        if (struct_types.count(target) || enum_types.count(target))
          struct_types.insert(n->value);
      }
    }
    else if (n->type == "UsingNamespace")
      imported_namespaces.insert(n->value);
    else if (n->type == "UsingNsAlias")
    {
      if (!n->children.empty())
        ns_aliases[n->value] = n->children[0]->value;
    }
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
    else if (n->type == "IndexAssignment")
      check_index_assignment(n);
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

  // class Name { fields; def method(params) { body } }
  // Each method is checked as if it were:
  //   def Name_method(Name* self, params) { body }
  // using the FuncSig already registered in collect_signatures.
  void check_class(Parser::ASTNode *n)
  {
    // Collect field names and types for the class
    std::vector<std::pair<std::string, std::string>> class_fields;
    for (auto *c : n->children)
    {
      if (c->type == "Fields")
        for (auto *f : c->children)
        {
          if (f->type == "Field")
          {
            auto sp = f->value.find(' ');
            if (sp != std::string::npos)
              class_fields.push_back({f->value.substr(0, sp), f->value.substr(sp + 1)});
          }
        }
    }

    Parser::ASTNode *methods_node = nullptr;
    for (auto *c : n->children)
      if (c->type == "Methods")
        methods_node = c;
    if (!methods_node)
      return;

    for (auto *m : methods_node->children)
    {
      std::string fn_name = n->value + "_" + m->value;

      Parser::ASTNode *orig_params = nullptr;
      Parser::ASTNode *body = nullptr;
      for (auto *c : m->children)
      {
        if (c->type == "Parameters")
          orig_params = c;
        else if (c->type == "Block")
          body = c;
      }

      auto *synth = new Parser::ASTNode("Function", fn_name, m->line, m->depth);
      auto *params = new Parser::ASTNode("Parameters", "", m->line, m->depth);
      params->children.push_back(
          new Parser::ASTNode("Param", n->value + "* self", m->line, m->depth));
      if (orig_params)
        for (auto *p : orig_params->children)
          params->children.push_back(p);
      synth->children.push_back(params);
      synth->children.push_back(body ? body : new Parser::ASTNode("Block", "", m->line, m->depth));

      // Pass the struct field list so check_function can inject them into scope.
      // We use a per-call map keyed on the synthetic node pointer.
      synth->meta = "__class_method__";
      class_method_fields[synth] = class_fields;

      check_function(synth);
      class_method_fields.erase(synth);
    }
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
        add_warning(field->line, "Unknown field type '" + ftype + "' in struct '" + n->value + "'");
      if (ftype.size() >= 2 && ftype.substr(ftype.size() - 2) == "[]")
        add_warning(field->line,
                    "'" + ftype +
                        "' as a struct/class field only supports raw element "
                        "access (no .len(), no subscript bounds tracking) — use Vector<" +
                        ftype.substr(0, ftype.size() - 2) + "> instead for length-aware access.");
    }
  }

  // Collect all unique tunnel names declared in a block (recursive).
  void collect_body_tunnels(Parser::ASTNode *n, std::unordered_set<std::string> &out)
  {
    if (!n)
      return;
    if (n->type == "Tunnel")
    {
      // Tunnel node: children[1] is a Target node with value "type name"
      for (auto *c : n->children)
      {
        if (c->type == "Target")
        {
          auto sp = c->value.rfind(' ');
          if (sp != std::string::npos)
            out.insert(c->value.substr(sp + 1)); // name after type
        }
      }
      return; // don't recurse into tunnel children further
    }
    for (auto *c : n->children)
      collect_body_tunnels(c, out);
  }

  void check_function(Parser::ASTNode *n)
  {
    push_scope();
    FuncSig &sig = func_table[n->value];
    FuncSig *prev_func = current_func;
    bool prev_in_function = in_function;
    current_func = &sig;
    in_function = true;

    // Cross-validate dec <-> def tunnel signatures.
    if (funcdecl_names.count(n->value))
    {
      std::unordered_set<std::string> body_tunnels;
      if (n->children.size() >= 2)
        collect_body_tunnels(n->children[1], body_tunnels);

      // dec_sig_table holds what the dec ITSELF declared (not overwritten by def)
      std::unordered_set<std::string> dec_tunnels;
      auto dit = dec_sig_table.find(n->value);
      if (dit != dec_sig_table.end())
        for (auto &t : dit->second.tunnels)
          dec_tunnels.insert(t.name);

      // dec declares tunnel not present in body
      for (auto &dt : dec_tunnels)
        if (body_tunnels.count(dt) == 0)
          add_error(n->line, "dec '" + n->value + "' declares tunnel '" + dt +
                                 "' but def body has no matching 'tunnel ... -> ... " + dt + "'");

      // body has tunnels but dec declares none → codegen will crash
      if (!body_tunnels.empty() && dec_tunnels.empty())
        add_error(
            n->line, "def '" + n->value + "' has tunnel output(s) [" +
                         [&]
                         {
                           std::string s;
                           for (auto &t : body_tunnels)
                             s += t + " ";
                           return s;
                         }() +
                         "] but its forward declaration is missing '-> type name' — "
                         "write:  dec " +
                         n->value + "(params) -> " + *body_tunnels.begin() + ";");
    }

    // Declare parameters in function scope
    if (!n->children.empty() && n->children[0]->type == "Parameters")
    {
      for (auto *p : n->children[0]->children)
      {
        std::string ptype = extract_base_type(p->value);
        std::string pname = extract_name(p->value);
        declare({pname, ptype, arena_depth, false, false, is_pointer_type(ptype), false, false});
      }
    }

    // If this is a class method, inject struct fields into scope
    if (n->meta == "__class_method__")
    {
      auto fit = class_method_fields.find(n);
      if (fit != class_method_fields.end())
        for (auto &[ftype, fname] : fit->second)
          declare({fname, ftype, arena_depth, false, false, is_pointer_type(ftype), false, false});
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
      add_error(n->line, "Unknown type '" + type +
                             "' — did you forget to import a header or define the struct?");
    declare({name, type, arena_depth, false, false, is_pointer_type(type), false, false});
    if (!n->children.empty())
    {
      auto *init = n->children[0];
      // Resolve 'using' aliases before type checks
      std::string resolved_type = type;
      {
        auto ait = type_aliases.find(type);
        if (ait != type_aliases.end())
          resolved_type = ait->second;
      }
      // Type-mismatch check: string literal assigned to non-string type
      if (init->type == "Expression" && init->children.size() == 1)
      {
        auto *tok = init->children[0];
        bool is_str_type =
            (resolved_type == "string" || resolved_type == "char*" || resolved_type == "u8*" ||
             resolved_type == "i8*" || is_pointer_type(resolved_type));
        if (tok->token_type == Lexer::TokenType::STRING && !is_str_type)
          add_error(n->line,
                    "Type mismatch: string literal assigned to '" + type + " " + name + "'");
        if (tok->token_type == Lexer::TokenType::NUMBER && resolved_type == "string")
          add_error(n->line,
                    "Type mismatch: numeric literal assigned to '" + type + " " + name + "'");
      }
      check_expression(init);
    }
  }

  void check_const(Parser::ASTNode *n)
  {
    std::string type = extract_base_type(n->value);
    std::string name = extract_name(n->value);

    if (!is_known_type(type))
      add_warning(n->line, "Unknown type '" + type + "' for const '" + name + "'");

    declare({name, type, arena_depth, false, false, is_pointer_type(type), true, false});

    if (!n->children.empty())
    {
      auto *init = n->children[0];
      // Same literal type-mismatch checks as check_declaration
      if (init->type == "Expression" && init->children.size() == 1)
      {
        auto *tok = init->children[0];
        if (tok->token_type == Lexer::TokenType::STRING && type != "string" && type != "char*" &&
            !is_pointer_type(type))
          add_error(n->line,
                    "Type mismatch: string literal assigned to const '" + type + " " + name + "'");
        if (tok->token_type == Lexer::TokenType::NUMBER && type == "string")
          add_error(n->line,
                    "Type mismatch: numeric literal assigned to const '" + type + " " + name + "'");
        if (tok->token_type == Lexer::TokenType::NUMBER && (type == "bool") && tok->value != "0" &&
            tok->value != "1")
          add_warning(n->line, "Suspicious: non-boolean numeric literal assigned to const 'bool " +
                                   name + "'");
        // float literal to integer type
        if (tok->token_type == Lexer::TokenType::NUMBER &&
            tok->value.find('.') != std::string::npos)
        {
          if (type == "int8" || type == "int16" || type == "int32" || type == "int64" ||
              type == "uint8" || type == "uint16" || type == "uint32" || type == "uint64")
            add_error(n->line, "Type mismatch: float literal assigned to integer const '" + type +
                                   " " + name + "'");
        }
      }
      check_expression(init);
    }
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

    // Extended syntax: reserve [type] name << tunnel_name [= call();]
    std::string explicit_tunnel_name;
    if (n->children.size() > child_start && n->children[child_start]->type == "TunnelBind")
    {
      explicit_tunnel_name = n->children[child_start]->value;
      child_start++;
    }

    declare({name, type, arena_depth, false, false, is_pointer_type(type), false, is_shared});

    if (n->children.size() > child_start)
    {
      auto *init = n->children[child_start];
      // init is an Expression containing function call tokens
      // Validate: if it's a function call, check exactly one tunnel matches
      check_reserve_init(n, type, name, init, explicit_tunnel_name);

      // If a TunnelBind name was given, verify the called function actually
      // has a tunnel with that name.
      if (!explicit_tunnel_name.empty() && !init->children.empty() &&
          init->children[0]->type == "Token")
      {
        std::string func_name = init->children[0]->value;
        std::string lookup_name = func_name;
        // Method call: obj.method(...) → resolve "ClassName_method"
        if (init->children.size() >= 4 && init->children[1]->value == "." &&
            init->children[3]->value == "(")
        {
          std::string obj_name = init->children[0]->value;
          std::string method_name = init->children[2]->value;
          auto *sym = lookup(obj_name);
          std::string base_type = sym ? sym->type : "";
          auto lt = base_type.find('<');
          if (lt != std::string::npos)
            base_type = base_type.substr(0, lt);
          lookup_name = base_type + "_" + method_name;
          func_name = lookup_name;
        }
        if (func_table.count(lookup_name))
        {
          auto &sig = func_table[lookup_name];
          bool found = false;
          for (auto &t : sig.tunnels)
            if (t.name == explicit_tunnel_name)
            {
              found = true;
              break;
            }
          if (!found)
            add_error(n->line, "reserve: '" + func_name + "' has no tunnel output named '" +
                                   explicit_tunnel_name + "'");
        }
      }
    }
  }

  void check_reserve_init(Parser::ASTNode *reserve_node, const std::string &want_type,
                          const std::string &want_name, Parser::ASTNode *expr,
                          const std::string &tunnel_bind_name = "")
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
          add_error(reserve_node->line,
                    "reserve without type: '" + func_name + "' has no tunnel outputs.");
        else if (!tunnel_bind_name.empty())
        {
          // Explicit binding: reserve name << tunnel_name = call();
          // Valid as long as the named tunnel exists (checked separately
          // in check_reserve via explicit_tunnel_name validation above).
        }
        else if (sig.tunnels.size() > 1)
          add_error(reserve_node->line,
                    "reserve without type: '" + func_name +
                        "' has multiple tunnels — specify the type explicitly or use "
                        "'reserve name << tunnel_name = " +
                        func_name + "();'.");
        // single tunnel, or explicit binding to an existing tunnel: OK
        check_expression(expr);
        return;
      }

      bool exact_match = false;
      int type_matches = 0;
      for (auto &ti : sig.tunnels)
      {
        bool ti_is_template_param =
            (!ti.type.empty() && ti.type.size() <= 2 && std::isupper((unsigned char)ti.type[0]) &&
             (ti.type.size() == 1 || std::isupper((unsigned char)ti.type[1])));
        bool type_ok = (ti.type == want_type) || ti_is_template_param;
        if (type_ok && ti.name == want_name)
        {
          exact_match = true;
          break;
        }
        if (type_ok)
          type_matches++;
      }
      if (!exact_match && type_matches == 0 && !sig.tunnels.empty())
        add_error(reserve_node->line, "reserve: no tunnel of type '" + want_type +
                                          "' found in function '" + func_name + "'");
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
        add_error(n->line, "Tunnel: unknown type '" + t_type + "' for target '" + t_name + "'");
      // VOP: warn if tunneling a pointer type out of a function
      if (is_pointer_type(t_type))
        add_warning(n->line, "VOP Tunnel Law: tunneling pointer type '" + t_type +
                                 "' out of function '" + (current_func ? current_func->name : "?") +
                                 "' — ensure the pointed-to arena outlives the call site");
      return;
    }

    // Block-scope tunnel: target must exist in a parent scope
    Symbol *sym = lookup(t_name);
    if (!sym)
    {
      add_error(n->line, "Tunnel target '" + t_name + "' not declared in any reachable scope");
      return;
    }
    // Type must match exactly
    if (sym->type != t_type)
    {
      add_error(n->line, "Tunnel type mismatch: '" + t_name + "' declared as '" + sym->type +
                             "' but tunnel specifies '" + t_type + "'");
    }
    // VOP Tunnel Law: cannot tunnel pointers to arenas that will be destroyed
    if (is_pointer_type(t_type) && sym->depth < arena_depth)
    {
      add_warning(n->line, "VOP Tunnel Law: tunneling pointer type '" + t_type + "' from depth " +
                               std::to_string(arena_depth) + " to depth " +
                               std::to_string(sym->depth) +
                               " — ensure the referenced arena outlives '" + t_name + "'");
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
    std::string base_name = (dot != std::string::npos) ? var_name.substr(0, dot) : var_name;

    Symbol *sym = lookup(base_name);
    if (!sym)
    {
      add_error(n->line, "Assignment to undeclared variable '" + base_name + "'");
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
                  "Assignment to shared (read-only after init) variable '" + base_name + "'");
    }
    for (auto *c : n->children)
      check_expression(c);
  }

  void check_call_statement(Parser::ASTNode *n)
  {
    const std::string &fullname = n->value;

    // Get arg spans up front (handles the parser's merged-args quirk)
    Parser::ASTNode *args_node = n->children.empty() ? nullptr : n->children[0];
    auto arg_spans = get_call_statement_arg_spans(args_node);

    auto dot = fullname.find('.');
    if (dot != std::string::npos)
    {
      // Method call: v.method(args) — check base var exists
      std::string obj = fullname.substr(0, dot);
      std::string method = fullname.substr(dot + 1);
      Symbol *obj_sym = lookup(obj);
      if (!obj_sym)
        add_error(n->line, "Undeclared variable '" + obj + "'");

      // Validate arity + undefined vars when object is known
      if (obj_sym)
      {
        // Only validate count for class-defined methods (resolved name in func_table)
        std::string base = strip_type_decorators(obj_sym->type);
        std::string resolved = base + "_" + method;
        std::string display = obj + "." + method;
        validate_call_args(resolved, display, /*is_method=*/true, arg_spans, n->line);
      }
      else
      {
        // Object undeclared — args still validated via check_expression() below
      }

      // Still recurse into arg expressions for nested call detection
      if (args_node)
        for (auto *a : args_node->children)
          check_expression(a);
      return;
    }

    // Free function call
    if (!func_table.count(fullname))
      add_error(n->line, "Call to undeclared function '" + fullname +
                             "' — declare it with: import voided " + fullname + "(params);");
    validate_call_args(fullname, fullname, /*is_method=*/false, arg_spans, n->line);

    // Recurse into arg expressions for nested call detection + move handling
    if (args_node)
      for (auto *a : args_node->children)
      {
        if (a->type == "Expression" && a->children.size() == 2 && a->children[0]->value == "move" &&
            a->children[1]->type == "Token")
        {
          const std::string &mname = a->children[1]->value;
          Symbol *sym = lookup(mname);
          if (!sym)
            add_error(n->line, "move: variable '" + mname + "' not declared");
          else if (sym->is_voided)
            add_error(n->line, "move: variable '" + mname + "' is already voided");
          else if (sym->is_const)
            add_error(n->line, "move: cannot move const variable '" + mname + "'");
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

    // Validate return type
    if (ret_type.rfind("flat:", 0) == 0)
    { /* flat struct return — always valid */
    }
    else if (!is_known_type(ret_type) && ret_type != "voided")
      add_warning(n->line, "CImport '" + func_name + "': unknown return type '" + ret_type + "'");

    // Validate each parameter type
    if (!n->children.empty() && n->children[0]->type == "CParams")
    {
      for (auto *p : n->children[0]->children)
      {
        if (p->type == "Variadic")
          continue; // ... is always valid
        std::string ptype = extract_base_type(p->value);
        // flat: types are expanded struct args from cheader — always valid
        if (ptype.rfind("flat:", 0) == 0)
          continue;
        if (!is_known_type(ptype) && ptype != "voided")
          add_warning(p->line, "CImport '" + func_name + "': unknown param type '" + ptype + "'");
      }
    }

    // Already registered in func_table by collect_signatures — nothing else
    // needed.
  }

  void check_index_assignment(Parser::ASTNode *n)
  {
    auto sp = n->value.rfind(' ');
    std::string arr = n->value.substr(0, sp);
    auto *sym = lookup(arr);
    if (!sym)
      add_error(n->line, "Undeclared variable '" + arr + "'");
    else if (sym->is_voided_maybe || sym->is_voided)
      add_error(n->line, "Assignment to voided variable '" + arr + "'");
    else if (sym->is_const)
      add_error(n->line, "Assignment to const variable '" + arr + "'");
    for (auto *child : n->children)
      check_expression(child);
  }

  void check_if(Parser::ASTNode *n)
  {
    if (n->children.empty())
      return;

    // Try to evaluate the condition at compile time
    int cond_static = eval_branch_condition(n->children[0]);
    check_expression(n->children[0]);

    // Annotate the If node so codegen can skip dead branches entirely
    if (cond_static == 1)
      n->meta = "always_true";
    if (cond_static == -1)
      n->meta = "always_false";

    // Snapshot voided state before branches
    std::unordered_map<std::string, bool> snap_before;
    for (auto &scope : scope_stack)
      for (auto &[nm, sym] : scope)
        snap_before[nm] = sym.is_voided;

    bool has_else = (n->children.size() > 2);

    if (cond_static == 1)
    {
      // Condition is definitely TRUE — only then-branch is taken
      if (n->children.size() > 1)
        check_block(n->children[1]);
      // else-branch never executes — skip it entirely (no voided-state effect)
      // Voided state after = voided state after then-branch: already updated
      return;
    }
    if (cond_static == -1)
    {
      // Condition is definitely FALSE — only else-branch (if any) is taken
      if (has_else)
      {
        auto *el = n->children[2];
        if (el->type == "If")
          check_if(el);
        else
          check_block(el);
      }
      // then-branch never executes
      return;
    }

    // Condition unknown at compile time — check both branches and merge
    if (n->children.size() > 1)
      check_block(n->children[1]);

    std::unordered_map<std::string, bool> snap_after_then;
    for (auto &scope : scope_stack)
      for (auto &[nm, sym] : scope)
        snap_after_then[nm] = sym.is_voided;

    // Restore to before-state for the else branch
    for (auto &scope : scope_stack)
      for (auto &[nm, sym] : scope)
        if (snap_before.count(nm))
          sym.is_voided = snap_before[nm];

    if (has_else)
    {
      auto *el = n->children[2];
      if (el->type == "If")
        check_if(el);
      else
        check_block(el);
    }

    // Merge states
    for (auto &scope : scope_stack)
    {
      for (auto &[nm, sym] : scope)
      {
        bool v_then = snap_after_then.count(nm) ? snap_after_then[nm]
                                                : (snap_before.count(nm) ? snap_before[nm] : false);
        bool v_else = sym.is_voided;
        bool v_before = snap_before.count(nm) ? snap_before[nm] : false;

        if (v_then && v_else && has_else)
          sym.is_voided = true; // definitely voided in ALL paths
        else if ((v_then || v_else) && !v_before)
        {
          sym.is_voided = false;
          sym.is_voided_maybe = true; // moved on SOME paths → needs runtime bool
        }
      }
    }
  }

  void check_while(Parser::ASTNode *n)
  {
    if (n->children.empty())
      return;

    // Snapshot before loop
    std::unordered_map<std::string, bool> snap_before;
    for (auto &scope : scope_stack)
      for (auto &[nm, sym] : scope)
        snap_before[nm] = sym.is_voided;

    check_expression(n->children[0]);

    if (n->children.size() > 1)
    {
      push_scope();
      loop_depth++;
      check_block(n->children[1]);
      loop_depth--;
      pop_scope();
    }

    // Any move inside the loop body is "maybe" — the loop may not run at all.
    // Promote newly-voided variables to maybe-voided.
    for (auto &scope : scope_stack)
    {
      for (auto &[nm, sym] : scope)
      {
        bool v_before = snap_before.count(nm) ? snap_before[nm] : false;
        if (sym.is_voided && !v_before)
        {
          sym.is_voided = false;
          sym.is_voided_maybe = true;
        }
      }
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
    declare({item, type, arena_depth, false, false, is_pointer_type(type), false, false});
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
    {
      auto *expr = n->children[0];
      if (expr->type == "Token")
        switched_var = expr->value;
      else if (!expr->children.empty() && expr->children[0]->type == "Token")
        switched_var = expr->children[0]->value;
    }

    // For a valid/voided guard, accessing a voided/maybe-voided variable in the switch
    // expression is legal — it IS the guard itself
    bool was_voided = false;
    bool was_voided_maybe = false;
    if (is_voided_guard && !switched_var.empty())
    {
      Symbol *sym = lookup(switched_var);
      if (sym)
      {
        was_voided = sym->is_voided;
        was_voided_maybe = sym->is_voided_maybe;
        sym->is_voided = false;
        sym->is_voided_maybe = false;
      }
    }
    check_expression(n->children[0]);
    // Restore
    if (is_voided_guard && !switched_var.empty())
    {
      Symbol *sym = lookup(switched_var);
      if (sym)
      {
        sym->is_voided = was_voided;
        sym->is_voided_maybe = was_voided_maybe;
      }
    }
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
        add_warning(n->line,
                    "switch on voided variable '" + switched_var + "' without 'case valid'");
      if (!has_voided_case)
        add_warning(n->line,
                    "switch on voided variable '" + switched_var + "' without 'case voided'");
    }

    // Annotate the Switch node with the statically-known voided state so that
    // codegen can emit a direct unconditional branch with zero runtime overhead.
    //   meta == "voided"  → variable was definitely voided here
    //   meta == "valid"   → variable was definitely valid here (guard is redundant
    //                        but legal; codegen takes the valid branch directly)
    //   meta == ""        → not a voided-state guard (regular numeric switch)
    if (is_voided_guard)
    {
      if (was_voided)
        n->meta = "voided";
      else
      {
        // Check if the variable is "maybe voided" — moved on some paths
        Symbol *sym2 = lookup(switched_var);
        if (sym2 && sym2->is_voided_maybe)
          n->meta = "unknown"; // needs runtime bool __track_validity_<var>
        else
          n->meta = "valid";
      }
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

  // ── Argument-validation helpers ───────────────────────────────────────────

  // Find the index of the closing ')' that matches the '(' at open_idx.
  size_t find_matching_close(const std::vector<Parser::ASTNode *> &toks, size_t open_idx)
  {
    int depth = 0;
    for (size_t i = open_idx; i < toks.size(); ++i)
    {
      const std::string &v = toks[i]->value;
      if (v == "(" || v == "[" || v == "{")
        ++depth;
      else if (v == ")" || v == "]" || v == "}")
      {
        --depth;
        if (depth == 0)
          return i;
      }
    }
    return toks.size(); // unmatched — return end
  }

  // Split tokens in [start, end) into argument spans by top-level commas.
  // Returns an empty vector for a zero-arg call (start >= end).
  std::vector<std::vector<Parser::ASTNode *>>
  split_top_level_args(const std::vector<Parser::ASTNode *> &toks, size_t start, size_t end)
  {
    std::vector<std::vector<Parser::ASTNode *>> result;
    if (start >= end)
      return result;
    int depth = 0;
    size_t span_start = start;
    for (size_t i = start; i < end; ++i)
    {
      const std::string &v = toks[i]->value;
      if (v == "(" || v == "[" || v == "{")
        ++depth;
      else if (v == ")" || v == "]" || v == "}")
        --depth;
      else if (v == "," && depth == 0)
      {
        result.push_back({toks.begin() + span_start, toks.begin() + i});
        span_start = i + 1;
      }
    }
    result.push_back({toks.begin() + span_start, toks.begin() + end});
    return result;
  }

  // Obtain arg spans for a CallStatement's Args node.
  // The parser folds all comma-separated args into a single Expression child,
  // so we must re-split on commas here.
  std::vector<std::vector<Parser::ASTNode *>>
  get_call_statement_arg_spans(Parser::ASTNode *args_node)
  {
    if (!args_node || args_node->children.empty())
      return {};
    // Common case: parser merged everything into one Expression
    if (args_node->children.size() == 1 && args_node->children[0]->type == "Expression")
    {
      auto &toks = args_node->children[0]->children;
      return split_top_level_args(toks, 0, toks.size());
    }
    // Fallback: each child is already its own argument
    std::vector<std::vector<Parser::ASTNode *>> result;
    for (auto *child : args_node->children)
      result.push_back({child});
    return result;
  }

  // Named color constants — also used by check_expression_tokens
  static const std::unordered_set<std::string> &known_color_names()
  {
    static const std::unordered_set<std::string> s = {
        "LIGHTGRAY", "GRAY",   "DARKGRAY", "YELLOW",     "GOLD",      "ORANGE",  "PINK",
        "RED",       "MAROON", "GREEN",    "LIME",       "DARKGREEN", "SKYBLUE", "BLUE",
        "DARKBLUE",  "PURPLE", "VIOLET",   "DARKPURPLE", "BEIGE",     "BROWN",   "DARKBROWN",
        "WHITE",     "BLACK",  "BLANK",    "MAGENTA",    "RAYWHITE"};
    return s;
  }

  // Check each token in a single argument span for undefined variables.
  void check_undefined_vars_in_span(const std::vector<Parser::ASTNode *> &span,
                                    const std::string &call_display)
  {
    for (size_t i = 0; i < span.size(); ++i)
    {
      auto *tok = span[i];
      if (!tok || tok->token_type != Lexer::TokenType::IDENTIFIER)
        continue;
      const std::string &nm = tok->value;
      // Skip built-ins
      if (nm == "__arena" || nm == "__arena_null")
        continue;
      if (known_color_names().count(nm))
        continue;
      // Skip type names
      if (struct_types.count(nm) || enum_types.count(nm) || template_types.count(nm) ||
          type_aliases.count(nm))
        continue;
      // Skip if followed by '(' — it's a nested call head, validated separately
      if (i + 1 < span.size() && span[i + 1]->value == "(")
        continue;
      // Skip if followed by '{' — struct literal type name
      if (i + 1 < span.size() && span[i + 1]->value == "{")
        continue;
      // Skip field/method access target (preceded by '.')
      if (i > 0 && span[i - 1]->value == ".")
        continue;
      // Skip namespace/enum qualifiers (preceded or followed by '::')
      if (i > 0 && span[i - 1]->value == "::")
        continue;
      if (i + 1 < span.size() && span[i + 1]->value == "::")
        continue;
      // Check if declared or a known function (bare function-pointer reference)
      if (!lookup(nm) && !func_table.count(nm))
        add_error(tok->line, "Undefined variable '" + nm + "' used as argument in call to '" +
                                 call_display + "'");
    }
  }

  // Validate arity and undefined variables for one call site.
  // resolved_name: key to look up in func_table.
  // is_method: true when self is the hidden first param (subtract 1 from expected).
  void validate_call_args(const std::string &resolved_name, const std::string &display_name,
                          bool is_method,
                          const std::vector<std::vector<Parser::ASTNode *>> &arg_spans, size_t line)
  {
    // Note: undefined-variable checking of argument tokens is handled by
    // scan_undefined_vars() which runs on every Expression node — no need
    // to duplicate it here.

    // Arity check — only when we know the signature
    auto it = func_table.find(resolved_name);
    if (it == func_table.end())
      return;
    const FuncSig &sig = it->second;
    size_t expected = sig.params.size();
    if (is_method && expected > 0)
      --expected; // don't count implicit self
    size_t actual = arg_spans.size();
    if (sig.is_variadic)
    {
      if (actual < expected)
        add_error(line, "Call to '" + display_name + "' expects at least " +
                            std::to_string(expected) + " argument(s) but got " +
                            std::to_string(actual));
    }
    else
    {
      if (actual != expected)
        add_error(line, "Call to '" + display_name + "' expects " + std::to_string(expected) +
                            " argument(s) but got " + std::to_string(actual));
    }
  }

  // Strip generic parameters and pointer suffix from a type string.
  // e.g. "Vec<i32>*" -> "Vec"
  std::string strip_type_decorators(const std::string &t)
  {
    std::string s = t;
    auto lt = s.find('<');
    if (lt != std::string::npos)
      s = s.substr(0, lt);
    while (!s.empty() && s.back() == '*')
      s.pop_back();
    return s;
  }

  // General undefined-variable scanner for any flat token list.
  // Catches uses of undeclared identifiers in conditions, RHS expressions, etc.
  // Does NOT check call names (handled by scan_calls_in_token_list) or
  // field names after '.'.
  void scan_undefined_vars(const std::vector<Parser::ASTNode *> &toks)
  {
    for (size_t i = 0; i < toks.size(); ++i)
    {
      auto *tok = toks[i];
      if (!tok || tok->token_type != Lexer::TokenType::IDENTIFIER)
        continue;
      const std::string &nm = tok->value;
      // Skip built-ins / arena sentinels
      if (nm == "__arena" || nm == "__arena_null")
        continue;
      // Skip raylib color names
      if (known_color_names().count(nm))
        continue;
      // Skip known type names (struct/enum/template/using-alias)
      if (struct_types.count(nm) || enum_types.count(nm) || template_types.count(nm) ||
          type_aliases.count(nm))
        continue;
      if (type_aliases.count(nm))
        continue;
      // Skip function call names (followed by '(')
      if (i + 1 < toks.size() && toks[i + 1]->value == "(")
        continue;
      // Skip struct literal type names (followed by '{')
      if (i + 1 < toks.size() && toks[i + 1]->value == "{")
        continue;
      // Skip field/method names (preceded by '.')
      if (i > 0 && toks[i - 1]->value == ".")
        continue;
      // Skip namespace/enum qualifiers
      if (i > 0 && toks[i - 1]->value == "::")
        continue;
      if (i + 1 < toks.size() && toks[i + 1]->value == "::")
        continue;
      // Skip bare function-pointer references (known function names)
      if (func_table.count(nm))
        continue;
      // Check declaration
      Symbol *sym = lookup(nm);
      if (!sym)
        add_error(tok->line, "Undefined variable '" + nm + "'");
      else if (sym->is_voided)
        add_error(tok->line, "Use of voided variable '" + nm + "'");
    }
  }

  // Scan a flat token list for call sites and validate each one.
  // This is the generic hook called from check_expression_tokens.
  void scan_calls_in_token_list(const std::vector<Parser::ASTNode *> &toks)
  {
    for (size_t i = 0; i < toks.size(); ++i)
    {
      auto *tok = toks[i];
      if (!tok || tok->token_type != Lexer::TokenType::IDENTIFIER)
        continue;
      // Must be followed by '('
      if (i + 1 >= toks.size() || toks[i + 1]->value != "(")
        continue;

      const std::string &call_name = tok->value;
      size_t close = find_matching_close(toks, i + 1);
      auto arg_spans = split_top_level_args(toks, i + 2, close);
      size_t call_line = tok->line;

      // Check if this is a method call: preceded by  '.' IDENTIFIER
      if (i >= 2 && toks[i - 1]->value == "." &&
          toks[i - 2]->token_type == Lexer::TokenType::IDENTIFIER)
      {
        const std::string &obj_name = toks[i - 2]->value;
        Symbol *obj_sym = lookup(obj_name);
        if (!obj_sym)
          continue; // object undeclared — avoid cascading errors
        std::string base = strip_type_decorators(obj_sym->type);
        std::string resolved = base + "_" + call_name;
        std::string display = obj_name + "." + call_name;
        validate_call_args(resolved, display, /*is_method=*/true, arg_spans, call_line);
      }
      else if (i == 0 || toks[i - 1]->value != ".")
      {
        // Free function call (skip if immediately after '.' — e.g. chained call tail)
        validate_call_args(call_name, call_name, /*is_method=*/false, arg_spans, call_line);
      }

      // Advance past the closing paren to avoid re-processing nested tokens
      i = close;
    }
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
        add_error(n->line, "Use of voided variable '" + n->value + "' — guard with switch(" +
                               n->value + ") { case valid: … case voided: … }");
      }
      else if (sym && sym->is_voided_maybe)
      {
        add_error(n->line, "Variable '" + n->value +
                               "' may be voided (moved on some paths) — "
                               "guard with switch(" +
                               n->value + ") { case valid: … case voided: … }");
      }
      // __arena / __arena_null are compiler built-ins — always valid
      if (n->value == "__arena" || n->value == "__arena_null")
        return;
      // Named color constants (WHITE, RED, etc.) — always valid
      if (known_color_names().count(n->value))
        return;
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
    if (n->type == "Expression" && n->children.size() == 2 && n->children[0]->value == "move" &&
        n->children[1]->type == "Token")
    {
      const std::string &mname = n->children[1]->value;
      Symbol *sym = lookup(mname);
      if (!sym)
        add_error(n->children[1]->line, "move: variable '" + mname + "' not declared");
      else if (sym->is_voided)
        add_error(n->children[1]->line, "move: variable '" + mname + "' is already voided");
      else if (sym->is_const)
        add_error(n->children[1]->line, "move: cannot move const variable '" + mname + "'");
      if (sym)
        sym->is_voided = true;
      return;
    }
    // Handle flat token list: look for 'move' keyword token followed by IDENT
    if (n->type == "Expression")
    {
      for (size_t i = 0; i + 1 < n->children.size(); ++i)
      {
        if (n->children[i]->value == "move" && n->children[i + 1]->type == "Token" &&
            n->children[i + 1]->token_type == Lexer::TokenType::IDENTIFIER)
        {
          const std::string &mname = n->children[i + 1]->value;
          Symbol *sym = lookup(mname);
          if (!sym)
            add_error(n->children[i]->line, "move: variable '" + mname + "' not declared");
          else if (sym->is_voided)
            add_error(n->children[i]->line, "move: variable '" + mname + "' is already voided");
          else if (sym->is_const)
            add_error(n->children[i]->line, "move: cannot move const variable '" + mname + "'");
          if (sym)
            sym->is_voided = true;
          ++i; // skip the variable token
          continue;
        }
      }
      // Scan for call sites and validate arity / undefined variables
      scan_calls_in_token_list(n->children);
      // Check all identifiers for declaration (conditions, RHS, etc.)
      scan_undefined_vars(n->children);
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
    else if (def->type == "CImport" || def->type == "ModuleImport" || def->type == "Import")
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
      add_error(n->line, "Template can only precede struct, function, or import definitions");
    }
  }

  bool is_known_type(const std::string &t) const
  {
    static const std::unordered_set<std::string> primitives = {
        "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32",
        "float64", "char", "bool", "string", "void",
        // Short aliases
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "byte"};
    std::string base = t;
    // Strip pointer/array decorators
    while (!base.empty() &&
           (base.back() == '*' || base.back() == ']' || base.back() == ':' || base.back() == '['))
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
    if (!base.empty() && base.size() <= 2 && std::isupper((unsigned char)base[0]) &&
        (base.size() == 1 || std::isupper((unsigned char)base[1])))
      return true;
    if (struct_types.count(base))
      return true;
    if (enum_types.count(base))
      return true;
    if (template_types.count(base))
      return true;
    if (template_types.count(base + "<...>"))
      return true;
    if (type_aliases.count(base))
      return true;
    // flat:<t0>,<t1> — expanded struct (from C header import)
    if (t.rfind("flat:", 0) == 0)
      return true;
    // Raw C type spellings that cheader may emit before canonicalization
    static const std::unordered_set<std::string> c_spellings = {
        "int",           "unsigned",       "unsigned int",
        "short",         "unsigned short", "long",
        "unsigned long", "long long",      "unsigned long long",
        "size_t",        "ssize_t",        "ptrdiff_t",
        "intptr_t",      "uintptr_t",      "int8_t",
        "int16_t",       "int32_t",        "int64_t",
        "uint8_t",       "uint16_t",       "uint32_t",
        "uint64_t",      "float",          "double",
        "_Bool"};
    if (c_spellings.count(base))
      return true;
    // If any struct type is registered with matching prefix (templated struct)
    for (auto &s : struct_types)
      if (s == base)
        return true;
    return false;
  }

  // Check if a numeric type assignment needs a coercion warning.
  // Returns "" if fine, a warning string if narrowing, an error string if incompatible.
  // "cannot cast 'from' to 'to'" is emitted when types are structurally incompatible.
  std::string check_type_coercion(const std::string &from_type, const std::string &to_type,
                                  size_t line)
  {
    if (from_type == to_type)
      return "";

    // Integer types — widths
    static const std::unordered_map<std::string, int> int_width = {
        {"int8", 8},    {"int16", 16},  {"int32", 32},  {"int64", 64}, {"uint8", 8},
        {"uint16", 16}, {"uint32", 32}, {"uint64", 64}, {"bool", 1},   {"char", 8}};
    static const std::unordered_set<std::string> fp_types = {"float32", "float64"};
    static const std::unordered_set<std::string> ptr_types = {"string", "voided*"};

    bool from_int = int_width.count(from_type);
    bool to_int = int_width.count(to_type);
    bool from_fp = fp_types.count(from_type);
    bool to_fp = fp_types.count(to_type);
    bool from_ptr = (from_type.find('*') != std::string::npos) || ptr_types.count(from_type);
    bool to_ptr = (to_type.find('*') != std::string::npos) || ptr_types.count(to_type);

    // int → int: ok but warn on narrowing
    if (from_int && to_int)
    {
      int fw = int_width.at(from_type), tw = int_width.at(to_type);
      if (fw > tw)
        add_warning(line, "Implicit narrowing: '" + from_type + "' assigned to '" + to_type +
                              "' — value may be truncated");
      return "";
    }
    // float → float: ok
    if (from_fp && to_fp)
      return "";
    // int → float or float → int: allowed with warning
    if ((from_int && to_fp) || (from_fp && to_int))
    {
      add_warning(line,
                  "Implicit numeric conversion: '" + from_type + "' assigned to '" + to_type + "'");
      return "";
    }
    // ptr → ptr or ptr ↔ voided*: allowed
    if (from_ptr && to_ptr)
      return "";
    // string (ptr) is compatible with any pointer target
    if ((from_ptr || from_type == "string") && (to_ptr || to_type == "string"))
      return "";

    // Everything else is incompatible — but literal mismatches are caught by
    // check_declaration/check_const already.  For function-call results we emit:
    if (!from_type.empty() && !to_type.empty())
      add_error(line, "cannot cast '" + from_type + "' to '" + to_type + "'");
    return "error";
  }
};

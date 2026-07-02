#pragma once
#include "lexer.hh"
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class Parser
{
  std::vector<Lexer::Token> token_stream;
  size_t cursor = 0;
  size_t current_depth = 0;
  bool has_entry = false;

public:
  struct ASTNode
  {
    std::string type;
    std::string value;
    std::vector<ASTNode *> children;
    size_t line;
    size_t depth;
    Lexer::TokenType token_type = Lexer::TokenType::IDENTIFIER; // for Token nodes
    // Generic annotation written by the checker, read by codegen.
    // For Switch nodes that are voided-state guards this is set to:
    //   "voided"  — the switched variable was statically known to be voided
    //   "valid"   — the switched variable was statically known to be valid
    //   ""        — unknown / not a voided-state guard (default)
    std::string meta;

    ASTNode(std::string t, std::string v, size_t l, size_t d)
        : type(std::move(t)), value(std::move(v)), line(l), depth(d)
    {
    }

    ~ASTNode()
    {
      for (auto child : children)
        delete child;
    }
  };

  Parser(const std::vector<Lexer::Token> &tokens) : token_stream(tokens) {}

  std::vector<ASTNode *> parse_program()
  {
    std::vector<ASTNode *> ast;
    while (peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      ast.push_back(parse_top_level());
    }
    return ast;
  }

private:
  Lexer::Token peek_token(size_t offset = 0)
  {
    if (cursor + offset >= token_stream.size())
      return {Lexer::TokenType::END_OF_FILE, "", 0};
    return token_stream[cursor + offset];
  }

  Lexer::Token advance_token()
  {
    if (cursor < token_stream.size())
    {
      return token_stream[cursor++];
    }
    return {Lexer::TokenType::END_OF_FILE, "", 0};
  }

  Lexer::Token match_token(Lexer::TokenType expected, const std::string &expected_value = "")
  {
    Lexer::Token current = peek_token();
    if (current.type == expected && (expected_value.empty() || current.value == expected_value))
    {
      return advance_token();
    }
    throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(current.line) +
                             ": Unexpected token: '" + current.value + "', expected: '" +
                             (expected_value.empty() ? "<token>" : expected_value) + "'");
  }

  size_t current_line() { return peek_token().line; }

  ASTNode *parse_top_level()
  {
    if (peek_token().type == Lexer::TokenType::KEYWORD)
    {
      const std::string &kw = peek_token().value;
      if (kw == "if")
        return parse_if();
      if (kw == "import")
        return parse_import();
      if (kw == "for")
        return parse_for();
      if (kw == "foreach")
        return parse_foreach();
      if (kw == "while")
        return parse_while();
      if (kw == "switch")
        return parse_switch();
      if (kw == "break")
        return parse_break();
      if (kw == "continue")
        return parse_continue();
      if (kw == "template")
        return parse_template();
      if (kw == "struct")
        return parse_struct();
      if (kw == "def")
        return parse_function();
      if (kw == "dec")
        return parse_dec();
      if (kw == "class")
        return parse_class();
      if (kw == "export")
      {
        // export def <name>(...) { ... }
        // Consume 'export', then parse the function normally and mark it.
        advance_token();
        ASTNode *fn = parse_function();
        fn->meta = "export";
        return fn;
      }
      if (kw == "enum")
        return parse_enum();
      if (kw == "tunnel")
        return parse_tunnel();
      if (kw == "reserve")
        return parse_reserve();
      if (kw == "move")
        return parse_move();
      if (kw == "namespace")
        return parse_namespace();
      if (kw == "using")
        return parse_using();
      if (kw == "const")
        return parse_const();
      if (kw == "reset")
        return parse_reset();
      if (kw == "entry")
      {
        if (has_entry)
          throw std::runtime_error("[SYNTAX ERROR] Multiple 'entry' definitions found");
        has_entry = true;
        return parse_entry();
      }
      if (is_type_keyword(kw))
        return parse_declaration();
      throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(current_line()) +
                               ": Unexpected keyword: " + kw);
    }
    if (peek_token().type == Lexer::TokenType::IDENTIFIER)
    {
      // Could be assignment or function call statement
      // Look ahead: IDENT ( => call, IDENT = => assign, IDENT += etc => assign
      // Also handles field access chains: IDENT.FIELD = ...
      size_t la = 1;
      // Skip template arguments: Vector<int32>, HashMap<K, V>, etc.
      if (peek_token(la).value == "<")
      {
        la++; // skip <
        int tpl_depth = 1;
        while (tpl_depth > 0 && peek_token(la).type != Lexer::TokenType::END_OF_FILE)
        {
          if (peek_token(la).value == "<")
            tpl_depth++;
          else if (peek_token(la).value == ">")
            tpl_depth--;
          la++;
        }
      }
      // Array element assignment: IDENT[expr] = ...  (check BEFORE decorator skip
      // so we don't confuse arr[i] = v with a type annotation like T[] name)
      if (peek_token(la).value == "[" && peek_token(la + 1).value != "]" &&
          peek_token(la + 1).value != ":" && peek_token(la + 1).value != ":")
        return parse_assignment();

      // skip potential pointer/slice decorators: *, []
      while (peek_token(la).value == "*")
        la++;
      // skip [] and [N] and [:]: consume [ then optional content then ]
      while (peek_token(la).value == "[" || peek_token(la).value == "[:]")
      {
        if (peek_token(la).value == "[:]")
        {
          la++;
          continue;
        }
        la++; // consume [
        while (peek_token(la).value != "]" && peek_token(la).type != Lexer::TokenType::END_OF_FILE)
          la++;
        if (peek_token(la).value == "]")
          la++; // consume ]
      }
      // skip dot-field chains: p.x  or  p.x.y  etc.
      while (peek_token(la).value == "." && peek_token(la + 1).type == Lexer::TokenType::IDENTIFIER)
        la += 2;
      if (peek_token(la).value == "(")
        return parse_call_statement();
      if (peek_token(la).value == "=" || peek_token(la).value == "+=" ||
          peek_token(la).value == "-=" || peek_token(la).value == "*=" ||
          peek_token(la).value == "/=" || peek_token(la).value == "%=" ||
          peek_token(la).value == "<<=" || peek_token(la).value == ">>=" ||
          peek_token(la).value == "**=")
        return parse_assignment();
      // Array append: IDENT << expr ;
      if (peek_token(la).value == "<<")
        return parse_array_append();
      // Struct-typed or generic-typed declaration: IDENT IDENT or IDENT<T> IDENT
      if (peek_token(la).type == Lexer::TokenType::IDENTIFIER)
        return parse_declaration();
    }
    // Anonymous block (sub-arena) — { ... }
    if (peek_token().value == "{")
      return parse_block();
    return parse_statement();
  }

  bool is_type_keyword(const std::string &kw)
  {
    static const std::vector<std::string> type_kws = {
        // Canonical names
        "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32",
        "float64", "char", "bool", "string",
        // Short aliases (now also keywords in lexer)
        "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64", "byte"};
    for (auto &t : type_kws)
      if (t == kw)
        return true;
    return false;
  }

  ASTNode *parse_block()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::OPERATOR, "{");

    ASTNode *block_node = new ASTNode("Block", "", ln, current_depth + 1);
    current_depth++;

    while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      block_node->children.push_back(parse_top_level());
    }

    match_token(Lexer::TokenType::OPERATOR, "}");
    current_depth--;

    return block_node;
  }

  ASTNode *parse_entry()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "entry");
    ASTNode *node = new ASTNode("Entry", "main", ln, current_depth);
    node->children.push_back(parse_block());
    return node;
  }

  // dec name(params);                      — no tunnel outputs
  // dec name(params) -> type t1, type t2;  — with tunnel outputs
  ASTNode *parse_dec()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "dec");
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;

    ASTNode *node = new ASTNode("FuncDecl", name, ln, current_depth);
    node->children.push_back(parse_parameters());

    // Optional tunnel-output signature list
    ASTNode *tunnels = new ASTNode("DeclTunnels", "", ln, current_depth);
    if (peek_token().value == "->")
    {
      advance_token();
      while (true)
      {
        std::string ttype = parse_type_string();
        std::string tname = match_token(Lexer::TokenType::IDENTIFIER).value;
        tunnels->children.push_back(
            new ASTNode("DeclTunnel", ttype + " " + tname, ln, current_depth));
        if (peek_token().value == ",")
        {
          advance_token();
          continue;
        }
        break;
      }
    }
    node->children.push_back(tunnels);
    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  // class Name { fields...; def method(params) { body } ... }
  // Desugars to: struct Name { fields }  +  free functions Name_method(Name* self, params)
  ASTNode *parse_class()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "class");
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
    match_token(Lexer::TokenType::OPERATOR, "{");

    ASTNode *node = new ASTNode("Class", name, ln, current_depth);
    ASTNode *fields = new ASTNode("Fields", "", ln, current_depth);
    ASTNode *methods = new ASTNode("Methods", "", ln, current_depth);

    while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      if (peek_token().value == "def")
      {
        // Method: def name(params) [-> type tname,...] { body }
        advance_token(); // 'def'
        std::string mname = match_token(Lexer::TokenType::IDENTIFIER).value;
        ASTNode *method = new ASTNode("Method", mname, current_line(), current_depth);
        method->children.push_back(parse_parameters());

        // Optional tunnel outputs in the signature (purely documentary —
        // actual tunnel statements inside the body drive codegen, same as def)
        if (peek_token().value == "->")
        {
          advance_token();
          ASTNode *tunnels = new ASTNode("DeclTunnels", "", current_line(), current_depth);
          while (true)
          {
            std::string ttype = parse_type_string();
            std::string tname = match_token(Lexer::TokenType::IDENTIFIER).value;
            tunnels->children.push_back(
                new ASTNode("DeclTunnel", ttype + " " + tname, current_line(), current_depth));
            if (peek_token().value == ",")
            {
              advance_token();
              continue;
            }
            break;
          }
          method->children.push_back(tunnels);
        }

        method->children.push_back(parse_block());
        methods->children.push_back(method);
      }
      else
      {
        // Field: type name;
        std::string ftype = parse_type_string();
        std::string fname = match_token(Lexer::TokenType::IDENTIFIER).value;
        match_token(Lexer::TokenType::OPERATOR, ";");
        fields->children.push_back(
            new ASTNode("Field", ftype + " " + fname, current_line(), current_depth));
      }
    }
    match_token(Lexer::TokenType::OPERATOR, "}");

    node->children.push_back(fields);
    node->children.push_back(methods);
    return node;
  }

  ASTNode *parse_function()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "def");
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;

    ASTNode *node = new ASTNode("Function", name, ln, current_depth);
    node->children.push_back(parse_parameters());
    node->children.push_back(parse_block());
    return node;
  }

  ASTNode *parse_parameters()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::OPERATOR, "(");
    ASTNode *params = new ASTNode("Parameters", "", ln, current_depth);

    while (peek_token().value != ")" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      std::string type = parse_type_string();
      std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
      params->children.push_back(new ASTNode("Param", type + " " + name, ln, current_depth));
      if (peek_token().value == ",")
        advance_token();
    }
    match_token(Lexer::TokenType::OPERATOR, ")");
    return params;
  }

  // Parses a type including pointer/slice suffixes: int32, int32*, int32[],
  // int32[:], and generic types like Vector<T>, HashMap<K, V>
  std::string parse_type_string()
  {
    std::string type;
    if (peek_token().type == Lexer::TokenType::KEYWORD ||
        peek_token().type == Lexer::TokenType::IDENTIFIER)
    {
      type = advance_token().value;
    }
    else
    {
      throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(current_line()) +
                               ": Expected type, got: " + peek_token().value);
    }
    // Generic type parameters: Foo<T>  Foo<T, U>  Foo<Bar<T>>
    if (peek_token().value == "<")
    {
      advance_token(); // consume <
      type += "<";
      int depth = 1;
      while (depth > 0 && peek_token().type != Lexer::TokenType::END_OF_FILE)
      {
        const std::string &v = peek_token().value;
        if (v == "<")
        {
          depth++;
          type += advance_token().value;
        }
        else if (v == ">")
        {
          depth--;
          if (depth > 0)
            type += advance_token().value;
          else
          {
            advance_token();
            type += ">";
          }
        }
        else
        {
          type += advance_token().value;
        }
      }
    }
    // pointer
    while (peek_token().value == "*")
    {
      type += "*";
      advance_token();
    }
    // slice or array
    if (peek_token().value == "[")
    {
      advance_token();
      if (peek_token().value == ":")
      {
        advance_token();
        match_token(Lexer::TokenType::OPERATOR, "]");
        type += "[:]";
      }
      else if (peek_token().value == "]")
      {
        advance_token();
        type += "[]";
      }
      else
      {
        // sized array
        std::string sz;
        while (peek_token().value != "]" && peek_token().type != Lexer::TokenType::END_OF_FILE)
          sz += advance_token().value;
        match_token(Lexer::TokenType::OPERATOR, "]");
        type += "[" + sz + "]";
      }
    }
    else if (peek_token().value == "[:]")
    {
      advance_token();
      type += "[:]";
    }
    return type;
  }

  ASTNode *make_token_node(const Lexer::Token &tok)
  {
    auto *n = new ASTNode("Token", tok.value, tok.line, current_depth);
    n->token_type = tok.type;
    return n;
  }

  ASTNode *parse_expression()
  {
    size_t ln = current_line();
    ASTNode *expr = new ASTNode("Expression", "", ln, current_depth);
    int paren_depth = 0;
    int bracket_depth = 0;

    while (peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      const std::string &val = peek_token().value;

      if (val == "(")
      {
        paren_depth++;
        expr->children.push_back(make_token_node(advance_token()));
        continue;
      }
      if (val == ")")
      {
        if (paren_depth == 0)
          break;
        paren_depth--;
        expr->children.push_back(make_token_node(advance_token()));
        continue;
      }
      if (val == "[")
      {
        bracket_depth++;
        expr->children.push_back(make_token_node(advance_token()));
        continue;
      }
      if (val == "]")
      {
        if (bracket_depth == 0 && paren_depth == 0)
          break;
        bracket_depth--;
        expr->children.push_back(make_token_node(advance_token()));
        continue;
      }
      if (paren_depth == 0 && bracket_depth == 0 &&
          peek_token().type == Lexer::TokenType::OPERATOR && (val == ";" || val == "}"))
        break;
      if (paren_depth == 0 && bracket_depth == 0 && val == "->" &&
          peek_token().type == Lexer::TokenType::OPERATOR)
        break;

      expr->children.push_back(make_token_node(advance_token()));
    }
    return expr;
  }

  ASTNode *parse_statement()
  {
    size_t ln = current_line();
    ASTNode *expr = parse_expression();
    match_token(Lexer::TokenType::OPERATOR, ";");
    expr->line = ln;
    return expr;
  }

  ASTNode *parse_call_statement()
  {
    size_t ln = current_line();
    // Consume base name and optional .method chain: "v.push" or "v.x.y"
    std::string name = advance_token().value; // first IDENT
    while (peek_token().value == "." && peek_token(1).type == Lexer::TokenType::IDENTIFIER &&
           (peek_token(2).value == "(" ||
            (peek_token(2).value == "." && peek_token(3).type == Lexer::TokenType::IDENTIFIER &&
             peek_token(4).value == "(")))
    {
      advance_token();                     // consume '.'
      name += "." + advance_token().value; // consume next IDENT
    }
    ASTNode *node = new ASTNode("CallStatement", name, ln, current_depth);
    match_token(Lexer::TokenType::OPERATOR, "(");
    ASTNode *args = new ASTNode("Args", "", ln, current_depth);
    while (peek_token().value != ")" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      args->children.push_back(parse_expression());
      if (peek_token().value == ",")
        advance_token();
    }
    match_token(Lexer::TokenType::OPERATOR, ")");
    match_token(Lexer::TokenType::OPERATOR, ";");
    node->children.push_back(args);
    return node;
  }

  ASTNode *parse_array_append()
  {
    size_t ln = current_line();
    std::string arr_name = advance_token().value; // IDENT
    advance_token();                              // consume <<
    ASTNode *node = new ASTNode("ArrayAppend", arr_name, ln, current_depth);
    node->children.push_back(parse_expression());
    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  ASTNode *parse_assignment()
  {
    size_t ln = current_line();
    std::string target = advance_token().value; // IDENT
    // Check for subscript: arr[expr] = ...
    // Store the index expression as the first child when present.
    ASTNode *index_expr = nullptr;
    if (peek_token().value == "[")
    {
      advance_token(); // consume '['
      index_expr = parse_expression();
      match_token(Lexer::TokenType::OPERATOR, "]");
    }
    // Check for field access
    while (peek_token().value == ".")
    {
      advance_token();
      target += "." + advance_token().value;
    }
    std::string op = advance_token().value; // = or += etc.
    ASTNode *node = new ASTNode(index_expr ? "IndexAssignment" : "Assignment", target + " " + op,
                                ln, current_depth);
    if (index_expr)
      node->children.push_back(index_expr);       // child[0] = index expr
    node->children.push_back(parse_expression()); // last child = rhs
    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  ASTNode *parse_tunnel()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "tunnel");
    ASTNode *node = new ASTNode("Tunnel", "", ln, current_depth);

    ASTNode *expr = new ASTNode("Expression", "", ln, current_depth);
    while (peek_token().value != "->" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      expr->children.push_back(make_token_node(advance_token()));
    }
    node->children.push_back(expr);

    match_token(Lexer::TokenType::OPERATOR, "->");

    std::string target_type = parse_type_string();
    std::string target_name = match_token(Lexer::TokenType::IDENTIFIER).value;
    node->children.push_back(
        new ASTNode("Target", target_type + " " + target_name, ln, current_depth));

    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  ASTNode *parse_import()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "import");

    // Form 0b: import <stdio.h>;  — angle-bracket C header
    if (peek_token().value == "<")
    {
      advance_token(); // consume <
      std::string header;
      while (peek_token().value != ">" && peek_token().type != Lexer::TokenType::END_OF_FILE)
        header += advance_token().value;
      match_token(Lexer::TokenType::OPERATOR, ">");
      match_token(Lexer::TokenType::OPERATOR, ";");
      return new ASTNode("HeaderImport", "<" + header + ">", ln, current_depth);
    }

    // Form 1: import "path/to/module";  (string literal)
    // Sub-form 1a: .h extension -> C header (quoted include)
    // Sub-form 1b: otherwise -> .cll file import
    if (peek_token().type == Lexer::TokenType::STRING)
    {
      std::string module_path = advance_token().value;
      match_token(Lexer::TokenType::OPERATOR, ";");
      if (module_path.size() > 2 && module_path.substr(module_path.size() - 2) == ".h")
        return new ASTNode("HeaderImport", module_path, ln, current_depth);
      return new ASTNode("Import", module_path, ln, current_depth);
    }

    // Form 2: import module;            (identifier — module import)
    // Form 3: import <type> <name>(<params>);  (C function declaration)
    //
    // Disambiguate: if the token after the first identifier is '('  or the
    // first token is a type keyword followed by an identifier, it's a C import.
    // Otherwise it's a plain module import.

    // Check for C-function import: starts with a type token (keyword or ident)
    // followed by an identifier and then '('
    //   import int32 puts(string s);
    //   import voided printf(string fmt);
    bool is_c_import = false;
    {
      size_t la = 0;
      // first token must be a type (keyword or ident)
      auto t0 = peek_token(la);
      if (t0.type == Lexer::TokenType::KEYWORD || t0.type == Lexer::TokenType::IDENTIFIER)
      {
        la++;
        // skip generic type params: Foo<T>  Foo<T, U>  Foo<Bar<T>>
        if (peek_token(la).value == "<")
        {
          la++;
          int depth = 1;
          while (depth > 0 && peek_token(la).type != Lexer::TokenType::END_OF_FILE)
          {
            if (peek_token(la).value == "<")
              depth++;
            else if (peek_token(la).value == ">")
              depth--;
            la++;
          }
        }
        // skip pointer stars
        while (peek_token(la).value == "*")
          la++;
        // skip [] / [:]
        if (peek_token(la).value == "[")
        {
          la++;
          while (peek_token(la).value != "]" &&
                 peek_token(la).type != Lexer::TokenType::END_OF_FILE)
            la++;
          la++; // consume ']'
        }
        else if (peek_token(la).value == "[:]")
        {
          la++;
        }
        // next must be an identifier (function name)
        if (peek_token(la).type == Lexer::TokenType::IDENTIFIER)
        {
          la++;
          // next must be '(' → C-function import
          if (peek_token(la).value == "(")
            is_c_import = true;
        }
      }
    }

    if (is_c_import)
    {
      // import <ret_type> <name> ( <params> ) ;
      std::string ret_type = parse_type_string();
      std::string func_name = match_token(Lexer::TokenType::IDENTIFIER).value;

      ASTNode *node = new ASTNode("CImport", ret_type + " " + func_name, ln, current_depth);
      node->children.push_back(parse_c_import_params());
      match_token(Lexer::TokenType::OPERATOR, ";");
      return node;
    }

    // Form 2: plain module import — import std;  import io::file;
    std::string module_name = match_token(Lexer::TokenType::IDENTIFIER).value;
    while (peek_token().value == "::")
    {
      advance_token();
      module_name += "::" + match_token(Lexer::TokenType::IDENTIFIER).value;
    }
    match_token(Lexer::TokenType::OPERATOR, ";");
    return new ASTNode("ModuleImport", module_name, ln, current_depth);
  }

  // Parse parameter list for a C-function import declaration.
  // Supports: ()  (void)  (int32 a, string b, ...)
  ASTNode *parse_c_import_params()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::OPERATOR, "(");
    ASTNode *params = new ASTNode("CParams", "", ln, current_depth);

    // empty or (voided) treated as no params
    if (peek_token().value == ")")
    {
      advance_token();
      return params;
    }
    if (peek_token().value == "voided" && (peek_token(1).value == ")"))
    {
      advance_token(); // consume 'voided'
      match_token(Lexer::TokenType::OPERATOR, ")");
      return params;
    }

    while (peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      // variadic: lexed as a single "..." token
      if (peek_token().value == "...")
      {
        advance_token();
        params->children.push_back(new ASTNode("Variadic", "...", ln, current_depth));
        break;
      }
      std::string ptype = parse_type_string();
      // name is optional in declarations
      std::string pname;
      // Allow keyword tokens as parameter names as well (e.g. "string string")
      if (peek_token().type == Lexer::TokenType::IDENTIFIER ||
          peek_token().type == Lexer::TokenType::KEYWORD)
        pname = advance_token().value;
      params->children.push_back(
          new ASTNode("CParam", ptype + (pname.empty() ? "" : " " + pname), ln, current_depth));
      if (peek_token().value == ",")
      {
        advance_token();
        continue;
      }
      break;
    }
    match_token(Lexer::TokenType::OPERATOR, ")");
    return params;
  }

  ASTNode *parse_namespace()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "namespace");
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
    while (peek_token().value == "::")
    {
      advance_token();
      name += "::" + match_token(Lexer::TokenType::IDENTIFIER).value;
    }
    ASTNode *ns_node = new ASTNode("Namespace", name, ln, current_depth);
    ns_node->children.push_back(parse_block());
    return ns_node;
  }

  // using Alias = type;             (type alias)
  // using namespace Foo;            (namespace import)
  // using Alias = namespace Foo;    (namespace alias)
  ASTNode *parse_using()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "using");

    if (peek_token().value == "namespace")
    {
      advance_token();
      std::string ns = match_token(Lexer::TokenType::IDENTIFIER).value;
      while (peek_token().value == "::")
      {
        advance_token();
        ns += "::" + match_token(Lexer::TokenType::IDENTIFIER).value;
      }
      match_token(Lexer::TokenType::OPERATOR, ";");
      return new ASTNode("UsingNamespace", ns, ln, current_depth);
    }

    std::string alias = match_token(Lexer::TokenType::IDENTIFIER).value;
    match_token(Lexer::TokenType::OPERATOR, "=");

    if (peek_token().value == "namespace")
    {
      advance_token();
      std::string ns = match_token(Lexer::TokenType::IDENTIFIER).value;
      while (peek_token().value == "::")
      {
        advance_token();
        ns += "::" + match_token(Lexer::TokenType::IDENTIFIER).value;
      }
      match_token(Lexer::TokenType::OPERATOR, ";");
      ASTNode *node = new ASTNode("UsingNsAlias", alias, ln, current_depth);
      node->children.push_back(new ASTNode("Target", ns, ln, current_depth));
      return node;
    }

    // type alias
    std::string target = parse_type_string();
    match_token(Lexer::TokenType::OPERATOR, ";");
    ASTNode *node = new ASTNode("UsingAlias", alias, ln, current_depth);
    node->children.push_back(new ASTNode("Target", target, ln, current_depth));
    return node;
  }

  ASTNode *parse_struct()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "struct");
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
    match_token(Lexer::TokenType::OPERATOR, "{");

    ASTNode *struct_node = new ASTNode("Struct", name, ln, current_depth);
    while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      std::string type = parse_type_string();
      std::string field_name = match_token(Lexer::TokenType::IDENTIFIER).value;
      match_token(Lexer::TokenType::OPERATOR, ";");
      struct_node->children.push_back(
          new ASTNode("Field", type + " " + field_name, ln, current_depth));
    }
    match_token(Lexer::TokenType::OPERATOR, "}");
    return struct_node;
  }

  ASTNode *parse_enum()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "enum");
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
    std::string backing = "int32";
    if (peek_token().value == ":")
    {
      advance_token();
      backing = advance_token().value;
    }
    match_token(Lexer::TokenType::OPERATOR, "{");

    ASTNode *enum_node = new ASTNode("Enum", name + ":" + backing, ln, current_depth);
    while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      std::string val = match_token(Lexer::TokenType::IDENTIFIER).value;
      std::string explicit_val = "";
      if (peek_token().value == "=")
      {
        advance_token();
        while (peek_token().value != "," && peek_token().value != "}")
          explicit_val += advance_token().value;
      }
      ASTNode *ev = new ASTNode("EnumValue", val, ln, current_depth);
      if (!explicit_val.empty())
        ev->value += "=" + explicit_val;
      enum_node->children.push_back(ev);
      if (peek_token().value == ",")
        advance_token();
    }
    match_token(Lexer::TokenType::OPERATOR, "}");
    return enum_node;
  }

  ASTNode *parse_reserve()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "reserve");

    // Check for reserve<shared>
    bool is_shared = false;
    if (peek_token().value == "<")
    {
      advance_token();
      if (peek_token().value == "shared")
      {
        advance_token();
        is_shared = true;
      }
      match_token(Lexer::TokenType::OPERATOR, ">");
    }

    // Type-inferred reserve: reserve varname = fn(...)  or  reserve varname << tunnel_name [= fn()]
    // Detected when an identifier is followed immediately by '=' or '<<' (no type kw).
    std::string type;
    std::string name;
    if ((peek_token().type == Lexer::TokenType::IDENTIFIER) &&
        (peek_token(1).value == "=" || peek_token(1).value == "<<"))
    {
      // No explicit type — will be inferred from the tunnel in codegen
      type = "__infer__";
      name = advance_token().value;
    }
    else
    {
      type = parse_type_string();
      name = match_token(Lexer::TokenType::IDENTIFIER).value;
    }

    ASTNode *node = new ASTNode("Reserve", type + " " + name, ln, current_depth);
    if (is_shared)
      node->children.push_back(new ASTNode("Shared", "shared", ln, current_depth));

    // Extended tunnel-binding syntax: reserve [type] name << tunnel_name [= call();]
    // Binds this reserve slot explicitly to a named tunnel output, instead of
    // relying on positional/type inference.
    if (peek_token().value == "<<")
    {
      advance_token();
      std::string tunnel_name = match_token(Lexer::TokenType::IDENTIFIER).value;
      node->children.push_back(new ASTNode("TunnelBind", tunnel_name, ln, current_depth));
    }

    if (peek_token().value == "=")
    {
      advance_token();
      node->children.push_back(parse_expression());
    }
    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  ASTNode *parse_move()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "move");
    std::string target = match_token(Lexer::TokenType::IDENTIFIER).value;
    match_token(Lexer::TokenType::OPERATOR, ";");
    return new ASTNode("Move", target, ln, current_depth);
  }

  ASTNode *parse_reset()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "reset");
    match_token(Lexer::TokenType::OPERATOR, ";");
    return new ASTNode("Reset", "", ln, current_depth);
  }

  ASTNode *parse_const()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "const");
    std::string type = parse_type_string();
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
    match_token(Lexer::TokenType::OPERATOR, "=");

    ASTNode *node = new ASTNode("Const", type + " " + name, ln, current_depth);
    node->children.push_back(parse_expression());
    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  // Parse a single initializer-list element: collect tokens until ',' or '}'
  // at depth 0, without consuming the delimiter itself.
  ASTNode *parse_initlist_elem()
  {
    size_t ln = current_line();
    ASTNode *expr = new ASTNode("Expression", "", ln, current_depth);
    int depth = 0; // tracks ( { [ nesting
    while (peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      const std::string &v = peek_token().value;
      if (v == "(" || v == "[" || v == "{")
      {
        depth++;
        expr->children.push_back(make_token_node(advance_token()));
        continue;
      }
      if (v == ")" || v == "]" || v == "}")
      {
        if (depth == 0)
          break;
        depth--;
        expr->children.push_back(make_token_node(advance_token()));
        continue;
      }
      if (depth == 0 && v == ",")
        break;
      expr->children.push_back(make_token_node(advance_token()));
    }
    return expr;
  }

  ASTNode *parse_declaration()
  {
    size_t ln = current_line();
    std::string type = parse_type_string();
    std::string name = match_token(Lexer::TokenType::IDENTIFIER).value;
    ASTNode *node = new ASTNode("Declaration", type + " " + name, ln, current_depth);

    if (peek_token().value == "=")
    {
      advance_token();
      // Brace initializer:  T x = { expr, expr, ... };
      // Works for: T[] arena arrays, Vector<T>, Buffer<T>, List, Deque,
      //            RingBuffer, SortedVec, and plain structs.
      // Nested:    Vector<Pair<A,B>> x = {{ a, b }, { c, d }};
      if (peek_token().value == "{")
      {
        advance_token(); // consume '{'
        ASTNode *init_list = new ASTNode("InitList", type, ln, current_depth);
        while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
        {
          // ── Nested brace { ... } → child InitList (struct/pair element) ─
          if (peek_token().value == "{")
          {
            advance_token(); // consume inner '{'
            ASTNode *inner = new ASTNode("InitList", "", ln, current_depth);
            while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
            {
              inner->children.push_back(parse_initlist_elem());
              if (peek_token().value == ",")
                advance_token();
            }
            match_token(Lexer::TokenType::OPERATOR, "}");
            init_list->children.push_back(inner);
          }
          else
          {
            // Plain expression element — stop at ',' or '}'
            init_list->children.push_back(parse_initlist_elem());
          }
          if (peek_token().value == ",")
            advance_token();
        }
        match_token(Lexer::TokenType::OPERATOR, "}");
        node->children.push_back(init_list);
      }
      else
      {
        node->children.push_back(parse_expression());
      }
    }
    match_token(Lexer::TokenType::OPERATOR, ";");
    return node;
  }

  ASTNode *parse_if()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "if");
    match_token(Lexer::TokenType::OPERATOR, "(");
    ASTNode *cond = parse_expression();
    match_token(Lexer::TokenType::OPERATOR, ")");

    ASTNode *node = new ASTNode("If", "", ln, current_depth);
    node->children.push_back(cond);
    node->children.push_back(parse_block());

    if (peek_token().value == "else")
    {
      advance_token();
      if (peek_token().value == "if")
      {
        node->children.push_back(parse_if());
      }
      else
      {
        node->children.push_back(parse_block());
      }
    }
    return node;
  }

  ASTNode *parse_while()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "while");
    match_token(Lexer::TokenType::OPERATOR, "(");
    ASTNode *cond = parse_expression();
    match_token(Lexer::TokenType::OPERATOR, ")");

    ASTNode *node = new ASTNode("While", "", ln, current_depth);
    node->children.push_back(cond);
    node->children.push_back(parse_block());
    return node;
  }

  ASTNode *parse_for()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "for");
    match_token(Lexer::TokenType::OPERATOR, "(");

    ASTNode *node = new ASTNode("For", "", ln, current_depth);
    node->children.push_back(parse_declaration());
    node->children.push_back(parse_expression());
    match_token(Lexer::TokenType::OPERATOR, ";");
    node->children.push_back(parse_expression());
    match_token(Lexer::TokenType::OPERATOR, ")");
    node->children.push_back(parse_block());
    return node;
  }

  ASTNode *parse_foreach()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "foreach");
    match_token(Lexer::TokenType::OPERATOR, "(");
    std::string type = parse_type_string();
    std::string item = match_token(Lexer::TokenType::IDENTIFIER).value;
    match_token(Lexer::TokenType::OPERATOR, ":");
    ASTNode *collection = parse_expression();
    match_token(Lexer::TokenType::OPERATOR, ")");

    ASTNode *node = new ASTNode("Foreach", type + " " + item, ln, current_depth);
    node->children.push_back(collection);
    node->children.push_back(parse_block());
    return node;
  }

  ASTNode *parse_switch()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "switch");
    match_token(Lexer::TokenType::OPERATOR, "(");
    ASTNode *target = parse_expression();
    match_token(Lexer::TokenType::OPERATOR, ")");
    match_token(Lexer::TokenType::OPERATOR, "{");

    ASTNode *node = new ASTNode("Switch", "", ln, current_depth);
    node->children.push_back(target);

    current_depth++;
    while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
    {
      if (peek_token().type == Lexer::TokenType::KEYWORD && peek_token().value == "case")
      {
        size_t cln = current_line();
        advance_token();
        std::string case_val = advance_token().value;
        match_token(Lexer::TokenType::OPERATOR, ":");
        ASTNode *case_node = new ASTNode("Case", case_val, cln, current_depth);

        while (peek_token().value != "case" && peek_token().value != "default" &&
               peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
        {
          case_node->children.push_back(parse_top_level());
        }
        node->children.push_back(case_node);
      }
      else if (peek_token().type == Lexer::TokenType::KEYWORD && peek_token().value == "default")
      {
        size_t dln = current_line();
        advance_token();
        match_token(Lexer::TokenType::OPERATOR, ":");
        ASTNode *default_node = new ASTNode("Default", "", dln, current_depth);

        while (peek_token().value != "}" && peek_token().type != Lexer::TokenType::END_OF_FILE)
        {
          default_node->children.push_back(parse_top_level());
        }
        node->children.push_back(default_node);
      }
      else
      {
        throw std::runtime_error(
            "[SYNTAX ERROR] Line " + std::to_string(current_line()) +
            ": Expected 'case' or 'default' inside switch, got: " + peek_token().value);
      }
    }
    current_depth--;
    match_token(Lexer::TokenType::OPERATOR, "}");
    return node;
  }

  ASTNode *parse_break()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "break");
    match_token(Lexer::TokenType::OPERATOR, ";");
    return new ASTNode("Break", "", ln, current_depth);
  }

  ASTNode *parse_continue()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "continue");
    match_token(Lexer::TokenType::OPERATOR, ";");
    return new ASTNode("Continue", "", ln, current_depth);
  }

  ASTNode *parse_template()
  {
    size_t ln = current_line();
    match_token(Lexer::TokenType::KEYWORD, "template");
    match_token(Lexer::TokenType::OPERATOR, "<");

    ASTNode *template_node = new ASTNode("Template", "", ln, current_depth);

    // Parse template parameters: typename T, typename U, etc.
    ASTNode *params = new ASTNode("TemplateParams", "", ln, current_depth);
    while (peek_token().value != ">")
    {
      if (peek_token().type == Lexer::TokenType::KEYWORD && peek_token().value == "typename")
      {
        advance_token();
        std::string param_name = match_token(Lexer::TokenType::IDENTIFIER).value;
        params->children.push_back(new ASTNode("TypeParam", param_name, ln, current_depth));

        if (peek_token().value == ",")
        {
          advance_token();
        }
      }
      else
      {
        throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(current_line()) +
                                 ": Expected 'typename' in template parameters");
      }
    }
    match_token(Lexer::TokenType::OPERATOR, ">");
    template_node->children.push_back(params);

    // Parse the templated definition (struct, function, or import)
    if (peek_token().type == Lexer::TokenType::KEYWORD)
    {
      const std::string &kw = peek_token().value;
      if (kw == "struct")
      {
        template_node->children.push_back(parse_struct());
      }
      else if (kw == "def")
      {
        template_node->children.push_back(parse_function());
      }
      else if (kw == "import")
      {
        // template<typename T> import ... ; — treat as a plain CImport
        // (generics in std.cll are type-annotated C externs, not real
        // templates)
        template_node->children.push_back(parse_import());
      }
      else if (kw == "template")
      {
        // Nested template params: template<K> template<V> struct ...
        // Collect additional type params and recurse into the inner definition
        template_node->children.push_back(parse_template());
      }
      else
      {
        throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(current_line()) +
                                 ": Template can only precede 'struct', 'def', or 'import'");
      }
    }
    else
    {
      throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(current_line()) +
                               ": Expected 'struct', 'def', or 'import' after template");
    }

    return template_node;
  }
};

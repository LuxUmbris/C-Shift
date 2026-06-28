#pragma once
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

class Lexer
{
public:
  enum class TokenType
  {
    IDENTIFIER,
    KEYWORD,
    NUMBER,
    STRING,
    OPERATOR,
    END_OF_FILE
  };

  struct Token
  {
    TokenType type;
    std::string value;
    int64_t line = 0; // source line number (1-based)
  };

private:
  // C<< Keyword set including primitives
  std::unordered_map<std::string, TokenType> keyword_lookup = {
      {"if", TokenType::KEYWORD},       {"else", TokenType::KEYWORD},
      {"switch", TokenType::KEYWORD},   {"case", TokenType::KEYWORD},
      {"default", TokenType::KEYWORD},  {"for", TokenType::KEYWORD},
      {"foreach", TokenType::KEYWORD},  {"while", TokenType::KEYWORD},
      {"tunnel", TokenType::KEYWORD},   {"reserve", TokenType::KEYWORD},
      {"struct", TokenType::KEYWORD},   {"namespace", TokenType::KEYWORD},
      {"def", TokenType::KEYWORD},      {"enum", TokenType::KEYWORD},
      {"move", TokenType::KEYWORD},     {"valid", TokenType::KEYWORD},
      {"voided", TokenType::KEYWORD},   {"raw", TokenType::KEYWORD},
      // Canonical integer/float types
      {"int8", TokenType::KEYWORD},     {"int16", TokenType::KEYWORD},
      {"int32", TokenType::KEYWORD},    {"int64", TokenType::KEYWORD},
      {"uint8", TokenType::KEYWORD},    {"uint16", TokenType::KEYWORD},
      {"uint32", TokenType::KEYWORD},   {"uint64", TokenType::KEYWORD},
      {"float32", TokenType::KEYWORD},  {"float64", TokenType::KEYWORD},
      {"char", TokenType::KEYWORD},     {"bool", TokenType::KEYWORD},
      {"string", TokenType::KEYWORD},
      // Short aliases (i8/u8/i32/u32 etc.) — identical semantics
      {"i8", TokenType::KEYWORD},       {"u8", TokenType::KEYWORD},
      {"i16", TokenType::KEYWORD},      {"u16", TokenType::KEYWORD},
      {"i32", TokenType::KEYWORD},      {"u32", TokenType::KEYWORD},
      {"i64", TokenType::KEYWORD},      {"u64", TokenType::KEYWORD},
      {"f32", TokenType::KEYWORD},      {"f64", TokenType::KEYWORD},
      {"byte", TokenType::KEYWORD},     // alias for u8
      {"true", TokenType::KEYWORD},     {"false", TokenType::KEYWORD},
      {"import", TokenType::KEYWORD},   {"entry", TokenType::KEYWORD},
      {"const", TokenType::KEYWORD},    
      {"reset", TokenType::KEYWORD},    {"break", TokenType::KEYWORD},
      {"continue", TokenType::KEYWORD}, {"template", TokenType::KEYWORD},
      {"typename", TokenType::KEYWORD}, {"export", TokenType::KEYWORD},
      {"dec", TokenType::KEYWORD},      {"class", TokenType::KEYWORD},
      {"using", TokenType::KEYWORD}};

  // Operators sorted by length (Maximal Munch)
  std::vector<std::pair<std::string, TokenType>> operator_lookup = {
      {"<<=", TokenType::OPERATOR}, {">>=", TokenType::OPERATOR}, {"**=", TokenType::OPERATOR},
      {"[:]", TokenType::OPERATOR}, {"...", TokenType::OPERATOR}, {"->", TokenType::OPERATOR},
      {"::", TokenType::OPERATOR},  {"==", TokenType::OPERATOR},  {"!=", TokenType::OPERATOR},
      {"<=", TokenType::OPERATOR},  {">=", TokenType::OPERATOR},  {"&&", TokenType::OPERATOR},
      {"||", TokenType::OPERATOR},  {"+=", TokenType::OPERATOR},  {"-=", TokenType::OPERATOR},
      {"*=", TokenType::OPERATOR},  {"/=", TokenType::OPERATOR},  {"%=", TokenType::OPERATOR},
      {"<<", TokenType::OPERATOR},  {">>", TokenType::OPERATOR},  {"{", TokenType::OPERATOR},
      {"}", TokenType::OPERATOR},   {"(", TokenType::OPERATOR},   {")", TokenType::OPERATOR},
      {"[", TokenType::OPERATOR},   {"]", TokenType::OPERATOR},   {"+", TokenType::OPERATOR},
      {"-", TokenType::OPERATOR},   {"*", TokenType::OPERATOR},   {"/", TokenType::OPERATOR},
      {"%", TokenType::OPERATOR},   {"=", TokenType::OPERATOR},   {"<", TokenType::OPERATOR},
      {">", TokenType::OPERATOR},   {";", TokenType::OPERATOR},   {":", TokenType::OPERATOR},
      {"&", TokenType::OPERATOR},   {"!", TokenType::OPERATOR},   {",", TokenType::OPERATOR},
      {"?", TokenType::OPERATOR},   {".", TokenType::OPERATOR}};

  std::string src;
  size_t pos = 0;
  int64_t line = 1;

  char peek(size_t offset = 0) const
  {
    if (pos + offset >= src.length())
      return '\0';
    return src[pos + offset];
  }

  char advance()
  {
    if (pos < src.length())
    {
      if (src[pos] == '\n')
        line++;
      return src[pos++];
    }
    return '\0';
  }

public:
  Lexer(const std::string &text) : src(text) {}

  std::vector<Token> tokenize()
  {
    std::vector<Token> tokens;
    while (pos < src.length())
    {
      char current = peek();

      // Ignore whitespace
      if (std::isspace(current))
      {
        advance();
        continue;
      }

      // Skip single-line comments
      if (current == '/' && peek(1) == '/')
      {
        while (peek() != '\n' && peek() != '\0')
          advance();
        continue;
      }

      // Skip multiline comments
      if (current == '/' && peek(1) == '*')
      {
        advance(); // Skip /
        advance(); // Skip *
        while (peek() != '\0')
        {
          if (peek() == '*' && peek(1) == '/')
          {
            advance(); // Skip *
            advance(); // Skip /
            break;
          }
          advance();
        }
        continue;
      }

      // Logic for C<< raw strings
      if (src.compare(pos, 4, "raw<") == 0)
      {
        tokens.push_back(readRawString());
        continue;
      }

      // Quoted strings
      if (current == '"')
      {
        tokens.push_back(readString());
        continue;
      }

      // Multi-char/Single-char operators
      bool found_op = false;
      for (const auto &[op, type] : operator_lookup)
      {
        if (src.compare(pos, op.length(), op) == 0)
        {
          tokens.push_back({type, op, line});
          for (size_t i = 0; i < op.length(); ++i)
            advance();
          found_op = true;
          break;
        }
      }
      if (found_op)
        continue;

      // Numbers (Int and Float)
      if (std::isdigit(current))
      {
        tokens.push_back(readNumber());
        continue;
      }

      // Identifiers and Keywords
      if (std::isalpha(current) || current == '_')
      {
        tokens.push_back(readIdentifierOrKeyword());
        continue;
      }

      std::cerr << "[ERROR] Line " << line << ": Unknown symbol '" << current << "'\n";
      throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(line) +
                               ": Unknown symbol '" + std::string(1, current) + "'");
    }
    tokens.push_back({TokenType::END_OF_FILE, "", line});
    return tokens;
  }

private:
  Token readString()
  {
    advance(); // Skip "
    std::string val;
    while (peek() != '"' && peek() != '\0')
    {
      if (peek() == '\\')
        val += advance(); // Basic escape pass-through
      val += advance();
    }
    advance(); // Skip "
    return {TokenType::STRING, val, line};
  }

  Token readRawString()
  {
    for (int i = 0; i < 4; ++i)
      advance(); // Skip "raw<"
    std::string content;

    if (src.compare(pos, 5, "until") == 0)
    {
      // Case: raw<until "DELIM">
      for (int i = 0; i < 6; ++i)
        advance(); // Skip "until "
      std::string delim = readString().value;
      while (peek() != '>' && peek() != '\0')
        advance();
      advance(); // Skip ">"
      if (peek() == '\n')
        advance();

      size_t end = src.find(delim, pos);
      if (end != std::string::npos)
      {
        content = src.substr(pos, end - pos);
        pos = end + delim.length();
      }
    }
    else
    {
      // Case: raw<N> (N lines)
      std::string numStr;
      while (std::isdigit(peek()))
        numStr += advance();
      advance(); // Skip ">"
      if (peek() == '\n')
        advance();

      int linesToRead = numStr.empty() ? 0 : std::stoi(numStr);
      while (linesToRead > 0 && pos < src.length())
      {
        char c = advance();
        content += c;
        if (c == '\n')
          linesToRead--;
      }
    }
    return {TokenType::STRING, content, line};
  }

  Token readNumber()
  {
    std::string val;
    // Hex literal: 0x... or 0X...
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X'))
    {
      val += advance(); // '0'
      val += advance(); // 'x'
      if (!std::isxdigit(peek()))
        throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(line) +
                                 ": Expected hex digit after '0x'");
      while (std::isxdigit(peek()))
        val += advance();
      return {TokenType::NUMBER, val, line};
    }
    // Binary literal: 0b... or 0B...
    if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B'))
    {
      val += advance(); // '0'
      val += advance(); // 'b'
      if (peek() != '0' && peek() != '1')
        throw std::runtime_error("[SYNTAX ERROR] Line " + std::to_string(line) +
                                 ": Expected binary digit after '0b'");
      while (peek() == '0' || peek() == '1')
        val += advance();
      return {TokenType::NUMBER, val, line};
    }
    // Decimal / float
    while (std::isdigit(peek()))
      val += advance();
    // Support decimal point for float
    if (peek() == '.' && std::isdigit(peek(1)))
    {
      val += advance();
      while (std::isdigit(peek()))
        val += advance();
    }
    // Optional exponent: 1e10, 3.14e-2
    if ((peek() == 'e' || peek() == 'E') &&
        (std::isdigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && std::isdigit(peek(2)))))
    {
      val += advance(); // 'e'
      if (peek() == '+' || peek() == '-')
        val += advance();
      while (std::isdigit(peek()))
        val += advance();
    }
    return {TokenType::NUMBER, val, line};
  }

  Token readIdentifierOrKeyword()
  {
    std::string val;
    while (std::isalnum(peek()) || peek() == '_')
      val += advance();
    if (keyword_lookup.count(val))
      return {keyword_lookup[val], val, line};
    return {TokenType::IDENTIFIER, val, line};
  }
};

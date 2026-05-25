# C<< (C-Shift) Language Syntax Specification

> **Language version:** C-Shift 0.3 (2026)  
> **Source extension:** `.cll`  
> **Entry point:** `entry { … }`  
> **Paradigm:** Arena-based, VOP (Vertical Ownership Programming), C-ABI-compatible

-----

## Table of Contents

1. [Lexical Structure](#1-lexical-structure)
1. [Primitive Types](#2-primitive-types)
1. [Type Expressions](#3-type-expressions)
1. [Top-Level Declarations](#4-top-level-declarations)
1. [The Entry Point](#5-the-entry-point)
1. [Functions and Tunnels](#6-functions-and-tunnels)
1. [Variables](#7-variables)
1. [The Voided State and `move`](#8-the-voided-state-and-move)
1. [Expressions and Operators](#9-expressions-and-operators)
1. [Statements and Control Flow](#10-statements-and-control-flow)
1. [Structs](#11-structs)
1. [Enums](#12-enums)
1. [Namespaces](#13-namespaces)
1. [Arrays and Slices](#14-arrays-and-slices)
1. [Raw Strings](#15-raw-strings)
1. [Imports and C-ABI Interop](#16-imports-and-c-abi-interop)
1. [Arena Model and VOP Rules](#17-arena-model-and-vop-rules)
1. [Compile-Time Constants](#18-compile-time-constants)
1. [Reserved Words](#19-reserved-words)
1. [Complete Operator Table](#20-complete-operator-table)
1. [Grammar Summary (EBNF)](#21-grammar-summary-ebnf)

-----

## 1. Lexical Structure

### 1.1 Comments

C<< supports both single-line and multiline comments:

**Single-line:**
```cll
// This is a comment. Everything until end of line is ignored.
```

**Multiline:**
```cll
/* This is a block comment.
   It can span multiple lines.
   Nesting is not supported. */
```

### 1.2 Identifiers

An identifier begins with a letter or underscore and is followed by any number of alphanumeric characters or underscores:

```
identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers that match a keyword are classified as keywords and cannot be used as names.

### 1.3 Integer and Float Literals

```
number ::= [0-9]+
         | [0-9]+ '.' [0-9]+
```

Decimal integers and decimal floats are supported. Hexadecimal, octal, and binary literals are not in the lexer.

### 1.4 String Literals

Ordinary quoted strings:

```
string_literal ::= '"' ( escape_char | any_char_except_quote )* '"'
```

Escape sequences are passed through as-is (the lexer does not interpret them; the backend expands them at IR generation time). Standard C escapes such as `\n`, `\t`, `\0`, `\\`, and `\"` are all valid.

### 1.5 Raw Strings

See [§15 Raw Strings](#15-raw-strings).

### 1.6 Whitespace

Spaces, tabs, carriage returns, and newlines are all treated as whitespace and are ignored between tokens.

-----

## 2. Primitive Types

|Type     |Width  |Description                        |
|---------|-------|-----------------------------------|
|`int8`   |8 bit  |Signed integer                     |
|`int16`  |16 bit |Signed integer                     |
|`int32`  |32 bit |Signed integer                     |
|`int64`  |64 bit |Signed integer                     |
|`uint8`  |8 bit  |Unsigned integer                   |
|`uint16` |16 bit |Unsigned integer                   |
|`uint32` |32 bit |Unsigned integer                   |
|`uint64` |64 bit |Unsigned integer                   |
|`float32`|32 bit |IEEE-754 single-precision float    |
|`float64`|64 bit |IEEE-754 double-precision float    |
|`bool`   |1 bit  |Boolean (`true` / `false`)         |
|`char`   |8 bit  |Single character (unsigned byte)   |
|`string` |pointer|Pointer to null-terminated C string|
|`voided` |—      |Absence of type (used in C-interop)|

`string` lowers to `i8*` in LLVM IR. `voided` lowers to `void`; `voided*` lowers to `i8*`.

-----

## 3. Type Expressions

Types may be decorated with pointer, array, or slice suffixes:

```
type_expr ::= base_type pointer_suffix? array_suffix?

base_type      ::= primitive_type | identifier   // struct or enum name

pointer_suffix ::= '*'+                          // one or more stars

array_suffix   ::= '[' ']'                       // arena-bound array (T[])
                 | '[' ':' ']'                   // non-owning slice (T[:])
                 | '[' expression ']'            // sized array (T[N])
```

### Examples

```cll
int32            // plain int
int32*           // pointer to int32
int32**          // pointer to pointer to int32
uint8[:]         // slice of bytes (non-owning)
float32[]        // arena-bound float array
voided*          // opaque pointer (like void* in C)
```

-----

## 4. Top-Level Declarations

A C<< source file is a flat sequence of top-level items, processed in order. There is no ordering constraint between definitions; the checker/codegen performs a forward-declaration pass first.

Top-level items are:

- `import` (module or C-function)
- `struct` definition
- `enum` definition
- `namespace` block
- `def` function definition
- `entry` block (exactly one per program)
- `const` declaration
- Type-prefixed variable declaration

-----

## 5. The Entry Point

Every executable C<< program must contain exactly one `entry` block. It compiles to a C `main()` function with no arguments.

```cll
entry
{
    // program body
}
```

Attempting to define `entry` more than once is a compile-time error.

-----

## 6. Functions and Tunnels

### 6.1 Function Definition

```
function_def ::= 'def' identifier '(' parameter_list ')' block
parameter_list ::= ( type_expr identifier ( ',' type_expr identifier )* )?
```

```cll
def add(int32 a, int32 b)
{
    tunnel a + b -> int32 result;
}
```

Functions have **no return type annotation** in the signature. Output values are communicated via `tunnel` statements.

### 6.2 Tunnel Statement

A tunnel writes a value from inside a function body into a slot declared in the caller’s scope:

```
tunnel_stmt ::= 'tunnel' expression '->' type_expr identifier ';'
```

```cll
def square(int32 x)
{
    tunnel x * x -> int32 result;
}
```

Rules:

- The expression before `->` is the value to tunnel out.
- The `type_expr identifier` after `->` names the tunnel target. When called from a scope that has `reserve`d a matching variable, the value flows into it.
- A function may contain any number of tunnel statements, including tunnels inside branches.
- Tunneling a pointer type out of a function generates a VOP checker warning; the caller is responsible for ensuring the referenced arena outlives the call site.

### 6.3 Calling a Function

**Classic form** — reserve first, then call:

```cll
reserve int32 result;
add(5, 7);
// result is now filled
```

**Inline form** — reserve-and-initialize in one line:

```cll
reserve int32 result = add(5, 7);
```

In the inline form the function must tunnel at least one value whose type matches the declared type of the reserve variable. If multiple tunnels of the same type exist, the one whose name matches the reserve variable name is preferred; otherwise any type-matching tunnel is accepted.

**Multiple tunnel targets:**

```cll
def compute(int32 x, int32 y)
{
    tunnel x + y -> int32 sum;
    tunnel x * y -> int32 product;
}

entry
{
    reserve int32 sum;
    reserve int32 product;
    compute(8, 4);   // fills both sum and product
}
```

All `reserve`d variables in the calling scope whose names and types match a tunnel in the function are filled by a single call. Unmatched tunnel targets are silently ignored.

### 6.4 Inline Usage Restriction

Using a function call in any position other than a `reserve` initializer or as a standalone call statement is **forbidden**:

```cll
// ILLEGAL:
int32 x = add(1, 2);  // direct use of call result in a plain declaration

// LEGAL:
reserve int32 x = add(1, 2);
```

-----

## 7. Variables

### 7.1 Plain Declaration

```
declaration ::= type_expr identifier ( '=' expression )? ';'
```

```cll
int32 x;
int32 y = 42;
string name = "Alice";
Player p;            // struct type
```

Declared variables are owned by the current arena (scope). They are destroyed when the enclosing block exits.

### 7.2 Reserve

`reserve` declares a variable whose value is to be filled by an upcoming `tunnel` call. The variable exists in the current scope and may outlive the called function:

```
reserve_stmt ::= 'reserve' ( '<' 'shared' '>' )?
                 type_expr identifier ( '=' expression )? ';'
```

```cll
reserve int32 answer;
compute(6, 7);
```

**`reserve<shared>`** marks the variable as read-only after its initialization (it cannot be reassigned):

```cll
reserve<shared> int32 config_value = load_config();
// config_value = 5;  <-- checker error
```

### 7.3 Constants

```
const_decl ::= 'const' type_expr identifier '=' expression ';'
```

```cll
const int32 MAX_SIZE = 1024;
const float64 PI = 3.14159265358979;
```

Constants are immutable; any assignment to a `const` variable is a compile-time error.

### 7.4 Assignment

```
assignment ::= lvalue assign_op expression ';'
lvalue     ::= identifier ( '.' identifier )*
assign_op  ::= '=' | '+=' | '-=' | '*=' | '/='
```

```cll
x = x + 1;
player.health -= 10.0;
score += 1;
```

Compound assignment operators `+=`, `-=`, `*=`, `/=` are all supported. Assignment to `const` or `reserve<shared>` variables is a checker error.

-----

## 8. The Voided State and `move`

### 8.1 Concept

Any variable can be in either the **valid** state (holds a live value) or the **voided** state (has been moved; accessing it is undefined). This is a compile-time-tracked property — there are no null pointers in C<<; instead, a variable tracks whether it has been moved away.

### 8.2 `move`

```
move_stmt ::= 'move' identifier ';'
```

`move` transitions a variable into the voided state. After a `move`, the variable’s storage is conceptually surrendered. Attempting to access a voided variable outside of a `switch` guard is a compile-time error.

```cll
int32 x = 10;
int32* p = &x;
move x;
// Accessing x here is a checker error.
```

Rules:

- `move` on a variable that is already voided is an error.
- `move` on a `const` variable is an error.

### 8.3 `switch` guard for voided state

To safely access a variable that may be voided, use a `switch` with `case valid` and `case voided`:

```cll
int32* p = &x;
move x;

switch (p)
{
    case valid
    {
        // p is safe to use here
        printf("%d\n", *p);
    }
    case voided
    {
        puts("p is voided");
    }
}
```

Inside `case valid`, the variable is accessible as though it were not voided. Outside the guard, any use of a known-voided variable is a compile-time error.

### 8.4 `reset`

```
reset_stmt ::= 'reset' ';'
```

`reset` clears the current arena. It is a hint to the compiler that all arena-owned allocations in the current scope should be freed or reset. Forbidden if child arenas contain pointers into the current arena.

At the codegen level `reset` currently has no emitted runtime action (it is a semantic marker).

-----

## 9. Expressions and Operators

Expressions are a general token stream between delimiters. The parser collects tokens into an `Expression` node and the codegen emits them with standard operator precedence (handled by the LLVM IR builder).

### 9.1 Arithmetic

|Operator|Meaning       |
|--------|--------------|
|`+`     |Addition      |
|`-`     |Subtraction   |
|`*`     |Multiplication|
|`/`     |Division      |
|`%`     |Modulo        |

### 9.2 Comparison

|Operator|Meaning              |
|--------|---------------------|
|`==`    |Equal                |
|`!=`    |Not equal            |
|`<`     |Less than            |
|`>`     |Greater than         |
|`<=`    |Less than or equal   |
|`>=`    |Greater than or equal|

### 9.3 Logical

|Operator|Meaning    |
|--------|-----------|
|`&&`    |Logical AND|
|`||`    |Logical OR |
|`!`     |Logical NOT|

### 9.4 Bitwise

|Operator|Meaning    |
|--------|-----------|
|`&`     |Bitwise AND|
|`<<`    |Left shift |
|`>>`    |Right shift|

### 9.5 Compound Assignment

```
+= -= *= /= %= <<= >>= **=
```

### 9.6 Pointer and Address

|Operator|Meaning                                                 |
|--------|--------------------------------------------------------|
|`&`     |Address-of                                              |
|`*`     |Dereference                                             |
|`->`    |Tunnel target (not a field accessor; use `.` for fields)|

### 9.7 Field Access

```
expr '.' identifier
```

Used for struct field access in both expressions and assignment targets.

### 9.8 Boolean Literals

```
true   false
```

### 9.9 Namespace Resolution

```
identifier '::' identifier
```

Used to qualify names inside namespaces:

```cll
Math::PI
```

-----

## 10. Statements and Control Flow

### 10.1 `if` / `else`

```
if_stmt ::= 'if' '(' expression ')' block
            ( 'else' ( if_stmt | block ) )?
```

```cll
if (x > 0)
{
    puts("positive");
}
else if (x < 0)
{
    puts("negative");
}
else
{
    puts("zero");
}
```

### 10.2 `while`

Each iteration is its own sub-arena.

```
while_stmt ::= 'while' '(' expression ')' block
```

```cll
while (i < 10)
{
    i += 1;
}
```

### 10.3 `for`

```
for_stmt ::= 'for' '(' declaration expression ';' expression ')' block
```

Note: the initializer is a full declaration (including the type). The update expression does **not** have a trailing semicolon inside the parentheses; the closing `)` follows the update expression directly.

```cll
for (int32 i = 0; i < 5;)
{
    printf("%d\n", i);
    i += 1;
}
```

Each iteration is its own sub-arena.

### 10.4 `foreach`

```
foreach_stmt ::= 'foreach' '(' type_expr identifier ':' expression ')' block
```

Iterates over an array or slice. The loop variable is declared fresh each iteration inside its own sub-arena.

```cll
foreach (int32 val : my_array)
{
    printf("%d\n", val);
}
```

### 10.5 `break` and `continue`

```
break_stmt     ::= 'break' ';'
continue_stmt  ::= 'continue' ';'
```

`break` exits the innermost enclosing loop or switch:

```cll
while (i < 10)
{
    if (i == 5)
        break;  // exits while loop
    i += 1;
}
```

`continue` restarts the next iteration of the innermost enclosing loop (not allowed in switch):

```cll
for (int32 i = 0; i < 10; i += 1)
{
    if (i % 2 == 0)
        continue;  // skip to next iteration
    printf("%d\n", i);
}
```

Both are compile-time checked to ensure they appear within a loop or (for break) switch context.

### 10.6 `switch` / `case` / `default`

```
switch_stmt  ::= 'switch' '(' expression ')' '{' case_clause* '}'
case_clause  ::= 'case' identifier ':' statement*
               | 'default' ':' statement*
```

There are no fallthrough semantics; each case is independent. Cases do not need braces (the body ends when the next `case`, `default`, or `}` is reached).

**Voided-state guard form:**

```cll
switch (ptr)
{
    case valid
    {
        // ptr is usable here
    }
    case voided
    {
        // ptr has been moved
    }
}
```

**Enum dispatch form:**

```cll
switch (status)
{
    case Active
    {
        puts("active");
    }
    case Inactive
    {
        puts("inactive");
    }
    default
    {
        puts("unknown");
    }
}
```

Tunnels inside `switch`/`case` must target variables declared in the parent scope.

### 10.7 Anonymous Blocks (Sub-arenas)

A bare `{ … }` block creates a new arena. Variables declared inside are destroyed when the block exits.

```cll
{
    int32 temp = compute_something();
    use(temp);
    // temp is gone here
}
```

-----

## 11. Templates

Templates enable compile-time generic programming. A template parameter is declared with `typename` and can be used throughout the definition:

```
template_def ::= 'template' '<' typename_param ( ',' typename_param )* '>' 
                 ( struct_def | function_def )
typenaming_param ::= 'typename' identifier
```

```cll
template<typename T>
struct Vector
{
    T*    data;
    uint64 len;
    uint64 capacity;
}

template<typename T>
def vec_push(Vector<T>* v, T elem)
{
    // …
    tunnel result -> int32 success;
}
```

Template instantiation happens at compile time. When a template is used with a concrete type (e.g., `Vector<int32>`), the compiler generates a monomorphic copy for that type. This allows zero-runtime polymorphism and enables safe generic containers.

-----

## 12. Structs

Structs are **data-only** — they may not contain methods.

```
struct_def ::= 'struct' identifier '{' field* '}'
field      ::= type_expr identifier ';'
```

```cll
struct Vec2
{
    float32 x;
    float32 y;
}

struct Player
{
    int32 id;
    float32 health;
    Vec2 position;
}
```

Usage:

```cll
Player p;
p.id = 1;
p.health = 100.0;
p.position.x = 0.0;
```

Struct types are valid as parameter types, field types, and local variable types. Pointer-to-struct (`Player*`) follows the usual pointer rules.

-----

## 13. Enums

Enums are **integer-backed**. The backing type defaults to `int32`.

```
enum_def   ::= 'enum' identifier ( ':' type_expr )? '{' enum_body '}'
enum_body  ::= enum_value ( ',' enum_value )* ','?
enum_value ::= identifier ( '=' expression )?
```

```cll
enum Direction { North, South, East, West }

enum Status : uint8 { Active, Inactive }

enum ErrorCode : int32
{
    Ok = 0,
    NotFound = 404,
    Internal = 500
}
```

Enum values are referenced by name (unqualified, or qualified with `::` if inside a namespace).

-----

## 14. Namespaces

Namespaces are **lexical groupings only** — they do not create new arenas.

```
namespace_def ::= 'namespace' ns_path block
ns_path       ::= identifier ( '::' identifier )*
```

```cll
namespace Math
{
    const float64 PI = 3.14159265358979;

    def square(float64 x)
    {
        tunnel x * x -> float64 result;
    }
}
```

Nested namespaces can be declared with a path in one statement:

```cll
namespace Engine::Physics
{
    // …
}
```

-----

## 15. Arrays and Slices

### 14.1 Arena-Bound Array (`T[]`)

An arena-bound array is owned by the current scope. It is destroyed when the arena exits.

```cll
int32[] numbers;
```

### 14.2 Sized Array (`T[N]`)

```cll
float32[16] matrix;
uint8[256] buffer;
```

### 14.3 Non-Owning Slice (`T[:]`)

A slice is a fat pointer: a base pointer plus a length. It does not own the memory it references.

```cll
int32[:] view;
```

VOP law: a slice may only reference arenas that outlive the slice variable itself.

-----

## 16. Raw Strings

Raw strings bypass all escape-sequence processing. They are lexed as `STRING` tokens and compiled identically to regular strings.

### 15.1 Delimiter form

```
raw_string_delim ::= 'raw<until "' identifier '">' newline content identifier
```

The content is everything between the line following the `raw<until "DELIM">` header and the first occurrence of the delimiter on its own line.

```cll
string banner = raw<until "END">
###########
# Hello C<< #
###########
END
```

### 15.2 Line-count form

```
raw_string_lines ::= 'raw<' integer '>' newline content
```

Exactly N lines are consumed verbatim, including any characters that would normally be escape sequences.

```cll
puts(raw<3>
Line one \n is literal
Line two \t is literal
Line three \0 is literal
);
```

-----

## 17. Imports and C-ABI Interop

### 16.1 Module Import

```
module_import ::= 'import' ns_path ';'
ns_path       ::= identifier ( '::' identifier )*
```

```cll
import std;
import io::file;
```

Module imports are resolved at link time. The standard library is provided as `std.cll` and bound with `-lc`.

### 16.2 File Import

```
file_import ::= 'import' string_literal ';'
```

```cll
import "path/to/module.cll";
```

### 16.3 C-Function Import

Declares an external C function for use within the current file. The function becomes callable like any C<< function (as a call statement or inline reserve initializer).

```
c_import ::= 'import' type_expr identifier '(' c_param_list ')' ';'

c_param_list ::= ( c_param ( ',' c_param )* ( ',' '...' )? )?
               | 'voided'
               | empty

c_param      ::= type_expr identifier?
```

```cll
import int32  printf(string fmt, ...);
import int32  puts(string s);
import voided free(voided* ptr);
import voided* malloc(uint64 size);
import int32  strcmp(string a, string b);
```

The return type uses `voided` for C `void`. Variadic functions use `...` as the last parameter. Parameter names are optional in declarations.

-----

## 18. Arena Model and VOP Rules

C<< uses **Vertical Ownership Programming (VOP)** instead of a garbage collector or borrow checker. The rules are:

1. **Depth law.** A pointer must only point to a variable whose arena depth is less than or equal to the pointer’s own depth. Depth is the nesting level of scopes at the point of declaration.
1. **Arena = scope.** Each `{…}` block that is a control-flow body (function body, `if`, `while`, `for`, `foreach`, `entry`) creates a new arena. When the block exits, the arena and all its variables are destroyed. Lexical scopes (namespaces, struct bodies) do **not** create arenas.
1. **No raw returns.** Functions must not use return values. Output travels through `tunnel` statements into slots declared in the caller’s scope.
1. **Tunnel pointer law.** A tunnel may not transfer data containing pointers to arenas that will be destroyed before the call site. The checker emits a warning when a pointer type is tunneled out of a function.
1. **`reset` law.** `reset` clears the current arena. It is forbidden when any child arena still holds pointers into the current arena.
1. **Voided-state law.** Any variable that has been `move`d is voided. Accessing a voided variable without a `switch (var) { case valid: … case voided: … }` guard is a compile-time error.

-----

## 19. Compile-Time Constants

```
const_decl ::= 'const' type_expr identifier '=' expression ';'
```

Constants must be initialized with a compile-time-evaluable expression. They are immutable; any attempt to assign to them after declaration is a checker error.

```cll
const int32 SCREEN_WIDTH  = 1920;
const int32 SCREEN_HEIGHT = 1080;
const float64 TAU = 6.28318530717958;
```

-----

## 20. Reserved Words

The following identifiers are keywords and cannot be used as variable or function names:

```
bool       break      case       char       const      continue
default    def        else       enum       entry      false
float32    float64    for        foreach    if         import
int8       int16      int32      int64      move       namespace
raw        reserve    reset      string     struct     switch
template   true       tunnel     typename   uint8      uint16
uint32     uint64     valid      voided     while
```

-----

## 21. Complete Operator Table

Listed by lexer precedence (longest match first):

|Operator|Category              |
|--------|----------------------|
|`<<=`   |Compound assignment   |
|`>>=`   |Compound assignment   |
|`**=`   |Compound assignment   |
|`[:]`   |Slice type sigil      |
|`...`   |Variadic parameter    |
|`->`    |Tunnel target         |
|`::`    |Namespace resolution  |
|`==`    |Equality              |
|`!=`    |Inequality            |
|`<=`    |Less-or-equal         |
|`>=`    |Greater-or-equal      |
|`&&`    |Logical AND           |
|`||`    |Logical OR            |
|`+=`    |Compound assignment   |
|`-=`    |Compound assignment   |
|`*=`    |Compound assignment   |
|`/=`    |Compound assignment   |
|`%=`    |Compound assignment   |
|`<<`    |Left shift            |
|`>>`    |Right shift           |
|`{`     |Block open            |
|`}`     |Block close           |
|`(`     |Paren open            |
|`)`     |Paren close           |
|`[`     |Bracket open          |
|`]`     |Bracket close         |
|`+`     |Addition              |
|`-`     |Subtraction / negation|
|`*`     |Multiplication / deref|
|`/`     |Division              |
|`%`     |Modulo                |
|`=`     |Assignment            |
|`<`     |Less than             |
|`>`     |Greater than          |
|`;`     |Statement terminator  |
|`:`     |Case label / type sep |
|`&`     |Address-of / bitwise  |
|`!`     |Logical NOT           |
|`,`     |Separator             |
|`?`     |(reserved)            |
|`.`     |Field access          |

The lexer uses **maximal munch**: it always matches the longest possible operator at the current position.

-----

## 22. Grammar Summary (EBNF)

```ebnf
program          ::= top_level_item*

top_level_item   ::= import_stmt
                   | struct_def
                   | enum_def
                   | namespace_def
                   | function_def
                   | entry_def
                   | const_decl
                   | declaration
                   | call_stmt
                   | assignment

(* Imports *)
import_stmt      ::= 'import' string_literal ';'
                   | 'import' ns_path ';'
                   | 'import' type_expr identifier '(' c_param_list ')' ';'

ns_path          ::= identifier ( '::' identifier )*

c_param_list     ::= c_param ( ',' c_param )* ( ',' '...' )?
                   | 'voided'
                   |
c_param          ::= type_expr identifier?

(* Struct *)
struct_def       ::= 'struct' identifier '{' field* '}'
field            ::= type_expr identifier ';'

(* Enum *)
enum_def         ::= 'enum' identifier ( ':' type_expr )? '{' enum_body '}'
enum_body        ::= enum_value ( ',' enum_value )* ','?
enum_value       ::= identifier ( '=' expression )?

(* Namespace *)
namespace_def    ::= 'namespace' ns_path block

(* Functions *)
function_def     ::= 'def' identifier '(' param_list ')' block
param_list       ::= ( param ( ',' param )* )?
param            ::= type_expr identifier

entry_def        ::= 'entry' block

(* Types *)
type_expr        ::= base_type '*'* ( '[' expression? ':' ']' | '[' expression? ']' )?
base_type        ::= 'int8' | 'int16' | 'int32' | 'int64'
                   | 'uint8' | 'uint16' | 'uint32' | 'uint64'
                   | 'float32' | 'float64'
                   | 'bool' | 'char' | 'string' | 'voided'
                   | identifier

(* Statements *)
block            ::= '{' statement* '}'
statement        ::= declaration
                   | const_decl
                   | reserve_stmt
                   | tunnel_stmt
                   | move_stmt
                   | reset_stmt
                   | assignment
                   | call_stmt
                   | if_stmt
                   | while_stmt
                   | for_stmt
                   | foreach_stmt
                   | switch_stmt
                   | block
                   | expression ';'

declaration      ::= type_expr identifier ( '=' expression )? ';'
const_decl       ::= 'const' type_expr identifier '=' expression ';'
reserve_stmt     ::= 'reserve' ( '<' 'shared' '>' )? type_expr identifier
                     ( '=' expression )? ';'
tunnel_stmt      ::= 'tunnel' expression '->' type_expr identifier ';'
move_stmt        ::= 'move' identifier ';'
reset_stmt       ::= 'reset' ';'
call_stmt        ::= identifier '(' arg_list ')' ';'
arg_list         ::= ( expression ( ',' expression )* )?
assignment       ::= lvalue assign_op expression ';'
lvalue           ::= identifier ( '.' identifier )*
assign_op        ::= '=' | '+=' | '-=' | '*=' | '/='

if_stmt          ::= 'if' '(' expression ')' block
                     ( 'else' ( if_stmt | block ) )?

while_stmt       ::= 'while' '(' expression ')' block

for_stmt         ::= 'for' '(' declaration condition ';' ')' block

foreach_stmt     ::= 'foreach' '(' type_expr identifier ':' expression ')' block

break_stmt       ::= 'break' ';'

continue_stmt    ::= 'continue' ';'

switch_stmt      ::= 'switch' '(' expression ')' '{' switch_arm* '}'
switch_arm       ::= 'case' identifier ':' statement*
                   | 'default' ':' statement*

template_def     ::= 'template' '<' typename_param ( ',' typename_param )* '>'
                     ( struct_def | function_def )
typenaming_param ::= 'typename' identifier

(* Expressions — token stream; full precedence handled by codegen *)
expression       ::= token+

(* Raw strings *)
raw_string       ::= 'raw<until "' identifier '">' newline ... identifier
                   | 'raw<' integer '>' newline N_lines
```

-----

## Appendix A: Annotated Examples

### Hello World

```cll
import std;

entry
{
    puts("Hello, C<< world!");
}
```

### Fibonacci

```cll
import std;

// tunnels int32 result
def fib(int32 n)
{
    int32 a = 0;
    int32 b = 1;
    int32 i = 0;
    int32 tmp = 0;

    while (i < n)
    {
        tmp = b;
        b = a + b;
        a = tmp;
        i = i + 1;
    }

    tunnel a -> int32 result;
}

entry
{
    int32 i = 0;
    while (i < 16)
    {
        reserve int32 result = fib(i);
        printf("fib(%2d) = %d\n", i, result);
        i = i + 1;
    }
}
```

### Voided-State Guard

```cll
import std;

entry
{
    int32 x = 99;
    int32* p = &x;

    move x;   // x is now voided

    switch (p)
    {
        case valid
        {
            printf("value: %d\n", *p);
        }
        case voided
        }
            puts("p is voided");
        }
    }
}
```

### C-ABI Interop

```cll
import int32 puts(string s);
import int32 printf(string fmt, ...);
import voided* malloc(uint64 size);
import voided  free(voided* ptr);

entry
{
    voided* buf = malloc(128);
    printf("allocated %llu bytes\n", 128);
    free(buf);
}
```

### Struct with Method-Style Functions

```cll
struct Vec2
{
    float32 x;
    float32 y;
}

// tunnels float32 len_sq
def vec2_len_sq(Vec2* v)
{
    tunnel v.x * v.x + v.y * v.y -> float32 len_sq;
}

entry
{
    Vec2 pos;
    pos.x = 3.0;
    pos.y = 4.0;

    reserve float32 len_sq = vec2_len_sq(&pos);
    printf("len_sq = %f\n", len_sq);
}
```
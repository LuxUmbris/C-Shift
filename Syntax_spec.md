# C<< (C-Shift) Language Syntax Specification

> **Language version:** C-Shift 0.7 (2026)  
> **Source extension:** `.cll`  
> **Entry point:** `entry { … }`  
> **Paradigm:** Arena-based, VOP (Vertical Ownership Programming), C-ABI-compatible

---

## Table of Contents

1. [Lexical Structure](#1-lexical-structure)
2. [Primitive Types](#2-primitive-types)
3. [Type Expressions](#3-type-expressions)
4. [Top-Level Declarations](#4-top-level-declarations)
5. [The Entry Point](#5-the-entry-point)
6. [Functions and Tunnels](#6-functions-and-tunnels)
7. [Variables](#7-variables)
8. [The Voided State and `move`](#8-the-voided-state-and-move)
9. [Expressions and Operators](#9-expressions-and-operators)
10. [Statements and Control Flow](#10-statements-and-control-flow)
11. [Templates and Generic Types](#11-templates-and-generic-types)
12. [Structs](#12-structs)
13. [Enums](#13-enums)
14. [Namespaces](#14-namespaces)
15. [Arrays and Slices](#15-arrays-and-slices)
16. [Raw Strings](#16-raw-strings)
17. [Imports and C-ABI Interop](#17-imports-and-c-abi-interop)
18. [Arena Model and VOP Rules](#18-arena-model-and-vop-rules)
19. [Compile-Time Constants](#19-compile-time-constants)
20. [Reserved Words](#20-reserved-words)
21. [Complete Operator Table](#21-complete-operator-table)
22. [Grammar Summary (EBNF)](#22-grammar-summary-ebnf)

---

## 1. Lexical Structure

### 1.1 Comments

```cll
// single-line comment
/* block comment — nesting not supported */
```

### 1.2 Identifiers

```
identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers matching a keyword are reserved and cannot be used as names.

### 1.3 Integer and Float Literals

```
number ::= [0-9]+ | [0-9]+ '.' [0-9]+
```

### 1.4 String Literals

```
string_literal ::= '"' ( escape_char | [^"] )* '"'
```

Standard C escape sequences (`\n`, `\t`, `\0`, `\\`, `\"`) are supported.

### 1.5 Raw Strings

See [§16 Raw Strings](#16-raw-strings).

### 1.6 Whitespace

All whitespace (space, tab, CR, LF) is ignored between tokens.

---

## 2. Primitive Types

| Type      | Width  | Description                         |
|-----------|--------|-------------------------------------|
| `int8`    | 8 bit  | Signed integer                      |
| `int16`   | 16 bit | Signed integer                      |
| `int32`   | 32 bit | Signed integer                      |
| `int64`   | 64 bit | Signed integer                      |
| `uint8`   | 8 bit  | Unsigned integer                    |
| `uint16`  | 16 bit | Unsigned integer                    |
| `uint32`  | 32 bit | Unsigned integer                    |
| `uint64`  | 64 bit | Unsigned integer                    |
| `float32` | 32 bit | IEEE-754 single-precision float     |
| `float64` | 64 bit | IEEE-754 double-precision float     |
| `bool`    | 1 bit  | Boolean (`true` / `false`)          |
| `char`    | 8 bit  | Single character (unsigned byte)    |
| `string`  | ptr    | Pointer to null-terminated C string |
| `voided`  | —      | Absence of type (C-interop)         |

`string` lowers to `i8*`. `voided` lowers to `void`; `voided*` lowers to `i8*`.

---

## 3. Type Expressions

```
type_expr      ::= base_type pointer_suffix? array_suffix?
base_type      ::= primitive_type | identifier | identifier '<' type_args '>'
pointer_suffix ::= '*'+
array_suffix   ::= '[]'          // arena-bound dynamic array
                 | '[:]'         // non-owning slice
                 | '[' expr ']'  // sized array
type_args      ::= type_expr ( ',' type_expr )*
```

### Examples

```cll
int32            // plain int
int32*           // pointer to int32
uint8[:]         // slice of bytes (non-owning view)
float32[]        // arena-bound dynamic array
Vector<int32>    // generic container (heap-allocated, arena-managed)
voided*          // opaque pointer (void* in C)
```

---

## 4. Top-Level Declarations

A C<< source file is a flat sequence of top-level items processed in order. A forward-declaration pass runs before codegen, so definitions may appear in any order.

Top-level items:
- `import` (module or C-function or C-header)
- `export def` / `def` — function definition
- `struct` definition
- `enum` definition
- `namespace` block
- `entry` block (exactly one per program)
- `const` declaration
- `template<typename T>` generic definition

### `export def`

```cll
export def my_function(int32 x)
{
    tunnel x * 2 -> int32 result;
}
```

`export def` gives the function **external linkage** so it is callable from C or other languages. Plain `def` gets **internal linkage** (invisible to the linker — safe for dead-code elimination).

---

## 5. The Entry Point

Every executable C<< program must contain exactly one `entry` block. It compiles to C `main()`.

```cll
entry
{
    // program body
}
```

---

## 6. Functions and Tunnels

### 6.1 Function Definition

```
function_def ::= ['export'] 'def' identifier '(' parameter_list ')' block
parameter_list ::= ( type_expr identifier ( ',' type_expr identifier )* )?
```

Functions have **no return type annotation**. Output travels through `tunnel` statements.

### 6.2 Tunnel Statement

```
tunnel_stmt ::= 'tunnel' expression '->' type_expr identifier ';'
```

```cll
def add(int32 a, int32 b)
{
    tunnel a + b -> int32 result;
}
```

Rules:
- The expression before `->` is the output value.
- The `type identifier` after `->` names the target in the caller's scope.
- Multiple tunnels are allowed; each fills a matching `reserve`d variable.
- Tunneling a pointer type warns the VOP checker (pointer escapes its arena).

### 6.3 Calling a Function

```cll
// Classic: reserve first, then call
reserve int32 result;
add(5, 7);

// Inline: reserve + call in one line
reserve int32 result = add(5, 7);

// Type-inferred (single tunnel output):
reserve result = add(5, 7);
```

### 6.4 Multiple Tunnel Outputs

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
    compute(8, 4);  // fills both
}
```

### 6.5 Implicit Tunnel in Expressions

When a function has exactly one tunnel output it may be used inline:

```cll
printf("%d\n", add(4, 8));  // add's tunnel value is passed directly
```

---

## 7. Variables

### 7.1 Plain Declaration

```
declaration ::= type_expr identifier ( '=' expression )? ';'
```

```cll
int32 x;
int32 y = 42;
string name = "Alice";
Vector<int32> v = vec_new(16);   // generic type — heap-allocated, arena-managed
```

Variables are owned by the current arena (scope block). Heap-allocated variables (e.g. `Vector<T>`, `T[]`) are freed automatically when their arena exits via a single bulk-free operation — not RAII.

### 7.2 Reserve

```
reserve_stmt ::= 'reserve' ('<' 'shared' '>')? type_expr identifier ( '=' expression )? ';'
```

`reserve` declares a variable that receives a `tunnel` value from an upcoming call. It lives in the current scope and survives the called function.

`reserve<shared>` is read-only after initialization:

```cll
reserve<shared> int32 config = load_config();
// config = 5;  // checker error
```

### 7.3 Constants

```
const_decl ::= 'const' type_expr identifier '=' expression ';'
```

```cll
const int32 MAX = 1024;
const float64 PI = 3.14159265358979;
```

Immutable; reassignment is a checker error. Type mismatches in the initializer are also caught by the checker.

### 7.4 Assignment

```
assignment ::= lvalue assign_op expression ';'
assign_op  ::= '=' | '+=' | '-=' | '*=' | '/='
```

---

## 8. The Voided State and `move`

### 8.1 Concept

Every variable tracks whether it holds a **valid** value or has been **moved** (voided). This is a compile-time-tracked property with optional runtime support when the state cannot be determined statically.

No null pointers — instead, a moved variable is explicitly voided, and access is guarded by a `switch` block.

### 8.2 `move`

```
move_stmt ::= 'move' identifier ';'
```

Transitions a variable to the voided state. Accessing a voided variable without a guard is a compile-time error.

```cll
int32 x = 10;
move x;
// using x here is a checker error
```

### 8.3 `switch` Guard for Voided State

```cll
switch (p)
{
    case valid:
        printf("%d\n", *p);
    case voided:
        puts("was moved");
}
```

The compiler distinguishes three situations:

| Situation | What happens |
|-----------|-------------|
| **Statically voided** — `move` always reached before switch | Checker sets `meta = "voided"` → codegen emits only the `case voided` body, no branch at all |
| **Statically valid** — `move` never reached | Checker sets `meta = "valid"` → codegen emits only the `case valid` body, no branch at all |
| **Conditionally voided** — `move` inside an `if`/`while` branch | Checker sets `meta = "unknown"` → codegen emits a hidden `__track_validity_<name>` bool flag; `move` stores `false` into it; the switch does a runtime `cond_br` |

The hidden flag has zero overhead in the static cases. In the conditional case the overhead is exactly one i1 alloca + one store per `move` + one load + branch at the guard.

### 8.4 `reset`

```
reset_stmt ::= 'reset' ';'
```

Frees all heap-allocated data tracked by the current scope's arena, **without** exiting the scope. Variables remain declared and can receive new values afterwards. Forbidden if any child scope holds pointers into the current arena.

---

## 9. Expressions and Operators

### 9.1 Arithmetic

| Operator | Meaning        |
|----------|----------------|
| `+`      | Addition       |
| `-`      | Subtraction    |
| `*`      | Multiplication |
| `/`      | Division       |
| `%`      | Modulo         |

### 9.2 Comparison

`==` `!=` `<` `>` `<=` `>=`

### 9.3 Logical

`&&` `||` `!`

### 9.4 Bitwise

`&` `<<` `>>`

### 9.5 Compound Assignment

`+= -= *= /= %= <<= >>= **=`

### 9.6 Pointer and Address

| Operator | Meaning              |
|----------|----------------------|
| `&`      | Address-of           |
| `*`      | Dereference          |
| `->`     | Tunnel target arrow  |

For heap-allocated types (`Vector<T>`, etc.), `&v` returns the stored heap pointer directly, not the address of the local slot.

### 9.7 Field Access

```
expr '.' identifier
```

### 9.8 Boolean Literals

`true` `false`

### 9.9 Namespace Resolution

```
identifier '::' identifier
```

### 9.10 Array Length

```cll
uint64 len = arr[[:]] ;
```

`arr[[:]]` returns the current element count of an arena-bound array `arr`.

---

## 10. Statements and Control Flow

### 10.1 `if` / `else`

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

Each iteration body is its own sub-arena.

```cll
while (i < 10)
{
    i += 1;
}
```

### 10.3 `for`

```cll
for (int32 i = 0; i < 5;)
{
    printf("%d\n", i);
    i += 1;
}
```

Each iteration body is its own sub-arena.

### 10.4 `foreach`

```cll
foreach (int32 val : my_array)
{
    printf("%d\n", val);
}
```

### 10.5 `break` and `continue`

`break` exits the innermost loop or switch. `continue` restarts the next iteration (not allowed inside switch).

### 10.6 `switch` / `case` / `default`

```cll
switch (status)
{
    case Active:
        puts("active");
    case Inactive:
        puts("inactive");
    default:
        puts("unknown");
}
```

No fallthrough. Each case body ends at the next `case`, `default`, or `}`.

**Voided-state guard form** — see [§8.3](#83-switch-guard-for-voided-state).

### 10.7 Anonymous Blocks (Sub-Arenas)

A bare `{ … }` block creates a new arena. All variables declared inside — including heap-allocated ones — are freed in bulk when the block exits.

```cll
{
    Vector<int32> tmp = vec_new(8);
    vec_push(&tmp, 42);
    // tmp is freed here automatically — one arena_free_all() call
}
```

---

## 11. Templates and Generic Types

```
template_def ::= 'template' '<' typename_param ( ',' typename_param )* '>'
                 ( struct_def | function_def )
typename_param ::= 'typename' identifier
```

```cll
template<typename T>
struct Pair
{
    T first;
    T second;
}

template<typename T>
def pair_sum(Pair<T>* p)
{
    tunnel p.first + p.second -> T result;
}
```

Template instantiation is monomorphic at compile time.

### Generic Container Types

The standard library provides heap-allocated generic containers. All are managed by the arena — you do **not** call `vec_free` etc. manually; the scope arena handles it.

| Type | Constructor | Description |
|------|-------------|-------------|
| `Vector<T>` | `vec_new(chunk_size)` | Dynamic array |
| `HashMap<K,V>` | `map_new()` | Hash map |
| `SortedVec<T>` | `svec_new(cmp)` | Sorted vector |
| `StringBuilder` | `sb_new()` | String builder |

```cll
import std;

entry
{
    Vector<int32> v = vec_new(16);
    vec_push(&v, 10);
    vec_push(&v, 20);

    uint64 len = vec_len(&v);   // 2
    int32 x = vec_get(&v, 0);   // 10

    // v is freed automatically when the scope exits
}
```

---

## 12. Structs

Structs are data-only (no methods).

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
```

---

## 13. Enums

Integer-backed; default backing type is `int32`.

```
enum_def   ::= 'enum' identifier ( ':' type_expr )? '{' enum_body '}'
enum_body  ::= enum_value ( ',' enum_value )* ','?
enum_value ::= identifier ( '=' expression )?
```

```cll
enum Direction { North, South, East, West }
enum ErrorCode : int32 { Ok = 0, NotFound = 404 }
```

---

## 14. Namespaces

Lexical grouping only — no arena boundary.

```cll
namespace Math
{
    const float64 PI = 3.14159265358979;
}
```

Nested namespace path in one statement:

```cll
namespace Engine::Physics { … }
```

---

## 15. Arrays and Slices

### 15.1 Arena-Bound Array (`T[]`)

Heap-allocated, owned by the current scope. Grows dynamically via `<<`. Freed in bulk when the scope's arena is released.

```cll
int32[] nums;
nums << 10;
nums << 20;
nums << 30;
uint64 len = nums[[:]] ;   // 3
int32 x = nums[1];         // 20
```

### 15.2 Sized Array (`T[N]`)

Stack-allocated, fixed size.

```cll
float32[16] matrix;
uint8[256] buffer;
```

### 15.3 Non-Owning Slice (`T[:]`)

A fat pointer (base + length). Does not own memory. VOP law: may only reference arenas that outlive the slice.

```cll
int32[:] view;
```

---

## 16. Raw Strings

### 16.1 Delimiter form

```cll
string banner = raw<until "END">
###########
# Hello   #
###########
END
```

### 16.2 Line-count form

```cll
puts(raw<3>
Line one \n is literal
Line two \t is literal
Line three
);
```

---

## 17. Imports and C-ABI Interop

### 17.1 Module Import

```cll
import std;
import io::file;
```

### 17.2 File Import

```cll
import "path/to/module.cll";
```

### 17.3 C-Header Import

```cll
import <raylib.h>;
import "mylib.h";
```

Uses libclang to parse the header and import all visible function declarations. Pass `-I<path>` to the compiler to add header search directories.

### 17.4 C-Function Import

```cll
import int32  printf(string fmt, ...);
import voided free(voided* ptr);
import voided* malloc(uint64 size);
```

`voided` = C `void`. Variadic functions use `...`.

### 17.5 `export def`

```cll
export def my_fn(int32 x)
{
    tunnel x * 2 -> int32 result;
}
```

Gives the function external C-ABI linkage so it can be called from C or linked into a shared library. Plain `def` gets internal linkage.

---

## 18. Arena Model and VOP Rules

C<< uses **Vertical Ownership Programming (VOP)** with a scope-arena memory model. The core rules:

### 18.1 Arena = Scope

Every `{…}` block that is a control-flow body creates a **scope arena**. When the block exits, **all** heap allocations registered with that arena are freed in one operation (`cshift_arena_free_all`). This is not RAII — there are no individual destructors, no drop order concerns. It is a single bulk free at the end of the scope.

The arena is **lazy**: if no heap allocations occur in a scope, no arena struct is allocated and the scope exit has zero overhead.

### 18.2 What Goes Into an Arena

- `T[]` arena-bound arrays (each `realloc` is tracked)
- `Vector<T>`, `HashMap<K,V>`, and all other generic container types (the heap pointer from `vec_new` etc. is tracked)

### 18.3 `reset`

`reset;` frees all currently tracked heap data in the current scope's arena but does not exit the scope. Variables are still declared and may receive new values. The arena struct is kept alive for re-use.

```cll
int32[] buf;
buf << 1;
buf << 2;
reset;          // buf data freed, len reset to 0
buf << 99;      // safe to reuse
```

### 18.4 No Raw Returns (Tunnel Law)

Functions output values via `tunnel`, not return statements. A tunnel may not transfer pointers into arenas that will be destroyed before the call site. The checker emits a warning when a pointer type is tunneled out.

### 18.5 Depth Law

A pointer must only point to a variable at arena depth ≤ the pointer's own depth. Depth is the nesting level of scope blocks at the point of declaration.

### 18.6 Voided-State Law

A `move`d variable is voided. Accessing it without a `switch(var) { case valid: … case voided: … }` guard is a compile-time error.

When the compiler cannot determine statically whether a variable is voided (e.g. `move` inside an `if` branch), it inserts a hidden runtime `__track_validity_<name>` boolean. This flag starts `true`, is set to `false` by `move`, and is tested by the switch guard. The flag name uses the `__track_validity_` prefix to avoid collisions with user-defined names.

---

## 19. Compile-Time Constants

```cll
const int32 SCREEN_WIDTH  = 1920;
const int32 SCREEN_HEIGHT = 1080;
const float64 TAU = 6.28318530717958;
```

Immutable after declaration. Type mismatches in the initializer are a checker error:

```cll
const int32 x = "hello";   // error: string literal assigned to int32
const int32 y = 3.14;      // error: float literal assigned to integer const
```

---

## 20. Reserved Words

```
bool       break      case       char       const      continue
default    def        else       enum       entry      export
false      float32    float64    for        foreach    if
import     int8       int16      int32      int64      move
namespace  raw        reserve    reset      string     struct
switch     template   true       tunnel     typename   uint8
uint16     uint32     uint64     valid      voided     while
```

---

## 21. Complete Operator Table

| Operator | Category               |
|----------|------------------------|
| `<<=`    | Compound assignment    |
| `>>=`    | Compound assignment    |
| `**=`    | Compound assignment    |
| `[:]`    | Slice type sigil       |
| `...`    | Variadic parameter     |
| `->`     | Tunnel target          |
| `::`     | Namespace resolution   |
| `==`     | Equality               |
| `!=`     | Inequality             |
| `<=`     | Less-or-equal          |
| `>=`     | Greater-or-equal       |
| `&&`     | Logical AND            |
| `\|\|`   | Logical OR             |
| `+=`     | Compound assignment    |
| `-=`     | Compound assignment    |
| `*=`     | Compound assignment    |
| `/=`     | Compound assignment    |
| `%=`     | Compound assignment    |
| `<<`     | Array append / shift   |
| `>>`     | Right shift            |
| `{` `}`  | Block delimiters       |
| `(` `)`  | Paren delimiters       |
| `[` `]`  | Bracket delimiters     |
| `+`      | Addition               |
| `-`      | Subtraction / negation |
| `*`      | Multiplication / deref |
| `/`      | Division               |
| `%`      | Modulo                 |
| `=`      | Assignment             |
| `<`      | Less than              |
| `>`      | Greater than           |
| `;`      | Statement terminator   |
| `:`      | Case label / type sep  |
| `&`      | Address-of / bitwise   |
| `!`      | Logical NOT            |
| `,`      | Separator              |
| `.`      | Field access           |

Lexer uses **maximal munch**.

---

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
                   | template_def
                   | declaration

(* Imports *)
import_stmt      ::= 'import' string_literal ';'
                   | 'import' '<' identifier '.' identifier '>' ';'
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
function_def     ::= ( 'export' )? 'def' identifier '(' param_list ')' block
param_list       ::= ( param ( ',' param )* )?
param            ::= type_expr identifier

entry_def        ::= 'entry' block

(* Templates *)
template_def     ::= 'template' '<' typename_param ( ',' typename_param )* '>'
                     ( struct_def | function_def )
typename_param   ::= 'typename' identifier

(* Types *)
type_expr        ::= base_type '*'* ( '[' expr? ':' ']' | '[' expr? ']' )?
base_type        ::= 'int8' | 'int16' | 'int32' | 'int64'
                   | 'uint8' | 'uint16' | 'uint32' | 'uint64'
                   | 'float32' | 'float64'
                   | 'bool' | 'char' | 'string' | 'voided'
                   | identifier
                   | identifier '<' type_args '>'
type_args        ::= type_expr ( ',' type_expr )*

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
for_stmt         ::= 'for' '(' declaration expression ';' ')' block
foreach_stmt     ::= 'foreach' '(' type_expr identifier ':' expression ')' block
break_stmt       ::= 'break' ';'
continue_stmt    ::= 'continue' ';'
switch_stmt      ::= 'switch' '(' expression ')' '{' switch_arm* '}'
switch_arm       ::= 'case' identifier ':' statement*
                   | 'default' ':' statement*

expression       ::= token+   (* full precedence handled by codegen *)

raw_string       ::= 'raw<until "' identifier '">' newline ... identifier
                   | 'raw<' integer '>' newline N_lines
```

---

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

### Arena-Managed Vector

```cll
import std;

entry
{
    Vector<int32> v = vec_new(16);
    vec_push(&v, 10);
    vec_push(&v, 20);
    vec_push(&v, 30);

    uint64 len = vec_len(&v);
    printf("len=%llu\n", len);

    int32 i = 0;
    while (i < 3)
    {
        printf("v[%d]=%d\n", i, vec_get(&v, i));
        i += 1;
    }

    // v freed automatically at scope exit — no vec_free needed
}
```

### Sub-Arena and `reset`

```cll
import std;

entry
{
    int32[] buf;

    int32 pass = 0;
    while (pass < 3)
    {
        buf << pass * 10;
        buf << pass * 10 + 1;
        printf("pass %d: len=%llu\n", pass, buf[[:]] );
        reset;          // free buf data, keep scope
        pass += 1;
    }
}
```

### Voided-State — Static (Zero Cost)

```cll
import std;

entry
{
    int32 x = 99;
    int32* p = &x;

    move x;   // x is definitely voided here

    // Compiler emits only the case voided body — no runtime branch
    switch (p)
    {
        case valid:
            printf("value: %d\n", *p);
        case voided:
            puts("x was moved");
    }
}
```

### Voided-State — Runtime (Conditional Move)

```cll
import std;

entry
{
    int32 x = 42;
    int32 cond = 1;

    if (cond > 0)
    {
        move x;   // only on one path
    }

    // Compiler inserts __track_validity_x bool; switch does runtime cond_br
    switch (x)
    {
        case valid:
            printf("x is still valid: %d\n", x);
        case voided:
            printf("x was moved\n");
    }
}
```

### C-ABI Interop with Raylib

```cll
import "raylib.h";
import "raylib_wrap.h";  // flat wrappers for struct-arg functions

entry
{
    InitWindow(800, 450, "Hello from C<<!");

    while (WindowShouldClose() == false)
    {
        BeginDrawing();
        ClearBg(30, 30, 46, 255);
        DrawTxt("Hello from C<<!", 220, 190, 30, 205, 214, 244, 255);
        EndDrawing();
    }

    CloseWindow();
}
```

### Export for C Interop

```cll
export def add(int32 a, int32 b)
{
    tunnel a + b -> int32 result;
}

// Callable from C as: extern void add(int32 a, int32 b, int32* result);
```

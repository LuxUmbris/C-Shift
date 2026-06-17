# C<< (C-Shift) Language Specification

> **Version:** 0.8 (2026)  
> **Extension:** `.cll`  
> **Entry point:** `entry { … }`  
> **Paradigm:** Arena-scoped, VOP (Vertical Ownership Programming), C-ABI-compatible

---

## Table of Contents

1. [Lexical Structure](#1-lexical-structure)
2. [Types](#2-types)
3. [Top-Level Items](#3-top-level-items)
4. [Entry Point](#4-entry-point)
5. [Functions and Tunnels](#5-functions-and-tunnels)
   - [5a. Forward Declarations (`dec`)](#5a-forward-declarations-dec)
   - [5b. Extended `reserve` — Tunnel Binding (`<<`)](#5b-extended-reserve--explicit-tunnel-binding-)
   - [5c. Zero-Cost Classes (`class`)](#5c-zero-cost-classes-class)
6. [Variables and Constants](#6-variables-and-constants)
7. [The Voided State and `move`](#7-the-voided-state-and-move)
8. [Expressions](#8-expressions)
9. [Control Flow](#9-control-flow)
10. [Arena Model and `reset`](#10-arena-model-and-reset)
11. [Arrays](#11-arrays)
12. [Generic Container Types](#12-generic-container-types)
13. [Method Syntax](#13-method-syntax)
14. [Structs and Enums](#14-structs-and-enums)
15. [Templates](#15-templates)
16. [Namespaces](#16-namespaces)
17. [Imports and C-ABI Interop](#17-imports-and-c-abi-interop)
18. [Standard Library (`std`)](#18-standard-library-std)
19. [Raylib Integration](#19-raylib-integration)
20. [Export](#20-export)
21. [Raw Strings](#21-raw-strings)
22. [Reserved Words](#22-reserved-words)
23. [Operator Table](#23-operator-table)
24. [Grammar (EBNF)](#24-grammar-ebnf)
25. [Annotated Examples](#25-annotated-examples)

---

## 1. Lexical Structure

### Comments
```cll
// single-line
/* block comment */
```

### Identifiers
```
[a-zA-Z_][a-zA-Z0-9_]*
```

### Literals
| Kind | Examples |
|------|----------|
| Integer | `0`, `42`, `0xFF` |
| Float | `3.14`, `1.0e-5` |
| String | `"hello\nworld"` (C escape sequences) |
| Bool | `true`, `false` |
| Raw string | see §21 |

---

## 2. Types

### Primitive types

| Type | Width | Notes |
|------|-------|-------|
| `int8` | 8 bit | signed |
| `int16` | 16 bit | signed |
| `int32` | 32 bit | signed |
| `int64` | 64 bit | signed |
| `uint8` | 8 bit | unsigned |
| `uint16` | 16 bit | unsigned |
| `uint32` | 32 bit | unsigned |
| `uint64` | 64 bit | unsigned |
| `float32` | 32 bit | IEEE-754 |
| `float64` | 64 bit | IEEE-754 double |
| `bool` | 1 bit | `true` / `false` |
| `char` | 8 bit | unsigned byte |
| `string` | ptr | null-terminated `char*` (C-ABI compatible) |
| `voided` | — | C `void`; `voided*` = opaque pointer |

### Pointer types
```cll
int32*      // pointer to int32
voided*     // void pointer (C-ABI opaque)
string      // already a pointer (char*)
```

### Array / slice types
```cll
int32[]     // arena-bound dynamic array (§11)
int32[:]    // non-owning slice
int32[16]   // fixed-size stack array
```

### Generic types
```cll
Vector<int32>
HashMap<string, int32>
```
Template params are stripped at compile time; the base name must match a known struct.

### Automatic numeric coercion

When a value of one numeric type is assigned to a different numeric type, the compiler automatically coerces it:

- **Widening** (e.g. `int32 → int64`) — always safe, emits `sext`/`fpext`
- **Narrowing** (e.g. `int64 → int32`) — emits `trunc`, produces a checker warning
- **Float ↔ int** — emits `sitofp` / `fptosi`, produces a checker warning
- **Incompatible types** — `cannot cast 'T' to 'U'` error

```cll
int32 len = strlen("hi");  // strlen returns uint64 → auto-truncated to int32
```

---

## 3. Top-Level Items

A `.cll` file is a flat list of top-level items (order doesn't matter — a forward-declaration pass runs first):

```
program ::= (import | struct | enum | namespace | def | export def
            | entry | const | template)*
```

---

## 4. Entry Point

Every executable has exactly one `entry` block, which compiles to C `main()`:

```cll
entry
{
    puts("Hello, C<< world!");
}
```

---

## 5. Functions and Tunnels

### Definition

```cll
def add(int32 a, int32 b)
{
    tunnel a + b -> int32 result;
}
```

Functions have **no return type**. Values leave through `tunnel` statements. This enforces a single clear data-flow direction.

### Tunnel statement

```
tunnel expression -> type identifier ;
```

The expression is computed and stored into the caller's `reserve`d slot.

### Calling a function

```cll
// reserve first, then call fills it
reserve int32 result;
add(3, 4);

// inline: reserve + call
reserve int32 result = add(3, 4);

// type-inferred (single tunnel output)
reserve result = add(3, 4);
```

### Multiple tunnel outputs

```cll
def divide(int32 a, int32 b)
{
    tunnel a / b -> int32 quotient;
    tunnel a % b -> int32 remainder;
}

entry
{
    reserve int32 quotient;
    reserve int32 remainder;
    divide(17, 5);
    printf("%d r %d\n", quotient, remainder);
}
```

### Inline usage

When a function has exactly one tunnel output it can be used directly as an expression:

```cll
printf("%d\n", add(4, 8));
```

---

---

## 5a. Forward Declarations (`dec`)

A function defined later in the file (or mutually recursive with another
function) can be **forward-declared** with `dec`. This registers the
function's signature — including its `tunnel` output names and types — so
calls before the `def` appears type-check correctly.

```
dec name(params) [-> type t1, type t2, ...];
```

```cll
dec is_even(int32 n) -> int32 result;
dec is_odd(int32 n)  -> int32 result;

def is_even(int32 n)
{
    if (n == 0)
    {
        tunnel 1 -> int32 result;
    }
    else
    {
        reserve int32 r = is_odd(n - 1);
        tunnel r -> int32 result;
    }
}

def is_odd(int32 n)
{
    if (n == 0)
    {
        tunnel 0 -> int32 result;
    }
    else
    {
        reserve int32 r = is_even(n - 1);
        tunnel r -> int32 result;
    }
}
```

### Rules

- The parameter list and tunnel signature of `dec` must match the later `def`. (The compiler does not currently cross-check this exhaustively — mismatches may surface as link-time or runtime errors.)
- If no tunnel outputs are declared (`dec name(params);`), the function must not contain `tunnel` statements with named outputs that callers rely on before the `def` is seen.
- `dec` with no matching `def` anywhere in the program leaves an external declaration — useful for linking against functions defined in other translation units or C code (combined with `export def` on the defining side).
- `export` may be combined: `export dec name(params) -> type t;` is accepted by the parser but linkage is determined by the `def`'s own `export` modifier.

---

## 5b. Extended `reserve` — Explicit Tunnel Binding (`<<`)

When a function has **multiple** `tunnel` outputs, a plain `reserve` matches
by name (the reserve's variable name must equal the tunnel's target name) or
by type (if exactly one tunnel of that type exists). The `<<` syntax lets you
**explicitly** bind a reserve slot to a specific named tunnel output,
regardless of the reserve variable's own name:

```
reserve [type] target_name << tunnel_name [= func_name(args);]
```

```cll
def compute(int32 x)
{
    tunnel x * 2 -> int32 doubled;
    tunnel x * 3 -> int32 tripled;
}

entry
{
    // Bind to "tripled" explicitly — works even though the local
    // variable is named "t", not "tripled"
    reserve int32 t << tripled = compute(10);
    printf("tripled = %d\n", t);   // 30

    // Type can also be inferred when binding explicitly
    reserve r << doubled = compute(5);
    printf("doubled = %d\n", r);   // 10
}
```

### Rules

- `tunnel_name` must be the name of one of the tunnel outputs declared (via `tunnel expr -> type tunnel_name;` in the body, or via `dec ... -> type tunnel_name;`) by the called function — checked at compile time:
  ```
  [CHECKER ERROR] reserve: 'compute' has no tunnel output named 'nonexistent'
  ```
- Without `<<`, normal name/type matching rules apply (§5).
- With `<<` and **no explicit type** (`reserve name << tunnel_name = call();`), the type is inferred from the bound tunnel's declared type — even when the function has multiple tunnels (the usual "multiple tunnels — specify the type explicitly" error does not apply when `<<` disambiguates).
- Works with both plain function calls and method calls (`reserve r << result = obj.method();`).

---

## 5c. Zero-Cost Classes (`class`)

`class` is sugar for a `struct` plus free functions that take a pointer to
that struct as their first parameter (`self`). There is no vtable, no
inheritance, and no runtime overhead — `obj.method(args)` compiles to a
direct call `ClassName_method(&obj, args)`.

```
class Name
{
    type field1;
    type field2;

    def method1(params)
    {
        // self.field1, self.field2 ...
    }

    def method2(params) -> type tname
    {
        tunnel expr -> type tname;
    }
}
```

### Desugaring

```cll
class Player
{
    float32 x;
    float32 y;
    int32 health;

    def take_damage(int32 amount)
    {
        self.health -= amount;
    }

    def is_alive() -> int32 alive
    {
        if (self.health > 0)
        {
            tunnel 1 -> int32 alive;
        }
        else
        {
            tunnel 0 -> int32 alive;
        }
    }
}
```

is equivalent to:

```cll
struct Player
{
    float32 x;
    float32 y;
    int32 health;
}

def Player_take_damage(Player* self, int32 amount)
{
    self.health -= amount;
}

def Player_is_alive(Player* self) -> int32 alive
{
    if (self.health > 0)
    {
        tunnel 1 -> int32 alive;
    }
    else
    {
        tunnel 0 -> int32 alive;
    }
}
```

### Usage

```cll
entry
{
    Player p;
    p.x = 0.0;
    p.y = 0.0;
    p.health = 100;

    p.take_damage(30);

    reserve int32 alive = p.is_alive();
    printf("health=%d alive=%d\n", p.health, alive);  // health=70 alive=1
}
```

`p.take_damage(30)` desugars to `Player_take_damage(&p, 30)`. `self` inside a
method has type `Player*`, so `self.field` uses the same auto-dereferencing
field-access codegen as any other pointer-to-struct field access (§8).

### Rules

- Field declarations come first, methods after (order within each group is preserved).
- A method may declare `tunnel` outputs exactly like a top-level `def` — including the `-> type tname` signature form, and `reserve r = obj.method()` / `reserve r << tname = obj.method()` both work (§5b).
- A method name cannot be a reserved keyword (e.g. `move`, `reset`) — this is the same restriction as for any identifier.
- `class` instances live on the stack (or in their enclosing arena) like any `struct` — there is no separate heap allocation or `class_free`. Arena/VOP rules (§10) apply identically: a `Player` declared inside a sub-scope is destroyed when that scope exits, same as any other stack value.
- Classes do not support inheritance, virtual methods, constructors, or destructors. Initialize fields manually after declaration, as shown above.

## 6. Variables and Constants

### Declaration

```cll
int32 x = 42;
float32 pi = 3.14159;
string name = "Alice";
Vector<int32> v = vec_new(16);   // arena-managed, freed automatically
```

### Reserve (tunnel target)

```cll
reserve int32 result = some_fn(x);
reserve<shared> int32 config = load_config();  // read-only after first fill
```

### Constants

```cll
const int32 MAX = 1024;
const float64 TAU = 6.283185;
```

The checker enforces correct types for constant initialisers:
```cll
const int32 x = "hello";  // [ERROR] Type mismatch: string assigned to int32
const int32 y = 3.14;     // [ERROR] Float literal assigned to integer const
```

---

## 7. The Voided State and `move`

### Concept

Every variable is either **valid** or **voided**. A voided variable has had ownership transferred via `move`. Using a voided variable without a guard is a compile-time error.

### `move`

```cll
int32 x = 42;
move x;          // x is now voided
printf("%d", x); // [ERROR] Use of voided variable 'x'
```

### `switch` guard

```cll
switch (x)
{
    case valid:
        printf("x = %d\n", x);
    case voided:
        puts("x was moved");
}
```

### Three resolution modes

| Situation | What the compiler does |
|-----------|----------------------|
| `move` always reached before `switch` | **Static** — emits only the `case voided` body. Zero runtime cost. |
| `move` never reached | **Static** — emits only the `case valid` body. Zero runtime cost. |
| `move` inside an `if`/`while` | **Runtime** — emits a hidden `__track_validity_<name>` bool flag. `move` sets it `false`. The switch does a `cond_br` on it. |

The `__track_validity_` prefix is reserved and cannot be used in user code.

---

## 8. Expressions

### Arithmetic
`+` `-` `*` `/` `%`

### Comparison
`==` `!=` `<` `>` `<=` `>=`

### Logical
`&&` `||` `!`

### Bitwise / shift
`&` `|` `<<` `>>`

### Compound assignment
`+=` `-=` `*=` `/=` `%=` `<<=` `>>=`

### Address-of and dereference
```cll
int32* p = &x;   // address of x
int32 v = *p;    // dereference
```

For **managed container types** (`Vector<T>` etc.), `&v` returns the stored heap pointer directly (not the address of the local slot), so `&v` can be passed to C functions expecting a `Vector*`.

### Array length
```cll
uint64 len = arr[[:]] ;   // length of a T[] arena array
```

Also available as method syntax (see §13):
```cll
uint64 len = arr.len();
```

### Array element access and assignment
```cll
int32 x = arr[i];    // read
arr[i] = 99;         // write
arr[i] += 1;         // compound assign
```

### Field access
```cll
point.x
player.position.y
```

### Namespace resolution
```cll
Math::PI
Engine::Physics::gravity
```

### Mixed-type arithmetic

Numeric types are automatically promoted before binary operations:
- `float32 * int32` → both promoted to `float64`, result truncated back to `float32` if hint says so
- `int32 < uint64` → `int32` widened to `uint64`

---

## 9. Control Flow

### `if` / `else`

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

**Constant folding:** conditions that are compile-time constants (`1 == 1`, `0 != 0`, etc.) cause the dead branch to be completely omitted from IR — no runtime overhead.

### `while`

```cll
while (i < 10)
{
    i += 1;
}
```

### `for`

```cll
for (int32 i = 0; i < n;)
{
    printf("%d\n", i);
    i += 1;
}
```

### `foreach`

```cll
foreach (int32 val : arr)
{
    printf("%d\n", val);
}
```

### `switch` / `case` / `default`

```cll
switch (status)
{
    case 0:
        puts("ok");
    case 1:
        puts("error");
    default:
        puts("unknown");
}
```

No fallthrough. Voided-state guard form: `case valid:` / `case voided:` — see §7.

### `break` / `continue`

Standard loop control. `break` also exits a `switch`.

---

## 10. Arena Model and `reset`

### Arena = Scope

Every `{…}` block is a **scope arena**. All heap allocations in that scope — arena arrays (`T[]`), `Vector<T>`, `HashMap<K,V>`, etc. — are tracked by a `cshift_arena_t` struct. On scope exit, **one** call to `__cshift_arena_free_all()` releases everything. There are no individual destructors.

The arena struct is created lazily: if a scope makes no heap allocations, it costs zero.

### `reset`

```cll
reset;
```

Frees all heap data tracked by the current scope's arena **without exiting the scope**. Variables remain declared and can receive new values.

```cll
int32[] buf;
buf << 1;
buf << 2;
reset;          // buf data freed, len = 0
buf << 99;      // safe to reuse
```

### Sub-arenas

Every nested `{…}` block gets its own arena. Data allocated in a sub-block is freed when that block exits — without affecting the parent scope.

```cll
{
    Vector<int32> tmp = vec_new(8);
    tmp.push(42);
    // tmp freed here automatically
}
// parent scope unaffected
```

### VOP Depth Law

A pointer must only point to a variable at arena depth ≤ the pointer's own depth. Tunneling a pointer out of a function warns the checker (pointer escapes its arena).

---

## 11. Arrays

### Arena-bound (`T[]`)

Dynamic, heap-allocated, owned by the current scope arena:

```cll
int32[] nums;
nums << 10;                  // append
nums << 20;
nums << 30;
uint64 len = nums[[:]] ;     // length (or: nums.len())
int32 x = nums[1];           // read
nums[1] = 99;                // write
nums[1] += 5;                // compound assign
```

Multiple element types work:
```cll
string[] words;
words << "hello";
words << "world";
printf("%s %s\n", words[0], words[1]);
```

### Fixed-size (`T[N]`)

Stack-allocated:
```cll
float32[16] matrix;
uint8[256] buf;
```

### Non-owning slice (`T[:]`)

Fat pointer (base + length), does not own memory:
```cll
int32[:] view;
```

---

## 12. Generic Container Types

All containers are **arena-managed** — you never call `vec_free` etc. manually. The scope arena frees them automatically.

### `Vector<T>`

```cll
Vector<int32> v = vec_new(16);
v.push(10);
v.push(20);
int32 x = v.get(0);        // 10
uint64 len = v.len();      // 2
v.set(0, 99);
v.remove(0);
int32 contains = v.contains(99); // 0 after remove
```

### `HashMap<K, V>`

```cll
HashMap<string, int32> scores = map_new();
scores.set("alice", 100);
scores.set("bob",   80);
int32 out = 0;
scores.get("alice", &out);   // out = 100
int32 has = scores.has("charlie"); // 0
```

### `StringBuilder`

```cll
StringBuilder sb = sb_new();
sb.append("Hello, ");
sb.append_int(42);
sb.append("!");
string result = sb.build();   // arena-tracked automatically
printf("%s\n", result);
```

### `SortedVec<T>`

```cll
SortedVec<int32> sv = svec_new(cmp_int32);
sv.push(30);
sv.push(10);
sv.push(20);
int32 first = sv.get(0);   // 10 (sorted)
```

### `LinkedList<T>`, `Set<T>`, `BitSet`, `Deque<T>`

Same method-call pattern.

---

## 13. Method Syntax

Container types and arena arrays support dot-method calls instead of free function calls:

```cll
v.push(x)          // same as vec_push(&v, x)
v.get(i)           // same as vec_get(&v, i)
v.len()            // same as vec_len(&v)
arr.len()          // same as arr[[:]]
sb.append("hi")    // same as sb_append(&sb, "hi")
```

Method calls work in **statement** and **expression** position:

```cll
if (v.len() > 0)
{
    printf("first=%d last=%d\n", v.get(0), v.get(v.len() - 1));
}
```

### Full method table

| Type | Method | Equivalent |
|------|--------|-----------|
| `Vector<T>` | `.push(x)` `.get(i)` `.set(i,x)` `.len()` `.pop()` `.remove(i)` `.contains(x)` `.clear()` | `vec_*` |
| `HashMap<K,V>` | `.set(k,v)` `.get(k,&out)` `.has(k)` `.remove(k)` `.len()` `.clear()` | `map_*` |
| `SortedVec<T>` | `.push(x)` `.get(i)` `.len()` `.find(x)` `.remove(i)` | `svec_*` |
| `StringBuilder` | `.append(s)` `.append_char(c)` `.append_int(n)` `.append_float(f,prec)` `.build()` `.len()` `.clear()` | `sb_*` |
| `LinkedList<T>` | `.push(x)` `.pop()` `.get(i)` `.len()` | `list_*` |
| `Set<T>` | `.insert(x)` `.contains(x)` `.remove(x)` `.len()` | `set_*` |
| `BitSet` | `.set(i)` `.get(i)` `.clear(i)` | `bitset_*` |
| `T[]` | `.len()` | `arr[[:]]` |

---

## 14. Structs and Enums

### Struct

Data-only (no methods):
```cll
struct Vec2
{
    float32 x;
    float32 y;
}

Vec2 p;
p.x = 3.0;
p.y = 4.0;
```

### Enum

Integer-backed:
```cll
enum Direction { North, South, East, West }
enum ErrorCode : int32 { Ok = 0, NotFound = 404 }
```

---

## 15. Templates

```cll
template<typename T>
struct Pair
{
    T first;
    T second;
}

template<typename T>
def swap(T* a, T* b)
{
    T tmp = *a;
    *a = *b;
    *b = tmp;
}
```

Monomorphically instantiated at compile time.

---

## 16. Namespaces

Lexical grouping — does **not** create a new arena:

```cll
namespace Math
{
    const float64 PI = 3.14159265358979;
}

// access:
float64 area = Math::PI * r * r;
```

Nested path in one statement:
```cll
namespace Engine::Physics { … }
```

---

## 17. Imports and C-ABI Interop

### Module import
```cll
import std;
import io::file;
```

### File import
```cll
import "path/to/module.cll";
```

### C-header import (via libclang)
```cll
import "raylib.h";
import <stdio.h>;
```

Pass `-I<path>` to the compiler to add include search directories.

**Struct-by-value parameters** are automatically expanded: `Color{r,g,b,a}` becomes four separate `i8` parameters in the IR. This is transparent — you just pass the four bytes:

```cll
DrawText("Hello!", 10, 10, 20, 255, 255, 255, 255);  // explicit RGBA
DrawText("Hello!", 10, 10, 20, WHITE);                // named color constant
```

### Single C-function import
```cll
import int32  printf(string fmt, ...);
import voided free(voided* ptr);
```

`voided` = C `void`. Variadic functions use `...`.

### Named color constants

When importing `raylib.h`, the following named constants are recognised anywhere a `Color` (flat `uint8,uint8,uint8,uint8`) is expected:

`LIGHTGRAY` `GRAY` `DARKGRAY` `YELLOW` `GOLD` `ORANGE` `PINK` `RED` `MAROON`
`GREEN` `LIME` `DARKGREEN` `SKYBLUE` `BLUE` `DARKBLUE` `PURPLE` `VIOLET`
`DARKPURPLE` `BEIGE` `BROWN` `DARKBROWN` `WHITE` `BLACK` `BLANK` `MAGENTA` `RAYWHITE`

```cll
ClearBackground(RAYWHITE);
DrawText("Hello!", 50, 100, 30, RED);
DrawCircle(200, 200, 50, SKYBLUE);
```

---

## 18. Standard Library (`std`)

`import std;` makes the following available. All functions are null-safe unless noted.

### I/O
```cll
printf(string fmt, ...);
puts(string s);
putchar(int32 c);
scanf(string fmt, ...);
read_line_a(voided* arena)   -> string   // arena-tracked stdin line
```

### Safe strings
```cll
str_len(string s)                              -> int32
str_char_at(string s, int32 idx)               -> char    // 0 if OOB
str_eq(string a, string b)                     -> int32   // 1 if equal
str_starts_with(string s, string prefix)       -> int32
str_ends_with(string s, string suffix)         -> int32
str_index_of(string s, string needle)          -> int32   // -1 if not found
str_to_int(string s)                           -> int32
str_to_float(string s)                         -> float64

// Arena-tracked (freed with scope):
str_concat_a(string a, string b, __arena)      -> string
str_slice_a(string s, int32 start, int32 end, __arena) -> string
str_replace_a(string s, string from, string to, __arena) -> string
str_to_upper_a(string s, __arena)              -> string
str_to_lower_a(string s, __arena)              -> string
str_trim_a(string s, __arena)                  -> string
int_to_str_a(int64 val, __arena)               -> string
float_to_str_a(float64 val, int32 prec, __arena) -> string
```

Use `__arena` as the last argument to have the result tracked by the current scope's arena.

### File I/O
```cll
read_file_s_a(string path, __arena)   -> string   // whole file, arena-tracked
write_file_s(string path, string s)   -> int32    // 0 on success, -1 on error
append_file_s(string path, string s)  -> int32
file_exists(string path)              -> int32    // 1 if readable
file_size_s(string path)              -> int64    // bytes, or -1
```

### Math
```cll
sin(float64 x) cos(float64 x) tan(float64 x)
sqrt(float64 x) pow(float64 x, float64 y)
fabs(float64 x) floor(float64 x) ceil(float64 x)
abs(int32 x)  fmin(float64 a, float64 b) fmax(float64 a, float64 b)
rand() -> int32      // random integer
srand(int32 seed)    // seed random
```

### Memory (internal — normally not needed)
The arena handles all allocation. Direct `malloc`/`free` are intentionally not exported. Use the arena-tracked container types.

---

## 19. Raylib Integration

### Build
```bash
cshift game.cll -I/path/to/raylib/src -c -o game.o
gcc game.o frt.o libraylib.a -lm -ldl -lpthread -lGL -lX11 \
    -lXrandr -lXinerama -lXcursor -lXi -o game
```

### Color constants

All 26 raylib named colors work directly in any function that takes a `Color` parameter. No struct literal needed.

```cll
import "raylib.h";

entry
{
    InitWindow(800, 450, "My Game");
    SetTargetFPS(60);

    while (WindowShouldClose() == false)
    {
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawText("Hello World!", 190, 200, 20, DARKGRAY);
        DrawCircle(400, 225, 50, RED);
        DrawRectangle(10, 10, 100, 40, BLUE);
        EndDrawing();
    }

    CloseWindow();
}
```

### Struct-by-value parameters

Functions like `ClearBackground(Color)` accept flat RGBA bytes directly:

```cll
ClearBackground(30, 30, 46, 255);    // explicit r,g,b,a
ClearBackground(DARKBLUE);           // named constant
DrawCircle(200, 200, 30, 0, 200, 0, 255); // explicit green
DrawCircle(200, 200, 30, GREEN);     // named constant
```

---

## 20. Export

```cll
export def add(int32 a, int32 b)
{
    tunnel a + b -> int32 result;
}
```

`export def` gives the function **external linkage** (visible to the C linker). Plain `def` gets **internal linkage** (dead-code eligible, invisible outside the module).

---

## 21. Raw Strings

```cll
// Delimiter form
string banner = raw<until "END">
###########
# Hello!  #
###########
END

// Line-count form
puts(raw<3>
Line 1 — no escape processing
Line 2 — \n is literal
Line 3
);
```

---

## 22. Reserved Words

```
bool       break      case       char       class      const
continue   dec        default    def        else       enum
entry      export     false      float32    float64    for
foreach    if         import     int8       int16      int32
int64      move       namespace  raw        reserve    reset
string     struct     switch     template   true       tunnel
typename   uint8      uint16     uint32     uint64     valid
voided     while
```

Built-in identifiers (not keywords, but reserved by the compiler):
```
__arena    __arena_null    __track_validity_*
```

---

## 23. Operator Table

| Op | Category | Notes |
|----|----------|-------|
| `->` | Tunnel arrow | |
| `::` | Namespace resolution | |
| `==` `!=` `<` `>` `<=` `>=` | Comparison | |
| `&&` `\|\|` `!` | Logical | |
| `+` `-` `*` `/` `%` | Arithmetic | auto-coerce numeric types |
| `+=` `-=` `*=` `/=` `%=` | Compound assign | |
| `<<=` `>>=` | Shift-assign | |
| `<<` | Array append / left-shift | |
| `>>` | Right shift | |
| `&` | Address-of | For managed types: returns heap ptr |
| `*` | Dereference | |
| `.` | Field / method access | |
| `[i]` | Subscript | |
| `[[:]]` | Length-of | arena arrays |

Lexer uses **maximal munch** (longest match wins).

---

## 24. Grammar (EBNF)

```ebnf
program          ::= top_level*
top_level        ::= import_stmt | struct_def | enum_def | namespace_def
                   | function_def | func_decl | class_def
                   | entry_def | const_decl | template_def

import_stmt      ::= 'import' ( string_literal | '<' header '>'
                              | ns_path
                              | type ident '(' c_params ')' ) ';'
ns_path          ::= ident ( '::' ident )*

function_def     ::= ['export'] 'def' ident '(' params ')' block
params           ::= ( type ident ( ',' type ident )* )?
entry_def        ::= 'entry' block

// Forward declaration — registers the signature (incl. tunnel outputs)
// without a body. A later `def` with the same name supplies the body.
func_decl        ::= 'dec' ident '(' params ')' [ '->' tunnel_sig_list ] ';'
tunnel_sig_list  ::= type ident ( ',' type ident )*

// Zero-cost class — desugars to a struct + free functions taking
// `ClassName* self` as the first parameter.
class_def        ::= 'class' ident '{' ( field_decl | method_def )* '}'
field_decl       ::= type ident ';'
method_def       ::= 'def' ident '(' params ')' [ '->' tunnel_sig_list ] block

template_def     ::= 'template' '<' 'typename' ident '>' ( struct_def | function_def )

struct_def       ::= 'struct' ident '{' ( type ident ';' )* '}'
enum_def         ::= 'enum' ident [':' type] '{' ident ['=' expr] (',' ident ['=' expr])* '}'
namespace_def    ::= 'namespace' ns_path block

type             ::= base_type '*'* ( '[]' | '[:]' | '[' expr ']' )?
base_type        ::= primitive | ident | ident '<' type (',' type)* '>'
primitive        ::= 'int8'|'int16'|'int32'|'int64'|'uint8'|'uint16'|'uint32'|'uint64'
                   | 'float32'|'float64'|'bool'|'char'|'string'|'voided'

block            ::= '{' stmt* '}'
stmt             ::= declaration | const_decl | reserve_stmt | tunnel_stmt
                   | move_stmt | reset_stmt | assignment | index_assignment
                   | call_stmt | method_call_stmt | if_stmt | while_stmt
                   | for_stmt | foreach_stmt | switch_stmt | block | expr ';'

declaration      ::= type ident ['=' expr] ';'
const_decl       ::= 'const' type ident '=' expr ';'

// reserve [<shared>] [type] name [<< tunnel_name] ['=' expr] ';'
// - type omitted          → inferred from the callee's single tunnel
//                            (or from the `<< tunnel_name`-bound tunnel)
// - '<< tunnel_name'       → explicitly bind this slot to the named
//                            tunnel output of the upcoming call
reserve_stmt     ::= 'reserve' ['<' 'shared' '>'] [type] ident
                     [ '<<' ident ] ['=' expr] ';'

tunnel_stmt      ::= 'tunnel' expr '->' type ident ';'
move_stmt        ::= 'move' ident ';'
reset_stmt       ::= 'reset' ';'
assignment       ::= lvalue assign_op expr ';'
index_assignment ::= ident '[' expr ']' assign_op expr ';'
call_stmt        ::= ident '(' args ')' ';'
method_call_stmt ::= ident '.' ident '(' args ')' ';'
lvalue           ::= ident ('.' ident)*
assign_op        ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
args             ::= (expr (',' expr)*)?

if_stmt          ::= 'if' '(' expr ')' block ['else' (if_stmt | block)]
while_stmt       ::= 'while' '(' expr ')' block
for_stmt         ::= 'for' '(' declaration expr ';' ')' block
foreach_stmt     ::= 'foreach' '(' type ident ':' expr ')' block
switch_stmt      ::= 'switch' '(' expr ')' '{' switch_arm* '}'
switch_arm       ::= ('case' ident | 'default') ':' stmt*
```

---

## 25. Annotated Examples

### Hello World

```cll
import std;

entry
{
    puts("Hello, C<< world!");
}
```

### Forward declarations, classes, and tunnel binding

```cll
import std;

// Forward-declare a mutually recursive helper
dec is_odd(int32 n) -> int32 result;

def is_even(int32 n)
{
    if (n == 0)
    {
        tunnel 1 -> int32 result;
    }
    else
    {
        reserve int32 r = is_odd(n - 1);
        tunnel r -> int32 result;
    }
}

def is_odd(int32 n)
{
    if (n == 0)
    {
        tunnel 0 -> int32 result;
    }
    else
    {
        reserve int32 r = is_even(n - 1);
        tunnel r -> int32 result;
    }
}

// Zero-cost class
class Player
{
    float32 x;
    float32 y;
    int32 health;

    def move_by(float32 dx, float32 dy)
    {
        self.x += dx;
        self.y += dy;
    }

    def take_damage(int32 amount)
    {
        self.health -= amount;
    }

    def is_alive() -> int32 alive
    {
        if (self.health > 0)
        {
            tunnel 1 -> int32 alive;
        }
        else
        {
            tunnel 0 -> int32 alive;
        }
    }
}

// Multi-tunnel function for the reserve << binding example
def compute(int32 x)
{
    tunnel x * 2 -> int32 doubled;
    tunnel x * 3 -> int32 tripled;
}

entry
{
    reserve int32 e = is_even(10);
    printf("is_even(10) = %d\n", e);

    Player p;
    p.x = 0.0;
    p.y = 0.0;
    p.health = 100;

    p.move_by(5.0, 3.0);
    p.take_damage(30);

    reserve int32 alive = p.is_alive();
    printf("pos=(%.1f,%.1f) health=%d alive=%d\n", p.x, p.y, p.health, alive);

    // Explicit tunnel binding — pick "tripled" regardless of compute()'s
    // first/positional tunnel
    reserve int32 t << tripled = compute(10);
    printf("tripled = %d\n", t);   // 30

    // Type-inferred + explicit binding
    reserve r << doubled = compute(5);
    printf("doubled = %d\n", r);   // 10
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
    while (i < n)
    {
        int32 tmp = b;
        b = a + b;
        a = tmp;
        i += 1;
    }
    tunnel a -> int32 result;
}

entry
{
    int32 i = 0;
    while (i < 10)
    {
        reserve int32 r = fib(i);
        printf("fib(%d) = %d\n", i, r);
        i += 1;
    }
}
```

### Arena arrays

```cll
import std;

entry
{
    int32[] nums;
    nums << 10;
    nums << 20;
    nums << 30;

    printf("len=%llu\n", nums.len());

    int32 i = 0;
    while (i < 3)
    {
        printf("nums[%d] = %d\n", i, nums[i]);
        i += 1;
    }

    nums[1] = 99;
    printf("nums[1] after assign = %d\n", nums[1]);
}
```

### Vector with method syntax

```cll
import std;

entry
{
    Vector<int32> v = vec_new(16);
    v.push(10);
    v.push(20);
    v.push(30);

    printf("len=%llu first=%d last=%d\n",
           v.len(), v.get(0), v.get(v.len() - 1));

    {
        Vector<int32> tmp = vec_new(4);
        tmp.push(99);
        printf("inner=%d\n", tmp.get(0));
        // tmp freed automatically here
    }

    // v freed when entry scope exits
}
```

### Safe strings

```cll
import std;

entry
{
    string s = "Hello, World!";
    printf("len=%d\n", str_len(s));
    printf("eq=%d\n", str_eq(s, "Hello, World!"));
    printf("upper=%s\n", str_to_upper_a(s, __arena));

    string joined = str_concat_a("foo", "bar", __arena);
    printf("%s\n", joined);
}
```

### File I/O

```cll
import std;

entry
{
    int32 ok = write_file_s("/tmp/test.txt", "hello from C<<\n");
    if (ok == 0)
    {
        string content = read_file_s_a("/tmp/test.txt", __arena);
        printf("read: %s", content);
    }

    printf("exists=%d size=%lld\n",
           file_exists("/tmp/test.txt"),
           file_size_s("/tmp/test.txt"));
}
```

### Voided-state guard — static (zero cost)

```cll
import std;

entry
{
    int32 x = 42;
    move x;

    // Compiler knows x is definitely voided — emits only the voided branch
    switch (x)
    {
        case valid:   printf("valid: %d\n", x);
        case voided:  puts("x was moved");
    }
}
```

### Voided-state guard — runtime (conditional move)

```cll
import std;

entry
{
    int32 x = 42;
    int32 coin = rand() % 2;

    if (coin == 0)
    {
        move x;   // only on one path
    }

    // Compiler inserts __track_validity_x bool; runtime cond_br
    switch (x)
    {
        case valid:  printf("x = %d\n", x);
        case voided: puts("x was moved");
    }
}
```

### Raylib window with named colors

```cll
import "raylib.h";

entry
{
    InitWindow(800, 450, "C<< + Raylib");
    SetTargetFPS(60);

    while (WindowShouldClose() == false)
    {
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawText("Hello World!", 190, 200, 20, DARKGRAY);
        DrawCircle(400, 225, 50, RED);
        DrawRectangle(10, 10, 100, 40, BLUE);
        EndDrawing();
    }

    CloseWindow();
}
```

### Build: `import std;` program

```bash
cshift myprog.cll -c -o myprog.o
gcc myprog.o frt_native.o -o myprog
```

### Build: raylib program

```bash
cshift game.cll -I/path/to/raylib/src -c -o game.o
gcc game.o frt_native.o libraylib.a \
    -lm -ldl -lpthread -lGL -lX11 -lXrandr -lXinerama -lXcursor -lXi \
    -o game
```

`frt_native.o` is found in the build output directory after running `cmake + make`.

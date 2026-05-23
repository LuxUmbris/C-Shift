# C<< (C-Shift / C-Less-Less)

A small LLVM-based compiler for a safe, arena-oriented systems language.
C<< focuses on deterministic, zero-runtime behavior and direct C ABI interoperability.

## Build

### Native build with Make

### Build with CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

## Usage

Compile a C<< source file:

```bash
./cshift examples/hello.cll -o hello
```

Emit LLVM IR:

```bash
./cshift examples/hello.cll --emit-llvm
```

Emit assembly:

```bash
./cshift examples/hello.cll --emit-asm
```

Compile only to object file:

```bash
./cshift examples/hello.cll -c
```

Semantic check only:

```bash
./cshift examples/hello.cll --check-only
```

### Hello World

```cll
import std;

entry
{
    puts("Hello, C<< world!");
}
```

## Philosophy
C<< is designed to be safe without fighting the compiler, and easy to use without runtime abstractions. 
So it does not have a GC (Garbage Collector), Borrow Checker or similar things and is fully arena-based. 
The runtime is only a minimal C-file for the standard library. It strictly separates:
- Data Patterns
- Logic Units
  
In summary, the design goals are:
- Performance
- Determinism
- Developer Experience
- Memory Safety
- Zero Runtime Abstractions
- Full C-ABI-interop with extern functions
- Cross Compilation

## VOP (Vertical Ownership Programming)
VOP (Vertical Ownership Programming) can be explained like this:
1. A pointer MUST only point to a variable with a depth <= its own, while depth defines the current arena.
2. A scope is equal to a arena, except of the lexical scopes (namespaces, structs, etc.). When the scope ends, the arena gets deleted.
3. Functions MUST not use return values. VOP only uses tunnels, similar to pointers, declared under 'Syntax'
4. A tunnel may not transfer data containing pointers to arenas that will be destroyed.
5. reset clears the current arena. Forbidden if child arenas contain pointers into it.

## Syntax

### Primitve Types
C<< contains the following primitive Types:
```CShift
int8
int16
int32
int64

uint8
uint16
uint32
uint64

float32
float64

bool
string

T*
```

### Structs
Structs MUST only contain data, no methods. example:
```CShift
struct Player
{
    int32 id;
    float32 health;
}
```

### Enums
Enums are integer-backed. example:
```
enum Status : uint8 { Active, Inactive }
```

### The Voided State
A pointer is never null. A variable is valid or voided. Any variable may be in the voided state. Accessing a variable wich could be voided without an switch-guard causes compile-time termination.
```
entry
{
    int32 x;
    int32* p = &x;
    move x;
    switch(p)
    {
        case valid {}
        case voided {}
    }
}
```

## Raw Strings
### Delimitter
```
string banner = raw<until "Your_delimitter">
###########
# Hello C<< #
###########
Your_delimitter
```
### Line count
```
puts(raw<3>
I can type \n here or \0 or \t and nothing happens
A Real newline is integrated into the string
No matter what you write here the string does not break.
);
```
## Functions
Definition:
```
def name(parameters)
{
    // arena
}
```
Properties:
- Functions have no type, but it is recommended to comment out their tunneled type.
- Functions may contain any number of tunnel operations.
- A function call is a statement.
- Tunnel values produced inside a function appear in the call scope if they were reserved.
- Inline usage of function results is forbidden except in reserve-initializers or for anonymous tunnels.
Example:
```cll
def compute(int32 x) // tunnels int32 doubled
{
    tunnel x * 2 -> int32 doubled;
}
```
anonymous:
```
def compute(int32 x) // tunnels int32 doubled
{
    tunnel x * 2;
}
```
### Function calls
classic:
```
reserve int32 result;
add(5, 7);
```
inline:
```
reserve int32 result = add(5, 7);
```
anonymous:
```
int32 result = add(5, 7);
```
A function can tunnel multiple values. All values wich were reserved in the past and are tunneled by the function get filled. The others are ignored.
```
def compute(int32 x, int32 y)
{
    tunnel x + y -> int32 sum;
    tunnel x * y -> int32 product;
}

entry
{
    reserve int32 sum;
    reserve int32 product;

    compute(8, 4);
}
```
### Arrays & Slices
Arrays:
```
T[]   // arena-bound array
```
Slices:
```
T[:]  // pointer + length, non-owning
```
Slices may only reference arenas that outlive them.

### Control Flow
Supported constructs:
if / else
switch / case
while
for
foreach

Each iteration of while/for/foreach is its own sub-arena. Tunnels inside switch/case must target variables in the parent scope.

### C-ABI-interop
Syntax:
```
import <type> <fn_name> (<params>);
```

## License
This Project is licensed under the [Apache 2.0 License](LICENSE)

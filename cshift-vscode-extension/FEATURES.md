# C<< Language Support - Detailed Features

Complete documentation of all features in the C<< VS Code extension v1.1.0.

## Table of Contents

1. [Syntax Highlighting](#syntax-highlighting)
2. [IntelliSense & Autocomplete](#intellisense--autocomplete)
3. [Error Detection](#error-detection)
4. [Code Snippets](#code-snippets)
5. [Commands & Keyboard Shortcuts](#commands--keyboard-shortcuts)
6. [Configuration](#configuration)

---

## Syntax Highlighting

### Keywords (Color: Keyword Purple)
All C<< keywords are highlighted with dedicated colors and scopes:

**Control Flow:**
- `if`, `else` — Conditional execution
- `while`, `for`, `foreach` — Loop constructs
- `switch`, `case`, `default` — Case dispatch
- `break`, `continue` — Loop control

**Declarations:**
- `def` — Function definition
- `struct` — Structure definition
- `enum` — Enumeration definition
- `namespace` — Namespace grouping
- `const` — Constant declaration
- `import` — Module/function import
- `entry` — Program entry point

**VOP-Specific:**
- `tunnel` — Value output from functions
- `move` — Transition to voided state
- `reset` — Clear arena
- `valid`, `voided` — State guards
- `reserve` — Reserve slot for tunnel output

**Templates:**
- `template` — Generic definition
- `typename` — Type parameter

**Boolean Literals:**
- `true`, `false`

### Types (Color: Type Cyan)
All primitive and user-defined types:

**Primitive Integer Types:**
- `int8`, `int16`, `int32`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`

**Primitive Float Types:**
- `float32`, `float64`

**Other Primitives:**
- `bool` — Boolean
- `char` — Single character
- `string` — Null-terminated C string
- `voided` — Absence of type (void equivalent)

**Type Modifiers:**
- `*` — Pointer (appears in purple/operator color)
- `[]` — Array bounds
- `[:]` — Slice notation

### Comments (Color: Comment Green)
Both comment styles are fully supported:

**Single-line Comments:**
```cll
// Everything after // until end of line is ignored
```

**Multi-line Comments:**
```cll
/* Block comments can span
   multiple lines and are properly
   highlighted throughout */
```

### Strings (Color: String Orange)
All string literals with escape sequence recognition:

**Quoted Strings:**
```cll
"This is a string with \n escape sequences"
```

**Raw Strings (Delimited):**
```cll
raw<until "MARKER">
No escapes here \ / are literal
MARKER
```

**Raw Strings (Line Count):**
```cll
raw<3>
Line one
Line two  
Line three
```

### Numbers (Color: Number Blue)
Integer and floating-point literals:

**Integers:**
- `42`, `0`, `1024`

**Floats:**
- `3.14`, `0.5`, `2.718e-1`

### Operators (Color: Operator Red)
Proper highlighting for all operators:

**Arithmetic:** `+`, `-`, `*`, `/`, `%`
**Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=`
**Logical:** `&&`, `||`, `!`
**Bitwise:** `&`, `<<`, `>>`
**Assignment:** `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `**=`
**Special:**
- `->` — Tunnel target operator
- `::` — Namespace resolution
- `.` — Field access
- `&` — Address-of (context-sensitive)
- `*` — Dereference (context-sensitive)

### Brackets & Delimiters
Properly matched and highlighted:
- `{`, `}` — Blocks
- `[`, `]` — Arrays & slices
- `(`, `)` — Parentheses
- `;` — Statement terminator
- `:` — Type separator, case label

---

## IntelliSense & Autocomplete

### Keyword Suggestions
Trigger: Press `Ctrl+Space` or start typing a keyword

All 35 C<< keywords with brief descriptions:
- `if`, `else`, `while`, `for`, `foreach`
- `switch`, `case`, `default`
- `break`, `continue`
- `def`, `struct`, `enum`, `namespace`
- `const`, `reserve`, `import`, `entry`
- `tunnel`, `move`, `reset`, `valid`, `voided`
- `template`, `typename`
- `true`, `false`, `raw`

### Type Suggestions
All primitive types available in autocomplete:
- Integer types: `int8` through `uint64`
- Float types: `float32`, `float64`
- Other: `bool`, `char`, `string`, `voided`

### Function Suggestions
Standard library functions pre-populated:
- I/O: `printf`, `puts`, `putchar`, `scanf`, `getchar`
- Memory: `malloc`, `free`, `calloc`, `realloc`
- Strings: `strlen`, `strcmp`, `strcpy`, `strcat`
- Process: `exit`, `abort`, `system`

### Container Template Suggestions
All 13 generic containers with `<T>` placeholder:

**Collections:**
- `Vector<T>` — Chunk-based dynamic array
- `HashMap<K,V>` — Hash table
- `LinkedList<T>` — Doubly-linked list
- `Set<T>` — Hash set
- `Deque<T>` — Double-ended queue
- `RingBuffer<T>` — Circular buffer
- `Pool<T>` — Object pool

**Utilities:**
- `Pair<A,B>` — Tuple type
- `Lazy<T>` — Lazy evaluation
- `BitSet` — Bitfield
- `Guard` — RAII cleanup
- `StringBuilder` — String builder
- `Buffer<T>` — Typed buffer
- `SortedVec<T>` — Sorted collection

### Hover Documentation
Hover over keywords to see inline documentation:

Example hover information for `tunnel`:
> Output a value from a function to the caller's scope.
> 
> Syntax: `tunnel <expr> -> <type> <name>;`
> 
> The expression is evaluated and the result is written to the named slot in the calling scope if it was reserved.

---

## Error Detection

### Real-time Diagnostics
Errors and warnings appear as you type with squiggly underlines:

### Error Categories

**Syntax Errors:**
1. **Missing Semicolons** (Warning)
   - Detected on declaration, tunnel, move, reset, const, break, continue
   - Excludes control flow statements

2. **Invalid Tunnel Syntax** (Error)
   - Tunnel without `->` operator
   - Example: `tunnel x + y` (missing type annotation)

3. **Reserved Keyword Misuse** (Error)
   - Using `continue` outside a loop
   - (Detected as warning when in switch context)

**Semantic Warnings:**
1. **Moving Const Variables** (Error)
   - Using `move` on variables declared with `const`
   - Prevents accidental state violations

2. **Continue in Switch** (Warning)
   - `continue` only valid in loops, not switches
   - Helps catch VOP violations

### Error Display
- **Location:** Line and column indicators
- **Severity Levels:**
  - 🔴 Error (Red squiggle) — Must fix before compiling
  - 🟡 Warning (Yellow squiggle) — Should review
- **Hover:** Shows error description
- **Problems Panel:** All diagnostics listed with quick navigation

---

## Code Snippets

### 20+ Pre-built Snippets

Trigger: Type prefix and press `Tab` or select from autocomplete

#### Core Language Snippets

1. **Function Definition** → `fn`
   ```cll
   def functionName(parameters)
   {
   	
   }
   ```

2. **Function with Tunnel** → `fn_tunnel`
   ```cll
   def functionName(parameters)
   {
   	tunnel expression -> type result;
   }
   ```

3. **Struct Definition** → `struct`
   ```cll
   struct StructName
   {
   	int32 field1;
   	float32 field2;
   }
   ```

4. **Enum Definition** → `enum`
   ```cll
   enum EnumName : int32
   {
   	Value1,
   	Value2
   }
   ```

#### Template Snippets

5. **Template Struct** → `template_struct`
   ```cll
   template<typename T>
   struct GenericName
   {
   	T data;
   	uint64 length;
   }
   ```

6. **Template Function** → `template_fn`
   ```cll
   template<typename T>
   def functionName(T param)
   {
   	tunnel expression -> T result;
   }
   ```

#### Control Flow Snippets

7. **If Statement** → `if`
8. **If-Else** → `ifelse`
9. **While Loop** → `while`
10. **For Loop** → `for`
11. **Foreach Loop** → `foreach`
12. **Switch Statement** → `switch`

#### VOP Snippets

13. **Voided State Guard** → `voided_guard`
    ```cll
    switch (pointer)
    {
        case valid
        {
            // pointer is valid
        }
        case voided
        {
            // pointer is voided
        }
    }
    ```

14. **Reserve Statement** → `reserve`
15. **Move Statement** → `move`

#### Module Snippets

16. **Import Standard Library** → `import_std`
17. **Import C Function** → `import_c`
18. **Entry Point** → `entry`

#### Program Templates

19. **Hello World** → `hello`
    ```cll
    import std;

    entry
    {
        puts("Hello, C<< world!");
    }
    ```

20. **Namespace** → `namespace`
21. **Const Declaration** → `const`
22. **Variable Declaration** → `var`

---

## Commands & Keyboard Shortcuts

### Built-in Commands

**C<<: Compile File** (Ctrl+Shift+B)
- Compiles the current file using the `cshift` compiler
- Output appears in problems panel
- Errors are highlighted with line/column references

**C<<: Check Syntax** (Command Palette)
- Validates syntax without full compilation
- Faster feedback for immediate editing

**C<<: Format Document** (Alt+Shift+F)
- Auto-formats code with smart indentation
- Follows C<< style conventions:
  - 2-space indentation
  - Consistent bracket alignment
  - Proper spacing around operators

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+Space` | Trigger autocomplete |
| `Ctrl+Shift+B` | Compile current file |
| `Alt+Shift+F` | Format document |
| `Ctrl+K Ctrl+0` | Fold all regions |
| `Ctrl+K Ctrl+J` | Unfold all regions |
| `Ctrl+Shift+/` | Toggle block comment |
| `Ctrl+/` | Toggle line comment |

### Command Palette Commands

Press `Ctrl+Shift+P` (Cmd+Shift+P on Mac):

- **C<<: Compile File** — Compile active file
- **C<<: Check Syntax** — Validate without compiling
- **C<<: Format Document** — Auto-format code

---

## Configuration

### Editor Settings

Add to VS Code `settings.json` for optimal C<< experience:

```json
"[cshift]": {
    "editor.formatOnSave": true,
    "editor.wordBasedSuggestions": false,
    "editor.tabSize": 2,
    "editor.insertSpaces": true,
    "editor.defaultFormatter": "cshift.cshift-language",
    "editor.semanticHighlighting.enabled": true,
    "files.trimTrailingWhitespace": true,
    "files.trimFinalNewlines": true
}
```

### File Association

Automatically associate `.cll` files with C<< language:

```json
"files.associations": {
    "*.cll": "cshift"
}
```

### Theme Integration

Syntax highlighting works with all VS Code themes:
- **Light themes** (Light+ Default)
- **Dark themes** (Dark+ Default)
- **Custom themes** (uses TextMate scopes)

### Extension Settings

*(Future additions may include configurable settings)*

---

## Technical Implementation Details

### Architecture

- **Extension Type:** Language Support
- **Language ID:** `cshift`
- **File Extensions:** `.cll`
- **Grammar Format:** TextMate JSON (`cshift.tmLanguage.json`)
- **Minimum VS Code Version:** 1.70.0

### Components

1. **extension.js** — Main extension code
   - CompletionItemProvider — Autocomplete
   - HoverProvider — Documentation on hover
   - Diagnostic collection — Error highlighting
   - Command handlers — Compile, format, check

2. **language-configuration.json** — Language rules
   - Comment delimiters
   - Auto-closing brackets
   - Indentation rules
   - Word pattern matching

3. **syntaxes/cshift.tmLanguage.json** — Syntax highlighting
   - 50+ regex patterns
   - 7 main repositories
   - Token classification scopes

4. **snippets/cshift.json** — Code templates
   - 20+ pre-built snippets
   - Trigger prefixes and descriptions
   - Snippet variables for tab stops

---

## Version History

### v1.1.0 (2026-05-25)
- Complete rewrite with IntelliSense
- Multi-line comment support
- Real-time error detection
- 20+ code snippets
- Template support
- Generic containers documentation
- Break/continue keywords

### v1.0.0 (Initial)
- Basic syntax highlighting
- Keyword recognition

---

## Troubleshooting

### Autocomplete not showing?
- Press `Ctrl+Space` to manually trigger
- Check that `editor.wordBasedSuggestions` is false

### Errors not highlighting?
- Save the file (diagnostics run on save)
- Check the Problems panel (Ctrl+Shift+M)

### Formatting not working?
- Ensure file is saved as `.cll`
- Try Alt+Shift+F manually

### Compilation not working?
- Verify `cshift` compiler is in PATH
- Run `which cshift` in terminal
- Check build output for errors

---

## Support & Feedback

- **Issues:** https://github.com/LuxUmbris/C-Shift/issues
- **Documentation:** https://github.com/LuxUmbris/C-Shift
- **Language Spec:** See `Syntax_spec.md` in main repo

---

**Happy coding in C<<! 🚀**

# C<< (C-Shift) Language Support for VS Code

Professional syntax highlighting, IntelliSense, error checking, and code completion for the C<< systems language.

## Features

### ✨ Syntax Highlighting
- **Keywords**: Control flow (`if`, `else`, `while`, `for`, `foreach`, `switch`), declarations (`def`, `struct`, `enum`, `namespace`), and special C<< keywords (`tunnel`, `move`, `reserve`, `voided`, `valid`)
- **Types**: Primitive types (`int8`, `int32`, `float64`, etc.), pointers, arrays, and slices
- **Comments**: Single-line (`//`) and multi-line (`/* */`) comments
- **Strings**: Quoted strings and raw strings (`raw<until "DELIM">`, `raw<N>`)
- **Numbers**: Integer and floating-point literals

### 🧠 IntelliSense & Autocomplete
- **Context-aware suggestions** for keywords, types, and identifiers
- **Function signatures** and standard library functions
- **Generic containers** from std.cll (Vector<T>, HashMap<K,V>, etc.)
- **Smart snippets** for common patterns:
  - Function definitions with tunnel
  - Struct and enum definitions
  - Control flow statements (if, while, for, foreach, switch)
  - Templates
  - Voided state guards

### 🔍 Error Highlighting
- **Syntax errors**: Missing semicolons, invalid tunnel syntax
- **Type errors**: Using `continue` in switch (warning)
- **Semantic errors**: Moving const variables
- **Real-time diagnostic feedback** as you type

### 📝 Code Completion
- **Keyword completion** with documentation
- **Type suggestions** for primitive and user-defined types
- **Function completions** from standard library
- **Container templates** pre-populated with type parameters

### ⚙️ Commands
- **Ctrl+Shift+B** — Compile current C<< file
- **C<<: Check Syntax** — Validate without compiling
- **C<<: Format Document** — Auto-format code with proper indentation

### 🎨 Language Configuration
- Auto-closing brackets and quotes
- Smart indentation for control flow
- Folding regions for code organization

## Installation

### From VS Code Extensions Marketplace
1. Open VS Code
2. Go to Extensions (Ctrl+Shift+X)
3. Search for "C<< Language Support"
4. Click Install

### Manual Installation
```bash
# Clone or download the extension
cd cshift-vscode-extension
npm install
vsce package
code --install-extension cshift-language-1.1.0.vsix
```

## Quick Start

### Hello World
```cll
import std;

entry
{
    puts("Hello, C<< world!");
}
```

### Function with Tunnel
```cll
def add(int32 a, int32 b)
{
    tunnel a + b -> int32 result;
}

entry
{
    reserve int32 sum = add(5, 7);
    printf("5 + 7 = %d\n", sum);
}
```

### Generic Template
```cll
template<typename T>
struct Box
{
    T value;
}

entry
{
    Box<int32> box;
    box.value = 42;
}
```

## Keyboard Shortcuts

| Shortcut | Command |
|----------|---------|
| Ctrl+Space | Trigger autocomplete |
| Ctrl+Shift+B | Compile file |
| Alt+Shift+F | Format document |
| Ctrl+K Ctrl+0 | Fold all |
| Ctrl+K Ctrl+J | Unfold all |

## Settings

Add to VS Code `settings.json`:

```json
"[cshift]": {
    "editor.formatOnSave": true,
    "editor.wordBasedSuggestions": false,
    "editor.tabSize": 2,
    "editor.insertSpaces": true,
    "files.associations": {
        "*.cll": "cshift"
    }
}
```

## Supported File Types

- `.cll` — C<< source code

## Documentation

- [C<< Syntax Specification](https://github.com/LuxUmbris/C-Shift/blob/main/Syntax_spec.md)
- [C<< Standard Library](https://github.com/LuxUmbris/C-Shift/blob/main/std.cll)
- [GitHub Repository](https://github.com/LuxUmbris/C-Shift)

## Requirements

- VS Code 1.70.0 or later
- C<< compiler (`cshift`) for compilation features

## Known Limitations

- Template instantiation errors are not highlighted in-editor (compile to see full diagnostics)
- Complex nested templates may not provide full autocomplete
- Hover documentation is basic; see Syntax_spec.md for detailed reference

## Changelog

### Version 1.1.0
- Complete rewrite with full IntelliSense and autocomplete
- Multi-line comment support in syntax highlighting
- Enhanced error detection and diagnostics
- 20+ code snippets for common patterns
- Real-time compilation diagnostics
- Support for all C<< 0.3 language features:
  - Templates with `template<typename T>`
  - Control flow: `break` and `continue`
  - Generic containers from std.cll
  - Improved VOP semantics visualization

### Version 1.0.0
- Initial release
- Basic syntax highlighting
- Keyword recognition

## Contributing

Contributions are welcome! Please submit issues and pull requests to [GitHub](https://github.com/LuxUmbris/C-Shift).

## License

Apache License 2.0 — See [LICENSE](LICENSE) for details.

## Acknowledgments

Built for the [C<< (C-Shift)](https://github.com/LuxUmbris/C-Shift) language project.

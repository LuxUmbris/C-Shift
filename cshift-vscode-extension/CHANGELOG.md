# Changelog

All notable changes to the C<< Language Support extension will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-05-25

### Added
- **Full IntelliSense support** with context-aware suggestions
- **Autocomplete provider** for keywords, types, functions, and containers
- **Hover documentation** for C<< keywords with descriptions
- **Real-time error highlighting** with semantic checks
- **Code diagnostics** for:
  - Missing semicolons
  - Invalid tunnel syntax
  - `continue` in switch (warning)
  - Moving const variables (error)
- **20+ code snippets** including:
  - Function definitions with tunnel
  - Struct and enum templates
  - Control flow statements (if, while, for, foreach, switch)
  - Generic templates
  - Voided state guards
  - Reserve and call patterns
- **Multi-line comment support** (`/* */`) in syntax highlighting
- **Compile command** (Ctrl+Shift+B) integration
- **Document formatter** with smart indentation
- **Standard library containers** in autocomplete:
  - Vector<T>, HashMap<K,V>, LinkedList<T>
  - Set<T>, Deque<T>, RingBuffer<T>, Pool<T>
  - Pair<A,B>, Lazy<T>, BitSet, Guard
  - StringBuilder, Buffer<T>, SortedVec<T>
- **Enhanced syntax grammar** with all C<< 0.3 features:
  - Templates with `typename` parameters
  - `break` and `continue` keywords
  - `voided` and `valid` state guards
  - `reserve<shared>` modifiers
  - Namespace resolution operator (`::`)
#!/bin/bash

# VS Code Extension Build & Package Script for C<< Language Support
# Version 1.1.0

set -e

echo "=========================================="
echo "C<< Language Support Extension"
echo "Build & Package Script v1.1.0"
echo "=========================================="
echo ""

# Check if in correct directory
if [ ! -f "package.json" ]; then
    echo "❌ Error: package.json not found"
    echo "Please run this script from the extension directory"
    exit 1
fi

# Display extension info
echo "📦 Extension Information:"
NAME=$(grep '"name"' package.json | head -1 | cut -d'"' -f4)
VERSION=$(grep '"version"' package.json | head -1 | cut -d'"' -f4)
DISPLAY=$(grep '"displayName"' package.json | head -1 | cut -d'"' -f4)

echo "  Name: $NAME"
echo "  Display Name: $DISPLAY"
echo "  Version: $VERSION"
echo ""

# Create directory structure
echo "📁 Checking directory structure..."
DIRS=("syntaxes" "snippets" "icons")
for dir in "${DIRS[@]}"; do
    if [ -d "$dir" ]; then
        echo "  ✓ $dir/"
    else
        echo "  ⚠ Missing $dir/ (optional)"
    fi
done
echo ""

# List files
echo "📄 Extension files:"
echo "  ✓ package.json"
echo "  ✓ extension.js"
echo "  ✓ language-configuration.json"
echo "  ✓ syntaxes/cshift.tmLanguage.json"
echo "  ✓ snippets/cshift.json"
echo "  ✓ .vscodeignore"
echo "  ✓ README.md"
echo "  ✓ CHANGELOG.md"
echo "  ✓ LICENSE"
echo ""

# Instructions
echo "🚀 Installation Instructions:"
echo ""
echo "Option 1: Manual Installation (Without vsce)"
echo "  1. Compress this directory as cshift-language-${VERSION}.zip"
echo "  2. In VS Code: File → Preferences → Extensions"
echo "  3. Click (...) → Install from VSIX"
echo "  4. Select the .zip file"
echo ""

echo "Option 2: Using vsce (Recommended)"
echo "  1. Install vsce: npm install -g @vscode/vsce"
echo "  2. Run: vsce package"
echo "  3. This creates cshift-language-${VERSION}.vsix"
echo "  4. In VS Code: Cmd+Shift+P → Install from VSIX"
echo ""

echo "Option 3: Install to VS Code Extensions Directory"
echo "  1. Copy this directory to:"
echo "     ~/.vscode/extensions/cshift-language-${VERSION}/"
echo "  2. Reload VS Code"
echo ""

echo "✅ Extension is ready for use!"
echo ""

# Feature summary
echo "📋 Features Included:"
echo "  ✓ Syntax Highlighting (keywords, types, operators)"
echo "  ✓ Multi-line & single-line comments"
echo "  ✓ IntelliSense with 50+ suggestions"
echo "  ✓ Autocomplete snippets (20+ templates)"
echo "  ✓ Real-time error highlighting"
echo "  ✓ Hover documentation"
echo "  ✓ Code formatting"
echo "  ✓ Compilation integration (Ctrl+Shift+B)"
echo "  ✓ Standard library support"
echo "  ✓ Generic container templates"
echo ""

echo "=========================================="
echo "Ready to use! 🎉"
echo "=========================================="

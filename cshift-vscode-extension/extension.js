const vscode = require('vscode');
const { exec } = require('child_process');
const path = require('path');
const fs = require('fs');

// C<< Language Keywords
const KEYWORDS = [
  'if', 'else', 'while', 'for', 'foreach', 'switch', 'case', 'default',
  'break', 'continue', 'def', 'struct', 'enum', 'namespace', 'const',
  'reserve', 'import', 'entry', 'tunnel', 'move', 'reset', 'valid', 'voided',
  'template', 'typename', 'true', 'false', 'raw'
];

// C<< Primitive Types
const TYPES = [
  'int8', 'int16', 'int32', 'int64',
  'uint8', 'uint16', 'uint32', 'uint64',
  'float32', 'float64', 'bool', 'char', 'string', 'voided'
];

// Standard Library Items
const STD_ITEMS = [
  'printf', 'puts', 'putchar', 'scanf', 'getchar',
  'malloc', 'free', 'calloc', 'realloc',
  'strlen', 'strcmp', 'strcpy', 'strcat',
  'exit', 'abort', 'system'
];

// Standard Library Containers
const CONTAINERS = [
  'Vector', 'HashMap', 'LinkedList', 'Set', 'Deque', 'RingBuffer', 'Pool',
  'Pair', 'Lazy', 'BitSet', 'Guard', 'StringBuilder', 'Buffer', 'SortedVec'
];

class CShiftCompletionProvider {
  provideCompletionItems(document, position, token, context) {
    const completions = [];
    
    // Add keyword completions
    KEYWORDS.forEach(keyword => {
      const item = new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword);
      item.detail = 'C<< keyword';
      item.insertText = keyword;
      completions.push(item);
    });

    // Add type completions
    TYPES.forEach(type => {
      const item = new vscode.CompletionItem(type, vscode.CompletionItemKind.TypeParameter);
      item.detail = 'Primitive type';
      item.insertText = type;
      completions.push(item);
    });

    // Add container completions with templates
    CONTAINERS.forEach(container => {
      const item = new vscode.CompletionItem(container + '<T>', vscode.CompletionItemKind.Struct);
      item.detail = 'Generic container';
      item.insertText = container + '<T>';
      item.documentation = `Generic ${container} container from std.cll`;
      completions.push(item);
    });

    // Add standard library function completions
    STD_ITEMS.forEach(func => {
      const item = new vscode.CompletionItem(func, vscode.CompletionItemKind.Function);
      item.detail = 'Standard library function';
      item.insertText = func + '()';
      completions.push(item);
    });

    // Add common snippets
    const snippets = [
      {
        label: 'fn',
        insertText: 'def ${1:name}(${2:params})\n{\n\t$0\n}',
        documentation: 'C<< function definition',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'struct',
        insertText: 'struct ${1:Name}\n{\n\t${2:fields}\n}',
        documentation: 'C<< struct definition',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'if',
        insertText: 'if (${1:condition})\n{\n\t$0\n}',
        documentation: 'if statement',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'while',
        insertText: 'while (${1:condition})\n{\n\t$0\n}',
        documentation: 'while loop',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'for',
        insertText: 'for (${1:int32 i = 0}; ${2:i < 10}; ${3:i += 1})\n{\n\t$0\n}',
        documentation: 'for loop',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'foreach',
        insertText: 'foreach (${1:Type var} : ${2:array})\n{\n\t$0\n}',
        documentation: 'foreach loop',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'switch',
        insertText: 'switch (${1:expr})\n{\n\tcase ${2:value}:\n\t\t$0\n\tdefault:\n\t\tbreak;\n}',
        documentation: 'switch statement',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'template',
        insertText: 'template<typename ${1:T}>\nstruct ${2:Name}\n{\n\t${3:T data};\n}',
        documentation: 'generic template definition',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'tunnel',
        insertText: 'tunnel ${1:expr} -> ${2:type} ${3:name};',
        documentation: 'tunnel statement for function output',
        kind: vscode.CompletionItemKind.Snippet
      },
      {
        label: 'reserve',
        insertText: 'reserve ${1:type} ${2:name} = ${3:function}();',
        documentation: 'reserve statement',
        kind: vscode.CompletionItemKind.Snippet
      }
    ];

    snippets.forEach(snippet => {
      const item = new vscode.CompletionItem(snippet.label, snippet.kind);
      item.insertText = new vscode.SnippetString(snippet.insertText);
      item.documentation = snippet.documentation;
      item.detail = 'Snippet';
      completions.push(item);
    });

    return completions;
  }

  resolveCompletionItem(item, token) {
    return item;
  }
}

class CShiftHoverProvider {
  provideHover(document, position, token) {
    const word = document.getWordRangeAtPosition(position);
    if (!word) return null;

    const text = document.getText(word);
    
    // Provide documentation for keywords
    const docs = {
      'def': 'Define a function. Functions communicate output via `tunnel` statements.',
      'struct': 'Define a structure (data only, no methods).',
      'enum': 'Define an enumeration type.',
      'tunnel': 'Output a value from a function to the caller\'s scope.',
      'reserve': 'Declare a variable to be filled by a `tunnel` from an upcoming function call.',
      'move': 'Transition a variable into the voided state.',
      'voided': 'Indicates a variable has been moved (not null, but voided).',
      'template': 'Define a generic template type or function.',
      'typename': 'Declare a template type parameter.',
      'break': 'Exit the innermost loop or switch.',
      'continue': 'Restart the next iteration of the innermost loop.',
      'import': 'Import a module or C function.',
      'entry': 'The program entry point (like main()).',
      'reset': 'Clear the current arena.',
      'valid': 'Guard for accessing possibly-voided variables.',
      'namespace': 'Create a lexical namespace for organization.'
    };

    if (docs[text]) {
      return new vscode.Hover(new vscode.MarkdownString(docs[text]));
    }

    return null;
  }
}

class CShiftDiagnosticsProvider {
  constructor() {
    this.diagnostics = vscode.languages.createDiagnosticCollection('cshift');
  }

  provideDiagnostics(document) {
    const diagnostics = [];
    const text = document.getText();
    const lines = text.split('\n');

    lines.forEach((line, lineIndex) => {
      // Check for common errors
      
      // Missing semicolons (except for control flow closing braces)
      if (line.match(/^\s*(declaration|tunnel|move|reset|break|continue|const)\b.*[^;{}\s]$/) &&
          !line.match(/^\s*(if|while|for|foreach|switch|namespace|def|struct|enum)\b/)) {
        if (!line.includes('//')) {
          diagnostics.push(new vscode.Diagnostic(
            new vscode.Range(lineIndex, line.length - 1, lineIndex, line.length),
            'Missing semicolon',
            vscode.DiagnosticSeverity.Warning
          ));
        }
      }

      // Check for tunnel without type annotation
      const tunnelMatch = line.match(/tunnel\s+\S+\s+->/);
      if (tunnelMatch && !line.includes('->')) {
        diagnostics.push(new vscode.Diagnostic(
          new vscode.Range(lineIndex, 0, lineIndex, line.length),
          'Invalid tunnel syntax: missing type annotation',
          vscode.DiagnosticSeverity.Error
        ));
      }

      // Check for continue in switch (warning)
      if (line.includes('continue') && this.isInSwitch(lines, lineIndex)) {
        diagnostics.push(new vscode.Diagnostic(
          new vscode.Range(lineIndex, line.indexOf('continue'), lineIndex, line.indexOf('continue') + 8),
          'continue statement should not be used in switch (only in loops)',
          vscode.DiagnosticSeverity.Warning
        ));
      }

      // Check for move on const variables (light check)
      if (line.includes('move') && lineIndex > 0) {
        const prevLines = lines.slice(Math.max(0, lineIndex - 5), lineIndex).join('\n');
        if (prevLines.includes('const')) {
          diagnostics.push(new vscode.Diagnostic(
            new vscode.Range(lineIndex, line.indexOf('move'), lineIndex, line.indexOf('move') + 4),
            'Cannot move a const variable',
            vscode.DiagnosticSeverity.Error
          ));
        }
      }
    });

    return diagnostics;
  }

  isInSwitch(lines, currentLine) {
    let depth = 0;
    for (let i = currentLine - 1; i >= 0; i--) {
      depth += (lines[i].match(/{/g) || []).length;
      depth -= (lines[i].match(/}/g) || []).length;
      if (lines[i].includes('switch') && depth <= 0) {
        return true;
      }
      if (depth < 0) break;
    }
    return false;
  }
}

function activate(context) {
  const diagnosticsProvider = new CShiftDiagnosticsProvider();
  
  // Register completion provider
  const completionProvider = vscode.languages.registerCompletionItemProvider(
    'cshift',
    new CShiftCompletionProvider(),
    ''
  );

  // Register hover provider
  const hoverProvider = vscode.languages.registerHoverProvider(
    'cshift',
    new CShiftHoverProvider()
  );

  // Register diagnostic provider
  if (vscode.workspace.workspaceFolders) {
    vscode.workspace.onDidChangeTextDocument(event => {
      if (event.document.languageId === 'cshift') {
        const diags = diagnosticsProvider.provideDiagnostics(event.document);
        diagnosticsProvider.diagnostics.set(event.document.uri, diags);
      }
    });

    // Initial diagnostics on active editor
    if (vscode.window.activeTextEditor && vscode.window.activeTextEditor.document.languageId === 'cshift') {
      const diags = diagnosticsProvider.provideDiagnostics(vscode.window.activeTextEditor.document);
      diagnosticsProvider.diagnostics.set(vscode.window.activeTextEditor.document.uri, diags);
    }
  }

  // Register compile command
  const compileCommand = vscode.commands.registerCommand('cshift.compileFile', () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cshift') {
      vscode.window.showErrorMessage('No C<< file is currently active');
      return;
    }

    const filePath = editor.document.fileName;
    const outputFile = filePath.replace(/\.cll$/, '');

    vscode.window.showInformationMessage(`Compiling ${path.basename(filePath)}...`);

    exec(`cshift "${filePath}" -o "${outputFile}"`, (error, stdout, stderr) => {
      if (error) {
        vscode.window.showErrorMessage(`Compilation failed: ${stderr || error.message}`);
        diagnosticsProvider.diagnostics.clear();
        const lines = editor.document.getText().split('\n');
        const diags = [];
        stderr.split('\n').forEach(line => {
          const match = line.match(/line\s+(\d+)/i);
          if (match) {
            const lineNum = parseInt(match[1]) - 1;
            diags.push(new vscode.Diagnostic(
              new vscode.Range(lineNum, 0, lineNum, lines[lineNum].length),
              line,
              vscode.DiagnosticSeverity.Error
            ));
          }
        });
        diagnosticsProvider.diagnostics.set(editor.document.uri, diags);
      } else {
        vscode.window.showInformationMessage(`Compilation successful: ${outputFile}`);
        diagnosticsProvider.diagnostics.set(editor.document.uri, []);
      }
    });
  });

  // Register format command
  const formatCommand = vscode.commands.registerCommand('cshift.formatDocument', () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cshift') {
      return;
    }

    const document = editor.document;
    const fullRange = new vscode.Range(0, 0, document.lineCount, 0);
    const formatted = formatCShiftCode(document.getText());
    
    editor.edit(editBuilder => {
      editBuilder.replace(fullRange, formatted);
    });
  });

  context.subscriptions.push(
    completionProvider,
    hoverProvider,
    compileCommand,
    formatCommand,
    diagnosticsProvider.diagnostics
  );
}

function formatCShiftCode(code) {
  // Basic formatting: standardize indentation and spacing
  let lines = code.split('\n');
  let indentLevel = 0;
  const formatted = lines.map(line => {
    const trimmed = line.trim();
    
    if (trimmed.match(/^}/)) {
      indentLevel = Math.max(0, indentLevel - 1);
    }
    
    const formatted = '  '.repeat(indentLevel) + trimmed;
    
    if (trimmed.match(/{$/)) {
      indentLevel++;
    }
    
    return formatted;
  });

  return formatted.join('\n');
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};

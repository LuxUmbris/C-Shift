'use strict';
const vscode = require('vscode');
const { exec, execFile } = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

// ── Language constants ──────────────────────────────────────────────────────

const KEYWORDS = [
  'if', 'else', 'while', 'for', 'foreach', 'switch', 'case', 'default',
  'break', 'continue', 'def', 'export', 'struct', 'enum', 'namespace',
  'const', 'reserve', 'import', 'entry', 'tunnel', 'move', 'reset',
  'valid', 'voided', 'template', 'typename', 'true', 'false', 'raw'
];

const TYPES = [
  'int8', 'int16', 'int32', 'int64',
  'uint8', 'uint16', 'uint32', 'uint64',
  'float32', 'float64', 'bool', 'char', 'string', 'voided'
];

const CONTAINERS = [
  'Vector', 'HashMap', 'LinkedList', 'Set', 'Deque', 'RingBuffer', 'Pool',
  'Pair', 'Lazy', 'BitSet', 'Guard', 'StringBuilder', 'Buffer', 'SortedVec'
];

const STD_FUNS = [
  'printf', 'puts', 'putchar', 'scanf', 'getchar', 'gets_s',
  'malloc', 'free', 'calloc', 'realloc',
  'strlen', 'strcmp', 'strcpy', 'strcat', 'strncmp', 'strncpy',
  'sprintf', 'snprintf', 'sscanf',
  'fopen', 'fclose', 'fread', 'fwrite',
  'exit', 'abort',
  'vec_new', 'vec_push', 'vec_get', 'vec_len', 'vec_free',
  'map_new', 'map_set', 'map_get', 'map_free',
  'sb_new', 'sb_append', 'sb_build', 'sb_free',
  '__cshift_arena_init', '__cshift_arena_push',
  '__cshift_arena_free_all', '__cshift_arena_reset'
];

// ── Configuration helpers ───────────────────────────────────────────────────

function getConfig() {
  return vscode.workspace.getConfiguration('cshift');
}

function getCompilerPath() {
  return getConfig().get('compilerPath') || 'cshift';
}

function getStdPath() {
  return getConfig().get('stdPath') || '';
}

function getCheckDelay() {
  return getConfig().get('checkDelay') ?? 800;
}

// ── Compiler-backed diagnostics ─────────────────────────────────────────────
// Runs `cshift --check-only` and parses the structured output into VS Code
// diagnostics with exact line+column highlighting.

class CShiftDiagnosticsProvider {
  constructor() {
    this.collection = vscode.languages.createDiagnosticCollection('cshift');
    this._timers = new Map();   // uri string → NodeJS.Timeout
  }

  // Schedule a debounced check (avoids hammering the compiler on every keystroke)
  schedule(document) {
    const key = document.uri.toString();
    if (this._timers.has(key)) clearTimeout(this._timers.get(key));
    const delay = getCheckDelay();
    this._timers.set(key, setTimeout(() => {
      this._timers.delete(key);
      this.check(document);
    }, delay));
  }

  check(document) {
    if (document.languageId !== 'cshift') return;

    const text = document.getText();
    const compilerPath = getCompilerPath();
    const stdPath = getStdPath();

    // Write to a temp file so unsaved changes are also checked
    const tmpFile = path.join(os.tmpdir(), `_cshift_check_${Date.now()}.cll`);
    try {
      fs.writeFileSync(tmpFile, text, 'utf8');
    } catch (e) {
      return;
    }

    const env = Object.assign({}, process.env);
    if (stdPath) env.CSHIFT_STD_PATH = stdPath;

    const args = [tmpFile, '--check-only'];
    execFile(compilerPath, args, { env, timeout: 10000 }, (err, stdout, stderr) => {
      fs.unlink(tmpFile, () => {});

      const combined = (stdout || '') + (stderr || '');
      const diagnostics = this._parse(combined, document);
      this.collection.set(document.uri, diagnostics);
    });
  }

  _parse(output, document) {
    const diagnostics = [];
    // Matches:
    //   [CHECKER ERROR] Line 12: message
    //   [CHECKER WARNING] Line 12: message
    //   [IR VERIFY ERROR] message (no line)
    //   [MODULE ERROR] message
    const lineRe = /\[(CHECKER ERROR|CHECKER WARNING|IR VERIFY ERROR|MODULE ERROR|PARSE ERROR)[^\]]*\]\s+(?:Line\s+(\d+):\s*)?(.+)/gi;
    let m;
    while ((m = lineRe.exec(output)) !== null) {
      const tag = m[1].toUpperCase();
      const lineNum = m[2] ? parseInt(m[2], 10) - 1 : 0;
      const message = m[3].trim();
      const severity = tag.includes('WARNING')
        ? vscode.DiagnosticSeverity.Warning
        : vscode.DiagnosticSeverity.Error;

      // Clamp to document bounds
      const safeLine = Math.max(0, Math.min(lineNum, document.lineCount - 1));
      const lineText = document.lineAt(safeLine).text;

      // Try to highlight the offending token if mentioned in the message
      let startCol = 0;
      let endCol = lineText.length || 1;
      const tokenMatch = message.match(/['`"]([^'`"]+)['`"]/);
      if (tokenMatch) {
        const tok = tokenMatch[1];
        const idx = lineText.indexOf(tok);
        if (idx !== -1) {
          startCol = idx;
          endCol = idx + tok.length;
        }
      }

      const range = new vscode.Range(safeLine, startCol, safeLine, endCol);
      const diag = new vscode.Diagnostic(range, message, severity);
      diag.source = 'C<< compiler';

      // Tag voided-state errors with a special code for code-actions
      if (message.includes('voided') || message.includes('may be voided')) {
        diag.code = 'voided-state';
      }

      diagnostics.push(diag);
    }
    return diagnostics;
  }

  dispose() {
    this.collection.dispose();
    for (const t of this._timers.values()) clearTimeout(t);
  }
}

// ── Completion provider ─────────────────────────────────────────────────────

class CShiftCompletionProvider {
  provideCompletionItems(document, position) {
    const items = [];
    const line = document.lineAt(position).text.slice(0, position.character);

    KEYWORDS.forEach(kw => {
      const item = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
      item.detail = 'C<< keyword';
      items.push(item);
    });

    TYPES.forEach(t => {
      const item = new vscode.CompletionItem(t, vscode.CompletionItemKind.TypeParameter);
      item.detail = 'Primitive type';
      items.push(item);
    });

    CONTAINERS.forEach(c => {
      const item = new vscode.CompletionItem(c, vscode.CompletionItemKind.Struct);
      item.detail = 'Generic container (std)';
      item.insertText = new vscode.SnippetString(`${c}<\${1:T}>`);
      item.documentation = new vscode.MarkdownString(
        `Arena-managed \`${c}<T>\` from std.  \nFreed automatically on scope exit.`
      );
      items.push(item);
    });

    STD_FUNS.forEach(fn => {
      const item = new vscode.CompletionItem(fn, vscode.CompletionItemKind.Function);
      item.detail = 'std / C runtime';
      item.insertText = new vscode.SnippetString(`${fn}($0)`);
      items.push(item);
    });

    // Inline snippets
    const snippets = [
      {
        label: 'def',
        insert: 'def ${1:name}(${2:params})\n{\n\t$0\n}',
        doc: 'Function definition'
      },
      {
        label: 'export def',
        insert: 'export def ${1:name}(${2:params})\n{\n\t$0\n}',
        doc: 'Exported C-ABI function'
      },
      {
        label: 'tunnel',
        insert: 'tunnel ${1:expr} -> ${2:type} ${3:name};',
        doc: 'Tunnel a value to the caller'
      },
      {
        label: 'reserve',
        insert: 'reserve ${1:type} ${2:name} = ${3:call}();',
        doc: 'Reserve + call in one line'
      },
      {
        label: 'switch valid/voided',
        insert: 'switch (${1:var})\n{\n\tcase valid:\n\t\t$0\n\tcase voided:\n\t\t\n}',
        doc: 'Voided-state guard'
      },
      {
        label: 'vec',
        insert: 'Vector<${1:int32}> ${2:v} = vec_new(${3:16});',
        doc: 'Arena-managed Vector'
      },
      {
        label: 'for',
        insert: 'for (${1:int32} ${2:i} = ${3:0}; ${2:i} < ${4:n};)\n{\n\t$0\n\t${2:i} += 1;\n}',
        doc: 'for loop'
      },
      {
        label: 'foreach',
        insert: 'foreach (${1:int32} ${2:val} : ${3:arr})\n{\n\t$0\n}',
        doc: 'foreach loop'
      },
      {
        label: 'struct',
        insert: 'struct ${1:Name}\n{\n\t${2:int32 field};\n}',
        doc: 'Struct definition'
      },
      {
        label: 'template struct',
        insert: 'template<typename ${1:T}>\nstruct ${2:Name}\n{\n\t${1:T} ${3:data};\n}',
        doc: 'Generic template struct'
      },
      {
        label: 'import C',
        insert: 'import ${1:voided} ${2:name}(${3:params});',
        doc: 'Import a C function'
      },
      {
        label: 'entry',
        insert: 'entry\n{\n\t$0\n}',
        doc: 'Program entry point'
      },
      {
        label: 'reset',
        insert: 'reset;',
        doc: 'Free all arena data in current scope (keep scope alive)'
      }
    ];

    snippets.forEach(s => {
      const item = new vscode.CompletionItem(s.label, vscode.CompletionItemKind.Snippet);
      item.insertText = new vscode.SnippetString(s.insert);
      item.documentation = new vscode.MarkdownString(s.doc);
      item.detail = 'Snippet';
      items.push(item);
    });

    return items;
  }
}

// ── Hover provider ──────────────────────────────────────────────────────────

class CShiftHoverProvider {
  provideHover(document, position) {
    const wordRange = document.getWordRangeAtPosition(position);
    if (!wordRange) return null;
    const word = document.getText(wordRange);

    const docs = {
      def:       '**`def name(params) { … }`**\n\nDefine a function. Returns values via `tunnel`.',
      'export':  '**`export def …`**\n\nGives the function external C-ABI linkage (visible to the linker).',
      tunnel:    '**`tunnel expr -> type name;`**\n\nOutput a value from a function into the caller\'s `reserve`d slot.',
      reserve:   '**`reserve type name = call();`**\n\nDeclare a variable filled by the next matching `tunnel`.',
      move:      '**`move var;`**\n\nVoid a variable. After `move`, access requires a `switch` guard.',
      reset:     '**`reset;`**\n\nFree all heap data tracked by the current scope\'s arena — without exiting the scope. Pointers into this arena held by child scopes become dangling.',
      valid:     'Guard arm: variable is in valid state.',
      voided:    'Guard arm: variable has been `move`d.',
      entry:     '**`entry { … }`**\n\nProgram entry point, compiles to `main()`.',
      import:    '**`import std;`** or **`import "header.h";`** or **`import type fn(…);`**\n\nImport a module, C header, or individual C function.',
      template:  '**`template<typename T> struct/def …`**\n\nGeneric definition — monomorphically instantiated at compile time.',
      struct:    '**`struct Name { fields }`**\n\nData-only aggregate type (no methods).',
      enum:      '**`enum Name { A, B, C }`**\n\nInteger-backed enumeration.',
      namespace: '**`namespace Name { … }`**\n\nLexical grouping — does **not** create a new arena.',
      Vector:    '**`Vector<T>`** — arena-managed dynamic array from `std`.\n\nFreed automatically when its scope exits.',
      HashMap:   '**`HashMap<K,V>`** — arena-managed hash map from `std`.',
      int32:     '32-bit signed integer.',
      int64:     '64-bit signed integer.',
      uint64:    '64-bit unsigned integer.',
      float32:   '32-bit IEEE-754 float.',
      float64:   '64-bit IEEE-754 float (double).',
      string:    'Pointer to a null-terminated C string (`i8*` in IR).',
    };

    if (docs[word]) {
      return new vscode.Hover(new vscode.MarkdownString(docs[word]));
    }
    return null;
  }
}

// ── Code actions: quick-fix for voided-state errors ─────────────────────────

class CShiftCodeActionProvider {
  provideCodeActions(document, range, context) {
    const actions = [];
    for (const diag of context.diagnostics) {
      if (diag.code !== 'voided-state') continue;

      // Extract variable name from message
      const varMatch = diag.message.match(/[Vv]ariable\s+'([^']+)'/);
      if (!varMatch) continue;
      const varName = varMatch[1];

      const fix = new vscode.CodeAction(
        `Wrap '${varName}' in voided-state guard`,
        vscode.CodeActionKind.QuickFix
      );
      fix.diagnostics = [diag];

      // Replace the offending line with a switch guard around it
      const lineText = document.lineAt(diag.range.start.line).text;
      const indent = lineText.match(/^(\s*)/)[1];
      const stmt = lineText.trim();
      fix.edit = new vscode.WorkspaceEdit();
      fix.edit.replace(
        document.uri,
        new vscode.Range(diag.range.start.line, 0, diag.range.start.line, lineText.length),
        `${indent}switch (${varName})\n${indent}{\n${indent}    case valid:\n${indent}        ${stmt}\n${indent}    case voided:\n${indent}        // handle voided ${varName}\n${indent}}`
      );
      fix.isPreferred = true;
      actions.push(fix);
    }
    return actions;
  }
}

// ── Document formatter ───────────────────────────────────────────────────────

class CShiftDocumentFormatter {
  provideDocumentFormattingEdits(document) {
    const text = document.getText();
    const formatted = formatCode(text);
    const fullRange = new vscode.Range(
      0, 0, document.lineCount - 1,
      document.lineAt(document.lineCount - 1).text.length
    );
    return [vscode.TextEdit.replace(fullRange, formatted)];
  }
}

function formatCode(code) {
  const lines = code.split('\n');
  let indent = 0;
  return lines.map(raw => {
    const t = raw.trim();
    if (!t) return '';

    // Dedent before emitting closing brace
    if (/^}/.test(t)) indent = Math.max(0, indent - 1);

    const out = '    '.repeat(indent) + t;

    // Indent after opening brace
    if (/{$/.test(t)) indent++;

    return out;
  }).join('\n');
}

// ── Compile command ──────────────────────────────────────────────────────────

function registerCompileCommand(context, diagnosticsProvider) {
  return vscode.commands.registerCommand('cshift.compileFile', async () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cshift') {
      vscode.window.showErrorMessage('No C<< file is active.');
      return;
    }

    await editor.document.save();
    const filePath = editor.document.fileName;
    const outFile = filePath.replace(/\.cll$/, '');
    const compiler = getCompilerPath();
    const stdPath = getStdPath();

    const env = Object.assign({}, process.env);
    if (stdPath) env.CSHIFT_STD_PATH = stdPath;

    vscode.window.withProgress(
      { location: vscode.ProgressLocation.Notification, title: `Compiling ${path.basename(filePath)}…`, cancellable: false },
      () => new Promise(resolve => {
        execFile(compiler, [filePath, '-o', outFile], { env, timeout: 30000 }, (err, stdout, stderr) => {
          resolve();
          const combined = (stdout || '') + (stderr || '');
          const diags = diagnosticsProvider._parse(combined, editor.document);
          diagnosticsProvider.collection.set(editor.document.uri, diags);

          if (err) {
            vscode.window.showErrorMessage(`Build failed — ${diags.length} error(s). See Problems panel.`);
          } else {
            diagnosticsProvider.collection.set(editor.document.uri, []);
            vscode.window.showInformationMessage(`Built: ${path.basename(outFile)}`);
          }
        });
      })
    );
  });
}

// ── Check-syntax command ─────────────────────────────────────────────────────

function registerCheckCommand(context, diagnosticsProvider) {
  return vscode.commands.registerCommand('cshift.checkSyntax', () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== 'cshift') return;
    diagnosticsProvider.check(editor.document);
    vscode.window.showInformationMessage('C<< check running…');
  });
}

// ── Extension lifecycle ──────────────────────────────────────────────────────

function activate(context) {
  const diagnostics = new CShiftDiagnosticsProvider();

  // Run checker on open and change
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(doc => {
      if (doc.languageId === 'cshift') diagnostics.check(doc);
    }),
    vscode.workspace.onDidChangeTextDocument(e => {
      if (e.document.languageId === 'cshift') diagnostics.schedule(e.document);
    }),
    vscode.workspace.onDidSaveTextDocument(doc => {
      if (doc.languageId === 'cshift') diagnostics.check(doc);
    }),
    vscode.workspace.onDidCloseTextDocument(doc => {
      diagnostics.collection.delete(doc.uri);
    })
  );

  // Check all already-open C<< docs
  vscode.workspace.textDocuments.forEach(doc => {
    if (doc.languageId === 'cshift') diagnostics.check(doc);
  });

  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider('cshift', new CShiftCompletionProvider(), '.', '<', '"'),
    vscode.languages.registerHoverProvider('cshift', new CShiftHoverProvider()),
    vscode.languages.registerCodeActionsProvider('cshift', new CShiftCodeActionProvider(),
      { providedCodeActionKinds: [vscode.CodeActionKind.QuickFix] }),
    vscode.languages.registerDocumentFormattingEditProvider('cshift', new CShiftDocumentFormatter()),
    registerCompileCommand(context, diagnostics),
    registerCheckCommand(context, diagnostics),
    diagnostics.collection
  );
}

function deactivate() {}

module.exports = { activate, deactivate };

#!/usr/bin/env node

/**
 * Basic test suite for C<< Language Support extension
 */

const assert = require('assert');
const fs = require('fs');
const path = require('path');

console.log('🧪 Testing C<< Language Support Extension...\n');

// Test 1: Check file structure
console.log('Test 1: Checking file structure...');
const requiredFiles = [
  'package.json',
  'extension.js',
  'language-configuration.json',
  'syntaxes/cshift.tmLanguage.json',
  'snippets/cshift.json',
  'README.md'
];

requiredFiles.forEach(file => {
  const filePath = path.join(__dirname, file);
  assert(fs.existsSync(filePath), `Missing required file: ${file}`);
  console.log(`  ✓ ${file}`);
});

// Test 2: Validate JSON files
console.log('\nTest 2: Validating JSON syntax...');
const jsonFiles = [
  'package.json',
  'language-configuration.json',
  'syntaxes/cshift.tmLanguage.json',
  'snippets/cshift.json'
];

jsonFiles.forEach(file => {
  try {
    const content = fs.readFileSync(path.join(__dirname, file), 'utf-8');
    JSON.parse(content);
    console.log(`  ✓ ${file}`);
  } catch (e) {
    console.error(`  ✗ ${file}: ${e.message}`);
    process.exit(1);
  }
});

// Test 3: Check package.json metadata
console.log('\nTest 3: Checking package.json metadata...');
const pkg = JSON.parse(fs.readFileSync(path.join(__dirname, 'package.json'), 'utf-8'));
assert(pkg.name === 'cshift-language', 'Invalid package name');
assert(pkg.version === '1.1.0', 'Invalid version number');
assert(pkg.engines.vscode, 'Missing VS Code engine requirement');
assert(pkg.contributes.languages, 'Missing language contribution');
assert(pkg.contributes.grammars, 'Missing grammar contribution');
assert(pkg.contributes.snippets, 'Missing snippet contribution');
console.log(`  ✓ Name: ${pkg.name}`);
console.log(`  ✓ Version: ${pkg.version}`);
console.log(`  ✓ VS Code: ${pkg.engines.vscode}`);
console.log(`  ✓ Language ID: cshift`);

// Test 4: Check grammar file
console.log('\nTest 4: Checking grammar file...');
const grammar = JSON.parse(fs.readFileSync(path.join(__dirname, 'syntaxes/cshift.tmLanguage.json'), 'utf-8'));
assert(grammar.scopeName === 'source.cshift', 'Invalid scope name');
assert(grammar.patterns, 'Missing patterns in grammar');
assert(grammar.repository, 'Missing repository in grammar');
assert(grammar.repository.keywords, 'Missing keywords in repository');
console.log(`  ✓ Scope: ${grammar.scopeName}`);
console.log(`  ✓ Patterns: ${grammar.patterns.length}`);
console.log(`  ✓ Repository rules: ${Object.keys(grammar.repository).length}`);

// Test 5: Check snippets
console.log('\nTest 5: Checking snippets...');
const snippets = JSON.parse(fs.readFileSync(path.join(__dirname, 'snippets/cshift.json'), 'utf-8'));
const snippetCount = Object.keys(snippets).length;
assert(snippetCount > 0, 'No snippets found');
console.log(`  ✓ Snippet count: ${snippetCount}`);
const sampleSnippet = snippets['Function Definition'];
assert(sampleSnippet.prefix === 'fn', 'Invalid snippet prefix');
assert(sampleSnippet.body, 'Snippet missing body');
console.log(`  ✓ Sample: ${sampleSnippet.prefix} → ${sampleSnippet.description}`);

// Test 6: Validate extension.js
console.log('\nTest 6: Validating extension.js...');
try {
  const ext = require('./extension.js');
  assert(typeof ext.activate === 'function', 'Missing activate function');
  assert(typeof ext.deactivate === 'function', 'Missing deactivate function');
  console.log('  ✓ Activate function exists');
  console.log('  ✓ Deactivate function exists');
} catch (e) {
  console.error(`  ✗ Extension validation failed: ${e.message}`);
  process.exit(1);
}

console.log('\n✅ All tests passed!\n');

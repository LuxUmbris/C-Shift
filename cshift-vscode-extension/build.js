#!/usr/bin/env node

/**
 * Build script for C<< VS Code Extension
 * Packages the extension into a .vsix file
 */

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const packageJson = require('./package.json');
const version = packageJson.version;

console.log(`📦 Building C<< Language Support v${version}...`);

// Check if vsce is installed
try {
  execSync('which vsce', { stdio: 'ignore' });
} catch (e) {
  console.error('❌ vsce not found. Install it with: npm install -g @vscode/vsce');
  process.exit(1);
}

// Create dist directory
const distDir = path.join(__dirname, 'dist');
if (!fs.existsSync(distDir)) {
  fs.mkdirSync(distDir, { recursive: true });
}

// Package the extension
try {
  console.log('🔨 Packaging extension...');
  const output = execSync(`vsce package -o dist/cshift-language-${version}.vsix`, {
    cwd: __dirname,
    encoding: 'utf-8'
  });
  console.log(output);
  console.log(`✅ Successfully built: dist/cshift-language-${version}.vsix`);
} catch (error) {
  console.error('❌ Build failed:');
  console.error(error.message);
  process.exit(1);
}

#Requires -Version 5.1
# ============================================================
# C<< (cshift) Windows Installer
#
# Usage (run in the extracted release directory):
#
#   # User-local install (no elevation needed):
#   .\install.ps1
#
#   # System-wide (run PowerShell as Administrator):
#   .\install.ps1 -InstallDir "C:\Program Files\cshift"
#
#   # Explicit user-local:
#   .\install.ps1 -InstallDir "$env:LOCALAPPDATA\cshift"
# ============================================================

[CmdletBinding()]
param(
    [string]$InstallDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── Defaults ──────────────────────────────────────────────────────────────────

$IsAdmin = ([Security.Principal.WindowsPrincipal]
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $InstallDir) {
    $InstallDir = if ($IsAdmin) { "C:\Program Files\cshift" }
                  else          { "$env:LOCALAPPDATA\cshift" }
}

$EnvTarget   = if ($IsAdmin) { "Machine" } else { "User" }
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Manifest    = Join-Path $InstallDir "share\cshift\.install_manifest"

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " C<< (cshift) Installer" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " Install directory : $InstallDir"
Write-Host " Package source    : $ScriptDir"
Write-Host " Environment scope : $EnvTarget"
Write-Host ""

# ── Remove previous installation ──────────────────────────────────────────────

function Remove-OldInstall {
    if (Test-Path $Manifest) {
        Write-Host "Removing previous installation (from manifest)..." -ForegroundColor Yellow
        $files = Get-Content $Manifest -ErrorAction SilentlyContinue
        foreach ($f in $files) {
            $f = $f.Trim()
            if ($f -and (Test-Path $f)) {
                Remove-Item -Force $f -ErrorAction SilentlyContinue
            }
        }
        # Prune empty directories deepest-first
        $dirs = $files | ForEach-Object { Split-Path $_ } | Sort-Object -Descending -Unique
        foreach ($d in $dirs) {
            if ((Test-Path $d) -and -not (Get-ChildItem $d -ErrorAction SilentlyContinue)) {
                Remove-Item -Force $d -ErrorAction SilentlyContinue
            }
        }
        Remove-Item -Force $Manifest -ErrorAction SilentlyContinue
        Write-Host "Previous installation removed." -ForegroundColor Green
    }
    elseif (Test-Path (Join-Path $InstallDir "bin\cshift.exe")) {
        Write-Host "Removing legacy installation (no manifest)..." -ForegroundColor Yellow
        @("bin\cshift.exe", "share\cshift", "lib\cshift") | ForEach-Object {
            $p = Join-Path $InstallDir $_
            if (Test-Path $p) { Remove-Item -Recurse -Force $p -ErrorAction SilentlyContinue }
        }
        Write-Host "Legacy installation removed." -ForegroundColor Green
    }
}

Remove-OldInstall

# ── Install new files ─────────────────────────────────────────────────────────

Write-Host "Installing..." -ForegroundColor Cyan
$installed = [System.Collections.Generic.List[string]]::new()

function Install-Tree {
    param([string]$Src, [string]$Dst)
    if (-not (Test-Path $Src)) { return }
    Get-ChildItem -Recurse -File $Src | ForEach-Object {
        $rel  = $_.FullName.Substring($Src.Length).TrimStart('\','/')
        $dest = Join-Path $Dst $rel
        $dir  = Split-Path $dest
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        Copy-Item -Force $_.FullName $dest
        $installed.Add($dest)
        Write-Host "  install: $dest"
    }
}

Install-Tree (Join-Path $ScriptDir "bin")   (Join-Path $InstallDir "bin")
Install-Tree (Join-Path $ScriptDir "share") (Join-Path $InstallDir "share")
Install-Tree (Join-Path $ScriptDir "lib")   (Join-Path $InstallDir "lib")

# Write manifest
$manifestDir = Join-Path $InstallDir "share\cshift"
if (-not (Test-Path $manifestDir)) { New-Item -ItemType Directory -Force $manifestDir | Out-Null }
$installed | Set-Content $Manifest -Encoding UTF8

Write-Host ""
Write-Host "Installed $($installed.Count) files." -ForegroundColor Green

# ── Environment variables ──────────────────────────────────────────────────────

$CshiftStd = Join-Path $InstallDir "share\cshift"
$CshiftBin = Join-Path $InstallDir "bin"

# CSHIFT_STD_PATH
[Environment]::SetEnvironmentVariable("CSHIFT_STD_PATH", $CshiftStd, $EnvTarget)
Write-Host ""
Write-Host "Set $EnvTarget env: CSHIFT_STD_PATH = $CshiftStd" -ForegroundColor Yellow

# PATH — add bin only if not already present
$currentPath = [Environment]::GetEnvironmentVariable("PATH", $EnvTarget)
if ($currentPath -notlike "*$CshiftBin*") {
    [Environment]::SetEnvironmentVariable("PATH", "$currentPath;$CshiftBin", $EnvTarget)
    Write-Host "Added to $EnvTarget PATH: $CshiftBin" -ForegroundColor Yellow
}
else {
    Write-Host "PATH already contains: $CshiftBin" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " cshift installed successfully!" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Restart your terminal for PATH changes to take effect."
Write-Host ""

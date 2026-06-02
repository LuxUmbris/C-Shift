#Requires -Version 5.1
# ============================================================
# C<< (cshift) Windows Uninstaller
#
# Usage:
#   .\uninstall.ps1                                  # auto-detect install dir
#   .\uninstall.ps1 -InstallDir "C:\Program Files\cshift"
#   .\uninstall.ps1 -InstallDir "$env:LOCALAPPDATA\cshift"
# ============================================================

[CmdletBinding()]
param(
    [string]$InstallDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$IsAdmin = ([Security.Principal.WindowsPrincipal]
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $InstallDir) {
    # Try to auto-detect: check both locations
    $candidates = @(
        "$env:LOCALAPPDATA\cshift",
        "C:\Program Files\cshift"
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "bin\cshift.exe")) {
            $InstallDir = $c
            break
        }
    }
    if (-not $InstallDir) {
        Write-Host "cshift does not appear to be installed in any known location." -ForegroundColor Yellow
        exit 0
    }
}

$EnvTarget = if ($IsAdmin) { "Machine" } else { "User" }
$Manifest  = Join-Path $InstallDir "share\cshift\.install_manifest"

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " C<< (cshift) Uninstaller" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " Directory : $InstallDir"
Write-Host ""

if (-not (Test-Path $InstallDir)) {
    Write-Host "Directory not found — nothing to remove." -ForegroundColor Yellow
    exit 0
}

$removed = 0
$missing = 0

if (Test-Path $Manifest) {
    Write-Host "Removing files from manifest..." -ForegroundColor Yellow
    $files = Get-Content $Manifest -ErrorAction SilentlyContinue
    foreach ($f in $files) {
        $f = $f.Trim()
        if (-not $f) { continue }
        if (Test-Path $f) {
            Remove-Item -Force $f -ErrorAction SilentlyContinue
            Write-Host "  removed: $f"
            $removed++
        } else {
            $missing++
        }
    }
    Remove-Item -Force $Manifest -ErrorAction SilentlyContinue

    # Prune empty directories deepest-first
    $dirs = $files | ForEach-Object { Split-Path $_.Trim() } |
            Where-Object { $_ } | Sort-Object -Descending -Unique
    foreach ($d in $dirs) {
        if ((Test-Path $d) -and -not (Get-ChildItem $d -ErrorAction SilentlyContinue)) {
            Remove-Item -Force $d -ErrorAction SilentlyContinue
            Write-Host "  pruned:  $d"
        }
    }
} else {
    Write-Host "No manifest found — removing known paths..." -ForegroundColor Yellow
    foreach ($rel in @("bin\cshift.exe", "share\cshift", "lib\cshift")) {
        $p = Join-Path $InstallDir $rel
        if (Test-Path $p) {
            Remove-Item -Recurse -Force $p -ErrorAction SilentlyContinue
            Write-Host "  removed: $p"
            $removed++
        }
    }
}

# Remove the top-level install dir if now empty
if ((Test-Path $InstallDir) -and -not (Get-ChildItem $InstallDir -ErrorAction SilentlyContinue)) {
    Remove-Item -Force $InstallDir -ErrorAction SilentlyContinue
    Write-Host "  pruned:  $InstallDir"
}

# ── Clean environment variables ───────────────────────────────────────────────

$stdVal = [Environment]::GetEnvironmentVariable("CSHIFT_STD_PATH", $EnvTarget)
if ($stdVal) {
    [Environment]::SetEnvironmentVariable("CSHIFT_STD_PATH", $null, $EnvTarget)
    Write-Host "Removed $EnvTarget env: CSHIFT_STD_PATH" -ForegroundColor Yellow
}

$CshiftBin   = Join-Path $InstallDir "bin"
$currentPath = [Environment]::GetEnvironmentVariable("PATH", $EnvTarget)
if ($currentPath -like "*$CshiftBin*") {
    $newPath = ($currentPath -split ';' | Where-Object { $_ -ne $CshiftBin }) -join ';'
    [Environment]::SetEnvironmentVariable("PATH", $newPath, $EnvTarget)
    Write-Host "Removed from $EnvTarget PATH: $CshiftBin" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " Uninstall complete  ($removed removed, $missing already absent)" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Restart your terminal for environment changes to take effect."
Write-Host ""

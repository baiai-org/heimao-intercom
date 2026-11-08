#Requires -Version 5.1
<#
  OpenRadioLink-v0.3  --  one-shot toolchain bootstrap
  ================================================================
  Installs esptool into the project-local  .pylib  folder so the flasher
  is self-contained and never fights with the machine's global Python.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\setup_toolchain.ps1

  ASCII-only messages on purpose (PowerShell 5.1 ANSI .ps1 handling).
#>
[CmdletBinding()]
param(
    [string]$EsptoolVersion = '4.8.1',
    [string]$IndexUrl = 'https://pypi.tuna.tsinghua.edu.cn/simple',
    [switch]$Force
)

$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$ToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ToolsDir
$PyLib = Join-Path $RepoRoot '.pylib'

Write-Host ''
Write-Host '  OpenRadioLink-v0.3 toolchain setup' -ForegroundColor Cyan
Write-Host "  target: $PyLib" -ForegroundColor Gray
Write-Host ''

# --- python ---
$py = $null
foreach ($cand in @('py', 'python')) {
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    if ($cmd) { $py = $cmd.Source; break }
}
if (-not $py) { throw 'Python not found. Install Python 3.9+ first.' }
Write-Host "  python launcher: $py" -ForegroundColor Gray
& $py -3.9 -c "import sys;print('  interpreter   :',sys.version.split()[0],sys.executable)"
if ($LASTEXITCODE -ne 0) { throw 'py -3.9 failed; a 64-bit Python 3.9 is required.' }

# --- already installed? ---
$probe = Join-Path $PyLib 'esptool'
if ((Test-Path $probe) -and -not $Force) {
    $old = $env:PYTHONPATH
    $env:PYTHONPATH = $PyLib
    try {
        $ver = & $py -3.9 -c "import esptool;print(esptool.__version__)" 2>$null
    } finally { $env:PYTHONPATH = $old }
    if ($ver) {
        Write-Host "  esptool already installed: $ver   (use -Force to reinstall)" -ForegroundColor Green
        Write-Host ''
        exit 0
    }
}

# --- install ---
Write-Host "  installing esptool==$EsptoolVersion  (this downloads ~15 MB)" -ForegroundColor Yellow
Write-Host ''
$env:PIP_USER = '0'   # machine pip config forces --user; --target needs it off
& $py -3.9 -m pip install `
    --target $PyLib `
    --no-cache-dir `
    --progress-bar off `
    --disable-pip-version-check `
    --timeout 60 --retries 2 `
    -i $IndexUrl `
    "esptool==$EsptoolVersion"
if ($LASTEXITCODE -ne 0) { throw "pip install failed (exit $LASTEXITCODE)" }

# --- verify ---
$old = $env:PYTHONPATH
$env:PYTHONPATH = $PyLib
try {
    & $py -3.9 -m esptool version
    if ($LASTEXITCODE -ne 0) { throw 'esptool verification failed' }
    & $py -3.9 -c "import serial;print('  pyserial',serial.__version__)"
} finally { $env:PYTHONPATH = $old }

Write-Host ''
Write-Host '  toolchain ready. Next:' -ForegroundColor Green
Write-Host '    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Action ports' -ForegroundColor Gray
Write-Host '    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Action probe' -ForegroundColor Gray
Write-Host ''

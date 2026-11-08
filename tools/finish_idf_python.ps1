#Requires -Version 5.1
<#
  Finish the ESP-IDF Python environment.

  install.bat normally installs requirements.core.txt into the IDF venv itself,
  but on this machine that step dies with:

      WARNING: Building wheel for esptool failed: [WinError 5] Access denied:
      'C:\Users\<user>\AppData\Local\pip'

  i.e. pip's *wheel cache* directory cannot be used by the child process.  The
  toolchains themselves download fine, so this script just redoes the Python
  half with the cache disabled and a fast index, then verifies idf.py works.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\finish_idf_python.ps1
    powershell -ExecutionPolicy Bypass -File tools\finish_idf_python.ps1 -IndexUrl https://pypi.org/simple

  ASCII-only messages (PowerShell 5.1 ANSI .ps1 handling).
#>
[CmdletBinding()]
param(
    [string]$IndexUrl = 'https://pypi.tuna.tsinghua.edu.cn/simple',
    [string]$ExtraIndexUrl = 'https://dl.espressif.com/pypi'
)

# pip and the native tools write to stderr constantly; Stop would abort on that.
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$ToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ToolsDir
$IdfDir = Join-Path $Root '.idf\esp-idf'
$IdfTools = Join-Path $Root '.idf\tools'
$PyEnv = Join-Path $IdfTools 'python_env\idf5.4_py3.11_env'
$VenvPy = Join-Path $PyEnv 'Scripts\python.exe'
$Constraints = Join-Path $IdfTools 'espidf.constraints.v5.4.txt'
$Requirements = Join-Path $IdfDir 'tools\requirements\requirements.core.txt'
$PipCache = Join-Path $Root '.idf\pip-cache'

function Ok($m)   { Write-Host "  [ ok ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red; exit 1 }

Write-Host ''
Write-Host '  finishing the ESP-IDF python environment' -ForegroundColor Cyan
Write-Host ''

if (-not (Test-Path $VenvPy)) { Die "IDF venv missing at $VenvPy -- run install_idf.ps1 first." }
if (-not (Test-Path $Requirements)) { Die "requirements file missing: $Requirements" }

# --- pip environment that works around the broken default cache -------------
New-Item -ItemType Directory -Force -Path $PipCache | Out-Null
$env:PIP_CACHE_DIR = $PipCache      # keep every wheel inside the project
$env:PIP_NO_CACHE_DIR = '0'
$env:PIP_USER = '0'                 # the machine pip config forces --user
$env:PIP_DISABLE_PIP_VERSION_CHECK = '1'
$env:PIP_INDEX_URL = $IndexUrl
$env:PIP_EXTRA_INDEX_URL = $ExtraIndexUrl

$ver = & $VenvPy --version 2>&1
Ok "venv interpreter: $ver"
Write-Host "  cache dir : $PipCache" -ForegroundColor Gray
Write-Host "  index     : $IndexUrl" -ForegroundColor Gray
Write-Host ''

$args = @('-m', 'pip', 'install', '--no-warn-script-location',
          '-r', $Requirements, '--upgrade')
if (Test-Path $Constraints) { $args += @('--constraint', $Constraints) }
if ($ExtraIndexUrl)         { $args += @('--extra-index-url', $ExtraIndexUrl) }

Write-Host '  installing requirements.core.txt ...' -ForegroundColor Yellow
& $VenvPy @args
$code = $LASTEXITCODE
if ($code -ne 0) {
    Die "pip failed with $code.  Retry with -IndexUrl https://pypi.org/simple if the mirror is short of a wheel."
}

Write-Host ''
Write-Host '  verifying: idf.py --version' -ForegroundColor Gray
foreach ($k in 'IDF_PATH', 'IDF_TOOLS_PATH', 'IDF_PYTHON_ENV_PATH') {
    Set-Item -Path "env:$k" -Value '' -ErrorAction SilentlyContinue
}
$env:IDF_PATH = $IdfDir
$env:IDF_TOOLS_PATH = $IdfTools
$env:IDF_PYTHON_ENV_PATH = $PyEnv

$out = & cmd.exe /c "call `"$IdfDir\export.bat`" && idf.py --version" 2>&1
$text = ($out | ForEach-Object { "$_" }) -join "`n"
if ($text -notmatch 'ESP-IDF v') {
    Write-Host $text
    Die "idf.py did not report a version"
}

Write-Host ''
Ok ("idf.py -> " + (($text -split "`n" | Where-Object { $_ -match 'ESP-IDF v' }) -join ' ').Trim())
Ok 'ESP-IDF is ready.  Build with: tools\idf.ps1 -Action build'
Write-Host ''

#Requires -Version 5.1
<#
  Install ESP-IDF into the project (self-contained, no admin, no global PATH).

  Layout produced:
    .idf\esp-idf\                  ESP-IDF source (git, v5.4.x)
    .idf\tools\                    IDF_TOOLS_PATH: toolchains, python venv, openocd
    .tools\Python311\              the interpreter IDF builds its venv from

  Everything lands under the project folder so it can be deleted with one
  Remove-Item and cannot break any other project on the machine.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\install_idf.ps1
    powershell -ExecutionPolicy Bypass -File tools\install_idf.ps1 -Version v5.4.1 -Targets esp32s3

  ASCII-only messages (PowerShell 5.1 ANSI .ps1 handling).
#>
[CmdletBinding()]
param(
    [string]$Version = 'v5.4.1',
    [string]$Targets = 'esp32s3',
    [string]$RepoUrl = 'https://github.com/espressif/esp-idf.git',
    [string]$AssetMirror = 'dl.espressif.cn/github_assets',
    [string]$PythonDir,          # defaults to <root>\.tools\Python311
    [switch]$SkipClone
)

# Native tools (git, cmd, pip) write progress and warnings to *stderr*.
# Under ErrorActionPreference='Stop' PowerShell 5.1 turns that into a
# terminating NativeCommandError and kills the script mid-install, so this
# script runs with 'Continue' and checks $LASTEXITCODE explicitly everywhere.
$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$ToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ToolsDir
$IdfRoot = Join-Path $Root '.idf'
$IdfDir = Join-Path $IdfRoot 'esp-idf'
$IdfTools = Join-Path $IdfRoot 'tools'
$Log = Join-Path $IdfRoot 'install.log'

if (-not $PythonDir) { $PythonDir = Join-Path $Root '.tools\Python311' }
$Py = Join-Path $PythonDir 'python.exe'
$PyEnv = Join-Path $IdfTools 'python_env\idf5.4_py3.11_env'

function Say($m)  { Write-Host "  $m" }
function Ok($m)   { Write-Host "  [ ok ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red; exit 1 }

Write-Host ''
Write-Host "  ESP-IDF $Version for OpenRadioLink-v0.3" -ForegroundColor Cyan
Write-Host "  idf : $IdfDir"
Write-Host "  py  : $Py"
Write-Host ''

New-Item -ItemType Directory -Force -Path $IdfRoot, $IdfTools | Out-Null

# ---------------------------------------------------------------- python
if (-not (Test-Path $Py)) {
    Die "Python not found at $Py -- run tools\install_python.ps1 first."
}
$pyVer = (& $Py --version 2>&1)
Ok "interpreter: $pyVer"
if ("$pyVer" -notmatch '3\.(9|10|11|12|13)') { Warn "ESP-IDF expects Python 3.9-3.13; got $pyVer" }

# ---------------------------------------------------------------- git
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die 'git is not on PATH.' }
$env:GIT_TERMINAL_PROMPT = '0'

# ---------------------------------------------------------------- clone
if ((Test-Path (Join-Path $IdfDir 'install.bat')) -and -not $SkipClone) {
    # A previous run may have died mid-clone, so verify the checkout is real
    # before trusting it: git must resolve HEAD and every submodule must be
    # populated.  Anything else gets re-cloned.
    $head = & git -C $IdfDir rev-parse --short HEAD 2>$null
    $sub = & git -C $IdfDir submodule status 2>$null
    $unpopulated = @($sub | Where-Object { $_ -match '^-' })
    if ($head -and $unpopulated.Count -eq 0) {
        Ok "ESP-IDF source present at $head (all submodules populated)"
        $SkipClone = $true
    } else {
        Warn ("incomplete clone detected (HEAD='$head', {0} unpopulated submodule(s))" -f $unpopulated.Count)
        Warn 're-cloning from scratch'
        Remove-Item $IdfDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not $SkipClone) {
    if (Test-Path $IdfDir) { Remove-Item $IdfDir -Recurse -Force }
    Write-Host "  cloning $RepoUrl  ($Version, shallow, 4 jobs)" -ForegroundColor Yellow
    Write-Host '  this is the slow part -- several hundred MB with submodules' -ForegroundColor Gray
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $env:GIT_TERMINAL_PROMPT = '0'
    & git -c advice.detachedHead=false clone --branch $Version --depth 1 `
        --shallow-submodules --recursive --jobs 4 $RepoUrl $IdfDir 2>&1 |
        ForEach-Object { Write-Host "    $_" }
    $code = $LASTEXITCODE
    $sw.Stop()
    if ($code -ne 0) {
        Die ("git clone failed (exit $code). If GitHub is unreachable from here, " +
             "retry with -RepoUrl https://jihulab.com/esp-mirror/esp-idf.git")
    }
    Ok ("cloned in {0:N1} min" -f $sw.Elapsed.TotalMinutes)

    $sub = & git -C $IdfDir submodule status 2>$null
    $unpopulated = @($sub | Where-Object { $_ -match '^-' })
    if ($unpopulated.Count -gt 0) {
        Warn ("{0} submodule(s) still unpopulated; running a second update pass" -f $unpopulated.Count)
        & git -C $IdfDir submodule update --init --recursive --depth 1 2>&1 |
            ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) { Die 'submodule update failed' }
    }
}

if (-not (Test-Path (Join-Path $IdfDir 'install.bat'))) { Die "install.bat not found under $IdfDir" }

# ---------------------------------------------------------------- install tools
$env:IDF_PATH = $IdfDir
$env:IDF_TOOLS_PATH = $IdfTools
$env:IDF_PYTHON_ENV_PATH = $PyEnv
$env:IDF_GITHUB_ASSETS = $AssetMirror
$env:PATH = "$PythonDir;$PythonDir\Scripts;$env:PATH"

Write-Host ''
Write-Host "  running install.bat $Targets" -ForegroundColor Yellow
Write-Host "  IDF_TOOLS_PATH = $IdfTools" -ForegroundColor Gray
Write-Host "  assets mirror  = $AssetMirror" -ForegroundColor Gray
Write-Host '  downloading toolchain + creating the IDF python venv (10-40 min)' -ForegroundColor Gray
Write-Host ''

$sw2 = [System.Diagnostics.Stopwatch]::StartNew()
Push-Location $IdfDir
try {
    # install.bat must run in cmd.exe so its `setlocal`/env handling works.
    & cmd.exe /c "install.bat $Targets > `"$Log`" 2>&1"
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
$sw2.Stop()

if (Test-Path $Log) {
    Get-Content $Log -Tail 25 | ForEach-Object { Write-Host "    $_" }
}
if ($code -ne 0) { Die "install.bat exited with $code -- full log: $Log" }
Ok ("toolchain installed in {0:N1} min" -f $sw2.Elapsed.TotalMinutes)

# ---------------------------------------------------------------- verify
Write-Host ''
Write-Host '  verifying: export.bat + idf.py --version' -ForegroundColor Gray
$verify = & cmd.exe /c "call `"$IdfDir\export.bat`" >nul 2>&1 && idf.py --version" 2>&1
if ($LASTEXITCODE -ne 0) { Die "idf.py verification failed: $verify" }
Ok "idf.py -> $verify"

Write-Host ''
Write-Host '  ESP-IDF ready. Build with:' -ForegroundColor Green
Write-Host '    powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -Action build' -ForegroundColor Gray
Write-Host ''

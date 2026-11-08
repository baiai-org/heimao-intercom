#Requires -Version 5.1
<#
  ESP-IDF build/driver wrapper for the OpenRadioLink-v0.3 firmware.

  It keeps the IDF environment entirely project-local:
      .idf\esp-idf        source
      .idf\tools          IDF_TOOLS_PATH (toolchain, python venv)
      .tools\Python311    interpreter the IDF venv is built from

  Flashing deliberately goes through tools\flash.ps1 (the tool verified against
  this board over native USB) instead of idf.py flash: one proven flashing path,
  not two.

  Usage:
    powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -Action build
    powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -Action flash
    powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -Action menuconfig
    powershell -ExecutionPolicy Bypass -File tools\idf.ps1 -Action size

  ASCII-only messages (PowerShell 5.1 ANSI .ps1 handling).
#>
[CmdletBinding()]
param(
    [ValidateSet('build', 'flash', 'appflash', 'monitor', 'menuconfig',
                 'size', 'clean', 'fullclean', 'shell', 'envcheck')]
    [string]$Action = 'build',

    [string]$Target = 'esp32s3',
    [switch]$NoBuild,          # with -Action flash: skip the rebuild
    [switch]$Monitor           # with -Action flash: open the monitor afterwards
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Split-Path -Parent $ToolsDir
$IdfDir = Join-Path $Root '.idf\esp-idf'
$IdfTools = Join-Path $Root '.idf\tools'
$PyDir = Join-Path $Root '.tools\Python311'
$FwDir = Join-Path $Root 'firmware'
$PyEnv = Join-Path $IdfTools 'python_env\idf5.4_py3.11_env'
$PyLib = Join-Path $Root '.pylib'

function Say($m)  { Write-Host "  $m" }
function Ok($m)   { Write-Host "  [ ok ] $m" -ForegroundColor Green }
function Die($m)  { Write-Host "  [FAIL] $m" -ForegroundColor Red; exit 1 }

function Show-BuildArtifacts {
    $bd = Join-Path $FwDir 'build'
    Write-Host ''
    foreach ($f in @('bootloader\bootloader.bin', 'partition_table\partition-table.bin',
                     'ota_data_initial.bin', 'openradiolink.bin', 'openradiolink.elf',
                     'flash_args')) {
        $p = Join-Path $bd $f
        if (Test-Path $p) {
            Write-Host ("    {0,-42} {1,12:N0} bytes" -f $f, (Get-Item $p).Length)
        }
    }
}

function Test-TargetConfigured {
    $sdkconfig = Join-Path $FwDir 'sdkconfig'
    if (-not (Test-Path $sdkconfig)) { return $false }
    $pattern = '^CONFIG_IDF_TARGET="' + [regex]::Escape($Target) + '"'
    return ((Select-String -Path $sdkconfig -Pattern $pattern -Quiet) -eq $true)
}

function Invoke-Idf {
    param([string]$IdfArgs, [switch]$AllowFail)
    $cmdline = "call `"$IdfDir\export.bat`" >nul 2>&1 && idf.py -C `"$FwDir`" $IdfArgs"
    Write-Host "  > idf.py $IdfArgs" -ForegroundColor DarkGray
    & cmd.exe /c $cmdline
    $code = $LASTEXITCODE
    if ($code -ne 0 -and -not $AllowFail) { Die "idf.py exited with $code" }
    return $code
}

function Invoke-Build {
    if (Test-TargetConfigured) {
        Invoke-Idf 'build' | Out-Null
    } else {
        # set-target runs fullclean first, and fullclean refuses to touch a
        # build dir without a CMakeCache.txt (left behind by a failed
        # configure).  Clear that debris ourselves.
        $bd = Join-Path $FwDir 'build'
        if ((Test-Path $bd) -and -not (Test-Path (Join-Path $bd 'CMakeCache.txt'))) {
            Write-Host "  removing the incomplete build dir left by an earlier failed configure" -ForegroundColor Yellow
            Remove-Item $bd -Recurse -Force -ErrorAction SilentlyContinue
        }
        Write-Host "  first build for this target: setting target to $Target" -ForegroundColor Yellow
        # set-target only regenerates sdkconfig and configures cmake; the
        # actual compile is a separate build pass.
        Invoke-Idf "set-target $Target" | Out-Null
        Invoke-Idf 'build' | Out-Null
    }
}

function Start-Monitor {
    $env:PYTHONPATH = $PyLib
    & py -3.9 (Join-Path $ToolsDir 'monitor.py')
}

# ---------------------------------------------------------------- preflight
if (-not (Test-Path (Join-Path $IdfDir 'export.bat'))) {
    Die "ESP-IDF not installed at $IdfDir -- run tools\install_idf.ps1 first."
}

$env:IDF_PATH = $IdfDir
$env:IDF_TOOLS_PATH = $IdfTools
if (Test-Path $PyEnv) { $env:IDF_PYTHON_ENV_PATH = $PyEnv }
$env:IDF_GITHUB_ASSETS = 'dl.espressif.cn/github_assets'
# The component manager's default cache lives under AppData and is not
# writable here ('Permission denied'); keep it inside the project instead.
$env:IDF_COMPONENT_CACHE_PATH = Join-Path $PSScriptRoot '..\.idf\component-cache'
$env:PATH = "$PyDir;$PyDir\Scripts;$env:PATH"

# ---------------------------------------------------------------------------
# Keep every scratch file inside the project tree.
#
# If TMP/TEMP point at C:\Users\<user>\AppData\Local\Temp, the toolchain can be
# denied write access there and the very first compiler probe fails with:
#     Cannot create temporary file in C:\Users\...\AppData\Local\Temp:
#     Permission denied
# which looks like a broken toolchain but is really a temp-directory problem.
# gcc/cc1/as, ninja, cmake and ccache all inherit these.
# ---------------------------------------------------------------------------
$TmpDir = Join-Path $Root '.tmp'
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null
$env:TMP = $TmpDir
$env:TEMP = $TmpDir
$env:TMPDIR = $TmpDir
$CcacheDir = Join-Path $Root '.idf\ccache'
New-Item -ItemType Directory -Force -Path $CcacheDir | Out-Null
$env:CCACHE_DIR = $CcacheDir

# ---------------------------------------------------------------------------
# MSYS2/git-bash sets MSYSTEM, and IDF's export.bat refuses to run with it
# defined ("This .bat file is for Windows CMD.EXE shell only.") -- and redirects
# its own output, so the only symptom is a bare "'idf.py' is not recognized".
# Drop it for the child cmd.exe.
# ---------------------------------------------------------------------------
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

Write-Host ''
$brandFile = Join-Path $ToolsDir 'portable_flasher/brand.json'
$displayName = if (Test-Path -LiteralPath $brandFile) { (Get-Content -LiteralPath $brandFile -Encoding UTF8 -Raw | ConvertFrom-Json).project } else { 'HeimaoLink' }
Write-Host "  $displayName firmware   action=$Action   target=$Target" -ForegroundColor Cyan
Write-Host ''

switch ($Action) {
    'envcheck' {
        Invoke-Idf '--version'
        Ok 'ESP-IDF environment is usable.'
    }

    'build' {
        Invoke-Build
        Ok 'build finished.'
        Show-BuildArtifacts
    }

    'menuconfig' { Invoke-Idf 'menuconfig' }
    'size'       { Invoke-Idf 'size' }
    'clean'      { Invoke-Idf 'clean';     Ok 'cleaned.' }
    'fullclean'  { Invoke-Idf 'fullclean'; Ok 'fully cleaned (sdkconfig kept).' }

    'shell' {
        Say 'dropping into an ESP-IDF shell in firmware\ -- type exit to return'
        & cmd.exe /c "call `"$IdfDir\export.bat`" && cd /d `"$FwDir`" && cmd"
    }

    'flash' {
        if (-not $NoBuild) { Invoke-Build }
        $bin = Join-Path $FwDir 'build\openradiolink.bin'
        if (-not (Test-Path $bin)) { Die "app image not found: $bin (build first)" }
        Ok ("app image {0:N0} bytes" -f (Get-Item $bin).Length)
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ToolsDir 'flash.ps1') -Action full
        if ($LASTEXITCODE -ne 0) { Die 'flashing failed' }
        if ($Monitor) { Start-Monitor }
    }

    'appflash' {
        if (-not $NoBuild) { Invoke-Build }
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ToolsDir 'flash.ps1') -Action app
        if ($LASTEXITCODE -ne 0) { Die 'flashing failed' }
        if ($Monitor) { Start-Monitor }
    }

    'monitor' { Start-Monitor }
}

Write-Host ''

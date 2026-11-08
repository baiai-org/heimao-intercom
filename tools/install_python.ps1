#Requires -Version 5.1
<#
  Install a *real* (non Microsoft-Store) Python 3.11 for the ESP-IDF toolchain.

  Why this exists: this machine's only Python 3.9 is the Microsoft Store
  build, and ESP-IDF's installer explicitly refuses to use the Store
  interpreter (its sandboxed paths break the IDF virtualenv).  A normal
  per-user install under .tools\Python311 avoids touching anything else --
  no launcher hijack, no PATH changes, no admin needed.

  ASCII-only messages (PowerShell 5.1 ANSI .ps1 handling).
#>
[CmdletBinding()]
param(
    [string]$Version = '3.11.9',
    [switch]$Force
)

$ErrorActionPreference = 'Continue'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$TargetDir = Join-Path $Root '.tools\Python311'
$Exe = Join-Path $TargetDir 'python.exe'

Write-Host ''
Write-Host "  Python $Version for ESP-IDF  ->  $TargetDir" -ForegroundColor Cyan
Write-Host ''

if ((Test-Path $Exe) -and -not $Force) {
    $v = & $Exe --version 2>&1
    Write-Host "  already present: $v" -ForegroundColor Green
    exit 0
}

$Url = "https://www.python.org/ftp/python/$Version/python-$Version-amd64.exe"
$Installer = Join-Path $env:TEMP "python-$Version-amd64.exe"

Write-Host "  downloading $Url" -ForegroundColor Gray
$sw = [System.Diagnostics.Stopwatch]::StartNew()
Invoke-WebRequest -Uri $Url -OutFile $Installer -UseBasicParsing
$sw.Stop()
$mb = [math]::Round((Get-Item $Installer).Length / 1MB, 1)
Write-Host ("  downloaded {0} MB in {1:N0} s" -f $mb, $sw.Elapsed.TotalSeconds) -ForegroundColor Gray

Write-Host '  installing (per-user, quiet, no PATH/launcher changes)...' -ForegroundColor Yellow
$args = @(
    '/quiet',
    'InstallAllUsers=0',
    'PrependPath=0',
    'Include_launcher=0',
    'Include_test=0',
    'Include_doc=0',
    'Include_pip=1',
    'Include_tcltk=1',
    "TargetDir=$TargetDir"
)
$p = Start-Process -FilePath $Installer -ArgumentList $args -Wait -PassThru
if ($p.ExitCode -ne 0) {
    if ($p.ExitCode -eq 3010) {
        Write-Host '  installer wants a reboot (3010); continuing anyway' -ForegroundColor Yellow
    } else {
        throw "python installer exited with $($p.ExitCode)"
    }
}
Remove-Item $Installer -ErrorAction SilentlyContinue

if (-not (Test-Path $Exe)) { throw "python.exe not found at $TargetDir after install" }
Write-Host ''
& $Exe --version
& $Exe -m pip --version
Write-Host ''
Write-Host '  ready. ESP-IDF can now use this interpreter.' -ForegroundColor Green
Write-Host ''

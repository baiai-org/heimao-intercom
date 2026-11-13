#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Python,
    [string]$OutputDirectory,
    [string]$FixtureBin,
    [string]$AppBin,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Python) { $Python = Join-Path $repoRoot '.tmp/flasher-build-env/Scripts/python.exe' }
if (-not (Test-Path -LiteralPath $Python)) {
    $command = Get-Command $Python -ErrorAction SilentlyContinue
    if ($command) { $Python = $command.Source }
    else { throw 'Create a Python 3.11 build venv and install requirements-lock.txt first, or pass -Python.' }
}
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot '.tmp/portable-flasher/stage' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
# Never reuse an output assets directory: an older build may contain a BIN.
# The only bundle assets are these two explicitly copied brand images.
$buildRoot = Join-Path $repoRoot ('.tmp/portable-flasher/public-' + [guid]::NewGuid().ToString('N'))
$assetRoot = Join-Path $buildRoot 'assets'
$workRoot = Join-Path $buildRoot 'pyinstaller'
New-Item -ItemType Directory -Force $assetRoot, $workRoot, $OutputDirectory | Out-Null
$brand = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'brand.json') -Encoding UTF8 -Raw | ConvertFrom-Json
Copy-Item -LiteralPath (Join-Path $repoRoot 'assets/brand/heimao.ico') -Destination (Join-Path $assetRoot 'heimao.ico')
Copy-Item -LiteralPath (Join-Path $repoRoot 'assets/brand/icon-192.png') -Destination (Join-Path $assetRoot 'brand-icon.png')
Write-Output 'Public tool: no embedded handset firmware; select an external full BIN.'
if ($FixtureBin) {
    $FixtureBin = (Resolve-Path -LiteralPath $FixtureBin).Path
    $env:ORL_FLASHER_FIXTURE = $FixtureBin
}
if ($AppBin) { $env:HM_FLASHER_APP = (Resolve-Path -LiteralPath $AppBin).Path }
elseif (Test-Path -LiteralPath (Join-Path $repoRoot 'firmware/build/openradiolink.bin')) {
    $env:HM_FLASHER_APP = Join-Path $repoRoot 'firmware/build/openradiolink.bin'
}
if (-not $SkipTests) {
    & $Python (Join-Path $PSScriptRoot 'test_flasher.py')
    if ($LASTEXITCODE -ne 0) { throw 'Flasher regression tests failed' }
}
& $Python -m PyInstaller --noconfirm --onefile --windowed --noupx --runtime-tmpdir . `
    --name $brand.exe_name --distpath $OutputDirectory --workpath $workRoot --specpath $workRoot `
    --icon (Join-Path $assetRoot 'heimao.ico') --add-data "$(Join-Path $PSScriptRoot 'brand.json');." `
    --version-file (Join-Path $PSScriptRoot 'version_info.txt') `
    --collect-all esptool --hidden-import serial.tools.list_ports_windows `
    --add-data "$assetRoot;assets" (Join-Path $PSScriptRoot 'app.py')
if ($LASTEXITCODE -ne 0) { throw 'EXE build failed' }
$exe = Join-Path $OutputDirectory ($brand.exe_name + '.exe')
$auditArgs = @((Join-Path $PSScriptRoot 'verify_bundle.py'), '--exe', $exe, '--report', (Join-Path $OutputDirectory 'bundle-verification.json'))
if ($FixtureBin) { $auditArgs += @('--forbidden-image', $FixtureBin) }
if ($AppBin) { $auditArgs += @('--forbidden-image', $AppBin) }
& $Python @auditArgs
if ($LASTEXITCODE -ne 0) { throw 'Public bundle verification failed' }
Write-Output $exe

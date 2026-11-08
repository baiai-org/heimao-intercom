#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Python,
    [string]$OutputDirectory,
    [string]$FirmwareBin,
    [switch]$WithFirmware,
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
$assetRoot = Join-Path $OutputDirectory 'assets'
$workRoot = Join-Path $repoRoot '.tmp/portable-flasher/pyinstaller'
New-Item -ItemType Directory -Force $assetRoot, $workRoot | Out-Null
$brand = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'brand.json') -Encoding UTF8 -Raw | ConvertFrom-Json
$binName = $brand.default_bin
Copy-Item -LiteralPath (Join-Path $repoRoot 'assets/brand/heimao.ico') -Destination (Join-Path $assetRoot 'heimao.ico') -Force
Copy-Item -LiteralPath (Join-Path $repoRoot 'assets/brand/icon-192.png') -Destination (Join-Path $assetRoot 'brand-icon.png') -Force

# Public builds do NOT embed firmware: the flasher is GPL-3.0 and the firmware
# is licensed separately, so bundling it would mix the two licences.  Pass
# -WithFirmware (internal builds) or -FirmwareBin to embed an image.
if ($WithFirmware -and -not $FirmwareBin) {
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot "assets/$binName")) {
        $FirmwareBin = Join-Path $PSScriptRoot "assets/$binName"
    } else {
        & $Python (Join-Path $PSScriptRoot 'make_firmware.py') --repo $repoRoot --output $assetRoot
        if ($LASTEXITCODE -ne 0) { throw 'Firmware packaging failed' }
    }
}
if ($FirmwareBin) {
    Copy-Item -LiteralPath $FirmwareBin -Destination (Join-Path $assetRoot $binName) -Force
    Write-Output "embedded firmware: $FirmwareBin"
} else {
    Write-Output 'building without embedded firmware: users select a full.bin in the tool'
}
if (-not $SkipTests) {
    $fixture = Join-Path $assetRoot $binName
    if (Test-Path -LiteralPath $fixture) { $env:ORL_FLASHER_FIXTURE = $fixture }
    $env:HM_FLASHER_APP = Join-Path $repoRoot 'firmware/build/openradiolink.bin'
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
Write-Output (Join-Path $OutputDirectory ($brand.exe_name + '.exe'))

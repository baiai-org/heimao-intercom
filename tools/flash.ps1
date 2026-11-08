#Requires -Version 5.1
<#
  OpenRadioLink-v0.3  --  direct-USB flashing tool
  ================================================================
  The board wires the ESP32-S3 native USB (GPIO19/20) straight to the
  J1 Type-C connector, so it enumerates as the chip's built-in
  "USB-Serial/JTAG" bridge (VID 303A).  That means:
    * no USB-UART chip, no driver install, no BOOT button press
    * this script finds the port by itself and flashes

  Quick use (from the project root):
    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Action probe
    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Action full
    powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Action full -Monitor

  NOTE: all messages here are ASCII on purpose -- PowerShell 5.1 reads
  .ps1 files as ANSI when they have no BOM, which would mangle Chinese.
#>
[CmdletBinding()]
param(
    [ValidateSet('probe', 'selftest', 'full', 'app', 'factory', 'merge', 'erase', 'read', 'ports')]
    [string]$Action = 'probe',

    [string]$Port,                       # COM5 / COM12 ... auto-detected when empty
    [string]$App,                        # -Action app  : path to an application .bin
    [string]$FactoryImage,               # -Action factory : merged image, flashes at 0x0
    [string]$BuildDir,                   # defaults to <repo>\firmware\build
    [string]$OutDir,                     # defaults to <repo>\bins

    [int]$Baud = 460800,
    [string]$FlashMode = 'dio',
    [string]$FlashFreq = '80m',
    [string]$FlashSize = '16MB',

    [string]$ReadSpec,                   # -Action read : "0xADDR:0xSIZE:outfile.bin"
    [string]$AppOffset,                  # -Action app : override the app offset
    [string]$ScratchOffset = '0x00FFF000',  # -Action selftest : last 4 KB sector
    [switch]$NoErase,                    # -Action full : skip full-chip erase
    [switch]$Monitor                     # open the serial monitor after flashing
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------- paths
$ToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ToolsDir
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'firmware\build' }
if (-not $OutDir)   { $OutDir   = Join-Path $RepoRoot 'bins' }
$PyLib = Join-Path $RepoRoot '.pylib'

# ESP-IDF standard flash layout for a 16 MB / OTA-capable build
$OFF_BOOTLOADER = '0x0'
$OFF_PARTITION  = '0x8000'
$OFF_OTA_DATA   = '0xd000'
$OFF_APP        = '0x10000'

# ---------------------------------------------------------------- helpers
function Write-Head($text) {
    Write-Host ''
    Write-Host ('=' * 68) -ForegroundColor DarkGray
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host ('=' * 68) -ForegroundColor DarkGray
}

function Write-Ok($text)   { Write-Host "  [ ok ] $text" -ForegroundColor Green }
function Write-Warn2($text) { Write-Host "  [warn] $text" -ForegroundColor Yellow }
function Write-Err($text)  { Write-Host "  [FAIL] $text" -ForegroundColor Red }
function Write-Info($text) { Write-Host "  $text" -ForegroundColor Gray }

function Get-Python {
    foreach ($cand in @('py', 'python')) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw 'No Python launcher found. Install Python 3.9+ (or the py launcher).'
}

$script:LastEsptoolOutput = @()

function Invoke-Esptool {
    param([string[]]$EsptoolArgs, [switch]$AllowFail, [switch]$Quiet)
    $py = Get-Python
    $oldPath = $env:PYTHONPATH
    $oldEap = $ErrorActionPreference
    $env:PYTHONPATH = $PyLib
    # esptool prints its progress to *stderr*.  With ErrorActionPreference=Stop
    # PowerShell 5.1 turns that into a terminating NativeCommandError, so the
    # call must run with Continue and have stderr merged into a captured array.
    $ErrorActionPreference = 'Continue'
    $captured = @()
    $code = 1
    try {
        $captured = & $py -3.9 -m esptool @EsptoolArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        $env:PYTHONPATH = $oldPath
        $ErrorActionPreference = $oldEap
    }
    $script:LastEsptoolOutput = $captured
    if (-not $Quiet) { foreach ($line in $captured) { Write-Host "  $line" } }
    if ($code -ne 0 -and -not $AllowFail) {
        throw "esptool exited with code $code"
    }
    return $code
}

function Get-EsptoolText { return ($script:LastEsptoolOutput -join "`n") }

function Get-SerialCandidates {
    # Returns objects: {Com, Name, Vid, Pid, InstanceId}
    $out = @()
    $devs = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -and $_.Name -match '\(COM(\d+)\)' }
    foreach ($d in $devs) {
        $m = [regex]::Match($d.Name, '\(COM(\d+)\)')
        if (-not $m.Success) { continue }
        $vid = ''; $prodId = ''
        if ($d.PNPDeviceID -match 'VID_([0-9A-Fa-f]{4})') { $vid = $Matches[1].ToUpper() }
        if ($d.PNPDeviceID -match 'PID_([0-9A-Fa-f]{4})') { $prodId = $Matches[1].ToUpper() }
        $out += [pscustomobject]@{
            Com        = [int]$m.Groups[1].Value
            Name       = $d.Name
            Vid        = $vid
            Pid        = $prodId
            InstanceId = $d.PNPDeviceID
        }
    }
    return $out
}

function Show-Ports {
    Write-Head 'serial ports on this machine'
    $all = Get-SerialCandidates | Sort-Object Com
    if (-not $all) { Write-Warn2 'no COM port found at all.'; return }
    foreach ($p in $all) {
        $tag = ''
        if ($p.Vid -eq '303A' -and $p.Pid -eq '1001') { $tag = '  <== ESP32-S3 native USB (this board)' }
        elseif ($p.Vid -eq '303A')                    { $tag = '  <== Espressif device' }
        elseif ($p.Vid -in @('1A86', '10C4', '0403')) { $tag = '  <== USB-UART bridge (H1 header path)' }
        Write-Host ("  COM{0,-4} VID_{1} PID_{2}  {3}{4}" -f $p.Com, $p.Vid, $p.Pid, $p.Name, $tag)
    }
}

function Find-BoardPort {
    $all = Get-SerialCandidates
    if (-not $all) {
        throw 'No COM port found. Is the board plugged into the Type-C connector with a DATA cable?'
    }
    # 1) the ESP32-S3 built-in USB-Serial/JTAG
    $hit = $all | Where-Object { $_.Vid -eq '303A' -and $_.Pid -eq '1001' } | Sort-Object Com
    if (-not $hit) { $hit = $all | Where-Object { $_.Vid -eq '303A' } | Sort-Object Com }
    # 2) fall back to any common USB-UART bridge (H1/UART0 path)
    if (-not $hit) { $hit = $all | Where-Object { $_.Vid -in @('1A86', '10C4', '0403') } | Sort-Object Com }

    if (-not $hit) {
        Write-Warn2 'No Espressif device and no USB-UART bridge found. Ports seen:'
        Show-Ports
        throw 'Cannot auto-detect the board port. Pass -Port COMx explicitly.'
    }
    $p = $hit | Select-Object -First 1
    Write-Ok ("auto-detected board on COM{0}  ({1})" -f $p.Com, $p.Name)
    return "COM$($p.Com)"
}

function Resolve-Port {
    if ($Port) {
        Write-Ok "using port $Port (from -Port)"
        return $Port
    }
    return (Find-BoardPort)
}

function Get-ImageSet {
    $set = [ordered]@{}

    # Preferred source: the flash_args file ESP-IDF writes next to the build.
    # It already carries the exact offsets for THIS build (so the app lands in
    # ota_0 rather than at a hardcoded 0x10000).
    $flashArgs = Join-Path $BuildDir 'flash_args'
    if (Test-Path $flashArgs) {
        foreach ($line in (Get-Content $flashArgs -ErrorAction SilentlyContinue)) {
            $t = $line.Trim()
            if (-not $t -or $t.StartsWith('--')) { continue }
            $parts = $t -split '\s+', 2
            if ($parts.Count -ne 2) { continue }
            if ($parts[0] -notmatch '^0x[0-9a-fA-F]+$') { continue }
            $p = Join-Path $BuildDir $parts[1].Trim()
            if (Test-Path $p) { $set[$parts[0]] = (Resolve-Path $p).Path }
        }
        if ($set.Count -gt 0) {
            Write-Info "image list taken from $flashArgs"
            return $set
        }
    }

    # Fallback: the conventional ESP-IDF layout.
    $boot = Join-Path $BuildDir 'bootloader\bootloader.bin'
    $part = Join-Path $BuildDir 'partition_table\partition-table.bin'
    $ota  = Join-Path $BuildDir 'ota_data_initial.bin'
    $app  = Join-Path $BuildDir 'openradiolink.bin'

    if ($App) { $app = $App }

    if (Test-Path $boot) { $set[$OFF_BOOTLOADER] = (Resolve-Path $boot).Path }
    if (Test-Path $part) { $set[$OFF_PARTITION]  = (Resolve-Path $part).Path }
    if (Test-Path $ota)  { $set[$OFF_OTA_DATA]   = (Resolve-Path $ota).Path }
    if (Test-Path $app)  { $set[$OFF_APP]        = (Resolve-Path $app).Path }
    return $set
}

function Show-ImageSet($set) {
    foreach ($k in $set.Keys) {
        $f = Get-Item $set[$k]
        Write-Host ("  {0,-10} {1,10:N0} bytes  {2}" -f $k, $f.Length, $f.FullName)
    }
}

function Test-ImageSet($set) {
    if ($set.Count -eq 0) {
        throw ("No firmware images found under '$BuildDir'. Build the firmware first, " +
               "or pass -App <file.bin> for the app-only action.")
    }
    if (-not $set.Contains($OFF_APP)) {
        Write-Warn2 'no application image in the set -- only bootloader/partition will be written.'
    }
}

function Open-Monitor($comPort) {
    $py = Get-Python
    $mon = Join-Path $ToolsDir 'monitor.py'
    Write-Head "serial monitor on $comPort  (Ctrl+C to quit)"
    $old = $env:PYTHONPATH
    $env:PYTHONPATH = $PyLib
    try { & $py -3.9 $mon --port $comPort --baud 115200 }
    finally { $env:PYTHONPATH = $old }
}

# ---------------------------------------------------------------- actions
function Action-Ports { Show-Ports }

function Action-Probe {
    $com = Resolve-Port
    Write-Head "probing the chip on $com"
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'flash_id') | Out-Null
    $text = Get-EsptoolText

    Write-Host ''
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'read_mac') | Out-Null
    $text += "`n" + (Get-EsptoolText)

    Write-Head 'board acceptance checks'
    $fail = 0
    if ($text -match 'Chip is (ESP32-S3[^\r\n]*)') {
        Write-Ok ("MCU     : {0}" -f $Matches[1].Trim())
    } else {
        Write-Err 'MCU     : not an ESP32-S3'; $fail++
    }
    if ($text -match 'Embedded PSRAM (\d+)MB') {
        Write-Ok ("PSRAM   : {0} MB embedded  (N16R8 requires 8 MB)" -f $Matches[1])
    } elseif ($text -match 'No embedded PSRAM') {
        Write-Warn2 'PSRAM   : none reported'
    } else {
        Write-Warn2 'PSRAM   : not reported by esptool'
    }
    if ($text -match 'Detected flash size:\s*([0-9]+[KM]B)') {
        if ($Matches[1] -eq '16MB') {
            Write-Ok 'Flash   : 16 MB  (N16R8 OK)'
        } else {
            Write-Err ("Flash   : {0} -- N16R8 needs 16 MB, check the module marking" -f $Matches[1]); $fail++
        }
    } else {
        Write-Err 'Flash   : size not reported'; $fail++
    }
    if ($text -match 'MAC:\s*([0-9a-fA-F:]{17})') {
        Write-Ok ("MAC     : {0}" -f $Matches[1])
    }

    Write-Host ''
    if ($fail -eq 0) {
        Write-Ok 'PASS -- native-USB flashing works on this board, no button press needed.'
    } else {
        throw "$fail board acceptance check(s) failed (see above)"
    }
}

function Action-SelfTest {
    # Proves the whole path end to end: write -> verify -> read back -> hash
    # compare -> clean up.  Uses the last 4 KB sector of the 16 MB flash so it
    # cannot collide with any partition, and restores it afterwards.
    $com = Resolve-Port
    $size = 0x1000
    $tmpW = Join-Path $env:TEMP 'orl_selftest_write.bin'
    $tmpR = Join-Path $env:TEMP 'orl_selftest_read.bin'

    Write-Head "write-path self test on $com"
    Write-Info ("scratch sector {0}  ({1} bytes, last sector of 16 MB)" -f $ScratchOffset, $size)

    $bytes = New-Object byte[] $size
    for ($i = 0; $i -lt $size; $i++) { $bytes[$i] = [byte](($i * 7 + 13) -band 0xFF) }
    [System.IO.File]::WriteAllBytes($tmpW, $bytes)
    $hW = (Get-FileHash $tmpW -Algorithm SHA256).Hash
    Write-Info "pattern sha256 $hW"

    Write-Host ''
    Write-Info '1/3 write + verify'
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'write_flash', '--verify', $ScratchOffset, $tmpW) | Out-Null

    Write-Host ''
    Write-Info '2/3 read back'
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'read_flash', $ScratchOffset, ("0x{0:X}" -f $size), $tmpR) | Out-Null

    $hR = (Get-FileHash $tmpR -Algorithm SHA256).Hash
    Write-Info "readback sha256 $hR"
    if ($hW -ne $hR) {
        throw "read-back mismatch: wrote $hW, read $hR -- the write path is NOT reliable"
    }
    Write-Ok 'read-back matches byte for byte.'

    Write-Host ''
    Write-Info '3/3 restoring the scratch sector to 0xFF'
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'erase_region', $ScratchOffset, ("0x{0:X}" -f $size)) | Out-Null

    Remove-Item $tmpW, $tmpR -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Ok 'PASS -- write, verify and read-back all work over native USB.'
}

function Action-Erase {
    $com = Resolve-Port
    Write-Head "FULL chip erase on $com  (this wipes everything, incl. NVS/OTA)"
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'erase_flash') | Out-Null
    Write-Ok 'chip erased.'
}

function Action-Full {
    $com = Resolve-Port
    $set = Get-ImageSet
    Test-ImageSet $set

    Write-Head "images to write"
    Show-ImageSet $set

    if (-not $NoErase) {
        Write-Head "full chip erase on $com"
        Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                         '--before', 'default_reset', '--after', 'hard_reset',
                         'erase_flash') | Out-Null
    }

    Write-Head "writing images on $com at $Baud baud"
    $args = @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
              '--before', 'default_reset', '--after', 'hard_reset',
              'write_flash', '--verify',
              '--flash_mode', $FlashMode, '--flash_freq', $FlashFreq,
              '--flash_size', $FlashSize)
    foreach ($k in $set.Keys) { $args += $k; $args += $set[$k] }
    Invoke-Esptool $args | Out-Null
    Write-Ok 'flash + verify complete.'

    if ($Monitor) { Open-Monitor $com }
}

function Get-AppOffset {
    # The app must land on the ota_0 partition boundary (0x20000 here), NOT at
    # the legacy 0x10000 -- getting this wrong makes the bootloader report
    # "image at 0x.... has invalid magic byte / not bootable" and boot-loop.
    # build\flash_args is written by ESP-IDF and is authoritative.
    $flashArgs = Join-Path $BuildDir 'flash_args'
    if (Test-Path $flashArgs) {
        foreach ($line in (Get-Content $flashArgs -ErrorAction SilentlyContinue)) {
            $t = $line.Trim()
            if ($t -notmatch '^(0x[0-9a-fA-F]+)\s+(.+)$') { continue }
            $off = $Matches[1]
            $path = $Matches[2].Trim()
            if ($path -match 'openradiolink\.bin$' -or $path -match '\.bin$') {
                if ($path -notmatch 'bootloader|partition-table|ota_data') {
                    return $off
                }
            }
        }
    }
    Write-Warn2 "could not read the app offset from flash_args; falling back to $OFF_APP"
    return $OFF_APP
}

function Action-App {
    $com = Resolve-Port
    if (-not $App) {
        $App = Join-Path $BuildDir 'openradiolink.bin'
    }
    if (-not (Test-Path $App)) { throw "application image not found: $App" }
    $App = (Resolve-Path $App).Path
    $size = (Get-Item $App).Length
    $offset = if ($AppOffset) { $AppOffset } else { Get-AppOffset }

    Write-Head "app-only flash on $com"
    Write-Info ("{0}  ({1:N0} bytes)  ->  {2}" -f (Split-Path -Leaf $App), $size, $offset)
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'write_flash', '--verify',
                     '--flash_mode', $FlashMode, '--flash_freq', $FlashFreq,
                     '--flash_size', $FlashSize,
                     $offset, $App) | Out-Null
    Write-Ok 'app written and verified (bootloader/partition untouched).'
    if ($Monitor) { Open-Monitor $com }
}

function Action-Factory {
    $com = Resolve-Port
    if (-not $FactoryImage) { throw '-Action factory needs -FactoryImage <merged.bin>' }
    if (-not (Test-Path $FactoryImage)) { throw "factory image not found: $FactoryImage" }
    $FactoryImage = (Resolve-Path $FactoryImage).Path
    Write-Head "factory image flash on $com (offset 0x0)"
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'write_flash', '--verify', '0x0', $FactoryImage) | Out-Null
    Write-Ok 'factory image written and verified.'
    if ($Monitor) { Open-Monitor $com }
}

function Action-Merge {
    $set = Get-ImageSet
    Test-ImageSet $set
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $outBin = Join-Path $OutDir "HeimaoLink_factory_$stamp.bin"

    Write-Head 'merging bootloader + partition table + app into one factory image'
    Show-ImageSet $set
    $args = @('--chip', 'esp32s3', 'merge_bin', '-o', $outBin,
              '--flash_mode', $FlashMode, '--flash_freq', $FlashFreq,
              '--flash_size', $FlashSize)
    foreach ($k in $set.Keys) { $args += $k; $args += $set[$k] }
    Invoke-Esptool $args | Out-Null
    $f = Get-Item $outBin
    Write-Ok ("merged image: {0}  ({1:N0} bytes)" -f $f.FullName, $f.Length)
    Write-Info 'flash it at 0x0 with -Action factory, or hand it to a factory programmer.'
}

function Action-Read {
    $com = Resolve-Port
    if (-not $ReadSpec) { throw '-Action read needs -ReadSpec "0xADDR:0xSIZE:outfile.bin"' }
    $parts = $ReadSpec.Split(':')
    if ($parts.Count -ne 3) { throw 'ReadSpec must look like 0x0:0x1000:dump.bin' }
    Write-Head "reading flash on $com"
    Invoke-Esptool @('--chip', 'esp32s3', '--port', $com, '--baud', "$Baud",
                     '--before', 'default_reset', '--after', 'hard_reset',
                     'read_flash', $parts[0], $parts[1], $parts[2]) | Out-Null
    Write-Ok ("read {0} bytes from {1} -> {2}" -f $parts[1], $parts[0], $parts[2])
}

# ---------------------------------------------------------------- main
$brandFile = Join-Path $PSScriptRoot 'portable_flasher/brand.json'
$displayName = if (Test-Path -LiteralPath $brandFile) { (Get-Content -LiteralPath $brandFile -Encoding UTF8 -Raw | ConvertFrom-Json).tool } else { 'HeimaoLink Flasher' }
Write-Head "$displayName   action=$Action"
if (-not (Test-Path $PyLib)) {
    Write-Err "toolchain missing: $PyLib"
    Write-Info 'run:  powershell -ExecutionPolicy Bypass -File tools\setup_toolchain.ps1'
    exit 2
}

try {
    switch ($Action) {
        'ports'    { Action-Ports }
        'probe'    { Action-Probe }
        'selftest' { Action-SelfTest }
        'erase'    { Action-Erase }
        'full'     { Action-Full }
        'app'      { Action-App }
        'factory'  { Action-Factory }
        'merge'    { Action-Merge }
        'read'     { Action-Read }
    }
} catch {
    Write-Host ''
    Write-Err $_.Exception.Message
    Write-Host ''
    Write-Info 'troubleshooting:'
    Write-Info '  * Is the board plugged into the Type-C connector with a DATA cable?'
    Write-Info '  * Run  -Action ports  to see what Windows actually enumerated.'
    Write-Info '  * If the firmware grabbed the USB-OTG PHY, hold SW1 (BOOT), tap SW2 (RST),'
    Write-Info '    release SW1, then retry -- that forces the ROM downloader.'
    Write-Info '  * USB hubs / charge-only cables are the two most common causes.'
    exit 1
}

Write-Host ''

<#
.SYNOPSIS
    Flash the container-built binaries from the host with esptool.

.DESCRIPTION
    Docker Desktop on Windows cannot pass a COM port into a container, so the
    build happens in Docker and the flash happens here. This needs nothing from
    ESP-IDF -- only esptool, which is a plain pip package:

        python -m pip install esptool esp-idf-monitor

    Works with esptool 4.x and 5.x. 5.0 renamed the subcommands (write_flash ->
    write-flash), so the spelling is picked from the detected version rather than
    trusting the deprecated aliases to stay. The --before/--after flags are simply
    omitted: the values this script wants are already the defaults, and their
    spelling changed in 5.0 too.

    The build directory contains flash_project_args, written by the build, which
    already lists every image and its offset. Reading it rather than restating
    offsets here means the partition layout can change without touching this file.

    The remote display is built once per board (remote/main/board.h), into
    remote/build/<board>/. -Board names the board. Without it, -Remote works it out:
    it resets the board and reads the boot log of the firmware already on it, which
    says "board: <name>" -- the board that image was built for, which is right once
    the board has been flashed with -Board. A board with no batmon-remote on it yet,
    one with remote firmware from before boards (0.4.24 and older, which does not say),
    or one that does not answer, needs -Board once.

.EXAMPLE
    .\tools\flash.ps1 -Remote                    # remote display, board read off the device
    .\tools\flash.ps1 -Board jc2432w328r         # remote display, GUITION JC2432W328R
    .\tools\flash.ps1 -Board cyd -Port COM7      # remote display, classic CYD
.EXAMPLE
    .\tools\flash.ps1                       # auto-detect the port
    .\tools\flash.ps1 -Port COM5
    .\tools\flash.ps1 -Port COM5 -Baud 921600
    .\tools\flash.ps1 -Erase                # full chip erase first
#>

[CmdletBinding()]
param(
    [string]$Port  = $env:BATMON_PORT,
    [int]   $Baud  = 460800,
    [switch]$Erase,
    [switch]$Monitor,
    # The remote display firmware in remote/: a classic ESP32 behind a CH340, not the
    # monitor's ESP32-C6.
    [switch]$Remote,
    # Which board the remote display is (remote/main/board.h). Implies -Remote.
    # Left out, it is read off the board itself.
    [ValidateSet('cyd', 'cyd2usb', 'jc2432w328r', 'jc1732w319')]
    [string]$Board
)

$ErrorActionPreference = 'Stop'

$boards = [ordered]@{
    'cyd'         = 'ESP32-2432S028R, single micro-USB (ILI9341)'
    'cyd2usb'     = 'ESP32-2432S028R, USB-C + micro-USB (ST7789)'
    'jc2432w328r' = 'GUITION JC2432W328R (ST7789, resistive)'
    'jc1732w319'  = 'GUITION JC1732W319, 1.9in 170x320 (ST7789, no touch)'
}
if ($Board) { $Remote = $true }

$proj     = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$chip     = if ($Remote) { 'esp32' } else { 'esp32c6' }

# esptool 5.0 hyphenated every subcommand. Detect rather than guess.
$esptoolVer = (& python -c "import esptool;print(esptool.__version__)" 2>$null)
if ($LASTEXITCODE -ne 0 -or -not $esptoolVer) {
    throw "esptool not importable. Install it:  python -m pip install esptool esp-idf-monitor"
}
$major = [int]($esptoolVer -split '\.')[0]
if ($major -ge 5) {
    $cmdWrite = 'write-flash'
    $cmdErase = 'erase-flash'
} else {
    $cmdWrite = 'write_flash'
    $cmdErase = 'erase_flash'
}
Write-Host "==> esptool $esptoolVer (using '$cmdWrite')" -ForegroundColor DarkGray

function Get-CandidatePorts {
    Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\(COM(\d+)\)' } |
        ForEach-Object {
            [pscustomobject]@{
                Port = [regex]::Match($_.Name, '\(COM(\d+)\)').Groups[0].Value.Trim('(', ')')
                Name = $_.Name
                # Only a USB port can be the board: the C6's native port, or a USB-UART
                # adapter (FTDIBUS\ under FTDI's own driver). Bluetooth SPP links
                # (BTHENUM\) and Intel AMT serial-over-LAN (PCI\) are COM ports too,
                # and opening a Bluetooth one stalls on a semaphore timeout that reads
                # like a busy port.
                Usb  = [bool]($_.PNPDeviceID -match '^(USB|FTDIBUS)\\')
                Espressif = [bool]($_.PNPDeviceID -match 'VID_303A')
            }
        }
}

if (-not $Port) {
    $all   = @(Get-CandidatePorts)
    $cands = @($all | Where-Object Usb)
    if ($cands.Count -eq 0) {
        Write-Host ""
        if ($Remote) {
            Write-Host "No USB serial port found. Is the CYD plugged in? Its micro-USB port is a" -ForegroundColor Red
            Write-Host "CH340, which needs its driver on some Windows installs." -ForegroundColor Red
        } else {
            Write-Host "No USB serial port found. Is the XIAO plugged in? Its native USB-C port" -ForegroundColor Red
            Write-Host "enumerates as a USB Serial device a second or two after connecting." -ForegroundColor Red
        }
        if ($all.Count -gt 0) {
            Write-Host ""
            Write-Host "Ignored, as they cannot be the board (Bluetooth, serial-over-LAN):" -ForegroundColor DarkGray
            $all | ForEach-Object { Write-Host "  $($_.Port)  $($_.Name)" -ForegroundColor DarkGray }
            Write-Host "Pass -Port COMn to use one anyway." -ForegroundColor DarkGray
        }
        exit 1
    }
    # Prefer the C6's built-in USB-Serial-JTAG bridge: Espressif's VID, or its name.
    # The monitor's C6 has Espressif's own USB; the remote's CYD has a CH340 (on some
    # batches a CP210x). Prefer whichever this flash is for.
    $pick = if ($Remote) {
        $cands | Where-Object { $_.Name -match 'CH34|CP210|USB-SERIAL' } | Select-Object -First 1
    } else {
        $cands | Where-Object { $_.Espressif -or $_.Name -match 'JTAG|Espressif' } | Select-Object -First 1
    }
    if (-not $pick) { $pick = $cands[0] }
    $Port = $pick.Port
    Write-Host "==> auto-detected $Port  ($($pick.Name))" -ForegroundColor Yellow
    if ($cands.Count -gt 1) {
        Write-Host "    other candidates:" -ForegroundColor DarkGray
        $cands | Where-Object { $_.Port -ne $Port } | ForEach-Object {
            Write-Host "      $($_.Port)  $($_.Name)" -ForegroundColor DarkGray
        }
        Write-Host "    pass -Port COMn if that guess is wrong." -ForegroundColor DarkGray
    }
}
else {
    # An explicitly named port that is not enumerated is a different fault from one
    # that is held by another program, and esptool reports both as "port is busy or
    # doesn't exist". That ambiguity sends you hunting for a terminal to close when
    # the real answer is that the board is not plugged in. Separate them here.
    $present = @(Get-CandidatePorts)
    if ($present.Port -notcontains $Port) {
        Write-Host ""
        Write-Host "$Port is not present on this machine." -ForegroundColor Red
        Write-Host "This is NOT a port-in-use problem -- Windows is not enumerating it" -ForegroundColor Red
        Write-Host "at all, so nothing can be holding it." -ForegroundColor Red
        Write-Host ""
        if ($present.Count -eq 0) {
            Write-Host "No serial ports at all. Replug the USB-C cable; the XIAO's native"
            Write-Host "port enumerates as a USB Serial device a second or two after."
        } else {
            Write-Host "Ports that do exist:"
            $present | ForEach-Object { Write-Host "  $($_.Port)  $($_.Name)" }
            Write-Host ""
            Write-Host "If the board is plugged in and none of these look like it, the"
            Write-Host "USB-Serial-JTAG peripheral may be wedged: hold BOOT, tap RESET,"
            Write-Host "release BOOT to enter the ROM bootloader, which always enumerates."
        }
        exit 1
    }
}

# Which remote board this is, from the firmware already running on it. A reset and
# the first few seconds of its boot log: the remote prints "board: <name>" with its
# version, before BLE starts. pyserial comes with esptool, so this needs nothing new.
#
# The CH340's auto-reset circuit: RTS alone pulls EN low, DTR alone pulls GPIO0 low,
# both or neither leave the chip alone. Both start released and only RTS is pulsed,
# so the chip resets into its own firmware, not the ROM bootloader.
function Get-RunningBoard([string]$port) {
    $py = @'
import re, sys, time, serial
s = serial.Serial()
s.port, s.baudrate, s.timeout = sys.argv[1], 115200, 0.1
s.dtr = False
s.rts = False
s.open()
s.rts = True
time.sleep(0.1)
s.rts = False
log, end = '', time.time() + 4
while time.time() < end:
    log += s.read(512).decode('ascii', 'replace')
    m = re.search(r'board: (\w+)', log)
    if m:
        print('board ' + m.group(1)); sys.exit(0)
s.close()
# Firmware from before boards says only which panel it was BUILD for, and a CYD image
# runs happily on a JC2432W328R -- dark, with its backlight on the wrong pin -- still
# logging 'ILI9341 panel up'. That names the image, not the board: no guess from it.
print('seen ' + ('batmon-remote from before boards, which does not say' if 'batmon-remote' in log
                 else 'other firmware' if log.strip() else 'nothing'))
'@
    $tmp = Join-Path ([IO.Path]::GetTempPath()) 'batmon-probe-board.py'
    Set-Content -Path $tmp -Value $py -Encoding ascii
    try { return (& python $tmp $port 2>&1 | Select-Object -Last 1) }
    finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
}

if ($Remote -and -not $Board) {
    Write-Host "==> reading the board off $Port (reset, boot log)" -ForegroundColor DarkGray
    $probe = "$(Get-RunningBoard $Port)"
    if ($probe -match '^board (\w+)$' -and $boards.Contains($Matches[1])) {
        $Board = $Matches[1]
        Write-Host "==> detected $Board  ($($boards[$Board]))" -ForegroundColor Yellow
    } else {
        $why = if ($probe -match '^seen (.+)$') { $Matches[1] } else { $probe }
        Write-Host ""
        Write-Host "Cannot tell which board is on $Port ($why)." -ForegroundColor Red
        Write-Host "The board is read off the batmon-remote firmware already on it; a new board,"
        Write-Host "or one running something else, has to be named once:"
        Write-Host ""
        $boards.Keys | ForEach-Object { Write-Host ("  .\tools\flash.ps1 -Board {0,-12} # {1}" -f $_, $boards[$_]) }
        exit 1
    }
}

$buildDir = if ($Remote) { Join-Path $proj "remote\build\$Board" } else { Join-Path $proj 'build' }
$buildCmd = if ($Remote) { ".\tools\idf.ps1 -Board $Board build" } else { '.\tools\idf.ps1 build' }
$argsFile = Join-Path $buildDir 'flash_project_args'

if (-not (Test-Path $argsFile)) {
    Write-Host ""
    Write-Host "No $argsFile." -ForegroundColor Red
    Write-Host "Build first:  $buildCmd"
    exit 1
}

# flash_project_args is written at CMake *configure* time, so its existence proves
# nothing about the images it names -- a configured-but-never-compiled build dir has
# the args file and no .bin at all. Left to esptool that surfaces as a bare
# "No such file or directory: 'bootloader/bootloader.bin'" under a heading about
# serial ports, which sends you hunting for a cable fault. Check it here instead.
$images = Get-Content $argsFile |
    ForEach-Object { ($_ -split '\s+') | Where-Object { $_ } } |
    Where-Object { $_ -notmatch '^(0x[0-9a-fA-F]+|--.*)$' -and $_ -match '\.bin$' }

$missing = @($images | Where-Object { -not (Test-Path (Join-Path $buildDir $_)) })
if ($missing.Count -gt 0) {
    # Not `throw`: PowerShell renders an exception message as one line inside the
    # error banner, so an embedded file list arrives run together and unreadable.
    # Print the lines and exit with a code instead.
    $rel = $buildDir.Substring($proj.Length + 1) -replace '\\', '/'
    Write-Host ""
    Write-Host "The build is incomplete -- $($missing.Count) of $($images.Count) images named in flash_project_args are missing:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "    $rel/$_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "Build first:  $buildCmd"
    Write-Host "If a build is running right now, wait for it: the images appear one by one" -ForegroundColor DarkGray
    Write-Host "as the build proceeds, and the app image is the last one written." -ForegroundColor DarkGray
    exit 1
}

# The directory name says which board a build is for, but its sdkconfig is what was
# compiled in, and menuconfig can change that without moving the directory. Flashing
# the wrong board's image gives a dark screen or dead touch, so check the build itself.
if ($Remote) {
    $hdr  = Join-Path $buildDir 'config\sdkconfig.h'
    $want = "CONFIG_REMOTE_BOARD_$($Board.ToUpper())"
    if (-not (Test-Path $hdr) -or -not (Select-String -Path $hdr -Pattern "#define $want 1" -SimpleMatch -Quiet)) {
        $built = if (Test-Path $hdr) {
            (Select-String -Path $hdr -Pattern '#define CONFIG_REMOTE_BOARD_(\w+) 1' |
                Select-Object -First 1).Matches.Groups[1].Value
        }
        Write-Host ""
        Write-Host "remote/build/$Board was built for board '$("$built".ToLower())', not '$Board'." -ForegroundColor Red
        Write-Host "Its sdkconfig (remote/sdkconfig.$Board) has another board chosen in menuconfig."
        Write-Host "Put it back to $Board in menuconfig, or delete remote/sdkconfig.$Board and rebuild:"
        Write-Host "  $buildCmd"
        exit 1
    }
    Write-Host "==> remote display for $Board  ($($boards[$Board]))" -ForegroundColor Cyan
}

# esptool must be run from build/ : the paths inside flash_project_args are
# relative to it.
Push-Location $buildDir
try {
    if ($Erase) {
        Write-Host "==> erasing flash on $Port" -ForegroundColor Yellow
        & python -m esptool --chip $chip -p $Port $cmdErase
        if ($LASTEXITCODE -ne 0) { throw "$cmdErase failed" }
    }

    Write-Host "==> flashing $Port at $Baud" -ForegroundColor Cyan
    # "@flash_project_args" must stay quoted. Unquoted, PowerShell reads a leading
    # @ as splatting syntax and the argument never reaches esptool.
    $etArgs = @('-m', 'esptool', '--chip', $chip, '-p', $Port, '-b', "$Baud",
                $cmdWrite, '@flash_project_args')
    Write-Verbose ("python " + ($etArgs -join ' '))
    & python $etArgs

    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "Flash failed. Things worth trying, in order:" -ForegroundColor Red
        Write-Host "  1. Close anything holding the port (a monitor, a terminal, Arduino IDE)."
        Write-Host "  2. Enter the bootloader by hand: hold BOOT, tap RESET, release BOOT."
        Write-Host "  3. Lower the baud:  -Baud 115200"
        Write-Host "  4. Confirm the port:  Get-CimInstance Win32_PnPEntity | ? Name -match 'COM'"
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}

Write-Host "==> flashed" -ForegroundColor Green

if ($Monitor) {
    & (Join-Path $PSScriptRoot 'monitor.ps1') -Port $Port
} else {
    Write-Host "==> open the console with:  .\tools\monitor.ps1 -Port $Port" -ForegroundColor Green
}

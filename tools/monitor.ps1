<#
.SYNOPSIS
    Open the M1 bring-up console on the host.

.DESCRIPTION
    Prefers esp-idf-monitor, which decodes panic backtraces into source lines by
    reading the ELF -- worth having the first time something crashes. Falls back to
    pyserial's miniterm, which comes with esptool anyway.

    Both are interactive, which the M1 console needs: 'scan', 'zero', 'stream 500'
    are typed at a prompt.

        python -m pip install esp-idf-monitor

.EXAMPLE
    .\tools\monitor.ps1
    .\tools\monitor.ps1 -Port COM5

.NOTES
    Exit esp-idf-monitor with Ctrl-]. Exit miniterm with Ctrl-].
#>

[CmdletBinding()]
param(
    [string]$Port = $env:BATMON_PORT,
    [int]   $Baud = 115200
)

$ErrorActionPreference = 'Stop'

$proj = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$elf  = Join-Path $proj 'build\bat-monitor.elf'

if (-not $Port) {
    # USB ports only, Espressif's VID first: Bluetooth SPP links (BTHENUM\) and Intel
    # AMT serial-over-LAN (PCI\) are COM ports too, and cannot be the board.
    $cand = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '\(COM\d+\)' -and $_.PNPDeviceID -match '^(USB|FTDIBUS)\\' } |
            Sort-Object { $_.PNPDeviceID -notmatch 'VID_303A' } | Select-Object -First 1
    if (-not $cand) { throw "No USB serial port found. Is the XIAO plugged in? Or pass -Port COMn." }
    $Port = [regex]::Match($cand.Name, 'COM\d+').Value
    Write-Host "==> auto-detected $Port  ($($cand.Name))" -ForegroundColor Yellow
}

# Is esp-idf-monitor importable?
& python -c "import esp_idf_monitor" *> $null
$haveIdfMonitor = ($LASTEXITCODE -eq 0)

if ($haveIdfMonitor) {
    Write-Host "==> esp-idf-monitor on $Port  (Ctrl-] to exit)" -ForegroundColor Cyan
    if (Test-Path $elf) {
        & python -m esp_idf_monitor --port $Port --baud $Baud $elf
    } else {
        & python -m esp_idf_monitor --port $Port --baud $Baud
    }
} else {
    Write-Host "==> miniterm on $Port  (Ctrl-] to exit)" -ForegroundColor Cyan
    Write-Host "    install esp-idf-monitor for decoded backtraces:" -ForegroundColor DarkGray
    Write-Host "      python -m pip install esp-idf-monitor" -ForegroundColor DarkGray
    & python -m serial.tools.miniterm --raw $Port $Baud
}

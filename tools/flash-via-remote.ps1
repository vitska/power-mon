<#
.SYNOPSIS
    Update the monitor's firmware through a remote display on USB: no cable to the
    monitor, no phone.

.DESCRIPTION
    For a monitor installed where no USB reaches it. The remote display already holds a
    BLE link to it; this hands the image to the remote over its serial port, and the
    remote passes it on through the monitor's own OTA protocol -- the one the phone app
    uses (CLI.md §6). The remote's side is its `monitor-ota` console command
    (remote/main/console.c).

      1. Checks the image: an ESP32-C6 app image of bat-monitor, version read from it.
      2. Waits for the remote to have its monitor connected.
      3. monitor-ota <bytes> <sha256>: the monitor erases its spare slot; then the image
         goes over in 4 KB frames with a CRC-32 each. The remote acknowledges a frame
         when it is in intact (or asks for it again), and forwards it to the monitor in
         acknowledged writes while the next one arrives. The serial line at 115200 baud
         is the limit: ~11 KB/s, so a minute and a half for 800 KB.
      4. `ota end` on the monitor checks size, SHA-256 and the image itself.
      5. Unless -NoReboot: reboots the monitor, waits for the remote to reconnect, checks
         the new version is the one running, and confirms it -- without the confirm the
         monitor rolls back in ten minutes.

    Needs only esptool's own pyserial: python -m pip install esptool

.EXAMPLE
    .\tools\flash-via-remote.ps1 -Port COM22
.EXAMPLE
    .\tools\flash-via-remote.ps1 -Port COM22 -Bin build\bat-monitor.bin -NoReboot
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [string]$Bin,
    # Stop after `ota end`: the new image waits in the spare slot for a `reboot`.
    [switch]$NoReboot
)

$ErrorActionPreference = 'Stop'

$proj = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Bin) { $Bin = Join-Path $proj 'build\bat-monitor.bin' }
if (-not (Test-Path $Bin)) {
    Write-Host "No $Bin. Build the monitor first:  .\tools\idf.ps1 build" -ForegroundColor Red
    exit 1
}
$Bin = (Resolve-Path $Bin).Path

# Python does the serial work: pyserial comes with esptool, and the byte-level framing
# is plainer there than in PowerShell. Every line it prints is shown as it comes.
$py = @'
import hashlib, re, sys, time, zlib
import serial

port, path, reboot = sys.argv[1], sys.argv[2], sys.argv[3] == '1'
img = open(path, 'rb').read()

# The image's own identity, as the phone app reads it: chip id at 12, the app
# descriptor at 32 -- version 16 bytes in, project name 48.
if img[0] != 0xE9:
    sys.exit('not an ESP32 app image (a merged or bootloader .bin will not do)')
chip = img[12] | img[13] << 8
ver = img[0x30:0x50].split(b'\0')[0].decode()
proj = img[0x50:0x70].split(b'\0')[0].decode()
if proj != 'bat-monitor' or chip != 13:
    sys.exit(f'image is {proj!r} for chip id {chip}, not bat-monitor for the ESP32-C6')
sha = hashlib.sha256(img).hexdigest()
print(f'==> {path}: bat-monitor {ver}, {len(img)} bytes', flush=True)

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False  # both released: opening the port must not reset the remote
s.rts = False
s.open()

def noteworthy(l):
    """A log line worth showing: the remote's warnings and errors, and its link."""
    return re.match(r'^[WE] \(', l) or ' link: ' in l

buf = ''
def lines(secs):
    """Lines from the remote for up to `secs`, log lines included."""
    global buf
    end = time.time() + secs
    while time.time() < end:
        buf += s.read(4096).decode('ascii', 'replace')
        while '\n' in buf:
            line, buf = buf.split('\n', 1)
            yield re.sub(r'\x1b\[[0-9;]*m', '', line).rstrip('\r')

def cmd(c, until, secs, show=True):
    """Send a console command; return the first line matching `until`, or None."""
    s.write((c + '\r').encode())
    for l in lines(secs):
        if show and l and (not re.match(r'^[IWE] \(\d+\) ', l) or noteworthy(l)):
            print('   ', l, flush=True)
        m = re.search(until, l)
        if m:
            return m
    return None

def wait_connected(secs):
    end = time.time() + secs
    while time.time() < end:
        m = cmd('status', r'^board .*\((\w[\w ]*)\)', 3, show=False)
        if m and m.group(1) == 'connected':
            return True
        time.sleep(2)
    return False

print('==> waiting for the remote to have the monitor connected', flush=True)
if not wait_connected(90):
    sys.exit('the remote is not connected to a monitor (its `status` says so)')
m = cmd('status', r'^monitor\s+firmware (\S+)', 3, show=False)
print(f'==> monitor runs {m.group(1) if m else "?"}; sending {ver}', flush=True)

m = cmd(f'monitor-ota {len(img)} {sha}', r'^(READY (\d+)|ERR .*)', 90)
if not m or not m.group(1).startswith('READY'):
    sys.exit('the remote did not start the update')
frame = int(m.group(2))

t0, off, resends = time.time(), 0, 0
while off < len(img):
    # A frame and its CRC-32, little-endian: the remote answers OK once it is in and
    # intact, or RESEND if the line lost or bent a byte of it.
    data = img[off:off + frame]
    s.write(data + zlib.crc32(data).to_bytes(4, 'little'))
    got = None
    for l in lines(60):
        if noteworthy(l):
            print('\n   ', l, flush=True)
        r = re.search(r'^(OK (\d+)|RESEND (\d+)|ERR .*)', l)
        if r:
            got = r
            break
    if got and got.group(1).startswith('RESEND'):
        resends += 1
        print(f'\n    frame at {off} again (serial error)', flush=True)
        continue
    if not got or not got.group(1).startswith('OK'):
        sys.exit(f'transfer stopped at {off}: {got.group(1) if got else "no answer from the remote"}')
    off = int(got.group(2))
    rate = off / max(time.time() - t0, 0.001) / 1024
    print(f'\r    {off}/{len(img)} bytes  {100 * off // len(img)} %  {rate:.1f} KB/s ',
          end='', flush=True)
print(flush=True)

m = None
for l in lines(90):
    if l and not re.match(r'^[IWE] \(\d+\) ', l):
        print('   ', l, flush=True)
    m = re.search(r'^(DONE|ERR .*)', l)
    if m:
        break
if not m or m.group(1) != 'DONE':
    sys.exit('the monitor did not accept the image')
print(f'==> {ver} written to the monitor in {time.time() - t0:.0f} s' + (f', {resends} frame(s) resent' if resends else ''), flush=True)

if not reboot:
    print('==> not rebooted (-NoReboot): it starts on the monitor\'s next reset', flush=True)
    sys.exit(0)

print('==> rebooting the monitor', flush=True)
cmd('monitor reboot', r'^(monitor: exit|no answer|cannot)', 20)
time.sleep(5)
print('==> waiting for it to come back', flush=True)
if not wait_connected(120):
    sys.exit('the monitor did not come back within 2 min; it rolls back on its own in 10')
m = cmd('status', r'^monitor\s+firmware (\S+)', 3, show=False)
now = m.group(1) if m else '?'
if now != ver:
    sys.exit(f'the monitor came back running {now}, not {ver}: it rolled back')
cmd('monitor ota confirm', r'^(monitor: exit|no answer|cannot)', 20)
print(f'==> monitor runs {ver}, confirmed', flush=True)
'@

$tmp = Join-Path ([IO.Path]::GetTempPath()) 'batmon-flash-via-remote.py'
Set-Content -Path $tmp -Value $py -Encoding utf8
try {
    & python $tmp $Port $Bin $(if ($NoReboot) { '0' } else { '1' })
    $code = $LASTEXITCODE
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
if ($code -ne 0) {
    Write-Host "==> failed" -ForegroundColor Red
    exit $code
}
Write-Host "==> done" -ForegroundColor Green

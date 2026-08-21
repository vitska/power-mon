<#
.SYNOPSIS
    Run an idf.py command inside the official Espressif ESP-IDF container.

.DESCRIPTION
    The project directory is bind-mounted at /project, so build/ , sdkconfig and
    all artefacts land on the host and persist between runs.

    Flashing and monitoring are NOT done here: Docker Desktop on Windows cannot
    pass a COM port into a container. Use tools\flash.ps1 and tools\monitor.ps1,
    which run esptool on the host against the binaries built here.

.EXAMPLE
    .\tools\idf.ps1 set-target esp32c6      # once, first time
    .\tools\idf.ps1 build
    .\tools\idf.ps1 menuconfig
    .\tools\idf.ps1 fullclean

.NOTES
    Override the image with $env:BATMON_IDF_IMAGE if you need a different
    ESP-IDF version. The project needs >= 5.3 (see README).
#>

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = 'Stop'

$image = if ($env:BATMON_IDF_IMAGE) { $env:BATMON_IDF_IMAGE } else { 'espressif/idf:release-v5.3' }
$proj  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Docker's -v parser chokes on Windows backslashes and reports the confusing
# "invalid reference format", as though the image name were at fault. Forward
# slashes are accepted on all platforms.
$mount = ($proj -replace '\\', '/').TrimEnd('/')

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "docker not found on PATH. Install Docker Desktop, or install ESP-IDF natively."
}

try { docker info *> $null } catch {
    throw "Docker is installed but not responding. Is Docker Desktop running?"
}

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) {
    $IdfArgs = @('build')
}

# Two ninjas in one build/ corrupt it. They do not fail cleanly: both write the
# same .a and .obj files, and the loser surfaces minutes later as
# "file too short", "ELF section name out of range" or
# "file format not recognized" from objdump/ld -- errors that read like a broken
# toolchain and cost an hour to trace back to a second terminal.
#
# The interlock is a container label rather than a lock file: no stale state to
# clean up after a Ctrl-C, because the label disappears with the container.
$label   = 'batmon-build=' + ($mount -replace '[^A-Za-z0-9]', '-')
$running = @(docker ps -q --filter "label=$label" 2>$null)
if ($running.Count -gt 0) {
    Write-Host ""
    Write-Host "A build is already running on this project (container $($running[0]))." -ForegroundColor Red
    Write-Host "Two builds sharing one build/ directory corrupt each other's archives," -ForegroundColor Red
    Write-Host "so this one is refused rather than started." -ForegroundColor Red
    Write-Host ""
    Write-Host "Wait for it, or stop it:  docker stop $($running[0])"
    exit 1
}

# menuconfig is a full-screen TUI and needs a TTY; plain builds do not, and
# allocating one there just garbles the log when output is redirected.
$interactive = $IdfArgs -contains 'menuconfig'
$ttyFlags = if ($interactive) { @('-it') } else { @('-i') }

Write-Host "==> $image : idf.py $($IdfArgs -join ' ')" -ForegroundColor Cyan

# Pull once rather than on every run; docker run would do it implicitly, but the
# progress output tangles with the first build's log.
# Test the exit code, not the output: `docker image inspect` prints a literal "[]"
# for a missing image, which is a non-empty -- and therefore truthy -- string.
docker image inspect $image *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "==> pulling $image (first run, ~2.5 GB)" -ForegroundColor Yellow
    docker pull $image
    if ($LASTEXITCODE -ne 0) { throw "docker pull failed" }
}

# Assembled as one explicit array rather than a backtick-continued argument list.
# PowerShell's handling of continuations plus array splatting in a native-command
# invocation is subtle enough that "what actually got passed" is worth being able
# to see; -Verbose prints it.
$dockerArgs = @('run', '--rm') +
              $ttyFlags +
              @('-v', "${mount}:/project",
                '-w', '/project',
                '--label', $label,
                '-e', 'IDF_CCACHE_ENABLE=1',
                $image,
                'idf.py') +
              $IdfArgs

Write-Verbose ("docker " + ($dockerArgs -join ' '))

& docker $dockerArgs

$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Host "==> idf.py exited with $code" -ForegroundColor Red
    exit $code
}

if ($IdfArgs -contains 'build') {
    $bin = Join-Path $proj 'build\bat-monitor.bin'
    if (Test-Path $bin) {
        $kb = [math]::Round((Get-Item $bin).Length / 1KB)
        Write-Host "==> build\bat-monitor.bin  ($kb KB)" -ForegroundColor Green
        Write-Host "==> flash with:  .\tools\flash.ps1 -Port COM5" -ForegroundColor Green
    }
}

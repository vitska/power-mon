<#
.SYNOPSIS
    Build the Android client in ./android and install it on a connected phone.

.DESCRIPTION
    Runs gradlew assembleDebug, then installs the APK with adb and starts the app.
    Neither java nor adb needs to be on PATH:

      - JDK:  $env:JAVA_HOME if set, else the JBR bundled with Android Studio.
      - SDK:  sdk.dir from android/local.properties, else $env:ANDROID_HOME /
              $env:ANDROID_SDK_ROOT, else the Android Studio default under
              %LOCALAPPDATA%. adb is taken from its platform-tools.

    The build and the install are separate steps, rather than one gradle
    installDebug, on purpose: installDebug installs to every attached device and
    reduces adb's failure reasons to a gradle stack trace. Calling adb directly
    targets one device and keeps its INSTALL_FAILED_* code readable.

    A debug build is signed with this machine's debug keystore. A copy of the app
    installed from another machine (or Android Studio on another PC) carries a
    different signature, and Android refuses to update over it. -Reinstall
    uninstalls first, which also wipes the app's data -- the remembered boards.

.EXAMPLE
    .\tools\android.ps1                     # build, install, launch
    .\tools\android.ps1 -Serial R58N12ABCDE # pick a device when several are attached
    .\tools\android.ps1 -NoBuild            # reinstall the last APK
    .\tools\android.ps1 -Clean -Logcat      # clean build, then follow the app's log
    .\tools\android.ps1 -Reinstall          # signature mismatch: uninstall first
#>

[CmdletBinding()]
param(
    [string]$Serial = $env:ANDROID_SERIAL,
    [switch]$Clean,
    [switch]$NoBuild,
    [switch]$NoLaunch,
    [switch]$Reinstall,
    [switch]$Logcat
)

$ErrorActionPreference = 'Stop'

$proj       = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$androidDir = Join-Path $proj 'android'
$apk        = Join-Path $androidDir 'app\build\outputs\apk\debug\app-debug.apk'
$appId      = 'ru.vitska.powermon'

# --- JDK -------------------------------------------------------------------------

if (-not $env:JAVA_HOME -or -not (Test-Path (Join-Path $env:JAVA_HOME 'bin\java.exe'))) {
    $jbr = @(
        "$env:ProgramFiles\Android\Android Studio\jbr",
        "$env:LOCALAPPDATA\Programs\Android Studio\jbr"
    ) | Where-Object { Test-Path (Join-Path $_ 'bin\java.exe') } | Select-Object -First 1
    if (-not $jbr -and -not $NoBuild) {
        throw "No JDK found. Set JAVA_HOME to a JDK 17, or install Android Studio (its bundled JBR works)."
    }
    $env:JAVA_HOME = $jbr
}
if (-not $NoBuild) {
    Write-Host "==> JDK  $env:JAVA_HOME" -ForegroundColor DarkGray
}

# --- SDK and adb -----------------------------------------------------------------

$localProps = Join-Path $androidDir 'local.properties'
$sdk = $null
if (Test-Path $localProps) {
    $line = Get-Content $localProps | Where-Object { $_ -match '^\s*sdk\.dir\s*=' } | Select-Object -First 1
    if ($line) {
        # local.properties is a Java properties file: "C\:\\Users\\..." is legal there.
        $sdk = ($line -replace '^\s*sdk\.dir\s*=\s*', '') -replace '\\(.)', '$1'
    }
}
if (-not $sdk) {
    $sdk = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "$env:LOCALAPPDATA\Android\Sdk") |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $sdk) {
        throw "No Android SDK found. Install it via Android Studio, or set ANDROID_HOME."
    }
    # Gradle needs it too, and this file is gitignored for exactly this purpose.
    "sdk.dir=$($sdk -replace '\\', '/')" | Set-Content $localProps
    Write-Host "==> wrote $localProps" -ForegroundColor Yellow
}

$adb = Join-Path $sdk 'platform-tools\adb.exe'
if (-not (Test-Path $adb)) {
    throw "No adb at $adb. Install 'Android SDK Platform-Tools' from the Android Studio SDK Manager."
}
Write-Host "==> SDK  $sdk" -ForegroundColor DarkGray

# --- device ----------------------------------------------------------------------

# Resolve the device before building: finding out there is no phone after a
# two-minute build is the wrong order.
function Get-AdbDevices {
    & $adb devices -l | Select-Object -Skip 1 | Where-Object { $_.Trim() } | ForEach-Object {
        $f = $_ -split '\s+'
        [pscustomobject]@{
            Serial = $f[0]
            State  = $f[1]
            Model  = ([regex]::Match($_, 'model:(\S+)').Groups[1].Value)
        }
    }
}

$devices = @(Get-AdbDevices)
$ready   = @($devices | Where-Object State -eq 'device')

# A phone that is attached but not usable says why, and the reason is always on the
# phone itself -- say so rather than reporting "no device".
foreach ($d in $devices | Where-Object State -ne 'device') {
    switch ($d.State) {
        'unauthorized' { Write-Host "$($d.Serial): unauthorized -- accept the 'Allow USB debugging?' prompt on the phone." -ForegroundColor Yellow }
        'offline'      { Write-Host "$($d.Serial): offline -- replug the cable, or toggle USB debugging off and on." -ForegroundColor Yellow }
        default        { Write-Host "$($d.Serial): $($d.State)" -ForegroundColor Yellow }
    }
}

if ($Serial) {
    if ($ready.Serial -notcontains $Serial) {
        Write-Host ""
        Write-Host "Device $Serial is not attached and ready." -ForegroundColor Red
        if ($ready.Count) {
            Write-Host "Ready devices:"
            $ready | ForEach-Object { Write-Host "  $($_.Serial)  $($_.Model)" }
        }
        exit 1
    }
} elseif ($ready.Count -eq 1) {
    $Serial = $ready[0].Serial
} elseif ($ready.Count -eq 0) {
    Write-Host ""
    Write-Host "No Android device ready." -ForegroundColor Red
    Write-Host "  1. Enable Developer options: Settings > About phone > tap 'Build number' 7 times."
    Write-Host "  2. Enable 'USB debugging' in Developer options, plug in, accept the prompt."
    Write-Host "  3. Or pair over Wi-Fi:  & '$adb' pair <ip>:<port>  then  connect <ip>:<port>"
    if (-not $NoBuild) {
        Write-Host ""
        Write-Host "Building anyway, so the APK is ready when a device is." -ForegroundColor DarkGray
    }
} else {
    Write-Host ""
    Write-Host "Several devices attached; pick one with -Serial:" -ForegroundColor Red
    $ready | ForEach-Object { Write-Host "  $($_.Serial)  $($_.Model)" }
    exit 1
}

if ($Serial) {
    $model = ($ready | Where-Object Serial -eq $Serial).Model
    Write-Host "==> device $Serial  $model" -ForegroundColor Yellow
}

# --- build -----------------------------------------------------------------------

if (-not $NoBuild) {
    $tasks = @()
    if ($Clean) { $tasks += 'clean' }
    $tasks += 'assembleDebug'

    Write-Host "==> gradlew $($tasks -join ' ')" -ForegroundColor Cyan
    Push-Location $androidDir
    try {
        & .\gradlew.bat @tasks
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $apk)) {
    throw "No APK at $apk. Build first (drop -NoBuild)."
}
if (-not $Serial) { exit 1 }

# --- install ---------------------------------------------------------------------

if ($Reinstall) {
    Write-Host "==> uninstalling $appId (this clears its data)" -ForegroundColor Yellow
    # Not installed is fine; the output is the only signal and it is not an error.
    & $adb -s $Serial uninstall $appId | Out-Null
}

Write-Host "==> installing $(Split-Path $apk -Leaf)" -ForegroundColor Cyan
$out = & $adb -s $Serial install -r $apk 2>&1 | Out-String
Write-Host $out.Trim()
if ($LASTEXITCODE -ne 0 -or $out -match 'Failure') {
    Write-Host ""
    if ($out -match 'INSTALL_FAILED_UPDATE_INCOMPATIBLE|signatures do not match') {
        Write-Host "The installed copy was signed with a different key (built on another machine)." -ForegroundColor Red
        Write-Host "Rerun with -Reinstall to uninstall it first. That clears the app's saved boards."
    } elseif ($out -match 'INSTALL_FAILED_USER_RESTRICTED|INSTALL_FAILED_ABORTED') {
        Write-Host "The phone refused the install. Some vendors (Xiaomi, Oppo) need" -ForegroundColor Red
        Write-Host "'Install via USB' enabled in Developer options, and a prompt accepted on the phone."
    } elseif ($out -match 'INSTALL_FAILED_OLDER_SDK') {
        Write-Host "The phone runs Android older than 8.0 (API 26), which this app requires." -ForegroundColor Red
    } elseif ($out -match 'INSTALL_FAILED_INSUFFICIENT_STORAGE') {
        Write-Host "Not enough free storage on the phone." -ForegroundColor Red
    } else {
        Write-Host "Install failed." -ForegroundColor Red
    }
    exit 1
}

# --- launch ----------------------------------------------------------------------

if (-not $NoLaunch) {
    Write-Host "==> launching $appId" -ForegroundColor Cyan
    & $adb -s $Serial shell am start -n "$appId/.MainActivity" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "am start failed" }
}

Write-Host "==> installed on $Serial" -ForegroundColor Green

if ($Logcat) {
    # Filter by the app's pid, which only exists once it is running. Retry briefly
    # since am start returns before the process is up.
    $appPid = $null
    for ($i = 0; $i -lt 20 -and -not $appPid; $i++) {
        $appPid = (& $adb -s $Serial shell pidof $appId 2>$null | Out-String).Trim()
        if (-not $appPid) { Start-Sleep -Milliseconds 250 }
    }
    if (-not $appPid) { throw "$appId is not running; cannot follow its log. Drop -NoLaunch." }
    Write-Host "==> logcat for pid $appPid (Ctrl+C to stop)" -ForegroundColor DarkGray
    & $adb -s $Serial logcat --pid=$appPid
}

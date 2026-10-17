<#
.SYNOPSIS
    Cut a release -- the monitor firmware, the remote display (-Remote), or the Android
    app (-App): bump the version, build, verify, tag, and publish on GitHub.

.DESCRIPTION
    Two firmwares live in this repository, each with its own version.txt and its own
    release series:

      monitor  version.txt         tag vX.Y.Z          asset bat-monitor.bin
      remote   remote/version.txt  tag remote-vX.Y.Z   assets batmon-remote-<board>.bin,
               one per board
      app      android/app/build.gradle.kts (versionName; a bump also raises
               versionCode)       tag app-vX.Y.Z      asset battery-monitor-X.Y.Z.apk

    The phone app offers a monitor update when GitHub's LATEST release of this repo is
    newer than the board (README.md "Versioning"). So a monitor release is always
    published as latest, and a remote release never is -- otherwise the newest remote
    release would hide the monitor firmware from the app, which finds no bat-monitor.bin
    on it and reports that nothing is published.

    Every release is made the same way:

      1. Refuses a dirty tree: a released binary must correspond to a commit.
      2. With -Bump, raises the version file and commits "Release [remote] X.Y.Z".
      3. Builds, and reads the version and project name back out of the built image's
         app descriptor. A mismatch -- a stale build dir, a PROJECT_VER override --
         stops here rather than publishing a mislabelled image.
      4. Tags, pushes the commit and the tag, and creates the GitHub release with the
         image attached. The notes list the commits that touched that firmware since
         its own previous release.

    GitHub access: gh, using the account that owns the repo (taken from the origin
    URL) if gh is logged in to it, so the active gh account need not be switched.

.EXAMPLE
    .\tools\release.ps1 -Bump patch -DryRun   # bump, build, verify; publish nothing
    .\tools\release.ps1 -Bump minor           # monitor 0.8.0 -> 0.9.0, published
    .\tools\release.ps1 -Remote -Bump patch   # remote display 0.1.0 -> 0.1.1
    .\tools\release.ps1 -App -Bump minor      # Android app, APK on GitHub
    .\tools\release.ps1                       # publish version.txt as it stands
#>

[CmdletBinding()]
param(
    [ValidateSet('patch', 'minor', 'major')]
    [string]$Bump,
    [string]$Notes,
    [switch]$Remote,
    [switch]$App,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$proj = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ($Remote -and $App) {
    Write-Host "-Remote and -App are different releases; pick one." -ForegroundColor Red
    exit 1
}

# Everything that differs between the three.
$t = if ($App) {
    @{
        Label       = 'Android app'
        VersionFile = 'android\app\build.gradle.kts'
        Bin         = 'android\app\build\outputs\apk\release\app-release.apk'
        TagPrefix   = 'app-v'
        Title       = 'Android app'
        Paths       = @('android')
        Latest      = $false
        Blurb       = 'The Battery monitor Android app: monitor, configure, console, and firmware updates for the monitor and the remote display.' +
                      "`n`nInstall: download the APK on the phone and open it (allow installing from your browser when Android asks). It installs over an earlier version, keeping the saved boards."
    }
} elseif ($Remote) {
    @{
        Label       = 'remote display'
        VersionFile = 'remote\version.txt'
        # One image per board (remote/main/board.h), each published as
        # batmon-remote-<board>.bin; the app picks the one the display's `ver` names,
        # and a remote too old to name one gets cyd's -- all it ever ran. No plain
        # batmon-remote.bin any more: a second copy of one board's image under a name
        # that fits every board is exactly how the wrong board gets updated.
        Boards      = @('cyd', 'cyd2usb', 'jc2432w328r', 'jc1732w319')
        Bin         = 'remote\build\cyd\batmon-remote.bin'
        Project     = 'batmon-remote'
        TagPrefix   = 'remote-v'
        Title       = 'Remote display'
        Paths       = @('remote')
        Latest      = $false
        Blurb       = 'Remote display firmware, one image per board:' +
                      "`n`n- ``batmon-remote-cyd.bin``: ESP32-2432S028R, single micro-USB (ILI9341)" +
                      "`n- ``batmon-remote-cyd2usb.bin``: ESP32-2432S028R, USB-C + micro-USB (ST7789)" +
                      "`n- ``batmon-remote-jc2432w328r.bin``: GUITION JC2432W328R" +
                      "`n- ``batmon-remote-jc1732w319.bin``: GUITION JC1732W319, 1.9in, no touch" +
                      "`n`nThe power-mon app picks the right one by itself. Over USB: tools/flash.ps1 -Board <board>."
    }
} else {
    @{
        Label       = 'monitor'
        VersionFile = 'version.txt'
        BuildArgs   = @('build')
        Bin         = 'build\bat-monitor.bin'
        Project     = 'bat-monitor'
        TagPrefix   = 'v'
        Title       = 'Firmware'
        Paths       = @('.', ':(exclude)remote')
        Latest      = $true
        Blurb       = 'Firmware for the battery monitor on the XIAO ESP32-C6.' +
                      "`n`nInstall from the power-mon app (Firmware tab), or over USB with tools/flash.ps1."
    }
}
$versionFile = Join-Path $proj $t.VersionFile
$bin         = Join-Path $proj $t.Bin
$asset       = Split-Path $t.Bin -Leaf

# The app's version lives in the Gradle script, not in a version.txt.
function Get-Version {
    $text = Get-Content $versionFile -Raw
    if ($App) {
        if ($text -notmatch 'versionName\s*=\s*"([^"]+)"') { return $null }
        return $Matches[1]
    }
    return $text.Trim()
}

function Fail([string]$msg) {
    Write-Host $msg -ForegroundColor Red
    exit 1
}

# Reads the version and project name back out of a built image, and its board tag when
# a board is expected, and stops on any mismatch -- a stale build dir, a PROJECT_VER
# override, a board switched in menuconfig -- rather than publish a mislabelled image.
# The app descriptor sits right after the 24-byte image header and the first 8-byte
# segment header, on the C6 and the classic ESP32 alike; version is 16 bytes into it,
# the project name 48. The board tag follows the 256-byte descriptor (main/ota.h,
# BOARD TAG): "BORD", then the name. Same offsets the phone app reads.
function Test-Image([string]$path, [string]$board) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $built = [Text.Encoding]::ASCII.GetString($bytes, 48, 32).TrimEnd([char]0)
    $name  = [Text.Encoding]::ASCII.GetString($bytes, 80, 32).TrimEnd([char]0)
    if ($built -ne $version -or $name -ne $t.Project) {
        Fail "$path says '$name' $built, not $($t.Project) $version. Try a fullclean."
    }
    if ($board) {
        $magic = [Text.Encoding]::ASCII.GetString($bytes, 0x120, 4)
        $tag   = [Text.Encoding]::ASCII.GetString($bytes, 0x124, 28).Split([char]0)[0]
        if ($magic -ne 'BORD' -or $tag -ne $board) {
            Fail "$path is tagged for board '$tag' ($magic), not $board."
        }
    }
}

Push-Location $proj
try {
    # --- preconditions -----------------------------------------------------------

    if (git status --porcelain) {
        Fail "The working tree has uncommitted changes. Commit or stash them first: a release must be a commit."
    }

    $version = Get-Version
    if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        Fail "$($t.VersionFile) holds '$version'; a release needs MAJOR.MINOR.PATCH."
    }
    $maj = [int]$Matches[1]; $min = [int]$Matches[2]; $pat = [int]$Matches[3]

    if ($Bump) {
        switch ($Bump) {
            'major' { $maj++; $min = 0; $pat = 0 }
            'minor' { $min++; $pat = 0 }
            'patch' { $pat++ }
        }
        $version = "$maj.$min.$pat"
    }
    $tag = "$($t.TagPrefix)$version"

    if (git tag --list $tag) {
        Fail "Tag $tag already exists. Bump the version (-Bump patch) instead of re-releasing it."
    }

    $origin = git remote get-url origin
    if ($origin -notmatch 'github\.com[:/]([^/]+)/([^/.]+)') {
        Fail "origin ($origin) is not a GitHub repository."
    }
    $owner = $Matches[1]; $repo = "$($Matches[1])/$($Matches[2])"

    # --- bump --------------------------------------------------------------------

    if ($Bump) {
        if ($App) {
            # versionName to the new version; versionCode up by one, because Android
            # refuses to install an update whose versionCode is not higher.
            $text = Get-Content $versionFile -Raw
            if ($text -notmatch 'versionCode\s*=\s*(\d+)') { Fail "no versionCode in $($t.VersionFile)" }
            $code = [int]$Matches[1] + 1
            $text = $text -replace 'versionCode\s*=\s*\d+', "versionCode = $code"
            $text = $text -replace 'versionName\s*=\s*"[^"]+"', "versionName = `"$version`""
            [IO.File]::WriteAllText($versionFile, $text)
        } else {
            Set-Content -Path $versionFile -Value $version -NoNewline:$false
        }
        git add $t.VersionFile
        $what = if ($Remote) { "Release remote $version" } elseif ($App) { "Release app $version" } else { "Release $version" }
        git commit -q -m $what
        if ($LASTEXITCODE -ne 0) { Fail "commit failed" }
        Write-Host "==> $($t.VersionFile) -> $version (committed)" -ForegroundColor Cyan
    }

    # --- build and verify ----------------------------------------------------------

    if ($App) {
        & (Join-Path $PSScriptRoot 'android.ps1') -Release -NoInstall
        if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

        # Read the version back out of the APK itself, as the firmware checks read the
        # image's descriptor: aapt from the newest installed build-tools.
        $sdkDir = ((Get-Content (Join-Path $proj 'android\local.properties') |
                    Where-Object { $_ -match '^sdk\.dir=' }) -replace '^sdk\.dir=', '') -replace '\\(.)', '$1'
        $aapt = Get-ChildItem (Join-Path $sdkDir 'build-tools') -Directory | Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'aapt.exe' } | Where-Object { Test-Path $_ } |
                Select-Object -First 1
        if (-not $aapt) { Fail "No aapt in the SDK's build-tools; cannot verify the APK." }
        $badging = (& $aapt dump badging $bin) -join "`n"
        # " versionName=" with its leading space: the same line also carries
        # platformBuildVersionName, which a looser pattern matches instead.
        if ($badging -notmatch "package: name='ru\.vitska\.powermon'[^\n]*? versionName='([^']+)'") {
            Fail "Cannot read the package and version out of $bin."
        }
        if ($Matches[1] -ne $version) {
            Fail "The built APK says version $($Matches[1]), not $version."
        }
        # Published under a name that says what it is, rather than app-release.apk.
        $asset = "battery-monitor-$version.apk"
        $named = Join-Path (Split-Path $bin) $asset
        Copy-Item $bin $named -Force
        $files = @([pscustomobject]@{ Asset = $asset; Path = $named })
    } elseif ($t.Boards) {
        # Every board, each built and checked on its own: a release missing one board's
        # image would leave that board's app offering an update it cannot find.
        $dist = Join-Path $proj 'remote\build\dist'
        New-Item -ItemType Directory -Force $dist | Out-Null
        $files = @()
        foreach ($b in $t.Boards) {
            & (Join-Path $PSScriptRoot 'idf.ps1') -Board $b build
            if ($LASTEXITCODE -ne 0) { Fail "Build failed for $b." }
            $built = Join-Path $proj "remote\build\$b\batmon-remote.bin"
            Test-Image $built $b
            $named = Join-Path $dist "batmon-remote-$b.bin"
            Copy-Item $built $named -Force
            $files += [pscustomobject]@{ Asset = "batmon-remote-$b.bin"; Path = $named }
        }
        # A stale copy from a release before this one would otherwise sit beside them.
        Remove-Item (Join-Path $dist 'batmon-remote.bin') -ErrorAction SilentlyContinue
    } else {
        & (Join-Path $PSScriptRoot 'idf.ps1') @($t.BuildArgs)
        if ($LASTEXITCODE -ne 0) { Fail "Build failed." }
        Test-Image $bin $null
        $files = @([pscustomobject]@{ Asset = $asset; Path = $bin })
    }
    foreach ($f in $files) {
        $f | Add-Member Sha (Get-FileHash $f.Path -Algorithm SHA256).Hash.ToLower()
        Write-Host "==> $($f.Asset)  $version  $([math]::Round((Get-Item $f.Path).Length / 1KB)) KB  sha256 $($f.Sha)" -ForegroundColor Green
    }

    if ($DryRun) {
        Write-Host "==> dry run: not tagging, pushing or publishing." -ForegroundColor Yellow
        if ($Bump) {
            Write-Host "    The version bump is committed locally. Undo it with:  git reset --hard HEAD~1" -ForegroundColor Yellow
        }
        exit 0
    }

    # --- publish -----------------------------------------------------------------

    $token = (& gh auth token --user $owner 2>$null)
    if ($LASTEXITCODE -eq 0 -and $token) {
        $env:GH_TOKEN = $token
        Write-Host "==> publishing as $owner" -ForegroundColor DarkGray
    } else {
        Write-Host "==> gh is not logged in as $owner; using the active gh account" -ForegroundColor Yellow
    }
    # Push with the same account gh uses, rather than whatever the credential store holds.
    $cred = '!f() { echo username=x-access-token; echo "password=$GH_TOKEN"; }; f'

    git tag -a $tag -m "$($t.Title) $version"
    if ($env:GH_TOKEN) {
        git -c credential.helper= -c "credential.helper=$cred" push origin HEAD $tag
    } else {
        git push origin HEAD $tag
    }
    if ($LASTEXITCODE -ne 0) { Fail "Push failed. The tag $tag exists locally only; delete it with: git tag -d $tag" }

    if (-not $Notes) {
        # The previous release of THIS firmware, and only the commits that touched it:
        # a remote release should not list monitor changes, nor the other way round.
        $prev  = git describe --tags --abbrev=0 --match "$($t.TagPrefix)[0-9]*" "$tag^" 2>$null
        $range = if ($prev) { "$prev..$tag" } else { $tag }
        $log   = @(git log --format='- %s' $range -- @($t.Paths))
        if ($log.Count -eq 0) { $log = @('- (no changes to this firmware since the last release)') }
        $shas  = ($files | ForEach-Object { "SHA-256 of $($_.Asset): ``$($_.Sha)``" }) -join "`n"
        $Notes = "$($t.Blurb)`n`n" +
                 "$shas`n`n" +
                 "Changes:`n" + ($log -join "`n")
    }

    # --latest decides what /releases/latest returns, which is what the phone app reads.
    $latest = if ($t.Latest) { '--latest' } else { '--latest=false' }
    $paths  = @($files | ForEach-Object Path)
    gh release create $tag @paths --repo $repo --title "$($t.Title) $version" --notes $Notes $latest
    if ($LASTEXITCODE -ne 0) { Fail "Creating the GitHub release failed; the tag is pushed. Retry with: gh release create $tag $($paths -join ' ') --repo $repo $latest" }

    Write-Host "==> released ${tag}: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
}
finally {
    Pop-Location
}

<#
.SYNOPSIS
    Cut a firmware release -- the monitor, or with -Remote the remote display: bump the
    version, build, tag, and publish on GitHub.

.DESCRIPTION
    Two firmwares live in this repository, each with its own version.txt and its own
    release series:

      monitor  version.txt         tag vX.Y.Z          asset bat-monitor.bin
      remote   remote/version.txt  tag remote-vX.Y.Z   asset batmon-remote.bin

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
    .\tools\release.ps1                       # publish version.txt as it stands
#>

[CmdletBinding()]
param(
    [ValidateSet('patch', 'minor', 'major')]
    [string]$Bump,
    [string]$Notes,
    [switch]$Remote,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$proj = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# Everything that differs between the two firmwares.
$t = if ($Remote) {
    @{
        Label       = 'remote display'
        VersionFile = 'remote\version.txt'
        BuildArgs   = @('--project-dir', 'remote', 'build')
        Bin         = 'remote\build\batmon-remote.bin'
        Project     = 'batmon-remote'
        TagPrefix   = 'remote-v'
        Title       = 'Remote display'
        Paths       = @('remote')
        Latest      = $false
        Blurb       = 'Remote display firmware for the ESP32-2432S028 ("Cheap Yellow Display").' +
                      "`n`nFlash over USB with tools/flash.ps1 -Remote."
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

function Fail([string]$msg) {
    Write-Host $msg -ForegroundColor Red
    exit 1
}

Push-Location $proj
try {
    # --- preconditions -----------------------------------------------------------

    if (git status --porcelain) {
        Fail "The working tree has uncommitted changes. Commit or stash them first: a release must be a commit."
    }

    $version = (Get-Content $versionFile -Raw).Trim()
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
        Set-Content -Path $versionFile -Value $version -NoNewline:$false
        git add $t.VersionFile
        $what = if ($Remote) { "Release remote $version" } else { "Release $version" }
        git commit -q -m $what
        if ($LASTEXITCODE -ne 0) { Fail "commit failed" }
        Write-Host "==> $($t.VersionFile) -> $version (committed)" -ForegroundColor Cyan
    }

    # --- build and verify ----------------------------------------------------------

    & (Join-Path $PSScriptRoot 'idf.ps1') @($t.BuildArgs)
    if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

    # The app descriptor sits right after the 24-byte image header and the first 8-byte
    # segment header, on the C6 and the classic ESP32 alike; version is 16 bytes into
    # it, the project name 48. Same offsets the phone app reads.
    $bytes = [IO.File]::ReadAllBytes($bin)
    $built = [Text.Encoding]::ASCII.GetString($bytes, 48, 32).TrimEnd([char]0)
    $name  = [Text.Encoding]::ASCII.GetString($bytes, 80, 32).TrimEnd([char]0)
    if ($built -ne $version -or $name -ne $t.Project) {
        Fail "The built image says '$name' $built, not $($t.Project) $version. Try a fullclean."
    }
    $sha = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
    Write-Host "==> $($t.Bin)  $version  $([math]::Round($bytes.Length / 1KB)) KB  sha256 $sha" -ForegroundColor Green

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
        $Notes = "$($t.Blurb)`n`n" +
                 "SHA-256 of ${asset}: ``$sha```n`n" +
                 "Changes:`n" + ($log -join "`n")
    }

    # --latest decides what /releases/latest returns, which is what the phone app reads.
    $latest = if ($t.Latest) { '--latest' } else { '--latest=false' }
    gh release create $tag $bin --repo $repo --title "$($t.Title) $version" --notes $Notes $latest
    if ($LASTEXITCODE -ne 0) { Fail "Creating the GitHub release failed; the tag is pushed. Retry with: gh release create $tag $bin --repo $repo $latest" }

    Write-Host "==> released ${tag}: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
}
finally {
    Pop-Location
}

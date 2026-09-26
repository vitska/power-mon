<#
.SYNOPSIS
    Cut a firmware release: bump version.txt, build, tag, and publish on GitHub.

.DESCRIPTION
    The phone app offers an update when the newest GitHub release of this repo is newer
    than what the board runs (README.md "Versioning"). This script is how such a release
    is made, so every one is made the same way:

      1. Refuses a dirty tree: a released binary must correspond to a commit.
      2. With -Bump, raises version.txt and commits that as "Release X.Y.Z".
      3. Builds (tools/idf.ps1 build) and reads the version back out of the built
         image's app descriptor. A mismatch with version.txt -- a stale build dir, a
         PROJECT_VER override -- stops here rather than publishing a mislabelled image.
      4. Tags vX.Y.Z, pushes the commit and the tag, and creates the GitHub release
         with build/bat-monitor.bin attached under exactly that name, which is the
         asset name the app looks for.

    GitHub access: gh, using the account that owns the repo (taken from the origin
    URL) if gh is logged in to it, so the active gh account need not be switched.

.EXAMPLE
    .\tools\release.ps1 -Bump patch -DryRun   # bump, build, verify; publish nothing
    .\tools\release.ps1 -Bump minor           # 0.6.3 -> 0.7.0, published
    .\tools\release.ps1                       # publish version.txt as it stands
#>

[CmdletBinding()]
param(
    [ValidateSet('patch', 'minor', 'major')]
    [string]$Bump,
    [string]$Notes,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$proj        = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$versionFile = Join-Path $proj 'version.txt'
$bin         = Join-Path $proj 'build\bat-monitor.bin'

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
        Fail "version.txt holds '$version'; a release needs MAJOR.MINOR.PATCH."
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
    $tag = "v$version"

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
        git add version.txt
        git commit -q -m "Release $version"
        if ($LASTEXITCODE -ne 0) { Fail "commit failed" }
        Write-Host "==> version.txt -> $version (committed)" -ForegroundColor Cyan
    }

    # --- build and verify ----------------------------------------------------------

    & (Join-Path $PSScriptRoot 'idf.ps1') build
    if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

    # The app descriptor sits right after the 24-byte image header and the first 8-byte
    # segment header; version is 16 bytes into it. Same offsets the phone app reads.
    $bytes = [IO.File]::ReadAllBytes($bin)
    $built = [Text.Encoding]::ASCII.GetString($bytes, 48, 32).TrimEnd([char]0)
    $name  = [Text.Encoding]::ASCII.GetString($bytes, 80, 32).TrimEnd([char]0)
    if ($built -ne $version -or $name -ne 'bat-monitor') {
        Fail "The built image says '$name' $built, not bat-monitor $version. Try: .\tools\idf.ps1 fullclean"
    }
    $sha = (Get-FileHash $bin -Algorithm SHA256).Hash.ToLower()
    Write-Host "==> build\bat-monitor.bin  $version  $([math]::Round($bytes.Length / 1KB)) KB  sha256 $sha" -ForegroundColor Green

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

    git tag -a $tag -m "Firmware $version"
    if ($env:GH_TOKEN) {
        git -c credential.helper= -c "credential.helper=$cred" push origin HEAD $tag
    } else {
        git push origin HEAD $tag
    }
    if ($LASTEXITCODE -ne 0) { Fail "Push failed. The tag $tag exists locally only; delete it with: git tag -d $tag" }

    if (-not $Notes) {
        $prev  = git describe --tags --abbrev=0 "$tag^" 2>$null
        $range = if ($prev) { "$prev..$tag" } else { $tag }
        $Notes = "Firmware $version for the XIAO ESP32-C6.`n`n" +
                 "Install from the power-mon app (Firmware tab), or over USB with tools/flash.ps1.`n`n" +
                 "SHA-256 of bat-monitor.bin: ``$sha```n`n" +
                 "Changes:`n" + ((git log --format='- %s' $range) -join "`n")
    }

    gh release create $tag $bin --repo $repo --title "Firmware $version" --notes $Notes
    if ($LASTEXITCODE -ne 0) { Fail "Creating the GitHub release failed; the tag is pushed. Retry with: gh release create $tag $bin --repo $repo" }

    Write-Host "==> released ${tag}: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
}
finally {
    Pop-Location
}

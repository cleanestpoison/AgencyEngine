#!/usr/bin/env pwsh
#
# release.ps1 — cut a release: bump, verify, build, tag, publish.
#
# The build happens here rather than in CI because compiling needs SkyrimNet's
# CppAPI/PublicAPI.h, which ships with the SkyrimNet mod and is not vendored
# into this repo. A runner has no way to get it. So this script is the whole
# pipeline: it produces the archive locally and uses `gh` only to publish.
#
# CMakeLists.txt is the single source of truth for the version. This rewrites
# the project() line, and the tag and archive name are both derived from it —
# so the version SKSE reports, the tag, and the filename cannot disagree.
#
# Usage:
#   pwsh -File release.ps1 0.2.0
#   pwsh -File release.ps1 0.2.0 -DryRun        # every check, no writes
#   pwsh -File release.ps1 0.2.0 -Draft         # publish as a draft
#   pwsh -File release.ps1 0.2.0 -PreRelease
#   pwsh -File release.ps1 0.2.0 -NotesFile notes.md
#   pwsh -File release.ps1 0.2.0 -SkipBuild     # reuse the existing out/ archive

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [string]$Version,

    [string]$Preset = 'local-release',

    [string]$NotesFile,

    # Cut everything except the writes: no file edits, no commit, no tag, no
    # push, no release. Prints what each step would do.
    [switch]$DryRun,

    [switch]$Draft,

    [switch]$PreRelease,

    # Reuse whatever is already in out/. Only for re-publishing a release that
    # failed at the `gh` step — it does not re-verify that the archive matches
    # current source.
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Every native call below is followed by an explicit $LASTEXITCODE check with a
# message that says what to do about it. PowerShell 7.4+ would otherwise turn a
# non-zero exit into a bare terminating error first, and `git describe` exiting
# 128 on "no tags yet" is a normal path here, not a failure.
$PSNativeCommandUseErrorActionPreference = $false

function Write-Step ($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok   ($msg) { Write-Host "    OK  $msg" -ForegroundColor DarkGray }
function Write-Skip ($msg) { Write-Host "    DRY-RUN: would $msg" -ForegroundColor Yellow }

$repo = $PSScriptRoot
Push-Location $repo
try {

# --- 0. Normalise and validate the version ----------------------------------
$Version = $Version -replace '^[vV]', ''
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version must be MAJOR.MINOR.PATCH (got '$Version'). CMake's project(VERSION) takes three components."
}
$tag = "v$Version"

Write-Step "Releasing $tag"

# --- 1. Preflight: the repo must be in a publishable state -------------------
#
# Every check here is one that produces a broken *published* artifact if it is
# skipped — a release built from uncommitted code, or tagged onto a commit that
# was never pushed, is worse than no release.
Write-Step 'Preflight'

foreach ($tool in @('git', 'gh')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is not on PATH."
    }
}

gh auth status *>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'gh is not authenticated. Run: gh auth login' }
Write-Ok 'gh authenticated'

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
if ($branch -ne 'main') {
    throw "On branch '$branch'. Releases are cut from main."
}

# A dirty tree means the archive would contain code that no commit describes,
# and the tag would point at something that isn't what shipped.
if (git status --porcelain) {
    Write-Host (git status --short | Out-String)
    throw 'Working tree is dirty. Commit or stash before releasing.'
}
Write-Ok 'clean tree on main'

git fetch origin --tags --quiet
if ($LASTEXITCODE -ne 0) { throw 'git fetch failed.' }

$ahead  = [int](git rev-list --count 'origin/main..HEAD')
$behind = [int](git rev-list --count 'HEAD..origin/main')
if ($behind -gt 0) {
    throw "Local main is $behind commit(s) behind origin/main. Pull first."
}
if ($ahead -gt 0) {
    Write-Ok "$ahead unpushed commit(s) — they will be pushed with the tag"
}

if ((git tag --list $tag) -or (git ls-remote --tags origin "refs/tags/$tag")) {
    throw "Tag $tag already exists (locally or on origin). Pick another version."
}
Write-Ok "$tag is free"

# --- 2. Papyrus: the .pex must be current, and must not carry an identity ----
#
# Both failures are invisible at build time and expensive afterwards: a stale
# .pex fails in someone's game as "function not registered", and an unscrubbed
# one publishes the compiling machine's username and hostname. The Bethesda
# compiler writes the source filename, the username and the machine name into
# every .pex header and has no flag to suppress it, so a rebuild silently puts
# them back.
Write-Step 'Papyrus checks'

Get-ChildItem (Join-Path $repo 'Source/Scripts') -Filter *.psc | ForEach-Object {
    $pex = Join-Path $repo "statics/Scripts/$($_.BaseName).pex"
    if (-not (Test-Path -LiteralPath $pex)) {
        throw "$($_.Name) has no compiled .pex in statics/Scripts. See the Papyrus section of README.md."
    }
    if ($_.LastWriteTime -gt (Get-Item -LiteralPath $pex).LastWriteTime) {
        throw "$($_.Name) is newer than its .pex — the shipped script is stale. Recompile and scrub it."
    }

    # Read the header as raw bytes: the strings are length-prefixed, not
    # null-terminated, so this is a substring search over the whole file.
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($pex))
    foreach ($leak in @($env:USERNAME, $env:COMPUTERNAME)) {
        if ($leak -and $text -match [regex]::Escape($leak)) {
            throw "$($_.BaseName).pex contains '$leak' in its header — it was recompiled and not scrubbed. See the Papyrus section of CLAUDE.md."
        }
    }
    if ($text -notmatch 'AGENCYENGINEBLD') {
        throw "$($_.BaseName).pex is missing the scrubbed machine name — expected 'AGENCYENGINEBLD'."
    }
    Write-Ok "$($_.BaseName).pex current and scrubbed"
}

# The MCM host is source-built too. Checking generated bytes rather than only
# timestamps catches a changed script binding or quest flag before the archive
# ships an old binary that simply never registers with SkyUI.
& python (Join-Path $repo 'tools/build_mcm_plugin.py') --check
if ($LASTEXITCODE -ne 0) {
    throw 'AgencyEngine.esp does not match tools/build_mcm_plugin.py. Regenerate it before releasing.'
}
Write-Ok 'AgencyEngine.esp matches its generator'

# --- 3. Leak scan over tracked files ----------------------------------------
#
# This repo is public. Cheap, targeted check for the three things that actually
# leak from this machine: the username, the hostname, and absolute paths under
# the user profile or this working copy.
Write-Step 'Leak scan'

$needles = @($env:USERNAME, $env:COMPUTERNAME, $env:USERPROFILE, (Split-Path -Parent $repo)) |
    Where-Object { $_ } | Select-Object -Unique
$tracked = git ls-files
$hits = @()
foreach ($file in $tracked) {
    # Skip the vendored header (500 KB of generated ImGui) and binaries; the
    # .pex is covered by its own check above.
    if ($file -like 'external/*' -or $file -like '*.pex') { continue }
    $content = Get-Content -LiteralPath $file -Raw -ErrorAction SilentlyContinue
    if (-not $content) { continue }
    foreach ($needle in $needles) {
        if ($content -match [regex]::Escape($needle)) {
            $hits += "$file contains '$needle'"
        }
    }
}
if ($hits) {
    $hits | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    throw 'Leak scan failed. Remove the above before releasing.'
}
Write-Ok "$($tracked.Count) tracked files, nothing personal"

# --- 4. Bump the version in CMakeLists.txt ----------------------------------
#
# Written before the build so the DLL compiles with the version it ships as,
# but not committed until the build succeeds — a failed build leaves the file
# restored rather than half-bumped.
Write-Step "Setting project version to $Version"

$cmakePath = Join-Path $repo 'CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakePath -Raw
$pattern   = '(project\s*\(\s*AgencyEngine\s+VERSION\s+)([0-9]+\.[0-9]+\.[0-9]+)'
# [regex]::Match rather than -match: -notmatch does not reliably populate
# $Matches, and this needs the captured old version either way.
$m = [regex]::Match($cmakeText, $pattern)
if (-not $m.Success) {
    throw "Couldn't find the project(AgencyEngine VERSION ...) line in CMakeLists.txt."
}
$oldVersion = $m.Groups[2].Value
$bumped     = $cmakeText -replace $pattern, "`${1}$Version"
$versionChanged = $oldVersion -ne $Version

if (-not $versionChanged) {
    Write-Ok "already $Version — no bump needed"
} elseif ($DryRun) {
    Write-Skip "bump CMakeLists.txt $oldVersion -> $Version"
} else {
    Set-Content -LiteralPath $cmakePath -Value $bumped -NoNewline
    Write-Ok "$oldVersion -> $Version"
}

# --- 5. Build and package ---------------------------------------------------
$archive = Join-Path $repo "out/AgencyEngine-v$Version.zip"

if ($SkipBuild) {
    if (-not (Test-Path -LiteralPath $archive)) {
        throw "-SkipBuild was passed but $archive does not exist."
    }
    Write-Step "Reusing existing archive (-SkipBuild)"
} elseif ($DryRun) {
    Write-Skip "run package.ps1 -Version $Version -Preset $Preset"
} else {
    Write-Step 'Building and packaging'
    try {
        & (Join-Path $repo 'package.ps1') -Version $Version -Preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "package.ps1 exited $LASTEXITCODE" }
    } catch {
        # Put CMakeLists.txt back so a failed release leaves no trace.
        if ($versionChanged) {
            Set-Content -LiteralPath $cmakePath -Value $cmakeText -NoNewline
            Write-Host "    Restored CMakeLists.txt to $oldVersion" -ForegroundColor Yellow
        }
        throw
    }
    if (-not (Test-Path -LiteralPath $archive)) {
        throw "package.ps1 reported success but $archive is missing."
    }
}

# --- 6. Release notes -------------------------------------------------------
#
# Commits since the previous tag. On the first release there is no previous
# tag, so fall back to a plain line rather than dumping the entire history.
Write-Step 'Release notes'

if ($NotesFile) {
    if (-not (Test-Path -LiteralPath $NotesFile)) { throw "Notes file not found: $NotesFile" }
    $notes = Get-Content -LiteralPath $NotesFile -Raw
} else {
    $prevTag = git describe --tags --abbrev=0 2>$null
    if ($LASTEXITCODE -eq 0 -and $prevTag) {
        $log = git log "$prevTag..HEAD" --no-merges --format='- %s'
        $body = if ($log) { $log -join "`n" } else { '- No changes recorded.' }
        $notes = "## Changes since $prevTag`n`n$body"
    } else {
        $notes = "First release."
    }
    $global:LASTEXITCODE = 0

    $notes += @"


## Install

Install ``AgencyEngine-v$Version.zip`` with MO2 or Vortex. Its root contains ``AgencyEngine.esp``, ``Scripts/``, and ``SKSE/``, so it installs directly with no FOMOD.

**Requires** SKSE64, Address Library, and [SkyrimNet](https://www.nexusmods.com/skyrimspecialedition/mods/153017). SKSE Menu Framework and SkyUI/SkyUI VR are optional parallel interfaces; install either or both. The Skyrim VR MCM host also requires [Skyrim VR ESL](https://github.com/Nightfallstorm/SkyrimVRESL).
"@
}

Write-Host ($notes -split "`n" | ForEach-Object { "    $_" } | Out-String)

# --- 7. Commit, tag, push ---------------------------------------------------
#
# The tag goes on the commit that carries the bumped version, so the tag and
# the DLL agree by construction.
Write-Step 'Commit, tag, push'

if ($DryRun) {
    Write-Skip "commit CMakeLists.txt as 'Release $tag'"
    Write-Skip "tag $tag and push it to origin"
    Write-Skip "create the GitHub release with $([IO.Path]::GetFileName($archive)) attached"
    Write-Host "==> Dry run complete — nothing was written." -ForegroundColor Green
    return
}

if ($versionChanged) {
    git add CMakeLists.txt
    git commit -m "Release $tag" --quiet
    if ($LASTEXITCODE -ne 0) { throw 'git commit failed.' }
    Write-Ok "committed the version bump"
}

git tag -a $tag -m "AgencyEngine $tag"
if ($LASTEXITCODE -ne 0) { throw 'git tag failed.' }

git push origin main --quiet
if ($LASTEXITCODE -ne 0) { throw 'git push failed.' }
git push origin $tag --quiet
if ($LASTEXITCODE -ne 0) {
    # The tag exists locally but not on origin; gh would create a release
    # pointing at nothing. Undo it so the run is retryable.
    git tag -d $tag | Out-Null
    throw 'Pushing the tag failed; removed the local tag so this can be re-run.'
}
Write-Ok "pushed main and $tag"

# --- 8. Publish -------------------------------------------------------------
Write-Step 'Creating the GitHub release'

$notesPath = Join-Path ([IO.Path]::GetTempPath()) "agencyengine-notes-$Version.md"
Set-Content -LiteralPath $notesPath -Value $notes -Encoding utf8

$ghArgs = @(
    'release', 'create', $tag, $archive,
    '--title', "AgencyEngine $tag",
    '--notes-file', $notesPath
)
if ($Draft)      { $ghArgs += '--draft' }
if ($PreRelease) { $ghArgs += '--prerelease' }

try {
    gh @ghArgs
    if ($LASTEXITCODE -ne 0) { throw "gh release create exited $LASTEXITCODE" }
} finally {
    Remove-Item -LiteralPath $notesPath -Force -ErrorAction SilentlyContinue
}

Write-Host "==> Released $tag" -ForegroundColor Green
Write-Host "    $(gh release view $tag --json url --jq .url)" -ForegroundColor Green

} finally {
    Pop-Location
}

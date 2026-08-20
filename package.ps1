#!/usr/bin/env pwsh
#
# package.ps1 — build, then zip the deployed mod folder into an archive that
# MO2 / Vortex can install directly.
#
# Runs build.ps1 first so _deploy/AgencyEngine/ reflects current source (the
# compiled DLL plus everything under statics/). Then packages the folder's
# *contents* — not the folder itself — so the archive's top-level entry is
# SKSE/, which is what a direct install expects at the mod root.
#
# Usage:
#   pwsh -File package.ps1                        # version read from CMakeLists.txt
#   pwsh -File package.ps1 -Version 0.2.0
#   pwsh -File package.ps1 -Version 0.2.0 -OutputDir C:\Releases
#   pwsh -File package.ps1 -SkipBuild             # package what's already deployed

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Version,

    [string]$OutputDir = (Join-Path $PSScriptRoot 'out'),

    [string]$Preset = 'local-release',

    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

# --- 1. Resolve the version -------------------------------------------------
#
# Default to the project version in CMakeLists.txt so the archive name can't
# drift from what the DLL reports to SKSE.
if (-not $Version) {
    $cmakeLists = Get-Content (Join-Path $PSScriptRoot 'CMakeLists.txt') -Raw
    if ($cmakeLists -match 'project\s*\(\s*AgencyEngine\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        throw "Couldn't read the project version out of CMakeLists.txt. Pass -Version explicitly."
    }
}
$Version = $Version -replace '^[vV]', ''

# --- 2. Build ---------------------------------------------------------------
#
# Any build failure aborts here; packaging a stale or partial mod folder would
# ship a broken archive.
if (-not $SkipBuild) {
    Write-Host '==> Running build.ps1 to sync the deployed mod folder' -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'build.ps1') build -Preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "build.ps1 failed (exit $LASTEXITCODE); refusing to package."
    }
}

# --- 3. Sanity-check the deployed folder ------------------------------------
#
# The deploy root matches CMakeUserPresets.json's SKYRIM_MODS_FOLDER; read it
# from the preset rather than duplicating the path here.
#
# Only local-base carries an environment block, so a preset inheriting it has
# no `environment` property at all. Probe PSObject.Properties rather than
# dotting straight through: this script is also called from release.ps1, which
# runs under Set-StrictMode -Latest, where a missing property is an error and
# not $null.
$presetFile = Join-Path $PSScriptRoot 'CMakeUserPresets.json'
$userPresets = Get-Content $presetFile -Raw | ConvertFrom-Json
$modsFolder = $null
foreach ($name in @($Preset, 'local-base')) {
    $entry = $userPresets.configurePresets | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if (-not $entry) { continue }
    if (-not $entry.PSObject.Properties['environment']) { continue }
    $block = $entry.environment
    if ($block -and $block.PSObject.Properties['SKYRIM_MODS_FOLDER']) {
        $modsFolder = $block.SKYRIM_MODS_FOLDER
        break
    }
}
if (-not $modsFolder) {
    throw "Couldn't resolve SKYRIM_MODS_FOLDER from $presetFile."
}

$modFolder = Join-Path $modsFolder 'AgencyEngine'
if (-not (Test-Path -LiteralPath $modFolder -PathType Container)) {
    throw "Expected mod folder does not exist: $modFolder (run a build first)"
}

# A mod folder without the DLL would install cleanly and do nothing — catch it
# here rather than in someone's game.
$dll = Join-Path $modFolder 'SKSE\Plugins\AgencyEngine.dll'
if (-not (Test-Path -LiteralPath $dll)) {
    throw "No AgencyEngine.dll in $modFolder — the build didn't deploy."
}
# Every prompt the shipped defaults dispatch, not just the spine: a lens whose
# file is missing renders as nothing and costs the whole impulse, which shows up
# as a lens that is simply always quiet rather than as an error.
foreach ($name in @('base', 'aspiration', 'relationship', 'activity', 'curiosity', 'resolved')) {
    $prompt = Join-Path $modFolder "SKSE\Plugins\SkyrimNet\prompts\agencyengine_impulse_$name.prompt"
    if (-not (Test-Path -LiteralPath $prompt)) {
        throw "No agencyengine_impulse_$name.prompt in $modFolder — the statics deploy didn't run."
    }
}

# NEVER ship a real AgencyEngine.json. An absent config means every setting is
# the one this build ships, which is what lets a retuned default reach an install
# that already exists; a shipped file freezes all of them at install time and
# every later release is then fighting it. The .example is the documented,
# never-read copy and must be there instead.
$config = Join-Path $modFolder 'SKSE\Plugins\AgencyEngine.json'
if (Test-Path -LiteralPath $config) {
    throw "$config exists — a shipped config pins every default at install time. Remove it."
}
if (-not (Test-Path -LiteralPath "$config.example")) {
    throw "No AgencyEngine.json.example in $modFolder — the statics deploy didn't run."
}

Write-Host '==> Mod folder contents:' -ForegroundColor Cyan
Get-ChildItem -LiteralPath $modFolder -Recurse -File |
    ForEach-Object { '    ' + $_.FullName.Substring($modFolder.Length + 1) }

# --- 4. Package -------------------------------------------------------------
#
# Contents at the archive root, not the folder itself, which is what MO2 /
# Vortex expect from a direct-install archive.
$archivePath = Join-Path $OutputDir "AgencyEngine-v$Version.zip"

if (-not (Test-Path -LiteralPath $OutputDir -PathType Container)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}

# NEVER ship a .pdb. It embeds the full source path of every translation unit,
# which on this machine means the drive layout AND the Windows username, in
# plain ASCII inside a 60 MB file nobody would think to open. A Debug build
# deploys one next to the DLL, and _deploy/ is shared across presets — so it
# only takes one debug build, ever, for the next release archive to carry it.
# The whole point of the deploy folder is that it mirrors the mod folder, so
# the guard belongs here, at the one place where files become an archive.
#
# Excluded rather than deleted: the deploy folder is a build output this script
# does not own, and a debugger wants the .pdb exactly where it is.
$excluded = @('.pdb', '.ilk', '.exp', '.lib')
$files = Get-ChildItem -LiteralPath $modFolder -Recurse -File |
    Where-Object { $excluded -notcontains $_.Extension.ToLowerInvariant() }

$skipped = Get-ChildItem -LiteralPath $modFolder -Recurse -File |
    Where-Object { $excluded -contains $_.Extension.ToLowerInvariant() }
foreach ($file in $skipped) {
    Write-Host ('    excluded from the archive: {0}' -f
        $file.FullName.Substring($modFolder.Length + 1)) -ForegroundColor Yellow
}

Write-Host "==> Packaging $modFolder -> $archivePath" -ForegroundColor Cyan

Add-Type -AssemblyName 'System.IO.Compression.FileSystem'
$zip = [System.IO.Compression.ZipFile]::Open($archivePath, 'Create')
try {
    foreach ($file in $files) {
        # Forward slashes: the zip spec asks for them, and MO2 is happier.
        $entryName = $file.FullName.Substring($modFolder.Length + 1) -replace '\\', '/'
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $file.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $zip.Dispose()
}

$archiveSize = (Get-Item -LiteralPath $archivePath).Length
Write-Host ('==> Done: {0} ({1:N2} MB)' -f $archivePath, ($archiveSize / 1MB)) -ForegroundColor Green

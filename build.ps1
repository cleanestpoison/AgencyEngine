#!/usr/bin/env pwsh
#
# AgencyEngine build driver.
#
# Loads the Visual Studio Developer environment (for cl.exe / cmake / ninja),
# then forwards to cmake with the right preset.
#
# Usage:
#   pwsh -File build.ps1 configure           # cmake --preset local-release
#   pwsh -File build.ps1 build               # cmake --build build/local-release
#   pwsh -File build.ps1 clean               # remove build/<preset>
#   pwsh -File build.ps1 build -Preset local-debug
#   pwsh -File build.ps1 test -Preset local-tests   # build, then ctest
#
# The first configure builds every vcpkg dependency from source
# (CommonLibSSE-NG is template-heavy) — expect a long first run. Subsequent
# builds reuse the binary cache.

param(
    [Parameter(Position = 0)]
    [ValidateSet('configure', 'build', 'clean', 'test')]
    [string]$Verb = 'build',

    [string]$Preset = 'local-release',

    # CommonLibSSE-NG compilation peaks around 2-3 GB per cl.exe instance.
    # Left at 0 this is derived from free RAM and core count.
    [int]$Threads = 0
)

$ErrorActionPreference = 'Stop'

# --- 1. Locate Launch-VsDevShell.ps1 -----------------------------------------
$vsDevShell = Get-ChildItem `
    -Path "C:\Program Files*\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1" `
    -ErrorAction SilentlyContinue |
    Select-Object -First 1

if (-not $vsDevShell) {
    throw "Couldn't find Launch-VsDevShell.ps1 under 'C:\Program Files*\Microsoft Visual Studio\2022\*'. Update build.ps1 if Visual Studio is installed elsewhere."
}

$savedLocation = Get-Location
& $vsDevShell.FullName -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Set-Location $savedLocation

# The Build Tools flavour of the dev shell puts cl.exe on PATH but not the
# CMake/Ninja that ship inside the VS install, and the Ninja generator resolves
# CMAKE_MAKE_PROGRAM from PATH. Add both directories explicitly (a system-wide
# cmake/ninja, if present, still wins because we append rather than prepend).
$vsRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $vsDevShell.FullName))
$vsCMakeRoot = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
foreach ($sub in @('CMake\bin', 'Ninja')) {
    $dir = Join-Path $vsCMakeRoot $sub
    if ((Test-Path $dir) -and ($env:PATH -notlike "*$dir*")) {
        $env:PATH = "$env:PATH;$dir"
    }
}

if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    throw "ninja not found on PATH. Install Ninja, or install the 'C++ CMake tools for Windows' component in the Visual Studio Installer."
}

# Launch-VsDevShell clobbers VCPKG_ROOT if VS bundles its own vcpkg. The preset
# carries the right value for cmake itself, but re-pin it here so any native
# cmake/vcpkg call in this session agrees.
# Walking the inherits chain by hand: only local-base carries an environment
# block, so a preset that inherits it has no `environment` property at all.
# Probing PSObject.Properties rather than dotting straight through, because
# this script is also called from release.ps1, which runs under
# Set-StrictMode -Latest — and strict mode makes a missing property an error
# rather than $null.
function Get-PresetEnvValue {
    param($Presets, [string[]]$Chain, [string]$Key)

    foreach ($name in $Chain) {
        $entry = $Presets.configurePresets | Where-Object { $_.name -eq $name } | Select-Object -First 1
        if (-not $entry) { continue }
        if (-not $entry.PSObject.Properties['environment']) { continue }
        $block = $entry.environment
        if ($block -and $block.PSObject.Properties[$Key]) {
            return $block.$Key
        }
    }
    return $null
}

$presetFile = Join-Path $PSScriptRoot 'CMakeUserPresets.json'
if (Test-Path $presetFile) {
    $userPresets = Get-Content $presetFile -Raw | ConvertFrom-Json
    $vcpkgRoot = Get-PresetEnvValue $userPresets @($Preset, 'local-base') 'VCPKG_ROOT'
    if ($vcpkgRoot) { $env:VCPKG_ROOT = $vcpkgRoot }
}

# --- 2. Thread budget --------------------------------------------------------
$ramAvailGB = [Math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)
$cpuCores = [Environment]::ProcessorCount
$ramSafeThreads = [Math]::Max(1, [Math]::Floor(($ramAvailGB - 8) / 3))
$cpuSafeThreads = [Math]::Max(1, [Math]::Floor($cpuCores / 4))

if ($Threads -le 0) {
    $Threads = [Math]::Min($ramSafeThreads, $cpuSafeThreads)
} else {
    $Threads = [Math]::Min($Threads, $ramSafeThreads)
}
$Threads = [Math]::Max(1, [Math]::Min(6, $Threads))

Write-Host "System: ${ramAvailGB} GB RAM free, ${cpuCores} cores -> $Threads build threads" -ForegroundColor Cyan
$env:VCPKG_MAX_CONCURRENCY = $Threads

# Keep the desktop responsive while template-heavy TUs compile.
$proc = [System.Diagnostics.Process]::GetCurrentProcess()
$proc.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal

$buildDir = Join-Path $PSScriptRoot "build/$Preset"

# Shared by 'build' and 'test': configure first if there is no cache yet, so a
# fresh clone needs one command rather than two.
function Invoke-Build {
    if (-not (Test-Path "$buildDir/CMakeCache.txt")) {
        Write-Host "==> cmake --preset $Preset (no cache yet)" -ForegroundColor Cyan
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
    }
    Write-Host "==> cmake --build $buildDir -j $Threads" -ForegroundColor Cyan
    cmake --build $buildDir -j $Threads
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

switch ($Verb) {
    'clean' {
        if (Test-Path $buildDir) {
            Write-Host "==> removing $buildDir" -ForegroundColor Cyan
            Remove-Item -Recurse -Force $buildDir
        }
    }
    'configure' {
        Write-Host "==> cmake --preset $Preset" -ForegroundColor Cyan
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }
    }
    'build' {
        Invoke-Build
    }
    'test' {
        # Goes through this script rather than being a bare ctest call in the
        # README, because ctest ships inside the VS install and only the dev
        # shell above puts it on PATH — `ctest` typed into a plain terminal on
        # this setup is "command not found".
        Invoke-Build

        Write-Host "==> ctest --test-dir $buildDir" -ForegroundColor Cyan
        ctest --test-dir $buildDir --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
    }
}

Write-Host "==> done ($Verb / $Preset)" -ForegroundColor Green

<#
.SYNOPSIS
    Configures, builds and tests the portable core for x64 (build\x64) and Win32 (build\win32).

.DESCRIPTION
    Uses the Visual Studio 2022 generator (no vcvars needed). Runs CTest for both
    architectures and exits non-zero on the first configure/build/test failure.
    The Win32 tree also builds adapters\re5 when it exists (see CMakeLists.txt); pass
    -CoreOnly to build just rm_core, rm_tests and rm_sim.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build_core.ps1
    powershell -ExecutionPolicy Bypass -File scripts\build_core.ps1 -CoreOnly -Config Debug
#>
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$CoreOnly,
    [switch]$SkipX64,
    [switch]$SkipWin32
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = $null

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} elseif (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -eq 0 -and $vsPath) {
        $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $candidate) { $cmake = $candidate }
    }
}
if (-not $cmake) {
    throw 'CMake was not found. Install CMake or Visual Studio C++ build tools with CMake support.'
}
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) {
    $ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
    if (-not $ctestCommand) { throw "ctest.exe not found next to $cmake" }
    $ctest = $ctestCommand.Source
}

function Stop-Build([string]$Message, [int]$Code) {
    Write-Host "=== FAILED: $Message (exit $Code)" -ForegroundColor Red
    exit $(if ($Code -ne 0) { $Code } else { 1 })
}

$targets = @()
if ($CoreOnly) { $targets = @('--target', 'rm_core', 'rm_tests', 'rm_sim') }

$builds = @()
if (-not $SkipX64) { $builds += @{ Name = 'x64'; Dir = 'build\x64'; Arch = 'x64' } }
if (-not $SkipWin32) { $builds += @{ Name = 'win32'; Dir = 'build\win32'; Arch = 'Win32' } }

foreach ($b in $builds) {
    $buildPath = Join-Path $repoRoot $b.Dir
    Write-Host "=== [$($b.Name)] configure -> $buildPath"
    & $cmake -S $repoRoot -B $buildPath -G 'Visual Studio 17 2022' -A $b.Arch -DRM_BUILD_TESTS=ON -DRM_BUILD_SIM=ON
    if ($LASTEXITCODE -ne 0) { Stop-Build "[$($b.Name)] configure failed" $LASTEXITCODE }

    Write-Host "=== [$($b.Name)] build ($Config)"
    & $cmake --build $buildPath --config $Config --parallel @targets
    if ($LASTEXITCODE -ne 0) { Stop-Build "[$($b.Name)] build failed" $LASTEXITCODE }

    Write-Host "=== [$($b.Name)] ctest"
    & $ctest --test-dir $buildPath -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { Stop-Build "[$($b.Name)] tests failed" $LASTEXITCODE }
}

Write-Host '=== build_core: all configured architectures built and tested'
exit 0

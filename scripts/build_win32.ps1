param(
    [string]$BuildDirectory = 'build\win32'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = $null

if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} elseif (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -eq 0 -and $vsPath) {
        $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $candidate) {
            $cmake = $candidate
        }
    }
}

if (-not $cmake) {
    throw 'CMake was not found. Install CMake or Visual Studio C++ build tools with CMake support.'
}

$buildPath = Join-Path $repoRoot $BuildDirectory
& $cmake -S $repoRoot -B $buildPath -A Win32
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $cmake --build $buildPath --config Release --parallel
exit $LASTEXITCODE

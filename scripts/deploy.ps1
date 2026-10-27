param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('install', 'uninstall', 'status')]
    [string]$Mode,

    # dinput8.dll to install; defaults to the full build, then the diagnostic build.
    [string]$Dll = '',

    # adapter.json to install (e.g. configs\adapter.dev.json for test runs).
    [string]$AdapterConfig = '',

    # Content configs to install (default: the repo's configs folder).
    [string]$ConfigDir = '',

    [string]$GameDir = 'G:\SteamLibrary\steamapps\common\Resident Evil 5'
)

# Installs the mod into the game folder and records every created file in a
# manifest; uninstall removes exactly those files and nothing else. An existing
# dinput8.dll that this script did not install is never overwritten.

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifestPath = Join-Path $repoRoot 'build\deploy-manifest.json'
$exe = Join-Path $GameDir 're5dx9.exe'
$expectedExeHash = '751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26'

function Get-Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

function Read-Manifest {
    if (Test-Path -LiteralPath $manifestPath) { Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json } else { $null }
}

if (Get-Process -Name re5dx9 -ErrorAction SilentlyContinue) { throw 'Close the game first.' }
if ((Get-Hash $exe) -ne $expectedExeHash) { throw "re5dx9.exe hash differs from the supported build; refusing." }

switch ($Mode) {
    'status' {
        $m = Read-Manifest
        if (-not $m) { 'not installed (no manifest)'; break }
        foreach ($f in $m.files) {
            $present = Test-Path -LiteralPath $f.path
            $same = $present -and ((Get-Hash $f.path) -eq $f.sha256)
            "{0} present={1} unchanged={2}" -f $f.path, $present, $same
        }
    }
    'install' {
        if (Read-Manifest) { throw "Already installed per $manifestPath; uninstall first." }
        if (-not $Dll) {
            foreach ($candidate in @('build\win32\adapters\re5\Release\dinput8.dll', 'build\adapter-diag\Release\dinput8.dll')) {
                $full = Join-Path $repoRoot $candidate
                if (Test-Path -LiteralPath $full) { $Dll = $full; break }
            }
        }
        if (-not $Dll -or -not (Test-Path -LiteralPath $Dll)) { throw 'No dinput8.dll build found.' }
        $targetDll = Join-Path $GameDir 'dinput8.dll'
        if (Test-Path -LiteralPath $targetDll) { throw "$targetDll exists and was not installed by this script; refusing to overwrite." }
        $modDir = Join-Path $GameDir 'RogueMercenaries'
        if (Test-Path -LiteralPath $modDir) { throw "$modDir exists without a manifest; refusing." }

        $files = @()
        Copy-Item -LiteralPath $Dll -Destination $targetDll
        $files += [pscustomobject]@{ path = $targetDll; sha256 = Get-Hash $targetDll }
        $configSrc = if ($ConfigDir) { $ConfigDir } else { Join-Path $repoRoot 'configs' }
        $configDst = Join-Path $modDir 'configs'
        New-Item -ItemType Directory -Force -Path $configDst | Out-Null
        if (Test-Path -LiteralPath $configSrc) {
            Get-ChildItem -LiteralPath $configSrc -Filter '*.json' -File | Where-Object { $_.Name -notlike 'adapter*.json' } | ForEach-Object {
                $dst = Join-Path $configDst $_.Name
                Copy-Item -LiteralPath $_.FullName -Destination $dst
                $files += [pscustomobject]@{ path = $dst; sha256 = Get-Hash $dst }
            }
        }
        if (-not $AdapterConfig) { $AdapterConfig = Join-Path $repoRoot 'configs\adapter.json' }
        if (Test-Path -LiteralPath $AdapterConfig) {
            $dst = Join-Path $modDir 'adapter.json'
            Copy-Item -LiteralPath $AdapterConfig -Destination $dst
            $files += [pscustomobject]@{ path = $dst; sha256 = Get-Hash $dst }
        }
        $manifest = [pscustomobject]@{
            installed = (Get-Date).ToString('o')
            source_dll = (Resolve-Path $Dll).Path
            exe_sha256 = $expectedExeHash
            dirs = @($configDst, $modDir)
            files = $files
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $manifestPath) | Out-Null
        $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8
        "installed $($files.Count) files; manifest $manifestPath"
    }
    'uninstall' {
        $m = Read-Manifest
        if (-not $m) { throw 'No manifest; nothing to uninstall.' }
        foreach ($f in $m.files) {
            if (Test-Path -LiteralPath $f.path) {
                Remove-Item -LiteralPath $f.path
                "removed $($f.path)"
            }
        }
        foreach ($d in $m.dirs) {
            if ((Test-Path -LiteralPath $d) -and -not (Get-ChildItem -LiteralPath $d -Force)) {
                Remove-Item -LiteralPath $d
                "removed empty dir $d"
            }
        }
        Remove-Item -LiteralPath $manifestPath
        if ((Get-Hash $exe) -ne $expectedExeHash) { throw 'WARNING: re5dx9.exe hash changed!' }
        'uninstalled; re5dx9.exe hash unchanged'
    }
}

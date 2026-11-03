# Rogue Mercenaries uninstaller (alpha). Run through Uninstall.bat.
#
# Removes exactly the files RogueMercenaries\install.json lists, puts back a dinput8.dll the
# installer kept aside, and leaves your progress and logs (%LOCALAPPDATA%\RogueMercenaries)
# unless you ask to delete them.

param(
    [string]$GameDir = '',
    [switch]$Yes,             # answer every question with yes (scripted runs)
    [switch]$KeepData,        # never ask about progress and logs (scripted runs)
    [switch]$FromGameFolder   # run by RogueMercenaries\Uninstall.bat, which deletes itself after
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dataDir = Join-Path $env:LOCALAPPDATA 'RogueMercenaries'

function Say([string]$text) { Write-Host $text }
function Fail([string]$text) { Write-Host ''; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }
function Ask([string]$question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$question (y/N)"
    return $answer -match '^(y|yes)$'
}

function Get-SteamRoots {
    $roots = @()
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($name in @('SteamPath', 'InstallPath')) {
                if ($p.$name) { $roots += ($p.$name -replace '/', '\') }
            }
        } catch { }
    }
    $roots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
}

function Find-GameDirs {
    $libraries = @()
    foreach ($root in Get-SteamRoots) {
        $libraries += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $text = Get-Content -LiteralPath $vdf -Raw
            foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) {
                $libraries += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    $libraries | Select-Object -Unique | ForEach-Object { Join-Path $_ 'steamapps\common\Resident Evil 5' } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'RogueMercenaries\install.json') }
}

Say 'Rogue Mercenaries - uninstaller'
Say '------------------------------------------------'

if (-not $GameDir) {
    if ($FromGameFolder -or (Test-Path -LiteralPath (Join-Path $here 'install.json'))) {
        $GameDir = Split-Path -Parent $here
    } else {
        $found = @(Find-GameDirs)
        if ($found.Count -ge 1) { $GameDir = $found[0] }
    }
}
if (-not $GameDir) { Fail 'No Rogue Mercenaries install was found. Run: Uninstall.bat -GameDir "<folder with re5dx9.exe>"' }

$modDir = Join-Path $GameDir 'RogueMercenaries'
$manifestPath = Join-Path $modDir 'install.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { Fail "No Rogue Mercenaries install in $GameDir (install.json is missing)." }
if (Get-Process -Name 're5dx9' -ErrorAction SilentlyContinue) { Fail 'Resident Evil 5 is running. Close it, then run Uninstall.bat again.' }

$m = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
Say "Removing Rogue Mercenaries $($m.version) from $GameDir"
$removed = 0
foreach ($f in $m.files) {
    $p = Join-Path $GameDir $f.path
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; $removed++ }
}
$targetDll = Join-Path $GameDir 'dinput8.dll'
if ($m.dinput8_backup) {
    $backup = Join-Path $GameDir $m.dinput8_backup
    if ((Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $targetDll)) {
        Move-Item -LiteralPath $backup -Destination $targetDll
        Say 'The other mod''s dinput8.dll is back in place.'
    }
}
Remove-Item -LiteralPath $manifestPath -Force
Remove-Item -LiteralPath (Join-Path $modDir 'uninstall.ps1') -Force -ErrorAction SilentlyContinue
if (-not $FromGameFolder) { Remove-Item -LiteralPath (Join-Path $modDir 'Uninstall.bat') -Force -ErrorAction SilentlyContinue }
# Folders left empty go too (deepest first); anything you added yourself stays.
if (Test-Path -LiteralPath $modDir) {
    Get-ChildItem -LiteralPath $modDir -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending |
        Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } | Remove-Item -Force
    if (-not (Get-ChildItem -LiteralPath $modDir -Force)) { Remove-Item -LiteralPath $modDir -Force }
}
Say "Removed $removed files. The game is as it was before the mod."

if ((Test-Path -LiteralPath $dataDir) -and -not $KeepData) {
    Say ''
    Say "Your progress, logs and save backups are in $dataDir"
    if (-not $Yes -and (Ask 'Delete them too? (keep them if you still want to send logs)')) {
        Remove-Item -LiteralPath $dataDir -Recurse -Force
        Say 'Deleted.'
    } else {
        Say 'Kept.'
    }
}
exit 0

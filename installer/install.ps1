# Rogue Mercenaries installer (alpha). Run through Install.bat.
#
# Finds Resident Evil 5 through Steam, checks the game build, copies a backup of the game's
# save, then installs dinput8.dll and the RogueMercenaries folder into the game folder and
# records every file in RogueMercenaries\install.json; Uninstall.bat removes exactly those.
# A dinput8.dll from another mod is kept in RogueMercenaries\backup and put back on uninstall.
# Mod progress (%LOCALAPPDATA%\RogueMercenaries) is never touched: a new player starts fresh.

param(
    [string]$GameDir = '',
    [switch]$Yes,         # answer every question with yes (scripted installs)
    [switch]$DetectOnly   # only report the game folder found
)

$ErrorActionPreference = 'Stop'
$Version = '{VERSION}'
$SupportedExe = '751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$payload = Join-Path (Split-Path -Parent $here) 'files'
$dataDir = Join-Path $env:LOCALAPPDATA 'RogueMercenaries'

function Say([string]$text) { Write-Host $text }
function Fail([string]$text) { Write-Host ''; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }
function Ask([string]$question) {
    if ($Yes) { return $true }
    $answer = Read-Host "$question (y/N)"
    return $answer -match '^(y|yes)$'
}
function Get-Hash([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

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
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 're5dx9.exe') }
}

function Test-OurDll([string]$path) {
    # Every Rogue Mercenaries build logs its name; another mod's dinput8.dll does not carry it.
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains('Rogue Mercenaries')
}

Say "Rogue Mercenaries $Version - installer"
Say '------------------------------------------------'

if (-not (Test-Path -LiteralPath (Join-Path $payload 'dinput8.dll'))) {
    Fail "The installer's files are missing ($payload). Extract the whole ZIP first, then run Install.bat from the extracted folder."
}

# 1. The game folder.
if (-not $GameDir) {
    $found = @(Find-GameDirs)
    if ($found.Count -ge 1) {
        $GameDir = $found[0]
        Say "Resident Evil 5 found: $GameDir"
    } elseif ($Yes -or $DetectOnly) {
        Fail 'Resident Evil 5 was not found in your Steam libraries. Run: Install.bat -GameDir "<folder with re5dx9.exe>"'
    } else {
        Say 'Resident Evil 5 was not found in your Steam libraries.'
        $GameDir = (Read-Host 'Paste the folder that contains re5dx9.exe (Steam: right-click the game > Manage > Browse local files)').Trim('"', ' ')
    }
}
$exe = Join-Path $GameDir 're5dx9.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail "re5dx9.exe is not in $GameDir" }
if ($DetectOnly) { Say "Game folder: $GameDir"; exit 0 }

# 2. The game must be closed.
if (Get-Process -Name 're5dx9' -ErrorAction SilentlyContinue) { Fail 'Resident Evil 5 is running. Close it, then run Install.bat again.' }

# 3. The build the mod was made for.
$exeHash = Get-Hash $exe
if ($exeHash -ne $SupportedExe) {
    Say ''
    Say 'WARNING: your re5dx9.exe is not the build this alpha supports (Steam build 11465250, version 1.0.0.129).'
    Say 'On another build the mod switches itself off and the game runs as usual.'
    if (-not (Ask 'Install anyway?')) { Say 'Nothing was installed.'; exit 1 }
} else {
    Say 'Game build: supported (Steam build 11465250).'
}

$modDir = Join-Path $GameDir 'RogueMercenaries'
$targetDll = Join-Path $GameDir 'dinput8.dll'
$manifestPath = Join-Path $modDir 'install.json'
$backupDir = Join-Path $modDir 'backup'
$dllBackup = ''

# 4. What is already there: an earlier install of this mod is replaced; another mod's
#    dinput8.dll is kept aside.
if (Test-Path -LiteralPath $manifestPath) {
    $old = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Say "Replacing the installed Rogue Mercenaries $($old.version)."
    foreach ($f in $old.files) {
        $p = Join-Path $GameDir $f.path
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
    }
    if ($old.dinput8_backup) { $dllBackup = $old.dinput8_backup }
} elseif (Test-Path -LiteralPath $targetDll) {
    if (Test-OurDll $targetDll) {
        Say 'An earlier Rogue Mercenaries copy (installed by hand) is here; it is replaced.'
        Remove-Item -LiteralPath $targetDll -Force
    } else {
        Say ''
        Say 'Another mod already uses dinput8.dll in the game folder. Only one can be active:'
        Say 'it is moved to RogueMercenaries\backup and put back when you uninstall Rogue Mercenaries.'
        if (-not (Ask 'Continue?')) { Say 'Nothing was installed.'; exit 1 }
        New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
        $dllBackup = 'RogueMercenaries\backup\dinput8.dll'
        Move-Item -LiteralPath $targetDll -Destination (Join-Path $GameDir $dllBackup) -Force
        Say "Kept aside: $dllBackup"
    }
}

# 5. A copy of the game's save, in case anything goes wrong (the mod never writes it on purpose).
$saved = 0
foreach ($root in Get-SteamRoots) {
    $userdata = Join-Path $root 'userdata'
    if (-not (Test-Path -LiteralPath $userdata)) { continue }
    foreach ($user in Get-ChildItem -LiteralPath $userdata -Directory -ErrorAction SilentlyContinue) {
        $save = Join-Path $user.FullName '21690\remote\savedata.bin'
        if (-not (Test-Path -LiteralPath $save)) { continue }
        $dst = Join-Path $dataDir 'save-backups'
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        $name = 'savedata-{0}-{1}.bin' -f $user.Name, (Get-Date -Format 'yyyyMMdd-HHmmss')
        Copy-Item -LiteralPath $save -Destination (Join-Path $dst $name)
        $saved++
    }
}
if ($saved -gt 0) { Say "Game save backed up to $dataDir\save-backups" } else { Say 'No game save found to back up (a new game is fine).' }

# 6. The files.
$installed = @()
try {
    Get-ChildItem -LiteralPath $payload -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($payload.Length + 1)
        $dst = Join-Path $GameDir $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
        # Files from a downloaded ZIP carry an "internet" mark that can keep a DLL from loading.
        Unblock-File -LiteralPath $dst -ErrorAction SilentlyContinue
        $installed += [pscustomobject]@{ path = $rel; sha256 = Get-Hash $dst }
    }
    foreach ($name in @('uninstall.ps1', 'Uninstall.bat')) {
        $src = Join-Path $here ($(if ($name -eq 'Uninstall.bat') { 'game-uninstall.bat' } else { $name }))
        Copy-Item -LiteralPath $src -Destination (Join-Path $modDir $name) -Force
    }
} catch {
    foreach ($f in $installed) { Remove-Item -LiteralPath (Join-Path $GameDir $f.path) -Force -ErrorAction SilentlyContinue }
    if ($dllBackup -and -not (Test-Path -LiteralPath $targetDll)) {
        Move-Item -LiteralPath (Join-Path $GameDir $dllBackup) -Destination $targetDll -ErrorAction SilentlyContinue
    }
    Fail "Copying failed and was undone: $_"
}

[pscustomobject]@{
    version = $Version
    installed = (Get-Date).ToString('o')
    game_exe_sha256 = $exeHash
    dinput8_backup = $dllBackup
    files = $installed
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8

Say ''
Say "Installed Rogue Mercenaries $Version ($($installed.Count) files)."
Say ''
Say 'How to play: start Resident Evil 5 from Steam. "Rogue Mercenaries" shows at the top right;'
Say 'in the Play menu pick ROGUE MERCENARIES (the last row, press Up from the first row).'
Say 'Read README.txt for controls and what to send when something breaks.'
if ((Get-Content -LiteralPath (Join-Path $payload 'RogueMercenaries\adapter.json') -Raw) -match '"everything_unlocked":\s*true') {
    Say ''
    Say 'This is the EVERYTHING-UNLOCKED package. It plays on its own progress file'
    Say "($dataDir\profile.unlocked.json); your normal progress is not used or changed."
    Say 'To go back to your normal progress, close the game and run Install.bat from the normal package.'
} elseif (Test-Path -LiteralPath (Join-Path $dataDir 'profile.json')) {
    Say ''
    Say "Your earlier Rogue Mercenaries progress is kept ($dataDir\profile.json)."
    Say 'Delete that file to start over with everything locked.'
}
Say ''
Say 'To remove the mod: run Uninstall.bat (here, or in the game folder''s RogueMercenaries folder).'
exit 0

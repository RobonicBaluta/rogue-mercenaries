param(
    [string]$Version = '',
    [string]$Dll = 'build\win32\adapters\re5\Release\dinput8.dll',
    [switch]$Unlocked,  # the everything-unlocked package: its own progress file, the normal one untouched
    [switch]$Manual,    # no installer: the files as they go in the game folder, copied by hand (Nexus Mods)
    [switch]$Plugin     # by hand, as another dinput8.dll loader's plugin: plugins\RogueMercenaries.dll
)

# Builds dist\RogueMercenaries-<version>.zip for testers: Install.bat / Uninstall.bat (the
# scripts in installer\), the files they install (files\: dinput8.dll and the RogueMercenaries
# folder with the player adapter.json, the content configs and icons), README.txt, third-party
# licenses and a SHA-256 manifest. Never includes game files, a mod profile, logs or a
# developer config. The version defaults to kModVersion in adapters\re5\src\version.h.
#
# -Manual builds RogueMercenaries-<version>[-unlocked]-manual.zip instead: no script or batch
# file (a mod site's scan flagged the installer's package, 2026-11-18), the ZIP's root laid out as
# the game folder (dinput8.dll, RogueMercenaries\), the README with by-hand install and uninstall
# steps, the licenses and the manifest inside RogueMercenaries\. The mod reads nothing the
# installer writes (install.json is the uninstaller's).
#
# -Plugin builds RogueMercenaries-<version>[-unlocked]-plugin.zip: the -Manual layout with the
# DLL as plugins\RogueMercenaries.dll, for players whose input fixes come with their own
# dinput8.dll plugin loader (2026-11-18, a player: "Rick and Maluc's fix depends on that very
# file"). The mod finds the game folder from the executable and runs once whichever way it loads.

$ErrorActionPreference = 'Stop'
if ($Plugin) { $Manual = $true }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dllPath = Join-Path $repoRoot $Dll
if (-not (Test-Path -LiteralPath $dllPath)) { throw "build the adapter first: $dllPath missing" }
if (-not $Version) {
    $header = Get-Content -LiteralPath (Join-Path $repoRoot 'adapters\re5\src\version.h') -Raw
    if ($header -notmatch 'kModVersion\[\]\s*=\s*"([^"]+)"') { throw 'kModVersion not found in version.h' }
    $Version = $Matches[1]
}
# The DLL must be the build of this version (its start line carries the version).
$dllText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dllPath))
if (-not $dllText.Contains($Version)) { throw "$dllPath is not a $Version build; rebuild it (scripts\build_core.ps1)" }

$player = Get-Content -LiteralPath (Join-Path $repoRoot 'configs\adapter.json') -Raw | ConvertFrom-Json
if ($player.developer_mode) { throw 'configs\adapter.json has developer_mode on; testers get the player config' }

$name = "RogueMercenaries-$Version"
if ($Unlocked) { $name += '-unlocked' }
if ($Plugin) { $name += '-plugin' } elseif ($Manual) { $name += '-manual' }
$stage = Join-Path $repoRoot "build\package\$name"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
if ($Manual) {
    $files = $stage                               # the ZIP's root is the game folder
    $modFiles = Join-Path $files 'RogueMercenaries'
    $docs = $modFiles                             # README, licenses and manifest beside the mod's files
} else {
    $files = Join-Path $stage 'files'
    $modFiles = Join-Path $files 'RogueMercenaries'
    $docs = $stage
    New-Item -ItemType Directory -Force -Path (Join-Path $stage 'installer') | Out-Null
}
New-Item -ItemType Directory -Force -Path (Join-Path $modFiles 'configs\icons') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $docs 'licenses') | Out-Null

# What goes into the game folder.
if ($Plugin) {
    New-Item -ItemType Directory -Force -Path (Join-Path $files 'plugins') | Out-Null
    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $files 'plugins\RogueMercenaries.dll')
} else {
    Copy-Item -LiteralPath $dllPath -Destination (Join-Path $files 'dinput8.dll')
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'configs\adapter.json') -Destination (Join-Path $modFiles 'adapter.json')
if ($Unlocked) {
    $adapterFile = Join-Path $modFiles 'adapter.json'
    $adapterText = Get-Content -LiteralPath $adapterFile -Raw
    $marker = '"developer_mode": false,'
    if (-not $adapterText.Contains($marker)) { throw "adapter.json has no $marker line to flag the package after" }
    [System.IO.File]::WriteAllText($adapterFile, $adapterText.Replace($marker, $marker + "`r`n  " + '"everything_unlocked": true,'), [System.Text.Encoding]::ASCII)
}
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'configs') -Filter '*.json' -File |
    Where-Object { $_.Name -notlike 'adapter*.json' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $modFiles 'configs') }
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'configs\icons') -Filter '*.png' -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $modFiles 'configs\icons') }

# Text files: Windows line endings, the version filled in.
function Write-Text([string]$text, [string]$dst, [string]$version) {
    $text = $text.Replace("`r`n", "`n").Replace("`n", "`r`n").Replace('{VERSION}', $version)
    [System.IO.File]::WriteAllText($dst, $text, [System.Text.Encoding]::ASCII)
}
$installerSrc = Join-Path $repoRoot 'installer'
if (-not $Manual) {
    # The installer: scripts with the version filled in, batch files with Windows line endings.
    foreach ($script in @('install.ps1', 'uninstall.ps1')) {
        $text = (Get-Content -LiteralPath (Join-Path $installerSrc $script) -Raw).Replace('{VERSION}', $Version)
        Set-Content -LiteralPath (Join-Path $stage "installer\$script") -Value $text -Encoding ascii -NoNewline
    }
    foreach ($pair in @(@('Install.bat', 'Install.bat'), @('Uninstall.bat', 'Uninstall.bat'), @('game-uninstall.bat', 'installer\game-uninstall.bat'))) {
        Write-Text (Get-Content -LiteralPath (Join-Path $installerSrc $pair[0]) -Raw) (Join-Path $stage $pair[1]) $Version
    }
}

# README.txt; by hand, its install and uninstall sections are the manual steps.
$readmeText = (Get-Content -LiteralPath (Join-Path $installerSrc 'README.txt') -Raw).Replace("`r`n", "`n")
function Set-Section([string]$text, [string]$from, [string]$to, [string]$with) {
    $start = $text.IndexOf($from)
    if ($start -lt 0 -or $text.IndexOf($from, $start + 1) -ge 0) { throw "installer\README.txt: '$($from.Trim())' is not there once; update package_release.ps1" }
    $end = $text.IndexOf($to, $start + $from.Length)
    if ($end -lt 0) { throw "installer\README.txt: '$($to.Trim())' does not follow '$($from.Trim())'" }
    return $text.Substring(0, $start) + $with + $text.Substring($end)
}
$saveSteps = @(
    '  1. Close Resident Evil 5.',
    '  2. Recommended: copy your game save somewhere safe first. It is the file',
    '     <Steam folder>\userdata\<your number>\21690\remote\savedata.bin.',
    '     The mod is designed never to write your game save during a run.',
    '  3. Open the game folder: in Steam, right-click Resident Evil 5 > Manage > Browse local',
    '     files. It is the folder with re5dx9.exe.')
$keptProgress = @(
    '  Your progress (Credits, unlocks) is kept in %LOCALAPPDATA%\RogueMercenaries, not in the',
    '  game folder, so it carries over to a new version.')
if ($Plugin) {
    $install = @('', 'INSTALL (WITH ANOTHER dinput8.dll MOD)',
        '  This package is for a game that already has a dinput8.dll mod which loads plugins: a',
        '  plugin loader that came with other fixes (a dinput8.dll and a plugins folder). Keep that',
        '  dinput8.dll; Rogue Mercenaries runs as one of its plugins.') + $saveSteps + @(
        '  4. Copy everything in this ZIP into the game folder: RogueMercenaries.dll goes into the',
        '     plugins folder, and the RogueMercenaries folder next to re5dx9.exe. When updating,',
        '     replace the files.',
        '  5. If the game folder also has Rogue Mercenaries'' own dinput8.dll from an earlier',
        '     install, put the other mod''s dinput8.dll back in its place.',
        '  With an ASI loader instead (it loads .asi files), copy RogueMercenaries.dll into the folder',
        '  it loads them from and rename it RogueMercenaries.asi.') + $keptProgress + @('') -join "`n"
    $uninstall = @('', 'UNINSTALL',
        '  Close the game, then delete plugins\RogueMercenaries.dll (or RogueMercenaries.asi) and the',
        '  RogueMercenaries folder from the game folder. Costume mods you put in',
        '  RogueMercenaries\mods go with it: move that folder out first to keep them. The other',
        '  mod''s dinput8.dll and plugins stay as they are.',
        '  Your progress and logs stay in %LOCALAPPDATA%\RogueMercenaries; delete that folder too',
        '  to remove everything.') -join "`n"
    $uninstall += "`n"
    $gone = 'If it is gone, allow it and copy RogueMercenaries.dll back into the plugins folder.'
} else {
    $install = @('', 'INSTALL (BY HAND)') + $saveSteps + @(
        '  4. If that folder already has a dinput8.dll from another mod, move it out first: only',
        '     one dinput8.dll mod can be active. If that dinput8.dll loads a plugins folder (input',
        '     fixes often come that way), use the plugin package of Rogue Mercenaries instead and',
        '     keep it.',
        '  5. Copy everything in this ZIP into the game folder: dinput8.dll goes next to',
        '     re5dx9.exe, and the RogueMercenaries folder beside it. When updating, replace the',
        '     files.') + $keptProgress + @('') -join "`n"
    $uninstall = @('', 'UNINSTALL',
        '  Close the game, then delete dinput8.dll and the RogueMercenaries folder from the game',
        '  folder. Costume mods you put in RogueMercenaries\mods go with it: move that folder out',
        '  first to keep them. If an earlier version was installed with Install.bat and another',
        '  mod''s dinput8.dll was set aside, it is in RogueMercenaries\backup: move it back to the',
        '  game folder. Put back any dinput8.dll you moved out yourself.',
        '  Your progress and logs stay in %LOCALAPPDATA%\RogueMercenaries; delete that folder too',
        '  to remove everything.',
        '  Optional: Steam > Resident Evil 5 > Properties > Installed Files > Verify integrity.', '') -join "`n"
    $gone = 'If it is gone, allow it and copy dinput8.dll back.'
}
if ($Manual) {
    $readmeText = Set-Section $readmeText "`nINSTALL`n" "`nOPTIONAL: COSTUME MODS" $install
    $readmeText = Set-Section $readmeText "`nUNINSTALL`n" "`nLICENSES" $uninstall
    $line = 'If it is gone, allow it and run Install.bat again.'
    if (-not $readmeText.Contains($line)) { throw "installer\README.txt: '$line' is not there; update package_release.ps1" }
    $readmeText = $readmeText.Replace($line, $gone)
    if ($Plugin) {
        $check = 'check that dinput8.dll'
        if (-not $readmeText.Contains($check)) { throw "installer\README.txt: '$check' is not there; update package_release.ps1" }
        $readmeText = $readmeText.Replace($check, 'check that plugins\RogueMercenaries.dll')
    }
}
if ($Unlocked) {
    $back = if ($Manual) { "game and copy the normal package's files over these." } else { 'game and run Install.bat from the normal package.' }
    $notice = @(
        'THIS IS THE EVERYTHING-UNLOCKED PACKAGE, FOR TRYING THINGS OUT',
        '  Every character, skin, perk, weapon, run length and hard boss is open from the start.',
        '  (A skin that wears another author''s costume mod still needs that mod.)',
        '  It plays on its own progress file, %LOCALAPPDATA%\RogueMercenaries\profile.unlocked.json.',
        '  Your normal progress (profile.json) is not used or changed. To go back to it, close the',
        "  $back",
        '', '') -join "`n"
    $readmeText = $notice + $readmeText
}
if ($Manual) {
    # No step may send the player to a script the package does not have (the uninstall note
    # about an earlier Install.bat install is the one mention allowed).
    $left = @([regex]::Matches($readmeText, '(?m)^.*(Install\.bat|Uninstall\.bat|installer).*$') |
        Where-Object { $_.Value -notmatch 'installed with Install\.bat' })
    if ($left.Count -gt 0) { throw "the manual README still names the installer: $(($left | ForEach-Object { $_.Value.Trim() }) -join ' | ')" }
}
Write-Text $readmeText (Join-Path $docs 'README.txt') $Version

$licenses = @{
    'MinHook-LICENSE.txt' = 'third_party\minhook\LICENSE.txt'
    'DearImGui-LICENSE.txt' = 'third_party\imgui\LICENSE.txt'
    'nlohmann-json-LICENSE.MIT' = 'third_party\nlohmann\LICENSE.MIT'
}
foreach ($license in $licenses.Keys) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $licenses[$license]) -Destination (Join-Path $docs "licenses\$license")
}

$manifest = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1)
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $rel
}
Set-Content -LiteralPath (Join-Path $docs 'SHA256SUMS.txt') -Value $manifest -Encoding ascii

$dist = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
if ($Manual) {
    # The ZIP's root is what goes into the game folder; entry paths with "/" (the ZIP standard;
    # Compress-Archive in Windows PowerShell 5.1 writes "\").
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [System.IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $archive.Dispose()
    }
} else {
    # The ZIP holds one folder, so extracting it never scatters files.
    Compress-Archive -Path $stage -DestinationPath $zip
}
"package: $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)) MB, $((Get-ChildItem -LiteralPath $stage -Recurse -File).Count) files)"
"sha256:  $((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)"

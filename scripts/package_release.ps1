param(
    [string]$Version = '0.1.0-dev',
    [string]$Dll = 'build\win32\adapters\re5\Release\dinput8.dll'
)

# Builds dist\RogueMercenaries-<version>.zip: the files a player copies into
# the game folder, install/uninstall notes, third-party licenses and a SHA-256
# manifest of everything inside. Never includes game files.

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dllPath = Join-Path $repoRoot $Dll
if (-not (Test-Path -LiteralPath $dllPath)) { throw "build the adapter first: $dllPath missing" }

$stage = Join-Path $repoRoot "build\package\RogueMercenaries-$Version"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$gameFiles = Join-Path $stage 'copy-into-game-folder'
New-Item -ItemType Directory -Force -Path (Join-Path $gameFiles 'RogueMercenaries\configs\icons') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'licenses') | Out-Null

Copy-Item -LiteralPath $dllPath -Destination (Join-Path $gameFiles 'dinput8.dll')
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'configs') -Filter '*.json' -File |
    Where-Object { $_.Name -notlike 'adapter*.json' } |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $gameFiles 'RogueMercenaries\configs') }
Get-ChildItem -LiteralPath (Join-Path $repoRoot 'configs\icons') -Filter '*.png' -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $gameFiles 'RogueMercenaries\configs\icons') }
Copy-Item -LiteralPath (Join-Path $repoRoot 'configs\adapter.json') -Destination (Join-Path $gameFiles 'RogueMercenaries\adapter.json')

$licenses = @{
    'MinHook-LICENSE.txt' = 'third_party\minhook\LICENSE.txt'
    'DearImGui-LICENSE.txt' = 'third_party\imgui\LICENSE.txt'
    'nlohmann-json-LICENSE.MIT' = 'third_party\nlohmann\LICENSE.MIT'
}
foreach ($name in $licenses.Keys) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $licenses[$name]) -Destination (Join-Path $stage "licenses\$name")
}

$readme = @"
Rogue Mercenaries $Version - roguelite mod for Resident Evil 5 Gold Edition (Steam)

SUPPORTED GAME BUILD
  Steam build 11465250, re5dx9.exe 1.0.0.129
  SHA-256 751726F0EC8BD01C00B037AF8AE64CDF0BC5CC5B9025F3F4D782EF6F66E04E26
  On any other build the mod disables itself (see the log) and the game runs unmodified.

INSTALL
  1. Close the game.
  2. Copy everything inside "copy-into-game-folder" into the folder that contains
     re5dx9.exe (Steam: right-click RE5 > Manage > Browse local files).
     If a dinput8.dll is already there, it belongs to another mod: keep a copy of it,
     as only one dinput8.dll can be installed at a time.
  3. Start the game from Steam. "Rogue Mercenaries - Play menu or F7" appears top right.

PLAY
  Play menu > ROGUE MERCENARIES (last row) > Solo > pick a stage and Chris or Sheva.
  The run starts with the match; each match is one arena. With Japanese, Chinese or
  Russian game text: The Mercenaries > Solo, then F7 > Start run (same character).
  Clear the kill target, pick a reward (1-3), shop (digits buy, Enter leaves), choose
  the next route (digit). Then start the next match. Three arenas make a run;
  Credits are kept for unlocks.

UNINSTALL / ROLLBACK
  Delete dinput8.dll and the RogueMercenaries folder from the game folder.
  Optional: Steam > Properties > Installed Files > Verify integrity.
  Mod save and logs: %LOCALAPPDATA%\RogueMercenaries (delete to reset progression).
  The mod never writes to the game's own save; normal play in Mercenaries still
  records scores as usual.

SUPPORT
  F7 > Developer section (developer_mode true in RogueMercenaries\adapter.json)
  > Export diagnostics, or zip %LOCALAPPDATA%\RogueMercenaries\logs.
"@
Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Value $readme -Encoding utf8

$manifest = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1)
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $rel
}
Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value $manifest -Encoding ascii

$dist = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist "RogueMercenaries-$Version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
"package: $zip"
"sha256:  $((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)"

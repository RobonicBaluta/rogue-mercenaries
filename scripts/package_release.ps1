param(
    [string]$Version = '',
    [string]$Dll = 'build\win32\adapters\re5\Release\dinput8.dll',
    [switch]$Unlocked   # the everything-unlocked package: its own progress file, the normal one untouched
)

# Builds dist\RogueMercenaries-<version>.zip for testers: Install.bat / Uninstall.bat (the
# scripts in installer\), the files they install (files\: dinput8.dll and the RogueMercenaries
# folder with the player adapter.json, the content configs and icons), README.txt, third-party
# licenses and a SHA-256 manifest. Never includes game files, a mod profile, logs or a
# developer config. The version defaults to kModVersion in adapters\re5\src\version.h.

$ErrorActionPreference = 'Stop'
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
$stage = Join-Path $repoRoot "build\package\$name"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$files = Join-Path $stage 'files'
$modFiles = Join-Path $files 'RogueMercenaries'
New-Item -ItemType Directory -Force -Path (Join-Path $modFiles 'configs\icons') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'installer') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'licenses') | Out-Null

# What the installer copies into the game folder.
Copy-Item -LiteralPath $dllPath -Destination (Join-Path $files 'dinput8.dll')
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

# The installer: scripts with the version filled in, batch files with Windows line endings.
$installerSrc = Join-Path $repoRoot 'installer'
foreach ($script in @('install.ps1', 'uninstall.ps1')) {
    $text = (Get-Content -LiteralPath (Join-Path $installerSrc $script) -Raw).Replace('{VERSION}', $Version)
    Set-Content -LiteralPath (Join-Path $stage "installer\$script") -Value $text -Encoding ascii -NoNewline
}
function Write-Crlf([string]$src, [string]$dst, [string]$version) {
    $text = (Get-Content -LiteralPath $src -Raw).Replace("`r`n", "`n").Replace("`n", "`r`n").Replace('{VERSION}', $version)
    [System.IO.File]::WriteAllText($dst, $text, [System.Text.Encoding]::ASCII)
}
Write-Crlf (Join-Path $installerSrc 'Install.bat') (Join-Path $stage 'Install.bat') $Version
Write-Crlf (Join-Path $installerSrc 'Uninstall.bat') (Join-Path $stage 'Uninstall.bat') $Version
Write-Crlf (Join-Path $installerSrc 'game-uninstall.bat') (Join-Path $stage 'installer\game-uninstall.bat') $Version
Write-Crlf (Join-Path $installerSrc 'README.txt') (Join-Path $stage 'README.txt') $Version
if ($Unlocked) {
    $readme = Join-Path $stage 'README.txt'
    $notice = @(
        'THIS IS THE EVERYTHING-UNLOCKED PACKAGE, FOR TRYING THINGS OUT',
        '  Every character, skin, perk, weapon, run length and hard boss is open from the start.',
        '  (A skin that wears another author''s costume mod still needs that mod.)',
        '  It plays on its own progress file, %LOCALAPPDATA%\RogueMercenaries\profile.unlocked.json.',
        '  Your normal progress (profile.json) is not used or changed. To go back to it, close the',
        '  game and run Install.bat from the normal package.',
        '', '') -join "`r`n"
    [System.IO.File]::WriteAllText($readme, $notice + [System.IO.File]::ReadAllText($readme), [System.Text.Encoding]::ASCII)
}

$licenses = @{
    'MinHook-LICENSE.txt' = 'third_party\minhook\LICENSE.txt'
    'DearImGui-LICENSE.txt' = 'third_party\imgui\LICENSE.txt'
    'nlohmann-json-LICENSE.MIT' = 'third_party\nlohmann\LICENSE.MIT'
}
foreach ($license in $licenses.Keys) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $licenses[$license]) -Destination (Join-Path $stage "licenses\$license")
}

$manifest = Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($stage.Length + 1)
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $rel
}
Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value $manifest -Encoding ascii

$dist = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
# The ZIP holds one folder, so extracting it never scatters files.
Compress-Archive -Path $stage -DestinationPath $zip
"package: $zip ($([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)) MB, $((Get-ChildItem -LiteralPath $stage -Recurse -File).Count) files)"
"sha256:  $((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)"

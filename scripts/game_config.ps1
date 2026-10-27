param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('test', 'restore', 'status')]
    [string]$Mode,

    [string]$Resolution = '1280x720'
)

# Switches the user's RE5 config.ini to a small windowed mode for automated
# tests, keeping a one-time backup of the original under the project. Restore
# puts that original back byte-for-byte.

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$config = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'CAPCOM\RESIDENT EVIL 5\config.ini'
$backupDir = Join-Path $repoRoot 'build\backup'
$backup = Join-Path $backupDir 'config.ini.original'

switch ($Mode) {
    'status' {
        "config=$config"
        "backup_present=$(Test-Path -LiteralPath $backup)"
        Select-String -LiteralPath $config -Pattern '^(Resolution|FullScreen|RefreshRate)=' | ForEach-Object { $_.Line }
    }
    'test' {
        New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
        if (-not (Test-Path -LiteralPath $backup)) {
            Copy-Item -LiteralPath $config -Destination $backup
            "backed up original to $backup"
        }
        $text = [IO.File]::ReadAllText($config)
        $text = $text -replace '(?m)^Resolution=.*$', "Resolution=$Resolution"
        $text = $text -replace '(?m)^FullScreen=.*$', 'FullScreen=OFF'
        $text = $text -replace '(?m)^RefreshRate=.*$', 'RefreshRate=60Hz'
        # Cap GPU use so a test session does not compete with other programs.
        $text = $text -replace '(?m)^VSYNC=.*$', 'VSYNC=ON'
        [IO.File]::WriteAllText($config, $text, [Text.Encoding]::ASCII)
        "test config written: $Resolution windowed"
    }
    'restore' {
        if (-not (Test-Path -LiteralPath $backup)) { throw "no backup at $backup" }
        Copy-Item -LiteralPath $backup -Destination $config -Force
        "restored $config"
    }
}

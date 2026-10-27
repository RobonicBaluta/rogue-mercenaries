param(
    # adapter.json to deploy for this run.
    [string]$AdapterConfig = 'configs\adapter.dev.json',
    [string]$Dll = '',
    [int]$Seconds = 30,
    [string]$Label = 'smoke',
    # Seconds after launch at which to request in-process frame captures
    # (developer builds). One final capture is always taken before closing.
    # Comma-separated (powershell -File cannot pass arrays), e.g. "8,12,16".
    [string]$CaptureAt = ''
)

# One unattended runtime check: install, launch (background, muted by the dev
# config), wait, capture frames and the adapter log, close the game, uninstall,
# and verify the executable is untouched.

$ErrorActionPreference = 'Stop'
$captureTimes = @($CaptureAt -split ',' | Where-Object { $_.Trim() } | ForEach-Object { [int]$_ })
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ps = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File')
$gamectl = Join-Path $PSScriptRoot 'gamectl.ps1'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir = Join-Path $repoRoot "build\runs\$stamp-$Label"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$dataDir = Join-Path $env:LOCALAPPDATA 'RogueMercenaries'
$logDir = Join-Path $dataDir 'logs'
$captureDir = Join-Path $dataDir 'captures'
Remove-Item -Path (Join-Path $captureDir '*.bmp') -ErrorAction SilentlyContinue

function Request-Capture([string]$tag) {
    Set-Content -Path (Join-Path $dataDir 'capture.request') -Value $tag
}

$deployArgs = @('-Mode', 'install', '-AdapterConfig', (Join-Path $repoRoot $AdapterConfig))
if ($Dll) { $deployArgs += @('-Dll', $Dll) }
& powershell @ps (Join-Path $PSScriptRoot 'deploy.ps1') @deployArgs
if ($LASTEXITCODE) { throw 'deploy failed' }

try {
    & powershell @ps $gamectl -Action launch | Out-Host
    $alive = 0
    for ($t = 1; $t -le $Seconds; $t++) {
        Start-Sleep -Seconds 1
        if ($captureTimes -contains $t) { Request-Capture "t$t" }
        if ($t % 5 -eq 0) {
            $status = & powershell @ps $gamectl -Action status
            "t+${t}s $status" | Tee-Object -FilePath (Join-Path $outDir 'status.txt') -Append | Out-Host
            if ($status -like 'running*') { $alive = $t }
        }
    }
    if ((& powershell @ps $gamectl -Action status) -like 'running*') {
        Request-Capture 'final'
        Start-Sleep -Seconds 2
        & powershell @ps $gamectl -Action shot -Out (Join-Path $outDir 'window.png') | Out-Host
        & powershell @ps $gamectl -Action close | Out-Host
        for ($i = 0; $i -lt 10 -and (Get-Process -Name re5dx9 -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 1 }
        if (Get-Process -Name re5dx9 -ErrorAction SilentlyContinue) {
            & powershell @ps $gamectl -Action kill | Out-Host
        }
    }
    "alive_until_s=$alive" | Out-File (Join-Path $outDir 'result.txt') -Encoding ascii
} finally {
    Start-Sleep -Seconds 2
    & powershell @ps $gamectl -Action reap | Out-Host
    Copy-Item -Path (Join-Path $logDir '*.log') -Destination $outDir -ErrorAction SilentlyContinue
    Copy-Item -Path (Join-Path $logDir '*.jsonl') -Destination $outDir -ErrorAction SilentlyContinue
    # Convert in-process captures (BMP) to PNG next to the logs.
    Add-Type -AssemblyName System.Drawing
    Get-ChildItem -Path $captureDir -Filter *.bmp -ErrorAction SilentlyContinue | ForEach-Object {
        $image = [System.Drawing.Image]::FromFile($_.FullName)
        $image.Save((Join-Path $outDir ($_.BaseName + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
        Remove-Item -LiteralPath $_.FullName
    }
    & powershell @ps (Join-Path $PSScriptRoot 'deploy.ps1') -Mode uninstall | Out-Host
}
"run artifacts: $outDir"

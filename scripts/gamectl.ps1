param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('launch', 'shot', 'capture', 'keys', 'focus', 'close', 'kill', 'reap', 'status')]
    [string]$Action,

    # keys: comma-separated tokens, each with optional ":holdMs" and "+waitMs":
    #   key names (enter,esc,up,down,left,right,space,a-z,0-9,tab,shift,ctrl,alt,f1..f12,...) tap a key
    #   down=<key> / up=<key>       press or release without the other half
    #   lclick rclick ldown lup rdown rup mclick   mouse buttons
    #   move=<dx>/<dy>              relative mouse move in mickeys
    #   wait                        just sleep (use +ms)
    # e.g. "down,enter:200+1500,rdown+300,lclick+200,rup"
    [string]$Keys = '',

    [string]$Out = '',

    [int]$DefaultHoldMs = 80,
    [int]$DefaultGapMs = 350
)

# Drives the windowed game for automated tests: launch, focus, screenshot,
# scancode key input (DirectInput reads scancodes, not virtual keys), close.

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$gameDir = 'G:\SteamLibrary\steamapps\common\Resident Evil 5'

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading;

public static class GameCtl {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public INPUTUNION u; }

    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);

    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);

    // Captures only the game window's own content (PW_CLIENTONLY |
    // PW_RENDERFULLCONTENT), even when other windows cover it. Never reads
    // the screen, so nothing else the user has open is recorded.
    public static string Shot(IntPtr h, string path) {
        RECT r; GetClientRect(h, out r);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        if (w <= 0 || ht <= 0) return "empty client rect";
        using (var bmp = new Bitmap(w, ht)) {
            bool ok;
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                ok = PrintWindow(h, hdc, 1 | 2);
                g.ReleaseHdc(hdc);
            }
            bmp.Save(path, ImageFormat.Png);
            return String.Format("{0}x{1} printwindow={2}", w, ht, ok);
        }
    }

    public static void Focus(IntPtr h) {
        // Already foreground: an Alt tap here would put the window into menu
        // mode and swallow (or redirect) the next key.
        if (GetForegroundWindow() == h) return;
        // Alt tap lets SetForegroundWindow succeed from a background process.
        keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero);
        ShowWindow(h, 9); SetForegroundWindow(h);
    }

    static void SendKey(ushort scan, bool extended, bool up) {
        var input = new INPUT[1];
        input[0].type = 1;
        input[0].u.ki.wScan = scan;
        input[0].u.ki.dwFlags = 0x8u | (extended ? 1u : 0u) | (up ? 2u : 0u);
        SendInput(1, input, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void KeyDown(ushort scan, bool extended) { SendKey(scan, extended, false); }
    public static void KeyUp(ushort scan, bool extended) { SendKey(scan, extended, true); }

    public static void Key(ushort scan, bool extended, int holdMs) {
        SendKey(scan, extended, false);
        Thread.Sleep(holdMs);
        SendKey(scan, extended, true);
    }

    // flags: MOUSEEVENTF_MOVE 1, LEFTDOWN 2, LEFTUP 4, RIGHTDOWN 8, RIGHTUP 0x10, MIDDLEDOWN 0x20, MIDDLEUP 0x40
    public static void Mouse(uint flags, int dx, int dy) {
        var input = new INPUT[1];
        input[0].type = 0;
        input[0].u.mi.dx = dx;
        input[0].u.mi.dy = dy;
        input[0].u.mi.dwFlags = flags;
        SendInput(1, input, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$scan = @{
    esc = 0x01; '1' = 0x02; '2' = 0x03; '3' = 0x04; '4' = 0x05; '5' = 0x06; '6' = 0x07; '7' = 0x08; '8' = 0x09; '9' = 0x0A; '0' = 0x0B
    tab = 0x0F; q = 0x10; w = 0x11; e = 0x12; r = 0x13; t = 0x14; y = 0x15; u = 0x16; i = 0x17; o = 0x18; p = 0x19
    enter = 0x1C; ctrl = 0x1D; a = 0x1E; s = 0x1F; d = 0x20; f = 0x21; g = 0x22; h = 0x23; j = 0x24; k = 0x25; l = 0x26
    shift = 0x2A; alt = 0x38; backspace = 0x0E; z = 0x2C; x = 0x2D; c = 0x2E; v = 0x2F; b = 0x30; n = 0x31; m = 0x32; space = 0x39
    f1 = 0x3B; f2 = 0x3C; f3 = 0x3D; f4 = 0x3E; f5 = 0x3F; f6 = 0x40; f7 = 0x41; f8 = 0x42; f9 = 0x43; f10 = 0x44; f11 = 0x57; f12 = 0x58
}
$extended = @{ up = 0x48; down = 0x50; left = 0x4B; right = 0x4D; insert = 0x52; delete = 0x53; home = 0x47; end = 0x4F; pgup = 0x49; pgdn = 0x51 }

function Get-GameProcess { Get-Process -Name re5dx9 -ErrorAction SilentlyContinue | Select-Object -First 1 }

function Get-GameWindow {
    $p = Get-GameProcess
    if (-not $p) { throw 're5dx9 is not running' }
    if ($p.MainWindowHandle -eq [IntPtr]::Zero) { throw 're5dx9 has no main window yet' }
    $p.MainWindowHandle
}

switch ($Action) {
    'status' {
        $p = Get-GameProcess
        if ($p) { "running pid=$($p.Id) window=$($p.MainWindowHandle) title='$($p.MainWindowTitle)' responding=$($p.Responding) ws=$([math]::Round($p.WorkingSet64/1MB))MB" } else { 'not running' }
    }
    'launch' {
        if (Get-GameProcess) { throw 're5dx9 already running' }
        # Launch through Steam: a direct start makes SteamStub terminate the
        # first process and ask Steam to start a second one.
        Start-Process 'steam://rungameid/21690'
        for ($i = 0; $i -lt 40 -and -not (Get-GameProcess); $i++) { Start-Sleep -Milliseconds 500 }
        $p = Get-GameProcess
        if ($p) { "launched via steam pid=$($p.Id)" } else { 'steam launch requested; process not seen within 20 s' }
    }
    'focus' { [GameCtl]::Focus((Get-GameWindow)); 'focused' }
    'shot' {
        $h = Get-GameWindow
        if (-not $Out) {
            $dir = Join-Path $repoRoot 'build\shots'
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            $Out = Join-Path $dir ("shot-{0:yyyyMMdd-HHmmss}.png" -f (Get-Date))
        }
        # .NET resolves relative paths against the process directory, not $PWD.
        if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
        $Out = [IO.Path]::GetFullPath($Out)
        $info = [GameCtl]::Shot($h, $Out)
        "$Out $info"
    }
    'keys' {
        $h = Get-GameWindow
        [GameCtl]::Focus($h)
        Start-Sleep -Milliseconds 150
        $buttons = @{ ldown = 2; lup = 4; rdown = 8; rup = 0x10; mdown = 0x20; mup = 0x40 }
        $clicks = @{ lclick = @(2, 4); rclick = @(8, 0x10); mclick = @(0x20, 0x40) }
        foreach ($token in ($Keys -split ',')) {
            $token = $token.Trim().ToLowerInvariant()
            if (-not $token) { continue }
            $wait = $DefaultGapMs; $hold = $DefaultHoldMs
            if ($token -match '\+(\d+)$') { $wait = [int]$Matches[1]; $token = $token -replace '\+\d+$', '' }
            if ($token -match ':(\d+)$') { $hold = [int]$Matches[1]; $token = $token -replace ':\d+$', '' }
            if ($token -eq 'wait') { Start-Sleep -Milliseconds $wait; continue }
            if ($token -match '^move=(-?\d+)/(-?\d+)$') {
                [GameCtl]::Mouse(1, [int]$Matches[1], [int]$Matches[2])
            } elseif ($buttons.ContainsKey($token)) {
                [GameCtl]::Mouse([uint32]$buttons[$token], 0, 0)
            } elseif ($clicks.ContainsKey($token)) {
                [GameCtl]::Mouse([uint32]$clicks[$token][0], 0, 0); Start-Sleep -Milliseconds $hold
                [GameCtl]::Mouse([uint32]$clicks[$token][1], 0, 0)
            } elseif ($token -match '^(down|up)=(.+)$') {
                $name = $Matches[2]; $isUp = $Matches[1] -eq 'up'
                if ($scan.ContainsKey($name)) { $code = [uint16]$scan[$name]; $ext = $false }
                elseif ($extended.ContainsKey($name)) { $code = [uint16]$extended[$name]; $ext = $true }
                else { throw "unknown key '$name'" }
                if ($isUp) { [GameCtl]::KeyUp($code, $ext) } else { [GameCtl]::KeyDown($code, $ext) }
            } elseif ($scan.ContainsKey($token)) { [GameCtl]::Key([uint16]$scan[$token], $false, $hold) }
            elseif ($extended.ContainsKey($token)) { [GameCtl]::Key([uint16]$extended[$token], $true, $hold) }
            else { throw "unknown key '$token'" }
            Start-Sleep -Milliseconds $wait
        }
        "sent $Keys"
    }
    'capture' {
        # In-process back buffer capture (developer builds of the mod).
        $dataDir = Join-Path $env:LOCALAPPDATA 'RogueMercenaries'
        $captureDir = Join-Path $dataDir 'captures'
        New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
        Remove-Item -Path (Join-Path $captureDir '*.bmp') -ErrorAction SilentlyContinue
        Set-Content -Path (Join-Path $dataDir 'capture.request') -Value 'gamectl'
        $bmp = $null
        for ($i = 0; $i -lt 30 -and -not $bmp; $i++) {
            Start-Sleep -Milliseconds 200
            $bmp = Get-ChildItem -Path $captureDir -Filter *.bmp -ErrorAction SilentlyContinue | Select-Object -First 1
        }
        if (-not $bmp) { throw 'no capture (game not presenting or mod not in developer mode)' }
        Start-Sleep -Milliseconds 200
        if (-not $Out) {
            $dir = Join-Path $repoRoot 'build\shots'
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            $Out = Join-Path $dir ("cap-{0:yyyyMMdd-HHmmss}.png" -f (Get-Date))
        }
        if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
        $image = [System.Drawing.Image]::FromFile($bmp.FullName)
        $image.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
        Remove-Item -LiteralPath $bmp.FullName
        $Out
    }
    'close' { [void][GameCtl]::PostMessage((Get-GameWindow), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); 'WM_CLOSE posted' }
    'kill' { $p = Get-GameProcess; if ($p) { Stop-Process -Id $p.Id -Force; "killed $($p.Id)" } else { 'not running' } }
    'reap' {
        # Steam keeps the app "running" while the game's Launcher.exe wrapper
        # lingers after re5dx9 exits, which blocks the next launch.
        if (Get-GameProcess) { throw 're5dx9 still running' }
        $launchers = Get-Process -Name Launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($gameDir) }
        foreach ($l in $launchers) { Stop-Process -Id $l.Id -Force; "reaped Launcher.exe $($l.Id)" }
        if (-not $launchers) { 'no launcher to reap' }
    }
}

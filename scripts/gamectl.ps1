param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('launch', 'shot', 'keys', 'focus', 'close', 'kill', 'reap', 'status')]
    [string]$Action,

    # keys: comma-separated key names (enter,esc,up,down,left,right,space,w,a,s,d,e,q,f,r,tab,shift,ctrl,f1..f12,insert,delete,home,end,0-9)
    # with optional ":holdMs" and "+waitMs" suffixes, e.g. "down,enter:200+1500"
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
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public KEYBDINPUT ki; public uint pad1, pad2; }

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
        // Alt tap lets SetForegroundWindow succeed from a background process.
        keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero);
        ShowWindow(h, 9); SetForegroundWindow(h);
    }

    public static void Key(ushort scan, bool extended, int holdMs) {
        uint ext = extended ? 1u : 0u;
        var down = new INPUT[1]; down[0].type = 1; down[0].ki.wScan = scan; down[0].ki.dwFlags = 0x8 | ext;
        var up = new INPUT[1]; up[0].type = 1; up[0].ki.wScan = scan; up[0].ki.dwFlags = 0x8 | 0x2 | ext;
        SendInput(1, down, Marshal.SizeOf(typeof(INPUT)));
        Thread.Sleep(holdMs);
        SendInput(1, up, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

$scan = @{
    esc = 0x01; '1' = 0x02; '2' = 0x03; '3' = 0x04; '4' = 0x05; '5' = 0x06; '6' = 0x07; '7' = 0x08; '8' = 0x09; '9' = 0x0A; '0' = 0x0B
    tab = 0x0F; q = 0x10; w = 0x11; e = 0x12; r = 0x13; t = 0x14; y = 0x15; u = 0x16; i = 0x17; o = 0x18; p = 0x19
    enter = 0x1C; ctrl = 0x1D; a = 0x1E; s = 0x1F; d = 0x20; f = 0x21; g = 0x22; h = 0x23; j = 0x24; k = 0x25; l = 0x26
    shift = 0x2A; z = 0x2C; x = 0x2D; c = 0x2E; v = 0x2F; b = 0x30; n = 0x31; m = 0x32; space = 0x39
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
        foreach ($token in ($Keys -split ',')) {
            $token = $token.Trim().ToLowerInvariant()
            if (-not $token) { continue }
            $wait = $DefaultGapMs; $hold = $DefaultHoldMs
            if ($token -match '\+(\d+)$') { $wait = [int]$Matches[1]; $token = $token -replace '\+\d+$', '' }
            if ($token -match ':(\d+)$') { $hold = [int]$Matches[1]; $token = $token -replace ':\d+$', '' }
            if ($token -eq 'wait') { Start-Sleep -Milliseconds $wait; continue }
            if ($scan.ContainsKey($token)) { [GameCtl]::Key([uint16]$scan[$token], $false, $hold) }
            elseif ($extended.ContainsKey($token)) { [GameCtl]::Key([uint16]$extended[$token], $true, $hold) }
            else { throw "unknown key '$token'" }
            Start-Sleep -Milliseconds $wait
        }
        "sent $Keys"
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

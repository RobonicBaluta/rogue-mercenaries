param(
    [Parameter(Mandatory = $true)]
    [int]$ProcessId,

    [string[]]$Rva = @('0x77CB30', '0x31CCB0', '0x321A26'),

    [ValidateRange(1, 256)]
    [int]$Length = 32
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ProcessReadOnly
{
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ReadProcessMemory(
        IntPtr process,
        IntPtr address,
        [Out] byte[] buffer,
        UIntPtr size,
        out UIntPtr bytesRead);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr handle);
}
'@

$process = Get-Process -Id $ProcessId
if ($process.ProcessName -ne 're5dx9') {
    throw "Read-only probe only permits re5dx9.exe; got '$($process.ProcessName)'."
}

$module = $process.Modules | Where-Object ModuleName -EQ 're5dx9.exe' | Select-Object -First 1
if (-not $module) {
    throw 'Could not find re5dx9.exe in the target process module list.'
}

$handle = [ProcessReadOnly]::OpenProcess(0x1010, $false, $ProcessId)
if ($handle -eq [IntPtr]::Zero) {
    throw "OpenProcess failed with Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())."
}

try {
    "PID=$ProcessId BASE=0x$('{0:X8}' -f $module.BaseAddress.ToInt64()) MODULE=$($module.FileName)"
    foreach ($item in $Rva) {
        $rvaValue = [Convert]::ToInt64(($item -replace '^0[xX]', ''), 16)
        $address = [IntPtr]::new($module.BaseAddress.ToInt64() + $rvaValue)
        $buffer = [byte[]]::new($Length)
        [UIntPtr]$bytesRead = [UIntPtr]::Zero
        $ok = [ProcessReadOnly]::ReadProcessMemory($handle, $address, $buffer, [UIntPtr]::new([uint32]$Length), [ref]$bytesRead)
        if (-not $ok -or $bytesRead.ToUInt64() -ne [uint64]$Length) {
            $win32Error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            "RVA=0x$('{0:X8}' -f $rvaValue) READ_FAILED error=$win32Error bytes=$($bytesRead.ToUInt64())"
            continue
        }
        $hex = ($buffer | ForEach-Object { $_.ToString('X2') }) -join ' '
        "RVA=0x$('{0:X8}' -f $rvaValue) ADDRESS=0x$('{0:X8}' -f $address.ToInt64()) BYTES=$hex"
    }
}
finally {
    [void][ProcessReadOnly]::CloseHandle($handle)
}

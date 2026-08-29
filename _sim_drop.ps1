$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class DropSim {
  [StructLayout(LayoutKind.Sequential)]
  public struct DROPFILES { public int pFiles; public POINT pt; public int fNC; public int fWide; }
  [StructLayout(LayoutKind.Sequential)]
  public struct POINT { public int X; public int Y; }
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalAlloc(int flags, UIntPtr bytes);
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalLock(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool GlobalUnlock(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@
$argHwnd = [IntPtr][long]$args[0]
$path = $args[1]
$bytes = [System.Text.Encoding]::Unicode.GetBytes($path + [char]0 + [char]0)
$structSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][DropSim+DROPFILES])
$total = $structSize + $bytes.Length
$h = [DropSim]::GlobalAlloc(0x0042, [UIntPtr][uint64]$total)
$p = [DropSim]::GlobalLock($h)
$df = New-Object DropSim+DROPFILES
$df.pFiles = $structSize
$df.fWide = 1
[System.Runtime.InteropServices.Marshal]::StructureToPtr($df, $p, $false)
[System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, [IntPtr]::Add($p, $df.pFiles), $bytes.Length)
[DropSim]::GlobalUnlock($h) | Out-Null
$ok = [DropSim]::PostMessage($argHwnd, 0x233, $h, [IntPtr]::Zero)
Write-Output "posted=$ok hwnd=$argHwnd path=$path"

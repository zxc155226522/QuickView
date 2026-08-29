$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class DropSim2 {
  [StructLayout(LayoutKind.Sequential)]
  public struct DROPFILES { public int pFiles; public POINT pt; public int fNC; public int fWide; }
  [StructLayout(LayoutKind.Sequential)]
  public struct POINT { public int X; public int Y; }
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalAlloc(int flags, UIntPtr bytes);
  [DllImport("kernel32.dll")] public static extern IntPtr GlobalLock(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool GlobalUnlock(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string cls, string title);
}
'@

function Send-Drop([IntPtr]$hwnd, [string]$path) {
  $bytes = [System.Text.Encoding]::Unicode.GetBytes($path + [char]0 + [char]0)
  $structSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][DropSim2+DROPFILES])
  $h = [DropSim2]::GlobalAlloc(0x0042, [UIntPtr][uint64]($structSize + $bytes.Length))
  $p = [DropSim2]::GlobalLock($h)
  $df = New-Object DropSim2+DROPFILES
  $df.pFiles = $structSize
  $df.fWide = 1
  [System.Runtime.InteropServices.Marshal]::StructureToPtr($df, $p, $false)
  [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, [IntPtr]::Add($p, $structSize), $bytes.Length)
  [DropSim2]::GlobalUnlock($h) | Out-Null
  [DropSim2]::PostMessage($hwnd, 0x233, $h, [IntPtr]::Zero) | Out-Null
}

$pdf = 'E:\项目\看图软件\test_multipage.pdf'
$png = 'E:\项目\看图软件\QuickView_favicon_large.png'

for ($i = 0; $i -lt 15; $i++) {
  $proc = Get-Process QuickView -ErrorAction SilentlyContinue
  if (-not $proc) { Write-Output "iter=$i CRASHED (process gone)"; break }
  $hwnd = [DropSim2]::FindWindowW($null, "$([char]0)")
  # find main window by process id
  $hwnd = [IntPtr]($proc.MainWindowHandle)
  $target = if ($i % 2 -eq 0) { $pdf } else { $png }
  Send-Drop $hwnd $target
  Write-Output "iter=$i dropped=$([System.IO.Path]::GetFileName($target)) hwnd=$hwnd"
  Start-Sleep -Milliseconds 1200
}
$proc = Get-Process QuickView -ErrorAction SilentlyContinue
if ($proc) { Write-Output "FINAL ALIVE pid=$($proc.Id)" } else { Write-Output 'FINAL CRASHED' }

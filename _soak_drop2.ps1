$ErrorActionPreference = 'Stop'
. { } # noop
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class DropSim3 {
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

function Send-Drop([IntPtr]$hwnd, [string]$path) {
  $bytes = [System.Text.Encoding]::Unicode.GetBytes($path + [char]0 + [char]0)
  $structSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][DropSim3+DROPFILES])
  $h = [DropSim3]::GlobalAlloc(0x0042, [UIntPtr][uint64]($structSize + $bytes.Length))
  $p = [DropSim3]::GlobalLock($h)
  $df = New-Object DropSim3+DROPFILES
  $df.pFiles = $structSize
  $df.fWide = 1
  [System.Runtime.InteropServices.Marshal]::StructureToPtr($df, $p, $false)
  [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, [IntPtr]::Add($p, $structSize), $bytes.Length)
  [DropSim3]::GlobalUnlock($h) | Out-Null
  [DropSim3]::PostMessage($hwnd, 0x233, $h, [IntPtr]::Zero) | Out-Null
}

# open first image of the test folder (many images -> bottom strip shows)
$pdf = 'E:\项目\看图软件\test_multipage.pdf'
$img = 'E:\项目\看图软件\_droptest\img2.png'

for ($i = 0; $i -lt 12; $i++) {
  $proc = Get-Process QuickView -ErrorAction SilentlyContinue
  if (-not $proc) { Write-Output "iter=$i CRASHED (process gone)"; break }
  $hwnd = [IntPtr]($proc.MainWindowHandle)
  $target = if ($i % 2 -eq 0) { $img } else { $pdf }
  Send-Drop $hwnd $target
  Write-Output "iter=$i dropped=$([System.IO.Path]::GetFileName($target))"
  Start-Sleep -Milliseconds 900
}
$proc = Get-Process QuickView -ErrorAction SilentlyContinue
if ($proc) { Write-Output "FINAL ALIVE pid=$($proc.Id)" } else { Write-Output 'FINAL CRASHED' }

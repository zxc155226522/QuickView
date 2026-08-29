param([string]$h, [string]$d, [string]$xx, [string]$yy)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class WheelSim {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
}
'@
$hwnd = [IntPtr][long]$h
$delta = [int]$d
$x = [int]$xx
$y = [int]$yy
$wParam = [IntPtr][long]((($delta -band 0xFFFF) -shl 16) -band 0xFFFFFFFF)
$lParam = [IntPtr][long]((($y -shl 16) -bor ($x -band 0xFFFF)) -band 0xFFFFFFFF)
$ok = [WheelSim]::PostMessage($hwnd, 0x20A, $wParam, $lParam)
Write-Output "posted=$ok delta=$delta at=($x,$y)"

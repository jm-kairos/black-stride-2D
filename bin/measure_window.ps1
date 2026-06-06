param([string]$Proc = "sandbox")

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
public static class WinGeo {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
}
"@

$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

$wr = New-Object RECT
[WinGeo]::GetWindowRect($h, [ref]$wr) | Out-Null
$cr = New-Object RECT
[WinGeo]::GetClientRect($h, [ref]$cr) | Out-Null
$o = New-Object POINT
$o.X = 0; $o.Y = 0
[WinGeo]::ClientToScreen($h, [ref]$o) | Out-Null

Write-Output ("WINRECT " + $wr.Left + "," + $wr.Top + "," + $wr.Right + "," + $wr.Bottom)
Write-Output ("CLIENT " + $cr.Right + "x" + $cr.Bottom)
Write-Output ("CLIENT_ORIGIN_SCREEN " + $o.X + "," + $o.Y)
Write-Output ("TITLE " + $p.MainWindowTitle)

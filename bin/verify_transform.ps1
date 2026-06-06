Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace Drv {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static class WinU {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, IntPtr extra);
  }
}
"@

$VK_W=0x57; $VK_D=0x44
$KEYUP=0x0002
$WHEEL=0x0800

$proc = Get-Process sandbox | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if (-not $proc) { Write-Output "NO_WINDOW"; exit 1 }
$h = $proc.MainWindowHandle

function Focus-Game {
  [Drv.WinU]::ShowWindow($h,9) | Out-Null
  [Drv.WinU]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 400
}
function Center-Cursor {
  $r = New-Object Drv.RECT
  [Drv.WinU]::GetWindowRect($h,[ref]$r) | Out-Null
  $cx = [int](($r.Left + $r.Right)/2); $cy = [int](($r.Top + $r.Bottom)/2)
  [Drv.WinU]::SetCursorPos($cx,$cy) | Out-Null
}
function Hold-Key([byte]$vk,[int]$ms) {
  [Drv.WinU]::keybd_event($vk,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds $ms
  [Drv.WinU]::keybd_event($vk,0,$KEYUP,[IntPtr]::Zero)
}
function Wheel([int]$notches) {
  # one notch = 120; negative zooms out, positive zooms in.
  for ($i=0; $i -lt [Math]::Abs($notches); $i++) {
    $delta = if ($notches -lt 0) { -120 } else { 120 }
    [Drv.WinU]::mouse_event($WHEEL,0,0,$delta,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 70
  }
}
function Shot([string]$name) {
  Start-Sleep -Milliseconds 250
  $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
  $bmp = New-Object System.Drawing.Bitmap($b.Width,$b.Height)
  ([System.Drawing.Graphics]::FromImage($bmp)).CopyFromScreen($b.X,$b.Y,0,0,$bmp.Size)
  $path = "C:\dev\blackstride\bin\$name.png"
  $bmp.Save($path)
  $bmp.Dispose()
  Write-Output "saved $name"
}

Focus-Game
Center-Cursor

# Phase A: initial local mode, crew at ship center.
Shot "proto_A_local_start"

# Walk the crew toward the nose (+Y local) so it is far from the rotation origin.
Focus-Game
Hold-Key $VK_W 900
Shot "proto_B_local_walked"

# Zoom out to global mode.
Focus-Game; Center-Cursor
Wheel -8
Start-Sleep -Milliseconds 300
# Turn the ship ~110 deg with D, then let the spin auto-stabilize.
Focus-Game
Hold-Key $VK_D 1200
Start-Sleep -Milliseconds 500
Shot "proto_C_global_turned"

# Zoom back in to local mode.
Focus-Game; Center-Cursor
Wheel 9
Start-Sleep -Milliseconds 400
Shot "proto_D_local_back"

Write-Output "DONE"

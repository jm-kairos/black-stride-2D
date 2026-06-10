param([string]$Proc = "sandbox", [string]$OutDir = "C:\dev\blackstride\bin\shots_avoid")

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Cap {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static class W {
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int d, IntPtr e);
    public static void Grab(IntPtr h, string path) {
      RECT r; GetWindowRect(h, out r);
      int w = r.Right - r.Left, hh = r.Bottom - r.Top;
      if (w <= 0 || hh <= 0) return;
      using (Bitmap bmp = new Bitmap(w, hh, PixelFormat.Format32bppArgb)) {
        using (Graphics g = Graphics.FromImage(bmp)) {
          IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 0x2); g.ReleaseHdc(hdc);
        }
        bmp.Save(path);
      }
    }
  }
}
"@

# Per-monitor DPI-aware (V2 = -4) BEFORE any window query, so client rect is true device px.
[void][Cap.W]::SetThreadDpiAwarenessContext([IntPtr](-4))

$LDOWN=0x0002; $LUP=0x0004; $RDOWN=0x0008; $RUP=0x0010
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

# Live client-origin + framebuffer scale (the verify_jobs.ps1 pattern — robust to DPI/window pos).
$cr = New-Object Cap.RECT; [Cap.W]::GetClientRect($h, [ref]$cr) | Out-Null
$origin = New-Object System.Drawing.Point; $origin.X = 0; $origin.Y = 0
[Cap.W]::ClientToScreen($h, [ref]$origin) | Out-Null
$FB_W = 1280.0; $FB_H = 720.0
$sx = $cr.Right / $FB_W; $sy = $cr.Bottom / $FB_H
Write-Output ("GEOM origin=" + $origin.X + "," + $origin.Y + " client=" + $cr.Right + "x" + $cr.Bottom + " scale=" + $sx + "," + $sy)

function FbToScreen([float]$fx, [float]$fy) { return ,@([int]($origin.X + $fx * $sx), [int]($origin.Y + $fy * $sy)) }
function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 400 }
function Shot([string]$n){ [Cap.W]::Grab($h, (Join-Path $OutDir $n)); Write-Output ("SHOT " + $n) }
function MoveTo([float]$fx,[float]$fy){ $s=FbToScreen $fx $fy; [Cap.W]::SetCursorPos($s[0],$s[1])|Out-Null; Start-Sleep -Milliseconds 120 }
function LeftClick([float]$fx,[float]$fy){ MoveTo $fx $fy; [Cap.W]::mouse_event($LDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90; [Cap.W]::mouse_event($LUP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90 }
function RightClick([float]$fx,[float]$fy){ MoveTo $fx $fy; [Cap.W]::mouse_event($RDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90; [Cap.W]::mouse_event($RUP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90 }

# Framebuffer mapping (verified by verify_jobs.ps1): fb = (640+(col-6)*44.8, 360+(row-8)*44.8) at zoom 1.40.
# crew[0] @ tile(6,8) -> (640.0, 360.0)   crew[1] @ tile(7,7) -> (684.8, 315.2)
$C0_X=640.0; $C0_Y=360.0
$C1_X=684.8; $C1_Y=315.2
# X-crossing: crew[0] up-right to (7,4) -> (684.8,180.8); crew[1] down-left to (6,10) -> (640.0,449.6).
# They start diagonally adjacent (crew0 lower-left, crew1 upper-right) and SWAP vertical halves,
# so their paths must cross in the row 6-8 band — the canonical avoidance conflict.
$D0_X=684.8; $D0_Y=180.8
$D1_X=640.0; $D1_Y=449.6

Focus
Shot "00_initial.png"

# Order crew[0] FIRST (clean selection: crew[1] is 63px away, outside the ~31px pick radius).
LeftClick $C0_X $C0_Y
Start-Sleep -Milliseconds 150
Shot "01_crew0_selected.png"
RightClick $D0_X $D0_Y
Shot "02_crew0_ordered.png"

# Let crew[0] travel up-clear of crew[1]'s click point BEFORE selecting crew[1] (avoids double-select).
Start-Sleep -Milliseconds 500
Shot "03_crew0_moving.png"

LeftClick $C1_X $C1_Y
Start-Sleep -Milliseconds 120
Shot "04_crew1_selected.png"
RightClick $D1_X $D1_Y
Shot "05_both_moving.png"

# Burst-capture across the crossing window (both steering opposite diagonals through the corridor).
Start-Sleep -Milliseconds 180; Shot "06_cross.png"
Start-Sleep -Milliseconds 160; Shot "07_cross.png"
Start-Sleep -Milliseconds 160; Shot "08_cross.png"
Start-Sleep -Milliseconds 160; Shot "09_cross.png"
Start-Sleep -Milliseconds 160; Shot "10_cross.png"
Start-Sleep -Milliseconds 160; Shot "11_cross.png"
Start-Sleep -Milliseconds 200; Shot "12_cross.png"

# Settle at goals.
Start-Sleep -Milliseconds 1400; Shot "13_settled.png"

Write-Output "DONE c0=(6,8)->(7,4) c1=(7,7)->(6,10)"

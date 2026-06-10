# Black Stride — EXCLUSIVE-SELECTION verification harness.
# Proves Fix #1: a left-click within pick-radius of TWO overlapping crew selects only the
# NEAREST one (not both). Steps:
#   1. select crew[1] (upper-right) and order it one tile LEFT so it parks directly ABOVE
#      crew[0] -> the two become ORTHOGONALLY adjacent (pick disks now overlap ~12 units).
#   2. left-click the overlap zone, nearer crew[0] but still within crew[1]'s pick radius.
#   3. capture: EXACTLY ONE yellow ring must appear (on crew[0]); crew[1] must NOT be ringed.
# Coordinates come from the validated harness model (crew[0]=FB(640,360); 1 tile=44.8px @ zoom 1.40),
# NOT from screenshot centroids (too noisy at sub-tile precision).
param(
  [string]$Proc   = "sandbox",
  [string]$OutDir = "C:\dev\blackstride\bin\shots"
)

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Cap {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
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

[void][Cap.W]::SetThreadDpiAwarenessContext([IntPtr](-4))
$LDOWN=0x0002; $LUP=0x0004; $RDOWN=0x0008; $RUP=0x0010

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p = Get-Process $Proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

$cr = New-Object Cap.RECT; [Cap.W]::GetClientRect($h, [ref]$cr) | Out-Null
$origin = New-Object System.Drawing.Point; $origin.X = 0; $origin.Y = 0
[Cap.W]::ClientToScreen($h, [ref]$origin) | Out-Null
$clientW = $cr.Right; $clientH = $cr.Bottom
$FB_W = 1280.0; $FB_H = 720.0
$sx = $clientW / $FB_W; $sy = $clientH / $FB_H
Write-Output ("GEOM origin=" + $origin.X + "," + $origin.Y + " client=" + $clientW + "x" + $clientH + " scale=" + $sx + "," + $sy)

function FbToScreen([float]$fx, [float]$fy) {
  return ,@([int]($origin.X + $fx * $sx), [int]($origin.Y + $fy * $sy))
}
function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350 }
function Shot([string]$name){ [Cap.W]::Grab($h, (Join-Path $OutDir $name)); Write-Output ("SHOT " + $name) }
function MoveTo([float]$fx, [float]$fy) {
  $s = FbToScreen $fx $fy
  [Cap.W]::SetCursorPos($s[0], $s[1]) | Out-Null
  Start-Sleep -Milliseconds 120
}
function LeftClick([float]$fx, [float]$fy) {
  MoveTo $fx $fy
  [Cap.W]::mouse_event($LDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 110
  [Cap.W]::mouse_event($LUP,0,0,0,[IntPtr]::Zero);   Start-Sleep -Milliseconds 110
}
function RightClick([float]$fx, [float]$fy) {
  MoveTo $fx $fy
  [Cap.W]::mouse_event($RDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 120
  [Cap.W]::mouse_event($RUP,0,0,0,[IntPtr]::Zero);   Start-Sleep -Milliseconds 90
}

# --- Validated coordinate model (from verify_jobs.ps1) ---
$TILE  = 44.8                      # screen px per tile at zoom 1.40
$C0_X  = 640.0; $C0_Y = 360.0      # crew[0] (ship center)
$C1_X  = $C0_X + $TILE; $C1_Y = $C0_Y - $TILE   # crew[1] start: 1 tile right, 1 up
$ABOVE_X = $C0_X;        $ABOVE_Y = $C0_Y - $TILE  # tile directly above crew[0] (1 tile LEFT of crew[1])

# Overlap-zone click: between the two stacked crew, nearer crew[0]. With crew[1] parked at
# (C0_X, C0_Y-TILE), this point is ~20px from crew[0] and ~25px from crew[1] — inside BOTH
# pick radii (30.8px), so the OLD code would ring BOTH; nearest-wins rings only crew[0].
$MID_X = $C0_X; $MID_Y = $C0_Y - ($TILE * 0.45)

# ============================ PHASES ============================
Focus
Shot "e0_baseline.png"                 # two crew, no rings

LeftClick $C1_X $C1_Y                   # select crew[1] (upper-right)
Start-Sleep -Milliseconds 250
Shot "e1_crew1_selected.png"           # ring on crew[1]; panel appears

RightClick $ABOVE_X $ABOVE_Y           # order crew[1] one tile LEFT -> parks directly above crew[0]
Start-Sleep -Milliseconds 2200         # let it walk + settle (short 1-tile hop)
Shot "e2_crew1_parked_above.png"       # crew[1] now ORTHOGONALLY adjacent above crew[0]

LeftClick $MID_X $MID_Y                 # click overlap zone, nearer crew[0]
Start-Sleep -Milliseconds 300
Shot "e3_overlap_click.png"            # ASSERT: exactly ONE ring (on crew[0]); crew[1] NOT ringed

Write-Output "DONE"

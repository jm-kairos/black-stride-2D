param([string]$Proc = "sandbox", [string]$OutDir = "C:\dev\blackstride\bin\shots_panel")

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

[void][Cap.W]::SetThreadDpiAwarenessContext([IntPtr](-4))
$LDOWN=0x0002; $LUP=0x0004
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

$cr = New-Object Cap.RECT; [Cap.W]::GetClientRect($h, [ref]$cr) | Out-Null
$origin = New-Object System.Drawing.Point; $origin.X = 0; $origin.Y = 0
[Cap.W]::ClientToScreen($h, [ref]$origin) | Out-Null
$FB_W = 1280.0; $FB_H = 720.0
$sx = $cr.Right / $FB_W; $sy = $cr.Bottom / $FB_H
Write-Output ("GEOM origin=" + $origin.X + "," + $origin.Y + " client=" + $cr.Right + "x" + $cr.Bottom)

function FbToScreen([float]$fx,[float]$fy){ return ,@([int]($origin.X + $fx*$sx), [int]($origin.Y + $fy*$sy)) }
function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 400 }
function Shot([string]$n){ [Cap.W]::Grab($h, (Join-Path $OutDir $n)); Write-Output ("SHOT " + $n) }
function MoveTo([float]$fx,[float]$fy){ $s=FbToScreen $fx $fy; [Cap.W]::SetCursorPos($s[0],$s[1])|Out-Null; Start-Sleep -Milliseconds 120 }
function LeftClick([float]$fx,[float]$fy){ MoveTo $fx $fy; [Cap.W]::mouse_event($LDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90; [Cap.W]::mouse_event($LUP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90 }

# crew[0] @ tile(6,8) -> (640.0,360.0)   crew[1] @ tile(7,7) -> (684.8,315.2)
$C0_X=640.0; $C0_Y=360.0
$C1_X=684.8; $C1_Y=315.2
# Empty-floor click to deselect (tile (6,9) directly below crew0, no crew there).
$EMPTY_X=640.0; $EMPTY_Y=404.8

Focus
Shot "p0_initial.png"          # nothing selected -> NO panel (negative control)

# --- Select crew[0] -> panel MUST appear (this already worked pre-fix) ---
LeftClick $C0_X $C0_Y
Start-Sleep -Milliseconds 250
Shot "p1_crew0_selected.png"

# --- Deselect (click empty floor) -> panel MUST disappear ---
LeftClick $EMPTY_X $EMPTY_Y
Start-Sleep -Milliseconds 250
Shot "p2_deselected.png"

# --- Select crew[1] -> panel MUST appear (THIS is the bug being fixed) ---
LeftClick $C1_X $C1_Y
Start-Sleep -Milliseconds 250
Shot "p3_crew1_selected.png"

Write-Output "DONE"

# Verify the local<->global camera does NOT spin a full revolution on zoom-in after the ship
# has been turned past 360 deg in global mode (the camera-rotation wrap bug).
#
# Repro matching the report: local -> global -> rotate the ship >=360 deg -> back to local.
# The camera.rotation is lerped from 0 (global) toward the ship heading across the zoom cross-fade.
# With the unbounded raw angle (~370 deg) the view unwinds a whole revolution mid-zoom (the BUG);
# with wrap_angle it takes the shortest arc (~10 deg) and the interior stays upright (the FIX).
#
# Witness = a MID-CROSS-FADE frame (a settled frame looks upright either way -- the static-shot
# trap). So we fire the zoom-in then BURST-CAPTURE with NO sleeps to catch the ~0.5s fade
# (ROOF_FADE_SPEED=8 => ~125ms tau). Pass (fix): every burst frame is near-upright. Fail (bug):
# mid-burst frames show the interior rotated up to ~180 deg (a whirl across the blend).
#
# Usage: powershell -ExecutionPolicy Bypass -File verify_camera_wrap.ps1 -Tag fix
param([string]$Tag = "run")

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
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte s, uint f, IntPtr e);
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

$VK_W=0x57; $VK_D=0x44; $KEYUP=0x0002; $WHEEL=0x0800
$dir = "C:\dev\blackstride\bin\shots"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$p = Get-Process sandbox -ErrorAction SilentlyContinue | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle
function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350 }
function CenterCursor { $r=New-Object Cap.RECT; [Cap.W]::GetWindowRect($h,[ref]$r)|Out-Null; [Cap.W]::SetCursorPos([int](($r.Left+$r.Right)/2),[int](($r.Top+$r.Bottom)/2))|Out-Null }
function KeyDown([byte]$vk){ [Cap.W]::keybd_event($vk,0,0,[IntPtr]::Zero) }
function KeyUp([byte]$vk){ [Cap.W]::keybd_event($vk,0,$KEYUP,[IntPtr]::Zero) }
function Wheel([int]$n){ $d= if($n -lt 0){-120}else{120}; for($i=0;$i -lt [Math]::Abs($n);$i++){ [Cap.W]::mouse_event($WHEEL,0,0,$d,[IntPtr]::Zero); Start-Sleep -Milliseconds 55 } }
function Shot([string]$name){ [Cap.W]::Grab($h, (Join-Path $dir $name)) }

Focus
# 1) Walk crew to the nose (+Y local) so it sits far from the rotation origin -> any view spin
#    sweeps it through a big screen arc, making the bug unmistakable.
CenterCursor; KeyDown $VK_W; Start-Sleep -Milliseconds 900; KeyUp $VK_W
Start-Sleep -Milliseconds 200
Shot "cam_${Tag}_00_local_start.png"

# 2) Zoom out to global.
CenterCursor; Wheel -8; Start-Sleep -Milliseconds 300
Shot "cam_${Tag}_01_global.png"

# 3) Turn the ship PAST 360 deg (hold D ~3.7s @ 1.8 rad/s after a 0.3s ramp ~= 370 deg), then
#    RELEASE and let the angular auto-stabilizer freeze the heading. angle is now ~370 deg and
#    constant, so the zoom fade is a clean test of the rotation PATH only.
Focus; KeyDown $VK_D; Start-Sleep -Milliseconds 3700; KeyUp $VK_D
Start-Sleep -Milliseconds 700
Shot "cam_${Tag}_02_global_turned.png"

# 4) Zoom straight back into local and BURST-CAPTURE the cross-fade. Fire many wheel-up notches
#    with no gap so zoom crosses ZOOM_TO_LOCAL in ~1 frame (mode flips, fade starts now), then
#    grab frames in a tight loop with NO sleeps to catch roof_alpha sweeping 1 -> 0.
CenterCursor
for($i=0;$i -lt 16;$i++){ [Cap.W]::mouse_event($WHEEL,0,0,120,[IntPtr]::Zero) }
for($f=0;$f -lt 18;$f++){ Shot ("cam_${Tag}_burst_{0:D2}.png" -f $f) }

# 5) Settled local reference (must be upright for BOTH bug and fix -- the non-discriminating shot).
Start-Sleep -Milliseconds 600
Shot "cam_${Tag}_03_local_settled.png"
Write-Output "DONE $Tag"

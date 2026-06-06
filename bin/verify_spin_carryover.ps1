# Verify the ship's SPIN settles after switching global->local (the carry-over bug).
# Repro matching the report: turn the ship in global mode and, while STILL turning, zoom
# straight into local. The angular auto-stabilizer must then bleed the carried spin to zero.
#
# Witness: in local mode the camera cancels the ship heading, so the interior reads upright
# while the fixed world debug grid behind it appears tilted by `angle`. If the ship keeps
# spinning (the bug) the grid tilt keeps changing across no-input frames; if the fix works the
# grid tilt is IDENTICAL at t1 and t2 (spin has settled). PrintWindow capture (works headless).
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
$VK_D=0x44; $KEYUP=0x0002; $WHEEL=0x0800
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
# 1) Zoom out to global.
CenterCursor; Wheel -8; Start-Sleep -Milliseconds 300
Shot "s01_global.png"
# 2) Start turning (hold D) and capture mid-turn — roof silhouette should be visibly rotated.
Focus; KeyDown $VK_D; Start-Sleep -Milliseconds 700
Shot "s02_global_turning.png"
# 3) STILL HOLDING D, zoom straight into local (mirrors "turn then immediately transition").
#    Holding the turn through the zoom guarantees the carried spin survives into local instead
#    of being bled off by the global-mode stabilizer during the wheel.
CenterCursor; Wheel 9
KeyUp $VK_D                      # release only once we're in local
Start-Sleep -Milliseconds 250
Shot "s03_local_t0.png"
# 4) WAIT with absolutely NO input, capturing two more frames. The carried spin must settle:
#    s04 and s05 must show the SAME world-grid tilt (ship stopped rotating). If they differ the
#    ship is still spinning indefinitely (the bug).
Start-Sleep -Milliseconds 1500
Shot "s04_local_t1.png"
Start-Sleep -Milliseconds 1500
Shot "s05_local_t2.png"
Write-Output "DONE"

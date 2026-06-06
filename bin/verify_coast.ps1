# Verify the ship keeps coasting after switching to local mode (momentum must persist).
# Repro: enter global -> thrust to build velocity -> zoom back to local -> WAIT with NO input,
# capturing two frames across the wait. If origin advances (world grid slides under the
# screen-fixed interior) the simulation is still running. PrintWindow capture (works headless).
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
$VK_W=0x57; $KEYUP=0x0002; $WHEEL=0x0800
$dir = "C:\dev\blackstride\bin\shots"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$p = Get-Process sandbox -ErrorAction SilentlyContinue | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle
function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350 }
function CenterCursor { $r=New-Object Cap.RECT; [Cap.W]::GetWindowRect($h,[ref]$r)|Out-Null; [Cap.W]::SetCursorPos([int](($r.Left+$r.Right)/2),[int](($r.Top+$r.Bottom)/2))|Out-Null }
function Hold([byte]$vk,[int]$ms){ [Cap.W]::keybd_event($vk,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds $ms; [Cap.W]::keybd_event($vk,0,$KEYUP,[IntPtr]::Zero) }
function Wheel([int]$n){ $d= if($n -lt 0){-120}else{120}; for($i=0;$i -lt [Math]::Abs($n);$i++){ [Cap.W]::mouse_event($WHEEL,0,0,$d,[IntPtr]::Zero); Start-Sleep -Milliseconds 55 } }
function Shot([string]$name){ [Cap.W]::Grab($h, (Join-Path $dir $name)) }

Focus
# 1) Zoom out to global.
CenterCursor; Wheel -8; Start-Sleep -Milliseconds 300
Shot "c01_global.png"
# 2) Thrust forward to build momentum, then RELEASE (coast).
Focus; Hold $VK_W 1100
Shot "c02_global_moving.png"
# 3) Zoom back to local while coasting.
Focus; CenterCursor; Wheel 9; Start-Sleep -Milliseconds 700
Shot "c03_local_t0.png"
# 4) WAIT with absolutely NO input, then capture again. If the ship still coasts, the world
#    grid will have visibly slid relative to the screen-fixed interior between t0 and t1.
Start-Sleep -Milliseconds 1400
Shot "c04_local_t1.png"
Start-Sleep -Milliseconds 1400
Shot "c05_local_t2.png"
Write-Output "DONE"

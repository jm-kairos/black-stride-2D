param([string]$OutDir = "C:\dev\blackstride\bin\shots")

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Cap2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static class W {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte s, uint f, IntPtr e);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int d, IntPtr e);
    public static void Grab(IntPtr h, string path) {
      RECT r; GetClientRect(h, out r);
      int w = r.Right - r.Left, hh = r.Bottom - r.Top;
      if (w <= 0 || hh <= 0) return;
      using (Bitmap bmp = new Bitmap(w, hh, PixelFormat.Format32bppArgb)) {
        using (Graphics g = Graphics.FromImage(bmp)) { IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 0x2); g.ReleaseHdc(hdc); }
        bmp.Save(path);
      }
    }
  }
}
"@

$VK_D=0x44; $KEYUP=0x0002; $LEFTDOWN=0x0002; $LEFTUP=0x0004; $RIGHTDOWN=0x0008; $RIGHTUP=0x0010; $WHEEL=0x0800
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p = Get-Process sandbox -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

function Focus { [Cap2.W]::ShowWindow($h,9)|Out-Null; [Cap2.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 300 }
function Shot([string]$n){ [Cap2.W]::Grab($h, (Join-Path $OutDir $n)) }
function LeftClick([int]$x,[int]$y){ [Cap2.W]::SetCursorPos($x,$y)|Out-Null; Start-Sleep -Milliseconds 90; [Cap2.W]::mouse_event($LEFTDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 45; [Cap2.W]::mouse_event($LEFTUP,0,0,0,[IntPtr]::Zero) }
function RightClick([int]$x,[int]$y){ [Cap2.W]::SetCursorPos($x,$y)|Out-Null; Start-Sleep -Milliseconds 90; [Cap2.W]::mouse_event($RIGHTDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 45; [Cap2.W]::mouse_event($RIGHTUP,0,0,0,[IntPtr]::Zero) }
function Wheel([int]$n){ $d=if($n -lt 0){-120}else{120}; for($i=0;$i -lt [Math]::Abs($n);$i++){ [Cap2.W]::mouse_event($WHEEL,0,0,$d,[IntPtr]::Zero); Start-Sleep -Milliseconds 40 } }
function HoldD([int]$ms){ [Cap2.W]::keybd_event($VK_D,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds $ms; [Cap2.W]::keybd_event($VK_D,0,$KEYUP,[IntPtr]::Zero) }

Focus
[Cap2.W]::SetCursorPos(592,368)|Out-Null

# 1) Select crew (center) and order a LONG move to the bottom room tile (6,14) = screen (592,583),
#    routing down through the bottom door (5,11): a multi-tile dogleg.
LeftClick 592 368
Start-Sleep -Milliseconds 100
RightClick 592 583
Start-Sleep -Milliseconds 70
Shot "g01_ordered.png"            # LOCAL: crew at center, long green path down to bottom room

# 2) Zoom OUT to global (5 notches -> zoom ~0.79 < 0.80). Cursor stays over window for wheel routing.
[Cap2.W]::SetCursorPos(592,368)|Out-Null
Wheel -5
Start-Sleep -Milliseconds 120
Shot "g02_global.png"             # GLOBAL: roof silhouette (interior+crew hidden under roof)

# 3) Turn the whole hull with D while the crew keeps walking underneath the roof.
Focus
HoldD 320
Start-Sleep -Milliseconds 60
Shot "g03_turned.png"             # GLOBAL: roof silhouette visibly ROTATED

# 4) Zoom BACK to local (6 notches -> ~1.57) and burst-capture: the crew should have advanced far
#    along its route (it walked during the entire global excursion). Interior re-uprights.
[Cap2.W]::SetCursorPos(592,368)|Out-Null
Wheel 6
Shot "g04_back.png"
Start-Sleep -Milliseconds 200
Shot "g05.png"
Start-Sleep -Milliseconds 250
Shot "g06.png"
Start-Sleep -Milliseconds 350
Shot "g07.png"
Start-Sleep -Milliseconds 450
Shot "g08_settled.png"

Write-Output "DONE-GLOBAL-TEST"

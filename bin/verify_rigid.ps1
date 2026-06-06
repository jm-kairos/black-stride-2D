# Black Stride rigid-body transform verification.
# Drives synthetic input and captures via PrintWindow(PW_RENDERFULLCONTENT) so it works
# even when screen-DC capture is blocked. Phases: local start -> walk crew to nose ->
# zoom out -> turn ship -> BURST capture during zoom-in transition -> settled local.
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
          IntPtr hdc = g.GetHdc();
          PrintWindow(h, hdc, 0x2); // PW_RENDERFULLCONTENT
          g.ReleaseHdc(hdc);
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

$p = Get-Process sandbox | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350 }
function CenterCursor {
  $r = New-Object Cap.RECT; [Cap.W]::GetWindowRect($h,[ref]$r)|Out-Null
  [Cap.W]::SetCursorPos([int](($r.Left+$r.Right)/2),[int](($r.Top+$r.Bottom)/2))|Out-Null
}
function Hold([byte]$vk,[int]$ms){ [Cap.W]::keybd_event($vk,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds $ms; [Cap.W]::keybd_event($vk,0,$KEYUP,[IntPtr]::Zero) }
function Wheel([int]$n){ $d = if($n -lt 0){-120}else{120}; for($i=0;$i -lt [Math]::Abs($n);$i++){ [Cap.W]::mouse_event($WHEEL,0,0,$d,[IntPtr]::Zero); Start-Sleep -Milliseconds 55 } }
function Shot([string]$name){ [Cap.W]::Grab($h, (Join-Path $dir $name)) }

Focus
Shot "01_local_start.png"

# Walk crew toward the nose (+Y local) so it's far from the ship origin.
Focus; Hold $VK_W 950
Shot "02_walked.png"

# Zoom out -> global mode.
Focus; CenterCursor; Wheel -8
Start-Sleep -Milliseconds 350
Shot "03_global.png"

# Turn the ship ~110 deg (D ramps angular velocity; release auto-stabilizes).
Focus; Hold $VK_D 1150
Start-Sleep -Milliseconds 450
Shot "04_global_turned.png"

# Zoom back in, then BURST-capture the cross-fade transition with no sleeps.
Focus; CenterCursor; Wheel 9
for ($i=0; $i -lt 10; $i++) { Shot ("05_blend_{0:D2}.png" -f $i) }

# Settled local view.
Start-Sleep -Milliseconds 900
Shot "06_local_back.png"

Write-Output "DONE"

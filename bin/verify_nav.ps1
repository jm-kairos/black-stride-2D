param([string]$Proc = "sandbox", [string]$OutDir = "C:\dev\blackstride\bin\shots")

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Cap {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static class W {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int d, IntPtr e);
    public static void Grab(IntPtr h, string path) {
      RECT r; GetClientRect(h, out r);
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

$LEFTDOWN=0x0002; $LEFTUP=0x0004; $RIGHTDOWN=0x0008; $RIGHTUP=0x0010
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 400 }
function Shot([string]$n){ [Cap.W]::Grab($h, (Join-Path $OutDir $n)) }
function LeftClick([int]$x,[int]$y){ [Cap.W]::SetCursorPos($x,$y)|Out-Null; Start-Sleep -Milliseconds 120; [Cap.W]::mouse_event($LEFTDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 60; [Cap.W]::mouse_event($LEFTUP,0,0,0,[IntPtr]::Zero) }
function RightClick([int]$x,[int]$y){ [Cap.W]::SetCursorPos($x,$y)|Out-Null; Start-Sleep -Milliseconds 120; [Cap.W]::mouse_event($RIGHTDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 60; [Cap.W]::mouse_event($RIGHTUP,0,0,0,[IntPtr]::Zero) }

# Screen-space click targets (physical pixels). Client origin (80,80), 0.8 logical->physical.
# Crew (select): logical (640,360) -> (592,368).  Order tile (2,8): logical (460.8,360) -> (449,368).
$SEL_X=592; $SEL_Y=368
$ORD_X=449; $ORD_Y=368

Focus
Shot "01_initial.png"

# --- Select the crew (left-click on it at center) ---
LeftClick $SEL_X $SEL_Y
Start-Sleep -Milliseconds 200
Shot "02_selected.png"

# --- Order a move to the left room through the col-4 door (right-click) ---
RightClick $ORD_X $ORD_Y
Start-Sleep -Milliseconds 120
Shot "03_ordered.png"           # path line should be visible (crew just started)
Start-Sleep -Milliseconds 350
Shot "04_walking.png"           # crew partway along the dogleg
Start-Sleep -Milliseconds 900
Shot "05_arrived.png"           # crew reached the left room

Write-Output "DONE sel=($SEL_X,$SEL_Y) ord=($ORD_X,$ORD_Y)"

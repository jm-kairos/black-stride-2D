# Black Stride — global-mode HUD probe. Zooms the camera OUT (mouse wheel down) to cross into
# global mode, which fades in the roof silhouette + the screen-anchored bitmap-text helm HUD
# (text_draw / text.cpp). Proves the 8x8 text layer still renders after ui.cpp was retired (Task 9).
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
namespace CapH {
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
[void][CapH.W]::SetThreadDpiAwarenessContext([IntPtr](-4))
$WHEEL=0x0800
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p = Get-Process $Proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle
$cr = New-Object CapH.RECT; [CapH.W]::GetClientRect($h, [ref]$cr) | Out-Null
$origin = New-Object System.Drawing.Point; $origin.X = 0; $origin.Y = 0
[CapH.W]::ClientToScreen($h, [ref]$origin) | Out-Null
$sx = $cr.Right / 1280.0; $sy = $cr.Bottom / 720.0
[CapH.W]::ShowWindow($h,9)|Out-Null; [CapH.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350
# Park the cursor over the ship center so wheel events land on the window.
$cx = [int]($origin.X + 640.0*$sx); $cy = [int]($origin.Y + 360.0*$sy)
[CapH.W]::SetCursorPos($cx,$cy) | Out-Null; Start-Sleep -Milliseconds 150
# Zoom OUT: 8 wheel-down notches (1.40 -> ~0.46, well below the global threshold).
for ($i=0; $i -lt 8; $i++) {
  [CapH.W]::mouse_event($WHEEL,0,0,-120,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
}
Start-Sleep -Milliseconds 900   # let the roof + HUD cross-fade finish
[CapH.W]::Grab($h, (Join-Path $OutDir "h00_global_hud.png")); Write-Output "SHOT h00_global_hud.png"
Write-Output "DONE"

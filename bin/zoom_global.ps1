# Zoom out to global mode via PostMessage(WM_MOUSEWHEEL) — bypasses the foreground lock (per
# blackstride-build-verify skill), then PrintWindow-capture both hulls as roof silhouettes.
$ErrorActionPreference = "Stop"
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Cap {
  public static class W {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l,t,r,b; }
    public static void Wheel(IntPtr h, int delta, int x, int y) {
      uint WM_MOUSEWHEEL = 0x020A;
      IntPtr w = (IntPtr)((delta << 16) & unchecked((int)0xFFFF0000));
      IntPtr l = (IntPtr)((y << 16) | (x & 0xFFFF));
      PostMessage(h, WM_MOUSEWHEEL, w, l);
    }
    public static void Grab(IntPtr h, string path) {
      RECT rc; GetClientRect(h, out rc);
      int w = rc.r - rc.l, hgt = rc.b - rc.t;
      Bitmap bmp = new Bitmap(w, hgt);
      Graphics g = Graphics.FromImage(bmp);
      IntPtr dc = g.GetHdc();
      PrintWindow(h, dc, 2);
      g.ReleaseHdc(dc); g.Dispose();
      bmp.Save(path, ImageFormat.Png); bmp.Dispose();
    }
  }
}
"@
$h = (Get-Process sandbox | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
if (-not $h -or $h -eq [IntPtr]::Zero) { Write-Output "NO_WINDOW"; exit 1 }
$rc = New-Object Cap.W+RECT
[Cap.W]::GetClientRect($h, [ref]$rc) | Out-Null
$cx = [int](($rc.r - $rc.l) / 2); $cy = [int](($rc.b - $rc.t) / 2)
# ~10 wheel-down notches (negative delta => zoom out to global)
for ($i = 0; $i -lt 10; $i++) { [Cap.W]::Wheel($h, -120, $cx, $cy); Start-Sleep -Milliseconds 120 }
Start-Sleep -Milliseconds 1000   # let the roof cross-fade settle
[Cap.W]::Grab($h, "C:/dev/blackstride/bin/enemy_global.png")
Write-Output "OK global"

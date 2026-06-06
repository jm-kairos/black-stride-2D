param([string]$Proc = "sandbox")

# Make THIS process per-monitor-DPI-aware v2 BEFORE any window/cursor calls, so GetClientRect,
# ClientToScreen, SetCursorPos and the PrintWindow grab all speak REAL physical pixels (no
# 0.8x virtualization). Must run before the window APIs touch any HWND.
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
namespace Dpi { public static class A {
  [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
}}
"@
[Dpi.A]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null   # PER_MONITOR_AWARE_V2

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Probe {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static class W {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    // Grab ONLY the client area: size bitmap to client rect, then BitBlt-equivalent via
    // PrintWindow renders whole window; we offset by translating the graphics to client origin.
    public static void GrabClient(IntPtr h, string path) {
      RECT cr; GetClientRect(h, out cr);
      int cw = cr.Right - cr.Left, ch = cr.Bottom - cr.Top;
      RECT wr; GetWindowRect(h, out wr);
      int ww = wr.Right - wr.Left, wh = wr.Bottom - wr.Top;
      if (ww <= 0 || wh <= 0) return;
      using (Bitmap full = new Bitmap(ww, wh, PixelFormat.Format32bppArgb)) {
        using (Graphics g = Graphics.FromImage(full)) {
          IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 0x2); g.ReleaseHdc(hdc);
        }
        // client origin in screen coords, minus window origin = offset of client within window
        POINT o; o.X = 0; o.Y = 0; ClientToScreen(h, ref o);
        int offx = o.X - wr.Left, offy = o.Y - wr.Top;
        using (Bitmap client = new Bitmap(cw, ch, PixelFormat.Format32bppArgb)) {
          using (Graphics g2 = Graphics.FromImage(client)) {
            g2.DrawImage(full, new Rectangle(0,0,cw,ch), new Rectangle(offx,offy,cw,ch), GraphicsUnit.Pixel);
          }
          client.Save(path);
        }
      }
    }
  }
}
"@

$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

$c = New-Object Probe.RECT; [Probe.W]::GetClientRect($h, [ref]$c) | Out-Null
$o = New-Object Probe.POINT; $o.X = 0; $o.Y = 0; [Probe.W]::ClientToScreen($h, [ref]$o) | Out-Null
$dpi = [Probe.W]::GetDpiForWindow($h)
Write-Output ("CLIENT=" + ($c.Right - $c.Left) + "x" + ($c.Bottom - $c.Top))
Write-Output ("CLIENTORIGIN_SCREEN=" + $o.X + "," + $o.Y)
Write-Output ("DPI=" + $dpi)

New-Item -ItemType Directory -Force -Path "C:\dev\blackstride\bin\shots" | Out-Null
[Probe.W]::GrabClient($h, "C:\dev\blackstride\bin\shots\00_baseline_dpiaware.png")
Write-Output "GRABBED 00_baseline_dpiaware.png"

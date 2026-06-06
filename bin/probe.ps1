param([string]$Proc = "sandbox")

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

$p = Get-Process $Proc -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

$c = New-Object Probe.RECT; [Probe.W]::GetClientRect($h, [ref]$c) | Out-Null
$w = New-Object Probe.RECT; [Probe.W]::GetWindowRect($h, [ref]$w) | Out-Null
$o = New-Object Probe.POINT; $o.X = 0; $o.Y = 0; [Probe.W]::ClientToScreen($h, [ref]$o) | Out-Null
$dpi = [Probe.W]::GetDpiForWindow($h)

Write-Output ("TITLE=" + $p.MainWindowTitle)
Write-Output ("CLIENT=" + ($c.Right - $c.Left) + "x" + ($c.Bottom - $c.Top))
Write-Output ("WINDOWRECT=" + $w.Left + "," + $w.Top + "," + $w.Right + "," + $w.Bottom)
Write-Output ("CLIENTORIGIN_SCREEN=" + $o.X + "," + $o.Y)
Write-Output ("DPI=" + $dpi + " SCALE=" + ([math]::Round($dpi / 96.0, 3)))

[Probe.W]::Grab($h, "C:\dev\blackstride\bin\shots\00_baseline.png")
Write-Output "GRABBED 00_baseline.png"

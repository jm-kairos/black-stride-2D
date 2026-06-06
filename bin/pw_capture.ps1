param([string]$Out = "C:\dev\blackstride\bin\pw_probe.png")
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
namespace Cap {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static class PW {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    public static Bitmap Grab(IntPtr h) {
      RECT r; GetWindowRect(h, out r);
      int w = r.Right - r.Left, hh = r.Bottom - r.Top;
      if (w <= 0 || hh <= 0) return null;
      Bitmap bmp = new Bitmap(w, hh, PixelFormat.Format32bppArgb);
      using (Graphics g = Graphics.FromImage(bmp)) {
        IntPtr hdc = g.GetHdc();
        // 0x2 = PW_RENDERFULLCONTENT (captures DWM-composited / GPU content)
        bool ok = PrintWindow(h, hdc, 0x2);
        g.ReleaseHdc(hdc);
        if (!ok) return null;
      }
      return bmp;
    }
  }
}
"@
$p = Get-Process sandbox | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
[Cap.PW]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
[Cap.PW]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500
$bmp = [Cap.PW]::Grab($p.MainWindowHandle)
if ($bmp -eq $null) { Write-Output "GRAB_FAILED"; exit 2 }
$bmp.Save($Out)
$bmp.Dispose()
Write-Output ("OK " + $Out)

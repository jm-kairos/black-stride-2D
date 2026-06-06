# Black Stride — Phase 1 ImGui verification. Captures one frame of sandbox.exe to prove the
# Dear ImGui demo window renders over the scene. DPI-aware (V2 = -4) so the 1280x720 framebuffer
# is captured in true device pixels (no right-edge clipping).
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
namespace Cap {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static class W {
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
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

[void][Cap.W]::SetThreadDpiAwarenessContext([IntPtr](-4))
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p = Get-Process $Proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle
[Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null
Start-Sleep -Milliseconds 600
[Cap.W]::Grab($h, (Join-Path $OutDir "imgui_demo.png"))
Write-Output "SHOT imgui_demo.png"
Write-Output "DONE"

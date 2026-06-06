# Black Stride — Phase 1 ImGui stability under minimize/restore + resize.
# Exercises the highest-risk path: NewFrame (begin_frame) must stay balanced with Render
# (end_frame) across window minimize (is_suspended -> begin_frame skipped) and the NULL-
# swapchain early-out. If the pairing is wrong, the process asserts/exits here. We minimize,
# wait, restore, resize smaller, resize back, then confirm the process is STILL ALIVE and
# captures a final frame.
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
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
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
$SW_MINIMIZE=6; $SW_RESTORE=9

Write-Output "MINIMIZE"
[Cap.W]::ShowWindow($h,$SW_MINIMIZE)|Out-Null
Start-Sleep -Milliseconds 1200          # several frames with is_suspended=TRUE (begin_frame skipped)

Write-Output "RESTORE"
[Cap.W]::ShowWindow($h,$SW_RESTORE)|Out-Null
[Cap.W]::SetForegroundWindow($h)|Out-Null
Start-Sleep -Milliseconds 800

Write-Output "RESIZE_SMALL"
[Cap.W]::MoveWindow($h, 100, 100, 720, 480, $true)|Out-Null
Start-Sleep -Milliseconds 800           # swapchain recreate at new size, ImGui display size update

Write-Output "RESIZE_BACK"
[Cap.W]::MoveWindow($h, 100, 100, 1296, 759, $true)|Out-Null
Start-Sleep -Milliseconds 800

# If we got here the process survived all transitions. Confirm it is still alive + grab a frame.
$alive = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if (-not $alive) { Write-Output "PROCESS_DIED"; exit 2 }
[Cap.W]::SetForegroundWindow($h)|Out-Null
Start-Sleep -Milliseconds 500
[Cap.W]::Grab($h, (Join-Path $OutDir "imgui_after_resize.png"))
Write-Output "SHOT imgui_after_resize.png"
Write-Output "ALIVE_AND_STABLE"
Write-Output "DONE"

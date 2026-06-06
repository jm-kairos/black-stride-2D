# Black Stride — Job Assignment + HUD verification harness.
# Drives sandbox.exe with synthetic OS input to prove the dynamic flow:
#   select crew (left-click) -> HUD appears -> Shift+Right-Click helm -> job assigned,
#   crew walks to helm, progress advances. Captures a frame at each phase via PrintWindow.
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
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
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
          IntPtr hdc = g.GetHdc(); PrintWindow(h, hdc, 0x2); g.ReleaseHdc(hdc);
        }
        bmp.Save(path);
      }
    }
  }
}
"@

# Make THIS thread per-monitor DPI-aware (V2 = -4) BEFORE any window query, so GetWindowRect/
# GetClientRect/PrintWindow all operate in true device pixels (1280x720) rather than the
# virtualized 0.8x size. Without this a DPI-unaware capture clips the window's right edge,
# hiding the top-right HUD panel. Must run before the first window measurement.
[void][Cap.W]::SetThreadDpiAwarenessContext([IntPtr](-4))

# mouse_event flags
$LDOWN=0x0002; $LUP=0x0004; $RDOWN=0x0008; $RUP=0x0010
# virtual keys
$VK_LSHIFT=0xA0; $KEYUP=0x0002

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p = Get-Process $Proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "NO_WINDOW"; exit 1 }
$h = $p.MainWindowHandle

# Client-area origin on screen + the logical->framebuffer scale, measured live.
$cr = New-Object Cap.RECT; [Cap.W]::GetClientRect($h, [ref]$cr) | Out-Null
$origin = New-Object System.Drawing.Point
$origin.X = 0; $origin.Y = 0
[Cap.W]::ClientToScreen($h, [ref]$origin) | Out-Null
$clientW = $cr.Right; $clientH = $cr.Bottom
$FB_W = 1280.0; $FB_H = 720.0
# screen_px = client_origin + fb * (clientW / FB_W)
$sx = $clientW / $FB_W
$sy = $clientH / $FB_H
Write-Output ("GEOM origin=" + $origin.X + "," + $origin.Y + " client=" + $clientW + "x" + $clientH + " scale=" + $sx + "," + $sy)

function FbToScreen([float]$fx, [float]$fy) {
  $X = [int]($origin.X + $fx * $sx)
  $Y = [int]($origin.Y + $fy * $sy)
  return ,@($X, $Y)
}
function Focus { [Cap.W]::ShowWindow($h,9)|Out-Null; [Cap.W]::SetForegroundWindow($h)|Out-Null; Start-Sleep -Milliseconds 350 }
function Shot([string]$name){ [Cap.W]::Grab($h, (Join-Path $OutDir $name)); Write-Output ("SHOT " + $name) }
function MoveTo([float]$fx, [float]$fy) {
  $s = FbToScreen $fx $fy
  [Cap.W]::SetCursorPos($s[0], $s[1]) | Out-Null
  Start-Sleep -Milliseconds 120   # let SDL process the motion event -> input position updates
}
function LeftClick([float]$fx, [float]$fy) {
  MoveTo $fx $fy
  [Cap.W]::mouse_event($LDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 110
  [Cap.W]::mouse_event($LUP,0,0,0,[IntPtr]::Zero);   Start-Sleep -Milliseconds 110
}
function ShiftRightClick([float]$fx, [float]$fy) {
  MoveTo $fx $fy
  [Cap.W]::keybd_event([byte]$VK_LSHIFT,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
  MoveTo $fx $fy   # re-assert position with shift held
  [Cap.W]::mouse_event($RDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 120
  [Cap.W]::mouse_event($RUP,0,0,0,[IntPtr]::Zero);   Start-Sleep -Milliseconds 90
  [Cap.W]::keybd_event([byte]$VK_LSHIFT,0,$KEYUP,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
}

# Targets (framebuffer space)
$CREW_X = 640.0; $CREW_Y = 360.0   # crew renders at screen center
$HELM_X = 640.0; $HELM_Y = 91.0    # 6 tiles above crew at zoom 1.40

# ============================ PHASES ============================
Focus
Shot "j00_start.png"                       # crew unselected -> NO panel

LeftClick $CREW_X $CREW_Y                   # select the crew
Start-Sleep -Milliseconds 250
Shot "j01_selected.png"                      # HUD appears: Job Idle, Queue (0)

ShiftRightClick $HELM_X $HELM_Y              # assign Piloting at the helm
Start-Sleep -Milliseconds 200
Shot "j02_assigned.png"                       # Queue(1) / current Piloting, crew starts moving

Start-Sleep -Milliseconds 900
Shot "j03_moving.png"                          # crew walking the A* path to the helm
Start-Sleep -Milliseconds 1300
Shot "j04_progress.png"                         # further along / performing
Start-Sleep -Milliseconds 1600
Shot "j05_performing.png"                        # at helm, Performing, progress full

Write-Output "DONE"

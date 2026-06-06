param([string]$Proc="sandbox",[string]$OutDir="C:\dev\blackstride\bin\shots")
# Phase-2 LEAK TEST. Proves a click on the screen-space UI does NOT fall through to the world
# (crew select/move), while a click on the world DOES — so the gate is specific, not a broken
# deselect. Coordinates are in WINDOW-GRAB pixels (same basis as PrintWindow/GetWindowRect),
# mapped to screen via the window's Left/Top. DPI-aware so cursor lands true on this 125% box.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
namespace H {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int Left,Top,Right,Bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct POINT{public int X,Y;}
 public static class W{
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr hdc,uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint dx,uint dy,uint d,IntPtr e);
  public static void Grab(IntPtr h,string path){RECT r;GetWindowRect(h,out r);int w=r.Right-r.Left,hh=r.Bottom-r.Top;if(w<=0||hh<=0)return;using(Bitmap b=new Bitmap(w,hh,PixelFormat.Format32bppArgb)){using(Graphics g=Graphics.FromImage(b)){IntPtr hdc=g.GetHdc();PrintWindow(h,hdc,0x2);g.ReleaseHdc(hdc);}b.Save(path);}}
 }}
"@
[H.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$ME_LDOWN=0x0002; $ME_LUP=0x0004
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p=Get-Process $Proc -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne 0}|Select-Object -First 1
if(-not $p){"NO_WINDOW";exit 1}
$h=$p.MainWindowHandle
[H.W]::ShowWindow($h,9)|Out-Null;[H.W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 400
$wr=New-Object H.RECT;[H.W]::GetWindowRect($h,[ref]$wr)|Out-Null
$cr=New-Object H.RECT;[H.W]::GetClientRect($h,[ref]$cr)|Out-Null
"WINDOW $($wr.Left),$($wr.Top) .. $($wr.Right),$($wr.Bottom)  ($($wr.Right-$wr.Left)x$($wr.Bottom-$wr.Top))"
"CLIENT $($cr.Right)x$($cr.Bottom)"
# Move cursor to a point given in WINDOW-GRAB image coords (origin = window top-left).
function MoveImg([int]$ix,[int]$iy){
  $sx=$wr.Left+$ix; $sy=$wr.Top+$iy
  [H.W]::SetCursorPos(($sx-9),($sy-9))|Out-Null; Start-Sleep -Milliseconds 45
  [H.W]::SetCursorPos($sx,$sy)|Out-Null; Start-Sleep -Milliseconds 170
  "  img($ix,$iy)->screen($sx,$sy)"
}
function Click(){ [H.W]::mouse_event($ME_LDOWN,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 150;[H.W]::mouse_event($ME_LUP,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 200 }
function Shot($n){[H.W]::Grab($h,(Join-Path $OutDir $n));"  shot $n"}

# Park cursor on empty space first so nothing is hovered for the baseline.
"--- A baseline (nothing selected) ---"
MoveImg 950 500; Shot "leak_A_baseline.png"

"--- B select crew (left-click ON crew) ---"
MoveImg 648 397; Click; MoveImg 950 500; Shot "leak_B_selected.png"   # park off-crew so ring isn't hidden by cursor

"--- C click Test button (must NOT deselect crew) ---"
MoveImg 141 183; Click; MoveImg 950 500; Shot "leak_C_after_button.png"

"--- D control: click empty floor (SHOULD deselect crew) ---"
MoveImg 820 300; Click; MoveImg 950 500; Shot "leak_D_after_world.png"
"DONE"

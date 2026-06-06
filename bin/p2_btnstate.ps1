param([string]$Proc="sandbox",[string]$OutDir="C:\dev\blackstride\bin\shots")
# Capture the [Test] button in three visual states to prove hover/press recolor:
#   idle  : cursor parked far away (button = NORMAL dark)
#   hot   : cursor hovering the button, not pressed (button = HOT, brighter)
#   held  : left button held down on it (button = HELD, brightest) -- captured WHILE held
# Window-grab pixel coords (same basis as PrintWindow). Button center ~ img(141,183).
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
namespace H2 {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int Left,Top,Right,Bottom;}
 public static class W{
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr hdc,uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint dx,uint dy,uint d,IntPtr e);
  public static void Grab(IntPtr h,string path){RECT r;GetWindowRect(h,out r);int w=r.Right-r.Left,hh=r.Bottom-r.Top;if(w<=0||hh<=0)return;using(Bitmap b=new Bitmap(w,hh,PixelFormat.Format32bppArgb)){using(Graphics g=Graphics.FromImage(b)){IntPtr hdc=g.GetHdc();PrintWindow(h,hdc,0x2);g.ReleaseHdc(hdc);}b.Save(path);}}
 }}
"@
[H2.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p=Get-Process $Proc -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne 0}|Select-Object -First 1
if(-not $p){"NO_WINDOW";exit 1}
$h=$p.MainWindowHandle
[H2.W]::ShowWindow($h,9)|Out-Null;[H2.W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 400
$wr=New-Object H2.RECT;[H2.W]::GetWindowRect($h,[ref]$wr)|Out-Null
function MoveImg([int]$ix,[int]$iy){$sx=$wr.Left+$ix;$sy=$wr.Top+$iy;[H2.W]::SetCursorPos(($sx-9),($sy-9))|Out-Null;Start-Sleep -Milliseconds 45;[H2.W]::SetCursorPos($sx,$sy)|Out-Null;Start-Sleep -Milliseconds 180}
function Grab($n){[H2.W]::Grab($h,(Join-Path $OutDir $n));"  shot $n"}
$LDOWN=0x0002;$LUP=0x0004
"idle";  MoveImg 950 500; Grab "btn_1_idle.png"
"hot";   MoveImg 141 183; Grab "btn_2_hot.png"
"held";  [H2.W]::mouse_event($LDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 160; Grab "btn_3_held.png"; [H2.W]::mouse_event($LUP,0,0,0,[IntPtr]::Zero)
MoveImg 950 500
"DONE"

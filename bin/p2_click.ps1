param([string]$Proc="sandbox",[string]$OutDir="C:\dev\blackstride\bin\shots")
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
namespace H {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int Left,Top,Right,Bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct POINT{public int X,Y;}
 public static class W{
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
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
$p=Get-Process $Proc -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne 0}|Select-Object -First 1
if(-not $p){"NO_WINDOW";exit 1}
$h=$p.MainWindowHandle
[H.W]::ShowWindow($h,9)|Out-Null;[H.W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 350
function MoveClient([int]$cx,[int]$cy){$pt=New-Object H.POINT;$pt.X=$cx;$pt.Y=$cy;[H.W]::ClientToScreen($h,[ref]$pt)|Out-Null;[H.W]::SetCursorPos(($pt.X-7),($pt.Y-7))|Out-Null;Start-Sleep -Milliseconds 40;[H.W]::SetCursorPos($pt.X,$pt.Y)|Out-Null;Start-Sleep -Milliseconds 150}
function Shot($n){[H.W]::Grab($h,(Join-Path $OutDir $n))}

MoveClient 134 152
# Use legacy mouse_event for the button (more reliable than SendInput on this box). Capture WHILE held.
[H.W]::mouse_event($ME_LDOWN,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 180
Shot "p2_g_press.png"
[H.W]::mouse_event($ME_LUP,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 180
Shot "p2_g_release.png"
# A full click on empty space (to later test crew/no-leak); move to center, click.
MoveClient 700 450; Start-Sleep -Milliseconds 120
Shot "p2_g_off.png"
"DONE"

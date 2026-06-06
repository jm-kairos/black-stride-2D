param([string]$Proc="sandbox",[string]$OutDir="C:\dev\blackstride\bin\shots")
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
namespace H {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int Left,Top,Right,Bottom;}
 [StructLayout(LayoutKind.Sequential)] public struct POINT{public int X,Y;}
 [StructLayout(LayoutKind.Sequential)] public struct INPUT{public uint type;public MOUSEINPUT mi;}
 [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT{public int dx;public int dy;public uint mouseData;public uint dwFlags;public uint time;public IntPtr dwExtraInfo;}
 public static class W{
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr hdc,uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n,INPUT[] p,int cb);
  public static void Grab(IntPtr h,string path){RECT r;GetWindowRect(h,out r);int w=r.Right-r.Left,hh=r.Bottom-r.Top;if(w<=0||hh<=0)return;using(Bitmap b=new Bitmap(w,hh,PixelFormat.Format32bppArgb)){using(Graphics g=Graphics.FromImage(b)){IntPtr hdc=g.GetHdc();PrintWindow(h,hdc,0x2);g.ReleaseHdc(hdc);}b.Save(path);}}
 }}
"@
[H.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$LDOWN=0x0002; $LUP=0x0004
$sz=[Runtime.InteropServices.Marshal]::SizeOf([type][H.INPUT])
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p=Get-Process $Proc -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne 0}|Select-Object -First 1
if(-not $p){"NO_WINDOW";exit 1}
$h=$p.MainWindowHandle
[H.W]::ShowWindow($h,9)|Out-Null;[H.W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 350
$cr=New-Object H.RECT;[H.W]::GetClientRect($h,[ref]$cr)|Out-Null
"CLIENT $($cr.Right)x$($cr.Bottom)"
function MoveClient([int]$cx,[int]$cy){
  $pt=New-Object H.POINT;$pt.X=$cx;$pt.Y=$cy;[H.W]::ClientToScreen($h,[ref]$pt)|Out-Null
  # SetCursorPos moves the cursor AND posts WM_MOUSEMOVE -> SDL motion event. Two steps to
  # guarantee a delta even if a prior position coincided.
  [H.W]::SetCursorPos(($pt.X-7),($pt.Y-7))|Out-Null; Start-Sleep -Milliseconds 40
  [H.W]::SetCursorPos($pt.X,$pt.Y)|Out-Null; Start-Sleep -Milliseconds 160
  $g=New-Object H.POINT;[H.W]::GetCursorPos([ref]$g)|Out-Null
  "  client($cx,$cy)->screen($($pt.X),$($pt.Y)) cursor=($($g.X),$($g.Y))"
}
function Down(){ $d=New-Object H.INPUT;$d.type=0;$d.mi.dwFlags=$LDOWN;[H.W]::SendInput(1,@($d),$sz)|Out-Null;Start-Sleep -Milliseconds 130 }
function Up(){ $u=New-Object H.INPUT;$u.type=0;$u.mi.dwFlags=$LUP;[H.W]::SendInput(1,@($u),$sz)|Out-Null;Start-Sleep -Milliseconds 130 }
function Shot($n){[H.W]::Grab($h,(Join-Path $OutDir $n))}

MoveClient 700 450; Shot "p2_f_off.png"
MoveClient 134 152; Shot "p2_f_hover.png"
Down; Shot "p2_f_press.png"; Up; Shot "p2_f_release.png"
"DONE"

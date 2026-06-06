param([string]$Proc="sandbox",[string]$OutDir="C:\dev\blackstride\bin\shots")
# PHASE 4 verify, part 1 (POSITIVE case — all deterministic, NO world clicks needed).
# The crew auto-walks to the helm via the init PILOTING job, so is_active_pilot becomes TRUE
# on its own. We just: peek local -> zoom global (expect HUD "HELM: MANNED / FLIGHT READY")
# -> hold D (expect the roof silhouette to ROTATE, proving the manned helm has flight authority).
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;using System.Drawing;using System.Drawing.Imaging;using System.Runtime.InteropServices;
namespace H {
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int Left,Top,Right,Bottom;}
 public static class W{
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr hdc,uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte s,uint f,IntPtr e);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,int d,IntPtr e);
  public static void Grab(IntPtr h,string path){RECT r;GetWindowRect(h,out r);int w=r.Right-r.Left,hh=r.Bottom-r.Top;if(w<=0||hh<=0)return;using(Bitmap b=new Bitmap(w,hh,PixelFormat.Format32bppArgb)){using(Graphics g=Graphics.FromImage(b)){IntPtr hdc=g.GetHdc();PrintWindow(h,hdc,0x2);g.ReleaseHdc(hdc);}b.Save(path);}}
 }}
"@
[H.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$WHEEL=0x0800; $VK_D=0x44; $KEYUP=0x0002
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p=Get-Process $Proc -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne 0}|Select-Object -First 1
if(-not $p){"NO_WINDOW";exit 1}
$h=$p.MainWindowHandle
$wr=New-Object H.RECT;[H.W]::GetWindowRect($h,[ref]$wr)|Out-Null
$midx=[int](($wr.Left+$wr.Right)/2); $midy=[int](($wr.Top+$wr.Bottom)/2)
"WINDOW $($wr.Left),$($wr.Top)..$($wr.Right),$($wr.Bottom)"
function Focus(){[H.W]::ShowWindow($h,9)|Out-Null;[H.W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 350}
function Center(){[H.W]::SetCursorPos($midx,$midy)|Out-Null;Start-Sleep -Milliseconds 60}
function Wheel([int]$n){$d=if($n -lt 0){-120}else{120};for($i=0;$i -lt [Math]::Abs($n);$i++){[H.W]::mouse_event($WHEEL,0,0,$d,[IntPtr]::Zero);Start-Sleep -Milliseconds 55}}
function HoldD([int]$ms){[H.W]::keybd_event($VK_D,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds $ms;[H.W]::keybd_event($VK_D,0,$KEYUP,[IntPtr]::Zero)}
function Shot($n){[H.W]::Grab($h,(Join-Path $OutDir $n));"  shot $n"}

Focus
Shot "p4_01_local_manned.png"          # local view: crew should be standing on the helm (teal) tile
Focus; Center; Wheel -8; Start-Sleep -Milliseconds 500
Shot "p4_02_global_manned.png"         # global: expect HUD green "HELM: MANNED - FLIGHT READY", heading 0
Focus; HoldD 1000; Start-Sleep -Milliseconds 450
Shot "p4_03_manned_turned.png"         # expect roof silhouette ROTATED (manned helm = flight authority)
"DONE"

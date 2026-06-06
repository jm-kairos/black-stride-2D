param([string]$Proc="sandbox",[string]$OutDir="C:\dev\blackstride\bin\shots")
# PHASE 4 verify, part 2 (NEGATIVE case - the reported bug).
# Demote the pilot by ordering the crew OFF the helm, then prove global-mode WASD is DEAD.
#   1. locate the crew by scanning for its cyan pixel (DPI-proof; no coordinate guessing)
#   2. left-click it -> select (expect yellow ring)
#   3. right-click a floor tile below it -> move order => JOB_INTERRUPTED => is_active_pilot=FALSE
#   4. zoom to global -> expect HUD orange "HELM: UNMANNED - FLIGHT LOCKED"
#   5. hold W (thrust) then D (turn) -> ship must NOT translate and NOT rotate (gate works)
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
  // Scan a grabbed window for the crew's cyan quad; returns "cx,cy,count" in window-image px.
  public static string FindCyan(IntPtr h){RECT r;GetWindowRect(h,out r);int w=r.Right-r.Left,hh=r.Bottom-r.Top;
    using(Bitmap b=new Bitmap(w,hh,PixelFormat.Format32bppArgb)){using(Graphics g=Graphics.FromImage(b)){IntPtr hdc=g.GetHdc();PrintWindow(h,hdc,0x2);g.ReleaseHdc(hdc);}
      long sx=0,sy=0,n=0;
      for(int y=0;y<hh;y+=2){for(int x=0;x<w;x+=2){Color c=b.GetPixel(x,y);
        if(c.R>=40 && c.R<=150 && c.G>=210 && c.B>=235){sx+=x;sy+=y;n++;}}}
      if(n==0)return "0,0,0"; return ((int)(sx/n)).ToString()+","+((int)(sy/n)).ToString()+","+n.ToString();}}
 }}
"@
[H.W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$WHEEL=0x0800;$LDOWN=0x0002;$LUP=0x0004;$RDOWN=0x0008;$RUP=0x0010
$VK_W=0x57;$VK_D=0x44;$KEYUP=0x0002
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$p=Get-Process $Proc -ErrorAction SilentlyContinue|Where-Object{$_.MainWindowHandle -ne 0}|Select-Object -First 1
if(-not $p){"NO_WINDOW";exit 1}
$h=$p.MainWindowHandle
function Focus(){[H.W]::ShowWindow($h,9)|Out-Null;[H.W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 350}
function Shot($n){[H.W]::Grab($h,(Join-Path $OutDir $n));"  shot $n"}
Focus
$wr=New-Object H.RECT;[H.W]::GetWindowRect($h,[ref]$wr)|Out-Null
$midx=[int](($wr.Left+$wr.Right)/2);$midy=[int](($wr.Top+$wr.Bottom)/2)
"WINDOW $($wr.Left),$($wr.Top)..$($wr.Right),$($wr.Bottom)"
function Center(){[H.W]::SetCursorPos($midx,$midy)|Out-Null;Start-Sleep -Milliseconds 60}
function MoveImg([int]$ix,[int]$iy){$sx=$wr.Left+$ix;$sy=$wr.Top+$iy;[H.W]::SetCursorPos(($sx-9),($sy-9))|Out-Null;Start-Sleep -Milliseconds 45;[H.W]::SetCursorPos($sx,$sy)|Out-Null;Start-Sleep -Milliseconds 150}
function LClick(){[H.W]::mouse_event($LDOWN,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 140;[H.W]::mouse_event($LUP,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 160}
function RClick(){[H.W]::mouse_event($RDOWN,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 140;[H.W]::mouse_event($RUP,0,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds 160}
function Wheel([int]$n){$d=if($n -lt 0){-120}else{120};for($i=0;$i -lt [Math]::Abs($n);$i++){[H.W]::mouse_event($WHEEL,0,0,$d,[IntPtr]::Zero);Start-Sleep -Milliseconds 55}}
function Hold([byte]$vk,[int]$ms){[H.W]::keybd_event($vk,0,0,[IntPtr]::Zero);Start-Sleep -Milliseconds $ms;[H.W]::keybd_event($vk,0,$KEYUP,[IntPtr]::Zero)}

Shot "p4_n0_local.png"
$res=[H.W]::FindCyan($h); "CYAN $res"
$parts=$res.Split(',');$cx=[int]$parts[0];$cy=[int]$parts[1];$cnt=[int]$parts[2]
if($cnt -lt 8){"CREW_NOT_FOUND count=$cnt";exit 2}
"  crew at img($cx,$cy), $cnt px"
# 1) select the crew
MoveImg $cx $cy; LClick; Center; Start-Sleep -Milliseconds 120
Shot "p4_n1_selected.png"
# 2) order it DOWN to a floor tile (demote). +170px in image-y = toward ship stern = floor.
MoveImg $cx ($cy+170); RClick; Start-Sleep -Milliseconds 120
Shot "p4_n2_ordered.png"
# give the job runner a few frames + let the crew step off the helm tile
Start-Sleep -Milliseconds 900
# 3) zoom out to global
Center; Wheel -8; Start-Sleep -Milliseconds 600
Shot "p4_n3_global_unmanned.png"    # expect orange HELM: UNMANNED - FLIGHT LOCKED
# 4) try to FLY: thrust then turn. Gate works => ship neither translates nor rotates.
Focus; Hold $VK_W 1300; Start-Sleep -Milliseconds 300
Shot "p4_n4_after_W.png"
Focus; Hold $VK_D 1300; Start-Sleep -Milliseconds 300
Shot "p4_n5_after_D.png"
"DONE"

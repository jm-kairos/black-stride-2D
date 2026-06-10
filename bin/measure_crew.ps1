# Scan a sandbox screenshot for the cyan crew quads (CREW_COLOR ~ RGB 89,235,255)
# and print the centroid + framebuffer coords of each blob. Used to calibrate the
# exclusive-selection harness (we need the true on-screen crew positions, not guesses).
param(
  [string]$Img = "C:\dev\blackstride\bin\shots\j00_start.png",
  # Window chrome offset: image(ix,iy) -> framebuffer(ix-ChromeX, iy-ChromeY).
  # Measured from this window: 9px left border, ~31px title bar. Refined by calibration below.
  [int]$ChromeX = 9,
  [int]$ChromeY = 31
)
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($Img)
$W = $bmp.Width; $H = $bmp.Height
Write-Output ("IMG " + $W + "x" + $H)

# Collect cyan-ish pixels: high G, high B, low-ish R, B>=G-ish.
$pts = New-Object System.Collections.Generic.List[object]
for ($y = 0; $y -lt $H; $y += 2) {
  for ($x = 0; $x -lt $W; $x += 2) {
    $c = $bmp.GetPixel($x, $y)
    if ($c.B -gt 200 -and $c.G -gt 190 -and $c.R -lt 160 -and ($c.B - $c.R) -gt 60) {
      $pts.Add([pscustomobject]@{X=$x; Y=$y})
    }
  }
}
$bmp.Dispose()
Write-Output ("CYAN_PIXELS " + $pts.Count)

# Greedy cluster: group pixels within 40px into blobs.
$clusters = New-Object System.Collections.Generic.List[object]
foreach ($p in $pts) {
  $placed = $false
  foreach ($cl in $clusters) {
    if ([math]::Abs($p.X - $cl.Cx) -lt 40 -and [math]::Abs($p.Y - $cl.Cy) -lt 40) {
      $cl.Sx += $p.X; $cl.Sy += $p.Y; $cl.N++
      $cl.Cx = $cl.Sx / $cl.N; $cl.Cy = $cl.Sy / $cl.N
      $placed = $true; break
    }
  }
  if (-not $placed) {
    $clusters.Add([pscustomobject]@{ Sx=[double]$p.X; Sy=[double]$p.Y; N=1; Cx=[double]$p.X; Cy=[double]$p.Y })
  }
}

$i = 0
foreach ($cl in ($clusters | Sort-Object -Property N -Descending)) {
  if ($cl.N -lt 8) { continue }   # ignore tiny specks
  $fbx = [int]($cl.Cx - $ChromeX)
  $fby = [int]($cl.Cy - $ChromeY)
  Write-Output ("BLOB " + $i + " img=(" + [int]$cl.Cx + "," + [int]$cl.Cy + ") fb=(" + $fbx + "," + $fby + ") px=" + $cl.N)
  $i++
}

<#
.SYNOPSIS
    Offline shader compiler for Black Stride (Windows / PowerShell).

.DESCRIPTION
    Compiles every HLSL source under assets/shaders/src to BOTH targets:
      * DXIL  -> assets/shaders/dxil/<name>.<stage>.dxil   (d3d12 backend)
      * SPIR-V -> assets/shaders/spirv/<name>.<stage>.spv  (vulkan backend)

    The runtime loads whichever blob matches SDL_GetGPUShaderFormats. build-all.bat
    calls this script with -Incremental to (re)compile any edited shaders, then stages
    assets/ into bin/assets/. Run it manually with no args to force a full recompile.

    This script AUTO-DISCOVERS every *.hlsl in the src folder and derives the shader
    stage/profile from the filename (<name>.<stage>.hlsl). That means it can never go
    stale when a new shader is added -- the historical failure mode of the old
    hand-maintained compile_shaders.sh.

    Stage -> profile mapping:
      vert -> vs_6_0     frag -> ps_6_0     comp -> cs_6_0

.PARAMETER Filter
    Optional wildcard to compile only matching shader base names (e.g. -Filter starfield*).
    Omit to compile everything.

.PARAMETER Incremental
    Skip any shader whose DXIL and SPIR-V blobs already exist and are newer than its .hlsl
    source. Used by build-all.bat so only edited shaders are recompiled.

.EXAMPLE
    ./tools/compile_shaders.ps1
    ./tools/compile_shaders.ps1 -Filter nebula_layer
#>
[CmdletBinding()]
param(
    [string]$Filter = "*",
    [switch]$Incremental
)

$ErrorActionPreference = "Stop"

# --- Locate dxc (prefer PATH, then Vulkan SDK) --------------------------------------
$dxc = (Get-Command dxc -ErrorAction SilentlyContinue).Source
if (-not $dxc) {
    if ($env:VULKAN_SDK) {
        $candidate = Join-Path $env:VULKAN_SDK "Bin\dxc.exe"
        if (Test-Path $candidate) { $dxc = $candidate }
    }
}
if (-not $dxc) {
    Write-Error "dxc.exe not found. Install the Vulkan SDK or add dxc to PATH."
    exit 1
}

# --- Resolve paths (script lives in tools/, repo root is its parent) -----------------
$root    = Split-Path -Parent $PSScriptRoot
$src     = Join-Path $root "assets\shaders\src"
$outDxil = Join-Path $root "assets\shaders\dxil"
$outSpv  = Join-Path $root "assets\shaders\spirv"
New-Item -ItemType Directory -Force -Path $outDxil, $outSpv | Out-Null

# --- Stage -> profile table ----------------------------------------------------------
$profiles = @{ "vert" = "vs_6_0"; "frag" = "ps_6_0"; "comp" = "cs_6_0" }

# SPIR-V: dxc emits each (register, space) as (binding, set=space) by default, matching
# SDL's binding model (vertex UBOs space1/set1, fragment UBOs space3/set3). Keep minimal.
$spvCommon = @("-spirv", "-fspv-target-env=vulkan1.2")

Write-Host "Compiling shaders with $(& $dxc --version 2>&1 | Select-Object -First 1)"
Write-Host "  src:  $src"

$failed = 0
$count  = 0
$skipped = 0

Get-ChildItem -Path (Join-Path $src "*.hlsl") | Sort-Object Name | ForEach-Object {
    # Parse "<name>.<stage>.hlsl"
    $base = $_.Name -replace '\.hlsl$', ''      # e.g. starfield_layer.frag
    $parts = $base -split '\.'
    if ($parts.Count -lt 2) {
        Write-Warning "  SKIP $($_.Name): expected <name>.<stage>.hlsl"
        return
    }
    $stage = $parts[-1]
    $name  = ($parts[0..($parts.Count - 2)] -join '.')

    if ($name -notlike $Filter) { return }

    $profile = $profiles[$stage]
    if (-not $profile) {
        Write-Warning "  SKIP $($_.Name): unknown stage '$stage'"
        return
    }

    $inFile   = $_.FullName
    $dxilFile = Join-Path $outDxil "$name.$stage.dxil"
    $spvFile  = Join-Path $outSpv  "$name.$stage.spv"

    # Incremental: skip when both compiled blobs exist and are newer than the source.
    if ($Incremental -and (Test-Path $dxilFile) -and (Test-Path $spvFile)) {
        $srcTicks  = $_.LastWriteTimeUtc.Ticks
        $dxilTicks = (Get-Item $dxilFile).LastWriteTimeUtc.Ticks
        $spvTicks  = (Get-Item $spvFile).LastWriteTimeUtc.Ticks
        if ($srcTicks -le $dxilTicks -and $srcTicks -le $spvTicks) { $script:skipped++; return }
    }

    $count++
    Write-Host "  [$name.$stage] -> DXIL + SPIR-V"

    # DXIL (d3d12). No signing required for SDL runtime use.
    & $dxc -T $profile -E main $inFile -Fo $dxilFile
    if ($LASTEXITCODE -ne 0) { Write-Warning "    DXIL FAILED: $name.$stage"; $script:failed++; return }

    # SPIR-V (vulkan fallback).
    & $dxc -T $profile -E main @spvCommon $inFile -Fo $spvFile
    if ($LASTEXITCODE -ne 0) { Write-Warning "    SPIR-V FAILED: $name.$stage"; $script:failed++; return }
}

Write-Host ""
Write-Host "Compiled $count shader(s); $skipped up-to-date; $failed failure(s)."
Write-Host "  dxil: $outDxil"
Write-Host "  spv:  $outSpv"
if ($failed -gt 0) { exit 1 }
Write-Host ""
Write-Host "NOTE: run build-all.bat next to stage the new blobs into bin/assets/shaders/."

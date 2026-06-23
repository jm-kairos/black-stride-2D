#!/usr/bin/env bash
# Offline shader compiler for Black Stride (Phase 2 toolchain).
#
# DECISION: We use the Vulkan SDK's `dxc` (NOT SDL_shadercross, which isn't installed on
# this machine). dxc compiles HLSL to BOTH DXIL and SPIR-V, covering the two desktop targets:
#   * DXIL  -> required by the d3d12 backend SDL selects on this box (probe: formats=0xC).
#   * SPIRV -> portability fallback for the vulkan backend.
#
# Output layout (runtime picks the blob matching SDL_GetGPUShaderFormats):
#   assets/shaders/dxil/<name>.<stage>.dxil
#   assets/shaders/spirv/<name>.<stage>.spv
#
# SDL3 GPU resource-binding contract (SDL_gpu.h) is honored via register spaces in the HLSL
# and the SPIR-V -fvk shift flags below:
#   vertex   uniform buffers => (b,space1) / SPIR-V set 1
#   fragment uniform buffers => (b,space3) / SPIR-V set 3
#   fragment sampled tex     => (t,space2) / SPIR-V set 2   (Phase 3)
set -euo pipefail

DXC="${DXC:-dxc}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/assets/shaders/src"
OUT_DXIL="$ROOT/assets/shaders/dxil"
OUT_SPV="$ROOT/assets/shaders/spirv"
mkdir -p "$OUT_DXIL" "$OUT_SPV"

# For SPIR-V we shift each HLSL register class into its space's descriptor set so the binding
# model matches SDL's expectations. dxc applies shift PER (class,space): we register-shift the
# uniform-buffer (b) class so that bN in spaceS lands on SPIR-V set S, binding N.
#   -fvk-b-shift N S  => add N to binding for b-registers in space S.
# We keep binding offsets at 0 and rely on -fvk-bind-register/space mapping via auto sets:
# dxc emits each (register, space) as (binding, set=space) when -fvk-use-dx-layout is off, which
# is the default for -spirv. So space1 -> set 1 automatically; no explicit shift needed for our
# simple single-UBO case. Flags kept minimal and verified by the engine actually loading them.
SPV_COMMON=(-spirv -fspv-target-env=vulkan1.2)

compile() {
    local name="$1" stage="$2" profile="$3" entry="main"
    local in="$SRC/${name}.${stage}.hlsl"
    echo "  [$name.$stage] -> DXIL + SPIR-V"
    # DXIL (required by d3d12). -Fo emits the object; no signing needed for SDL runtime use.
    "$DXC" -T "$profile" -E "$entry" "$in" -Fo "$OUT_DXIL/${name}.${stage}.dxil"
    # SPIR-V (vulkan fallback).
    "$DXC" -T "$profile" -E "$entry" "${SPV_COMMON[@]}" "$in" -Fo "$OUT_SPV/${name}.${stage}.spv"
}

echo "Compiling shaders with $($DXC --version 2>&1 | head -1)"
compile quad vert vs_6_0
compile quad frag ps_6_0
compile sprite vert vs_6_0
compile sprite frag ps_6_0
compile mapped_sprite vert vs_6_0
compile mapped_sprite frag ps_6_0
compile starfield vert vs_6_0
compile starfield frag ps_6_0
echo "Shaders compiled to:"
echo "  $OUT_DXIL"
echo "  $OUT_SPV"

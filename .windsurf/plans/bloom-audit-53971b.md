# Full-Scene HDR Bloom — Post-Implementation Audit

Date: 2026-06-16
Scope: All files changed for the full-scene HDR bloom post-process (shaders, backend, renderer API, game).

## Issues Found

### 1. Pipeline Color Format Mismatch — HIGH

**Location:** `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` — `create_postprocess_pipelines`

**Problem:** All 4 post-process pipelines are created with `swap_fmt` from `SDL_GetGPUSwapchainTextureFormat`. However, the offscreen targets (`scene_rt`, `bloom_a`, `bloom_b`) are hardcoded to `SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM` in `create_bloom_targets`. If the swapchain format differs (e.g., `BGRA8_UNORM` on some backends), the pipeline's declared color-target format will not match the actual render target format. SDL3 GPU validation may reject the render pass or produce undefined results.

**Fix:** Pass `SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM` explicitly to `create_postprocess_pipeline` for all 4 pipelines, matching the hardcoded offscreen target format. The composite pass already targets the swapchain, but since the composite shader only writes color (no blending-dependent format quirks), using `R8G8B8A8_UNORM` uniformly is safe.

---

### 2. Extra No-op Render Pass in Blur-H Setup — LOW

**Location:** `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` — `end_frame`, Pass 3

**Problem:**
```cpp
g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
bloom_target.texture = g_sdl.bloom_b;  // changed AFTER Begin!
SDL_EndGPURenderPass(g_sdl.pass);
```
The first `BeginGPURenderPass` opens a pass targeting `bloom_a` (still set from Pass 2), then it is immediately ended without drawing anything. A second begin/end pair then correctly targets `bloom_b`. This wastes one GPU render pass per frame.

**Fix:** Either:
- Set `bloom_target.texture = g_sdl.bloom_b` before the first `BeginGPURenderPass`, or
- Remove the first begin/end pair entirely.

---

### 3. Texture Recreation Without GPU Idle — LOW-MED

**Location:** `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` — `sdlgpu_backend_on_resize`

**Problem:** When the window resizes, the old bloom textures are released via `SDL_ReleaseGPUTexture` and new ones are created immediately, without calling `SDL_WaitForGPUIdle`. SDL3 GPU internally reference-counts resources, but if a command buffer referencing the old textures is still in flight on the GPU, the release may occur while the GPU is still reading them. This is a potential race condition.

**Fix:** Add `SDL_WaitForGPUIdle(g_sdl.device)` before releasing the old textures in `on_resize`.

---

### 4. Initial Bloom Target Size Guess — LOW

**Location:** `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` — `sdlgpu_backend_initialize`

**Problem:** `create_bloom_targets(1280, 720)` is called during backend initialization before the platform reports the actual window size. If the initial window size differs, the first frame renders to incorrectly-sized offscreen targets. `on_resize` eventually corrects this, but the timing depends on whether the platform fires a resize event before the first present.

**Fix:** Defer `create_bloom_targets` until the first `end_frame`, using the actual `swap_width/height` acquired from `SDL_WaitAndAcquireGPUSwapchainTexture`. Alternatively, query the drawable size via `SDL_GetWindowSize` at init time instead of hardcoding 1280×720.

---

### 5. Fullscreen Vertex Shader Comment Mismatch — COSMETIC

**Location:** `assets/shaders/src/fullscreen.vert.hlsl`

**Problem:** The comment claims `SV_VertexID 0 -> (-1,3)`, but the math:
```hlsl
float2 uv = float2(id % 2, id / 2) * 2.0;
```
produces `uv=(0,0)` for `id=0`, giving `position=(-1,-1)` — not `(-1,3)`. The resulting triangle still covers the entire clip space correctly (one large triangle with UVs from 0..2, clamped at edges by the sampler), so there is no rendering bug. The comment simply describes a different vertex ordering than what the code implements.

**Fix:** Update the comment to match the actual vertex positions emitted by the code.

---

## Verdict

- **Issue 1** should be fixed before the next test run — it is the only issue that could cause outright rendering failure or validation errors.
- **Issues 2–4** are cleanup/optimization items that improve correctness and efficiency but will not break the feature.
- **Issue 5** is documentation only.

# Bloom Audit — Independent Review

Date: 2026-06-16
Reviewer: Cascade
Scope: Review the claims and fixes proposed in `bloom-audit-53971b.md` against the current codebase.

---

## Issues Found by the Original Audit

### 1. Pipeline Color Format Mismatch — HIGH

**Original claim:** All 4 post-process pipelines are created with `swap_fmt`, but offscreen targets are hardcoded `R8G8B8A8_UNORM`. If `swap_fmt` differs, SDL GPU validation may reject the render pass.

**Finding: Correct, but the proposed fix is wrong.**

The original audit proposes using `R8G8B8A8_UNORM` for **all four** pipelines, including the composite pass. This is incorrect: the composite pass targets the **swapchain**, so its pipeline format **must** match the swapchain format. Using `R8G8B8A8_UNORM` for composite would cause a mismatch on any backend where the swapchain format is different (e.g. `BGRA8_UNORM`).

**Correct fix:** Use `R8G8B8A8_UNORM` for `extract`, `blur_h`, and `blur_v` (they target offscreen textures), and keep `swap_fmt` for `composite` (it targets the swapchain).

**Critical additional finding (missed by original audit):** The sprite pipelines in `create_pipelines()` have the exact same problem. They are also created with `swap_fmt`, but in the bloom path the sprite batch renders into `scene_rt` (`R8G8B8A8_UNORM`). If `swap_fmt != R8G8B8A8_UNORM`, every sprite draw in the bloom path has a pipeline–target format mismatch. This affects the main scene rendering and is at least as severe as the post-process mismatch.

| Item | Assessment |
|------|------------|
| Claim accuracy | Partially correct |
| Proposed fix | Incorrect (would break composite) |
| Severity | HIGH |
| Status | Needs corrected fix; sprite pipeline issue also needs addressing |

---

### 2. Extra No-op Render Pass in Blur-H Setup — LOW

**Original claim:** Pass 3 opens a render pass targeting `bloom_a` (left over from Pass 2), immediately ends it, then opens a second pass targeting `bloom_b` where the actual draw occurs.

**Finding: Correct.**

Lines 1353–1356 in `renderer_backend_sdlgpu.cpp` waste one GPU render pass per frame.

**Correct fix:** Set `bloom_target.texture = g_sdl.bloom_b` before calling `SDL_BeginGPURenderPass`, then remove the first no-op begin/end pair entirely.

| Item | Assessment |
|------|------------|
| Claim accuracy | Correct |
| Proposed fix | Correct |
| Severity | LOW |
| Status | Valid issue, straightforward fix |

---

### 3. Texture Recreation Without GPU Idle — LOW-MED

**Original claim:** `on_resize` releases bloom textures and immediately recreates them without `SDL_WaitForGPUIdle`, which is a potential race if the GPU is still reading the old textures.

**Finding: Real, but severity is overstated.**

`SDL_ReleaseGPUTexture` decrements an internal reference count; SDL GPU defers actual destruction until all in-flight command buffers that reference the texture have completed. The texture will not be destroyed while the GPU is still reading it. Adding `SDL_WaitForGPUIdle` is conservative and safe, but not strictly required for correctness.

| Item | Assessment |
|------|------------|
| Claim accuracy | Correct |
| Severity assessment | Overstated — LOW would be more appropriate |
| Proposed fix | Safe but not strictly necessary |
| Status | Valid observation, optional fix |

---

### 4. Initial Bloom Target Size Guess — LOW

**Original claim:** `create_bloom_targets(1280, 720)` is hardcoded during initialization. If the actual window size differs, the first frame renders to incorrectly-sized targets.

**Finding: Correct.**

Both proposed fixes are sound:
1. Query the actual window size at init time via `SDL_GetWindowSizeInPixels`.
2. Defer target creation to the first `end_frame`, using the real `swap_width/swap_height` acquired from `SDL_WaitAndAcquireGPUSwapchainTexture`.

| Item | Assessment |
|------|------------|
| Claim accuracy | Correct |
| Proposed fixes | Both valid |
| Severity | LOW |
| Status | Valid issue |

---

### 5. Fullscreen Vertex Shader Comment Mismatch — COSMETIC

**Original claim:** The comment claims `SV_VertexID 0 -> (-1,3)`, but the code produces `(-1,-1)` for `id=0`.

**Finding: Correct.**

Actual mapping from `fullscreen.vert.hlsl`:
- `id=0`: `uv=(0,0)` → `position=(-1,-1)`
- `id=1`: `uv=(2,0)` → `position=(3,-1)`
- `id=2`: `uv=(0,2)` → `position=(-1,3)`

The comment describes a different (but equally valid) vertex ordering. The triangle still covers clip space correctly.

| Item | Assessment |
|------|------------|
| Claim accuracy | Correct |
| Severity | COSMETIC |
| Status | Valid, documentation-only |

---

## Issues Missed by the Original Audit

### A. Sprite Pipeline Format Mismatch in Bloom Path — HIGH

**Location:** `create_pipelines()` in `renderer_backend_sdlgpu.cpp`

**Problem:** The sprite pipelines are created with `color_target.format = swap_fmt`. In the bloom path (`g_sdl.bloom_enabled == true`), the sprite batch renders into `scene_rt`, which is hardcoded `R8G8B8A8_UNORM`. If `swap_fmt != R8G8B8A8_UNORM`, every sprite draw in the bloom path has a pipeline–target format mismatch. The non-bloom path is unaffected because sprites render directly to the swapchain.

This is the same category of bug as Issue 1 but affects the **main scene rendering**.

**Fix:** Create a separate set of sprite pipelines for offscreen rendering (format `R8G8B8A8_UNORM`), or accept a format parameter in `create_pipelines()` and create the appropriate set for each target.

---

### B. Post-Process Sampler Uses NEAREST Filtering — MED

**Location:** `sdlgpu_backend_initialize` in `renderer_backend_sdlgpu.cpp:722–724`

**Problem:** The bloom blur passes sample the half-res bloom texture using the same `NEAREST` sampler used for pixel-art sprites:

```cpp
sinfo.min_filter = SDL_GPU_FILTER_NEAREST;
sinfo.mag_filter = SDL_GPU_FILTER_NEAREST;
```

`NEAREST` produces blocky, pixelated blur results. For separable Gaussian blur, `LINEAR` filtering is standard — it produces smoother output and effectively provides free 2× sample interpolation, reducing the number of taps needed for equivalent visual quality.

**Fix:** Create a second `LINEAR` sampler dedicated to post-process passes, or switch the existing sampler to `LINEAR` if the pixel-art look is acceptable (it usually is not for downsampled bloom).

---

### C. `create_bloom_targets` Return Value Ignored in `on_resize` — LOW

**Location:** `sdlgpu_backend_on_resize` in `renderer_backend_sdlgpu.cpp:797`

**Problem:** `create_bloom_targets` returns `b8`, but `on_resize` calls it as a void expression:

```cpp
create_bloom_targets((u32)width, (u32)height);
```

If texture creation fails, the textures remain `NULL`. The code degrades gracefully (`end_frame` checks for non-NULL), but there is no error log.

**Fix:** Check the return value and log a fatal or error message.

---

### D. ImGui Draw Data Rendered Inside Composite Pass — COSMETIC (likely non-issue)

**Location:** `end_frame`, Pass 5 in `renderer_backend_sdlgpu.cpp`

**Observation:** ImGui is rendered inside the composite pass, after the bloom composite draw but before `SDL_EndGPURenderPass`. The ImGui SDL_GPU backend manages its own sampler/texture bindings via `ImGui_ImplSDLGPU3_RenderDrawData`. As long as that backend properly rebinds its resources, there is no conflict. This is standard usage and likely not a bug.

**Status:** Not an issue (verification of ImGui backend internals would be required to confirm, but this is the documented pattern).

---

## Summary

| ID | Title | Original Verdict | Independent Verdict |
|----|-------|------------------|---------------------|
| 1 | Pipeline Color Format Mismatch | HIGH | **HIGH** — claim correct, proposed fix wrong |
| 2 | Extra No-op Render Pass in Blur-H | LOW | **LOW** — correct |
| 3 | Texture Recreation Without GPU Idle | LOW-MED | **LOW** — real but overstated |
| 4 | Initial Bloom Target Size Guess | LOW | **LOW** — correct |
| 5 | Fullscreen Vertex Shader Comment | COSMETIC | **COSMETIC** — correct |
| A | Sprite Pipeline Format Mismatch | *Missed* | **HIGH** — missed by original audit |
| B | Post-Process Sampler NEAREST | *Missed* | **MED** — missed by original audit |
| C | `create_bloom_targets` Return Ignored | *Missed* | **LOW** — missed by original audit |
| D | ImGui in Composite Pass | *Missed* | **Non-issue** |

---

## Recommended Actions

1. **Fix Issue 1 correctly:** Use `R8G8B8A8_UNORM` for `extract/blur_h/blur_v` pipelines, keep `swap_fmt` for `composite`.
2. **Fix Issue A (sprite pipelines):** The sprite pipeline format must match the render target. Either create a second pipeline set for offscreen rendering or parameterize `create_pipelines()`.
3. **Fix Issue 2:** Remove the no-op render pass in the blur-H setup.
4. **Address Issue B:** Create a `LINEAR` sampler for post-process sampling.
5. **Optional:** Fix Issues 3, 4, 5, and C as cleanup items.

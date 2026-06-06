# Black Stride — SDL3 GPU Integration Plan

> Goal: give the engine a clean, abstracted rendering backbone built **solely** on the
> SDL3 GPU API (`SDL_gpu.h`), suited to a 2D space-sim / sandbox in the spirit of
> *Starsector* (thousands of sprites, parallax starfields, additive thruster/weapon FX,
> a debug-line/vector layer, and a UI layer).
>
> The engine **does not** expose SDL types to the game. The game talks only to a small
> set of engine-owned "wrap" types (handles + structs). SDL3 GPU lives entirely behind
> the renderer module.

---

## 1. Current architecture (as studied)

A Kohi-style (Travis Vroman) layered C-with-structs engine, built as a DLL consumed by a
sandbox EXE.

```
entry.cpp (sandbox)  -> game_create() fills a Game struct (config + 4 fn pointers + state)
   main() (entry.h)  -> bs_memory_initialize() -> application_init() -> application_run()
                                                          |
   application.cpp (singleton ApplicationState) ----------+
       - subsystems: logger, input, event, platform, arena (frame arena 64MB)
       - game loop:  pump_messages -> game.update(dt) -> game.render(dt) -> input_update
```

Key facts that shape the design:

| Area | Current state | Implication |
|------|---------------|-------------|
| **Platform** | `platform_sdl3.cpp`, `InternalState { SDL_Window* window; }`, owns `SDL_Init(VIDEO\|EVENTS)` | Window already exists in SDL. Renderer needs the `SDL_Window*` but the game/app must not see it. |
| **API export** | `bs__api__` = dllexport/dllimport; SDL types never cross the DLL boundary today | New renderer public headers must expose **only** engine types, never `SDL_GPU*`. |
| **Loop timing** | `update`/`render` called with hardcoded `(f32)0` dt | dt plumbing is a prerequisite for animation; flag it. |
| **Memory** | `bs_memory_allocator(size, tag)` + virtual-memory arenas; tags include `MEMORY_TAG_RENDERER`, `MEMORY_TAG_TEXTURE`, `MEMORY_TAG_MATERIAL_INSTANCE` | Use these tags for GPU bookkeeping allocations. Per-frame transient data goes in the existing 64MB frame arena. |
| **Events** | `event_fire/register`; window resize is a TODO in `platform_pump_messages` | Swapchain resize must hook a new resize event. |
| **Math** | only `Vec2/3/4` + `clamp`; **no matrix type** | Need `Mat4` (or at least a 2D ortho + affine) for camera/transforms. Hard blocker for any rendering. |
| **Build** | `clang++`, globs all `*.cpp`, `-Ivendor/include`, links `-lSDL3` | New `.cpp` files are auto-picked up. No build edits needed except possibly shader compilation step. |
| **SDL** | 3.4.4, full GPU API present (`SDL_gpu.h`), `SDL3.lib`/`SDL3.dll` vendored | Everything needed is in-tree. No new third-party deps required for core. |

---

## 2. Design principles for the wrap

1. **Opaque handles, not pointers.** The game receives integer/opaque handles
   (`bs_texture`, `bs_pipeline`, `bs_buffer`, `bs_shader`, `bs_sampler`). SDL objects are
   stored in engine-side pools indexed by these handles. This keeps the DLL ABI stable and
   hides SDL entirely.

2. **One backend, cleanly seamed.** We commit to SDL3 GPU as the only backend, but route
   all calls through a `renderer_*` facade (a thin "renderer frontend") that delegates to a
   `renderer_backend` (the SDL3 GPU implementation). The seam costs almost nothing and keeps
   game code backend-agnostic.

3. **2D-first, retained-where-it-matters.** Expose an immediate-mode-feeling **sprite batch**
   and **line batch** as the primary game-facing API, backed by retained GPU buffers. This is
   exactly what a Starsector-like game needs: submit sprites/quads per frame, engine batches by
   texture/material and draws in few calls.

4. **No SDL types in public headers.** `renderer.h`, `renderer_types.h`, `sprite_batch.h`,
   `camera.h` include only `defines.h` / `math`. The SDL include lives only in
   `renderer_backend_sdlgpu.cpp`.

5. **Frame lifecycle is explicit and centralized** in the application loop, not the game. The
   game only issues draw submissions between `begin`/`end`.

---

## 3. Target module layout

```
engine/source/
  renderer/
    renderer_types.h            // public: handles, enums, descriptor structs, vertex formats
    renderer.h                  // public: frontend API (init/begin/end/submit/resize/shutdown)
    renderer.cpp                // frontend: owns backend, camera, batchers; orchestration
    renderer_backend.h          // internal: backend vtable interface (function-pointer struct)
    backend/
      renderer_backend_sdlgpu.h   // internal
      renderer_backend_sdlgpu.cpp // ONLY file that #includes <SDL3/SDL_gpu.h>
    resources/
      gpu_pool.h / .cpp         // handle<->SDL object pools (textures, buffers, pipelines...)
      texture_loader.h / .cpp   // image bytes -> bs_texture (uses transfer buffer + copy pass)
    batch/
      sprite_batch.h / .cpp     // public: quad/sprite submission, dynamic vertex/index buffers
      line_batch.h / .cpp       // public: debug vectors, orbits, hitboxes, grid
    camera/
      camera2d.h / .cpp         // public: ortho 2D camera (pan/zoom/rotate) -> view-proj
  math/
    math_utils.h (extend)       // add Mat4, ortho, translate/scale/rotate, Vec2 ops
  platform/
    platform.h (extend)         // add platform_get_window_handle() -> opaque void* for backend
assets/
  shaders/
    src/   sprite.vert.hlsl, sprite.frag.hlsl, line.vert.hlsl, line.frag.hlsl
    spv/   compiled SPIR-V (and/or .dxil, .msl) shipped next to the exe
```

Naming follows existing engine conventions (`bs_`, snake_case functions, `E`-prefixed enums,
`bs__api__` on exported symbols).

---

## 4. Public wrap API (game-facing surface)

All of the following live in headers that include **no SDL**. This is the contract the game
programs against.

### 4.1 Handles & core types (`renderer_types.h`)

```c
typedef struct bs_texture  { u32 id; } bs_texture;   // opaque
typedef struct bs_shader   { u32 id; } bs_shader;
typedef struct bs_pipeline { u32 id; } bs_pipeline;
typedef struct bs_buffer   { u32 id; } bs_buffer;
typedef struct bs_sampler  { u32 id; } bs_sampler;

#define BS_INVALID_HANDLE 0   // id 0 reserved = invalid

typedef enum ERendererBackend { RENDERER_BACKEND_SDL_GPU = 0 } ERendererBackend;

typedef struct bs_color { f32 r, g, b, a; } bs_color;

// 2D sprite submission record (engine batches these)
typedef struct bs_sprite {
    bs_math::Vec2 position;     // world units
    bs_math::Vec2 size;         // world units
    bs_math::Vec2 origin;       // 0..1 pivot for rotation
    f32           rotation;     // radians
    bs_math::Vec4 uv;           // (u0,v0,u1,v1) sub-rect for atlases
    bs_color      tint;
    bs_texture    texture;
    u32           layer;        // sort key / z-order
} bs_sprite;

typedef enum EBlendMode {
    BLEND_NONE, BLEND_ALPHA, BLEND_ADDITIVE, BLEND_MULTIPLY
} EBlendMode;
```

### 4.2 Renderer frontend (`renderer.h`)

```c
bs__api__ b8   renderer_initialize(const char* app_name, struct PlatformState* plat);
bs__api__ void renderer_shutdown();
bs__api__ void renderer_on_resize(u16 width, u16 height);

// Per-frame lifecycle (called by application.cpp, not the game)
bs__api__ b8   renderer_begin_frame(f32 dt);   // acquire cmd buffer + swapchain image, begin pass
bs__api__ b8   renderer_end_frame(f32 dt);     // end pass, submit, present

// Camera
bs__api__ void renderer_set_camera(const struct Camera2D* cam);

// Game-facing draw submission (valid between begin/end_frame)
bs__api__ void renderer_draw_sprite(const bs_sprite* sprite);
bs__api__ void renderer_draw_line(bs_math::Vec2 a, bs_math::Vec2 b, bs_color c, f32 thickness);
bs__api__ void renderer_draw_quad(bs_math::Vec2 pos, bs_math::Vec2 size, bs_color c);

// Resources
bs__api__ bs_texture renderer_texture_create(const u8* pixels, u32 w, u32 h, u32 channels);
bs__api__ bs_texture renderer_texture_load(const char* path);   // optional, via stb_image
bs__api__ void       renderer_texture_destroy(bs_texture t);
```

> Note: `renderer_begin_frame`/`renderer_end_frame` are driven by the **application loop**.
> The game's `render(dt)` runs between them and only calls `renderer_draw_*`. This mirrors how
> the existing loop already separates `update` and `render`.

### 4.3 Camera (`camera2d.h`)

```c
typedef struct Camera2D {
    bs_math::Vec2 position;   // world-space center
    f32 zoom;                 // 1.0 = 1 world unit : 1 pixel baseline
    f32 rotation;             // radians
    u16 viewport_w, viewport_h;
} Camera2D;

bs__api__ void camera2d_init(Camera2D* c, u16 w, u16 h);
bs__api__ bs_math::Mat4 camera2d_view_proj(const Camera2D* c);  // ortho * view
bs__api__ bs_math::Vec2 camera2d_screen_to_world(const Camera2D* c, bs_math::Vec2 screen);
```

A world-space 2D camera with zoom is the single most important primitive for a Starsector-like
game (zoom out to see the whole battle, zoom in on a ship).

---

## 5. Internal backend design (SDL3 GPU)

This is the only code that touches `SDL_gpu.h`. Maps engine concepts onto SDL GPU objects.

### 5.1 Device & swapchain bring-up (`renderer_backend_sdlgpu.cpp`)

1. `SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | _DXIL | _MSL, debug, NULL)`
   — request all formats so the same engine binary runs on Vulkan/D3D12/Metal.
2. `SDL_ClaimWindowForGPUDevice(device, window)` — needs the `SDL_Window*` from platform.
   → **Add `platform_get_window_handle()`** returning an opaque `void*` so the backend can
   retrieve the window without the renderer including `platform_sdl3.cpp` internals.
3. `SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
   SDL_GPU_PRESENTMODE_VSYNC)` — vsync by default, expose present mode later.
4. Query `SDL_GetGPUSwapchainTextureFormat()` to build pipelines with matching color format.

### 5.2 Per-frame flow (inside `renderer_begin/end_frame`)

```
begin_frame:
  cmd = SDL_AcquireGPUCommandBuffer(device)
  SDL_AcquireGPUSwapchainTexture(cmd, window, &swap_tex, &w, &h)
    - if swap_tex == NULL (minimized/occluded): skip frame cleanly, set is_suspended-like flag
  (record: upload any pending dynamic vertex data via a copy pass BEFORE the render pass)
  color_target = { texture=swap_tex, load_op=CLEAR, store_op=STORE, clear_color=space_black }
  pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, NULL)
  SDL_SetGPUViewport / SDL_SetGPUScissor

  ... game.render(dt) calls renderer_draw_* which APPEND to CPU-side batch arrays ...

end_frame:
  flush_batches():               // turn accumulated sprites/lines into draw calls
    sort by (layer, texture, blend)
    map dynamic transfer buffer, write interleaved vertices, copy-pass upload
    for each batch run:
       SDL_BindGPUGraphicsPipeline(pass, pipeline_for_blendmode)
       SDL_BindGPUVertexBuffers / SDL_BindGPUIndexBuffer
       SDL_BindGPUFragmentSamplers(pass, texture+sampler)
       SDL_PushGPUVertexUniformData(cmd, 0, &view_proj, sizeof(Mat4))
       SDL_DrawGPUIndexedPrimitives(pass, index_count, 1, 0, 0, 0)
  SDL_EndGPURenderPass(pass)
  SDL_SubmitGPUCommandBuffer(cmd)
```

> **Critical ordering rule (SDL GPU):** copy passes (buffer uploads) must be recorded on the
> command buffer **before** `SDL_BeginGPURenderPass`, or in a separate command buffer. The
> sprite batcher therefore accumulates all sprites during `render`, then uploads + draws during
> `end_frame`. Document this loudly — it's the #1 source of SDL GPU bugs.

### 5.3 Resource pools (`gpu_pool.cpp`)

- Fixed-capacity arrays (e.g. 4096 textures, 256 pipelines) allocated from
  `bs_memory_allocator(..., MEMORY_TAG_RENDERER)`.
- Handle = `index + generation<<16` to catch use-after-free of stale handles.
- Each slot stores the `SDL_GPUTexture*` / `SDL_GPUBuffer*` etc. Lookups are O(1).

### 5.4 Texture upload path (`texture_loader.cpp`)

```
SDL_CreateGPUTexture(SAMPLER usage, RGBA8, w, h)
SDL_CreateGPUTransferBuffer(UPLOAD, w*h*4)
map -> memcpy pixels -> unmap
cmd = AcquireCommandBuffer; copy = BeginGPUCopyPass
SDL_UploadToGPUTexture(copy, src, dst, cycle=false)
EndGPUCopyPass; SubmitGPUCommandBuffer
ReleaseGPUTransferBuffer
```

Pixel decode (PNG → RGBA) via `stb_image.h` (single-header, drop into `vendor/include`).
This is the one new vendored dependency and it is header-only.

### 5.5 Shaders

- Author in HLSL (SDL's recommended source), compile **offline** to SPIR-V / DXIL / MSL.
- Tooling: `shadercross` (SDL_shadercross) or `glslang` + `spirv-cross`. Add a
  `tools/compile_shaders.bat` that emits to `assets/shaders/spv|dxil|msl/`.
- Backend loads the blob matching `SDL_GetGPUShaderFormats(device)` at runtime via
  `SDL_CreateGPUShader`.
- **Minimum shader set for v1:**
  - `sprite.vert` + `sprite.frag` (textured, tinted quad; one Mat4 view-proj uniform)
  - `line.vert` + `line.frag` (colored, no texture)

---

## 6. Integration touch-points in existing code

| File | Change |
|------|--------|
| `platform/platform.h` | Add `bs__api__ VOID_PTR platform_get_window_handle(PlatformState*);` |
| `platform/platform_sdl3.cpp` | Implement the above (return `state->window`). Implement the resize TODO: fire a new `EVENT_CODE_WINDOW_RESIZED` on `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`. |
| `core/event.h` | Add `EVENT_CODE_WINDOW_RESIZED = 0x08`. |
| `core/application.cpp` | After `platform_initialize`: call `renderer_initialize(name, &platform)`. In loop: wrap `game.render` with `renderer_begin_frame(dt)` / `renderer_end_frame(dt)`. Register a resize handler → `renderer_on_resize`. On shutdown: `renderer_shutdown()` before `platform_terminate`. Also store real `width/height` (currently never set from window). |
| `core/application.cpp` (loop) | **Compute real dt** from `platform_get_absolute_time()` instead of `(f32)0`; pass to update/render/begin/end. (Prereq for any animation.) |
| `math/math_utils.h/.cpp` | Add `Mat4`, `mat4_ortho`, `mat4_mul`, `mat4_translate/scale/rotate_z`, plus `Vec2` add/sub/scale/rotate. |
| `engine/build.bat` | No structural change (globs cpp). Optionally add a pre-step calling `tools/compile_shaders.bat`. Ensure `assets/` is copied next to the exe (extend `sandbox/build.bat`'s SDL3.dll copy step). |
| `sandbox/source/game.cpp` | Demo: load a texture in `game_init`, set up a `Camera2D`, draw a few sprites + a grid in `game_render`. Proves the wrap end-to-end. |

> The renderer must initialize **after** the window exists. It does today: `platform_initialize`
> runs before `game_inst->init`. Insert `renderer_initialize` between them so the game can create
> textures inside its own `init`.

---

## 7. Phased execution plan

Each phase is independently buildable and leaves the engine in a runnable state.

### Phase 0 — Foundations (no SDL GPU yet)
- [ ] Add `Mat4` + matrix/vector math to `math_utils`.
- [ ] Add `platform_get_window_handle()` + window-resize event wiring.
- [ ] Plumb **real dt** through the loop.
- **Exit:** engine still runs the existing console demo; dt is non-zero; resize fires events.

### Phase 1 — Device & clear screen
- [ ] `renderer_backend_sdlgpu.cpp`: create device, claim window, swapchain params.
- [ ] `renderer.cpp` frontend skeleton + `gpu_pool`.
- [ ] `begin_frame`/`end_frame` that just **clears the swapchain to space-black** and presents.
- [ ] Wire into `application.cpp`; handle minimized window (null swapchain texture).
- **Exit:** sandbox opens a window cleared to a solid color via SDL GPU. **First pixels.**

### Phase 2 — Shaders + first triangle/quad
- [ ] Offline shader compile pipeline + `tools/compile_shaders.bat`.
- [ ] Load shaders, build a graphics pipeline matching swapchain format.
- [ ] Static vertex buffer; draw one hardcoded quad with the view-proj uniform.
- **Exit:** a colored quad renders. Pipeline/shader/uniform path proven.

### Phase 3 — Camera2D + sprite batch
- [ ] `Camera2D` → view-proj; `renderer_set_camera`.
- [ ] Dynamic vertex/index transfer buffers; `sprite_batch` accumulate→upload→draw.
- [ ] `texture_loader` + `stb_image`; `renderer_texture_create/load`.
- [ ] `renderer_draw_sprite` batched by texture; alpha + additive blend pipelines.
- **Exit:** sandbox draws hundreds of tinted, rotated, atlas-subrect sprites with a pannable,
  zoomable camera. **This is the Starsector backbone.**

### Phase 4 — Debug/vector layer & polish
- [ ] `line_batch` → `renderer_draw_line` / grid / circle helpers (orbits, ranges, hitboxes).
- [ ] Layer/z-sorting, blend-mode sort key, basic frame stats (draw calls, sprite count).
- [ ] `SetGPUAllowedFramesInFlight`, fence-based resource destruction safety.
- **Exit:** full 2D toolkit: sprites + lines + camera + blend modes, stable under resize.

### Phase 5 (optional, later) — Scale features
- Render-to-texture for bloom/glow (thrusters, weapons), post-process additive pass.
- Instanced rendering for starfields (`SDL_DrawGPUPrimitives` with per-instance buffer).
- Texture atlas packer; compute-pass particles.

---

## 8. Risks & decisions to confirm

1. **Shader toolchain.** Offline compilation needs `shadercross`/`glslang`. If we want zero
   external tools, fall back to shipping only SPIR-V and requiring Vulkan — but that loses
   D3D12/Metal portability. **Recommend** adopting SDL_shadercross offline. *(Decision needed.)*
2. **`stb_image` dependency.** Header-only, MIT/public-domain, trivial to vendor. Low risk.
3. **dt change** alters current loop semantics (today everything gets `0`). Harmless but touches
   shared code — do it in Phase 0 in isolation.
4. **ABI via handles.** Confirmed approach: opaque `{u32 id}` structs keep the DLL boundary clean
   and match how the engine already hides `internal_state`.
5. **Threading.** SDL GPU command buffers are single-thread-record for now; keep all rendering on
   the main thread (matches current single-threaded loop). Revisit if a job system lands.

---

## 9. Definition of done for "engine prepared for SDL3 GPU"

- The game can, using **only engine headers** (no SDL include), do:
  `renderer_texture_load` → set a `Camera2D` → `renderer_draw_sprite`/`draw_line` each frame,
  and get correct, batched, blended 2D output.
- `application.cpp` owns the frame lifecycle; the game only submits draws.
- SDL3 GPU is confined to `backend/renderer_backend_sdlgpu.cpp` + the resource pool/loader.
- Builds via existing `build-all.bat` with the added shader-compile step; `assets/` ship beside
  the exe.

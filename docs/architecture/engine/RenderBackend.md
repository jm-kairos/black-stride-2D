# RenderBackend

**Responsibility:** Owns every GPU interaction in the project — device and swapchain
management, the sorted dynamic sprite batcher, the procedural-effect pass chain (starfield,
nebula, heat map, sunburst, star and planet surfaces), the HDR bloom and anamorphic-streak post
stack, the texture pool — and binds all of it into the frontend's vtable. It is the only
translation unit permitted to include `<SDL3/SDL_gpu.h>`, the ImGui backends, or the RmlUi
headers. It explicitly does not own the public rendering API (RenderFrontend) — but it *does*
in practice own the implementation of the UiFacade headers, which is the subsystem's biggest
structural problem (see below).

**Public interface:** Almost none. `engine/source/renderer/renderer_backend.cpp` exposes only
`renderer_backend_create` / `_destroy` (declared in RenderFrontend's
`renderer/renderer_backend.h`), called from one place: `renderer/renderer.cpp:55,64,109`.
`engine/source/renderer/backend/renderer_backend_sdlgpu.h` declares ~31 `sdlgpu_backend_*`
entry points, none marked `bs__api__` — they are reached solely through the vtable. Nothing here
is visible to the sandbox.

**Depends on:** RenderFrontend, UiFacade, Platform, Diagnostics, MathCore, Foundation
(out-degree 12, the highest of any engine subsystem).
**Depended on by:** nothing, except the dead starfield module.

**Key invariants:**
- **Copy passes may not run inside a render pass** (SDL3 GPU rule, called "the #1 SDL GPU bug
  source" at `renderer_backend_sdlgpu.cpp:14`). Enforced by the frame split: `begin_frame`
  acquires only a command buffer (`:1592`), and the vertex upload, swapchain acquire and all
  render passes happen in `end_frame`.
- **RmlUi uploads must be submitted before the frame's main command buffer.** RmlUi can request
  GPU uploads while the render pass is open, so each takes its own command buffer batched into
  one shared submit (`rml_upload_flush`, `:3306`). Ordering is enforced only by call placement,
  relying on SDL executing command buffers in submission order (stated `:75-79`).
- **ImGui `NewFrame`/`Render` must stay paired.** `begin_frame` issues `NewFrame` (`:1614-1616`);
  `end_frame` must call `Render()` even on the failed-acquire and minimised early-outs
  (`:2153`). RenderFrontend's `frame_active` gate is the other half of this guarantee.
- **The 16384-sprite cap is a hard ceiling, not a tunable.** The index buffer is `u16` and index
  init computes `base = (u16)(s*4)`, so `s <= 16383`; raising `BS_MAX_SPRITES` produces
  *aliased* indices, not more capacity. Documented at length at `:146-153`; enforced only by a
  `BS_LOG_WARN` and a drop at `:1940-1944`.
- **Texture handles are generation-tagged.** `pool_alloc_texture` packs
  `(generation << 18) | (index + 1)` (`:485`) and `pool_resolve_texture` validates both
  (`:495-504`), so use-after-destroy is detectable and falls back to the 1×1 white texture.
- **`backend->internal_state` is decorative.** It is set to `&g_sdl` (`:1324`) and every
  function ignores its `backend` argument — the backend is a singleton.
- **The vtable must be fully populated.** `renderer_backend.cpp` assigns 31 function pointers and
  `renderer_backend_destroy` nulls 32 fields (31 + `internal_state`). Hand-maintained parallel
  lists; nothing detects a pointer added to one and forgotten in the other.
- Per-frame submission state is cleared in `begin_frame` (`:1597-1605`), so the game must
  resubmit heat map, nebula, streak source and all effect queues **every frame**; lights,
  camera, glow and bloom settings are sticky.

**Extension points:** **Adding a backend** means implementing the 31-entry vtable, adding a value
to `ERendererBackend` (`renderer_types.h:27-30`, currently one value) and a `case` in
`renderer_backend_create` (`renderer_backend.cpp:13-51`). The firewall header
`renderer_backend_sdlgpu.h` is the pattern to copy: it declares the entry points with
`struct renderer_backend` / `struct PlatformState` forward-declared so the factory never sees
SDL. **Adding an effect** follows the established shape: a `bs_*_params` struct in
`renderer_types.h`, a `draw_*` vtable entry, a fixed-size queue in `sdlgpu_state`, a queue-push
in the `draw_*` implementation, and a pass in `end_frame`. Every existing effect
(nebula, heat map, sunburst, star surface, planet surface) is built that way.

**Known limitations / tech debt:**
- **It is a god object.** 4888 lines carrying three unrelated public surfaces: the backend
  vtable, the ImGui facade, and the RmlUi facade plus HUD data model. Roughly **1866 lines (38%)
  are UiFacade implementation** (`bs_imgui_*` from `:2032`, the RmlUi section from `:3112`), and
  those facades read the `g_sdl` global directly rather than through an accessor.
- **All state is one file-scope global**, `g_sdl` (`:377`), plus `g_rml` (`:3222`) and ~10 loose
  statics for the RmlUi upload batch, pools and churn counters.
- **`sdlgpu_backend_initialize` leaks on every failure path.** It returns `FALSE` from a dozen
  places (`:1327,1371,1376,1408,1414,1418,…`) without releasing the device, pipelines or shaders
  already created.
- **Effect queues drop silently.** Sprite and mapped-sprite overflow logs a warning, but all
  four effect queues (8/4/4/32 entries) drop extras with no diagnostic — deliberately, because
  logging would spam per frame (`:1817-1822,1833-1838`).
- Draw runs break on `glow_override` **pointer identity** (`:2388`), so two identical-by-value
  glow structs at different addresses split the batch.
- `destroy_texture` calls `SDL_WaitForGPUIdle` (`:1713`) — a full GPU stall per destroy.
- **A hardcoded absolute path**: `bs_imgui_initialize` loads `C:\Windows\Fonts\consola.ttf`
  (`:2053`), warning but continuing if absent.
- Shaders are read from `assets/shaders/{dxil,spirv}/…` relative to the working directory
  (`:413`); missing blobs are a fatal init failure, coupling the backend to `build-all.bat`'s
  shader-compile and asset-staging steps.
- A driver-specific correction is baked in: `get_corrected_swapchain_format` forces
  `B8G8R8A8_UNORM` when D3D12 misreports (`:90-101`).
- The offscreen path is forced on by three independent conditions — bloom, an active streak, or
  `g_rml_frost_active()` (`:2619-2627`) — so enabling the in-game UI silently changes the whole
  render path.
- The mapped-sprite pass takes its light direction from `mapped_batch[0]` for the entire batch
  (`:2437-2441`), on the stated assumption that all ships share one star direction.
- `stb_image_impl.cpp` is a build artifact, not a module: it declares nothing, has no includers,
  and exists only to compile stb_image's body once under seven warning suppressions.
- The "Phase 3"/"Phase 4" section markers record build order rather than a designed grouping;
  the vtable grew by accretion, which is why it is 31 entries wide.

**Source paths:** `engine/source/renderer/backend/**`,
`engine/source/renderer/renderer_backend.cpp`

**Last verified:** 2026-08-07, commit `812680c`

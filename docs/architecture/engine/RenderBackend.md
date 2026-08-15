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
- **`g_sdl.offscreen_format` is the single source of truth for every offscreen target AND every
  pipeline that renders into one.** All eight targets (`scene_rt`, the bloom and aux-streak
  ping-pongs, `nebula_rt`, `heat_rt`, `ui_backdrop_rt`) share one `SDL_GPUTextureCreateInfo` in
  `create_bloom_targets`, and `create_postprocess_pipelines`' `offscreen_fmt` plus the two
  offscreen sprite pipeline sets all read the same field. SDL rejects a render pass whose
  pipeline format disagrees with its target, so these cannot be allowed to drift.
- **The scene renders in HDR (`RGBA16F`) and is tone-mapped exactly once, in the composite.**
  The swapchain is 8-bit, so the composite is the only place high-range values re-enter a
  clamped buffer — which is why `pipeline_composite` alone keeps `swap_fmt`. Two consequences:
  additive sprite stacking no longer flattens at 1.0 (three overlapping muzzle flashes stay
  distinguishable and keep their tint), and `bloom_threshold` above 1.0 is finally *reachable*.
  Before this the threshold's shipped default of 1.2 made `bloom_extract` output pure black on
  every frame — the bloom pass ran and contributed nothing.
- **The format is PROBED, not assumed.** `SDL_GPUTextureSupportsFormat` decides at init and falls
  back to `R8G8B8A8_UNORM` with a warning; `hdr_enabled` then drives the composite's tone-map
  flag, so the LDR fallback reproduces the old look exactly rather than shipping raw HDR into
  8 bits.
- **Under HDR the offscreen path is mandatory**, not one of the opportunistic conditions below:
  the direct-to-swapchain path is 8-bit and skips the composite, so allowing it would change the
  scene's exposure depending on whether bloom, a streak or the UI frost happened to be active.
  In practice this costs nothing today because bloom defaults on, which already forced the path
  every frame.
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
**An effect that renders into an offscreen target must build its pipeline with
`g_sdl.offscreen_format`**, never a literal: targets and pipelines share that one field and SDL
rejects a render pass whose formats disagree. An effect drawing straight to the swapchain uses
`swap_fmt` as before — `pipeline_composite` is the worked example of the latter, and it is
deliberate, because that pass is where the tone map converts HDR back to 8 bits.

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
- The offscreen path is forced on by bloom, an active streak, `g_rml_frost_active()`, or HDR
  (`:2619-2627`) — so enabling the in-game UI silently changes the whole render path. With HDR
  supported the fourth condition is always true, which makes the other three moot; they still
  matter on the `RGBA8` fallback.
- **The tone-map curve is a soft knee, not Reinhard, and the reason is worth keeping.** Reinhard
  compresses the whole range, mapping nominal white to 0.53 — correct for linear-authored HDR
  art, wrong here, where every hull and UI sprite is authored for an LDR target and only additive
  VFX overshoot ever exceeds 1.0. The shipped curve is identity below 0.8 and rolls off
  rationally above it, so LDR content is bit-for-bit unchanged (measured: hull brightness 246.6
  before, 246.5 after) and only previously-clipping pixels move. A rational tail was chosen over
  an exponential one because the exponential saturated by ~2x nominal white, which would have
  left a triple-barrel salvo barely brighter than a single shot.
- The mapped-sprite pass takes its light direction from `mapped_batch[0]` for the entire batch,
  on the stated assumption that all ships share one star direction.
- **The mapped batch interleaves with the sprite batch BY LAYER, not after it.**
  `mapped_layer_split` finds the first sorted sprite whose layer exceeds the mapped batch's
  highest `bs_mapped_sprite.layer`, and all three draw sites (offscreen path, direct path,
  each portrait scope) draw sprites-below → mapped hulls → sprites-above as three segments.
  Before the split the whole mapped batch painted after the full sprite pass, so everything
  above `LAYER_SHIP` — mount art, exhaust light spill, projectile streaks, muzzle/impact
  FX, the portrait's hardpoint boxes — was buried wherever a mapped hull overlapped it; the
  bug was latent from the mapped path's introduction and surfaced when the flagship card
  switched to `layer mapped`.
- **The `mapped_light` uniform now carries the scene's 16-slot point-light list alongside the
  star**, filled from the same `g_sdl.lights` the sprite pass packs; the mapped fragment shader
  shades each light per-pixel against the hull's normal map (direction rotated into ship-local
  space like the star, same quadratic radius falloff as `sprite.frag.hlsl`, no volumetric
  term). The layout MUST stay in sync across three sites: `mapped_light` here, `LightUBO` in
  `mapped_sprite.frag.hlsl`, and `preview_light_t` in `tools/map_extractor/preview.cpp` (the
  tool loads the same compiled blobs). The portrait pass (`vp_override != NULL`) deliberately
  keeps its point-light count at zero — studio look, matching `draw_sprite_batch`'s fullbright
  flag. Per-pixel light distance needs the fragment's true world position, so the mapped
  vertex shader forwards the raw corner position as a `world_xy` varying (the `world_pos`
  attribute is the sprite CENTER, same for all four corners).
- `stb_image_impl.cpp` is a build artifact, not a module: it declares nothing, has no includers,
  and exists only to compile stb_image's body once under seven warning suppressions.
- The "Phase 3"/"Phase 4" section markers record build order rather than a designed grouping;
  the vtable grew by accretion, which is why it is 31 entries wide.

**Source paths:** `engine/source/renderer/backend/**`,
`engine/source/renderer/renderer_backend.cpp`, and the post-chain shaders under
`assets/shaders/src/bloom_*.frag.hlsl`

**Last verified:** 2026-08-15, working tree on `game` (same day, later: the mapped batch
gains the layer interleave — `mapped_layer_split` + three-segment draws at all three sites;
live-verified via the inspector portrait, whose hardpoint boxes render over the mapped hull
again. Earlier: the mapped-sprite pass consumes the
scene point-light list: `mapped_light` grows the 16-light arrays + a count in `tuning.z`,
`draw_mapped_batch` packs `g_sdl.lights` except for portraits, and `mapped_sprite.vert/frag`
gain the `world_xy` varying + per-pixel normal-mapped accumulation loop — live-verified,
thruster-burn light warming a hull's stern plating).
Previously verified 2026-08-12 (adds the ship-portrait passes: capture
runs as per-frame SCOPE RECORDS — the main portrait plus one 256x256 fleet-thumbnail slot per
member — sharing one capture array with per-scope sub-range sorts; targets are the fixed-size
`portrait_rt` and the `thumb_rt` strip, both in `offscreen_format`, rendered as pre-passes
before the main path split (the strip in one pass with a viewport per slot). The RML render
interface's reserved "bs:portrait"/"bs:thumbs" texture names resolve to the live targets —
ReleaseTexture skips them, shutdown owns them, and the resize path deliberately does not touch
them. The vtable is 34 entries now — both hand-maintained lists in `renderer_backend.cpp`
updated together).
Previously verified 2026-08-09 (offscreen targets moved to `RGBA16F`
with a capability probe; tone-map added to `bloom_composite`; `bloom_extract`'s normaliser
rewritten — it divided by `max(1 - threshold, 0.0001)`, which silently zeroed all bloom at the
shipped 1.2 threshold and would have multiplied the scene by ~10,000 once HDR made that
threshold crossable)

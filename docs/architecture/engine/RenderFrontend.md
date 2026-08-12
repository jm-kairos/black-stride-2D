# RenderFrontend

**Responsibility:** Owns the backend-agnostic rendering API and the shared render vocabulary:
the renderer singleton, the backend vtable *interface*, the value types every layer exchanges
(`renderer_types.h`), the 2D camera transforms, CPU-side image decode, and the debug/vector
primitives synthesised from sprite quads. It forwards all GPU work through a function-pointer
table. It explicitly does not own any GPU code (RenderBackend), nor the concrete binding of that
vtable (`renderer_backend.cpp` is grouped with the backend) — but it *does* own the ImGui and
RmlUi **lifecycles**, which it brackets around the backend's own init and shutdown.

**Public interface:** `engine/source/renderer/renderer.h` — ~44 `bs__api__` functions: lifecycle
(`renderer_initialize`/`_shutdown`/`_on_resize`/`_begin_frame`/`_end_frame`), textures
(`_load_texture`/`_create_texture`/`_update_texture`/`_destroy_texture`), `renderer_set_camera`,
sprite and procedural-effect submission (`_draw_sprite`, `_draw_mapped_sprite`, `_draw_starfield`,
`_draw_sunburst`, `_draw_starsurface`, `_draw_planetsurface`, `_draw_heat_map`, `_draw_nebula`),
the ship-portrait offscreen scopes (`_portrait_begin`/`_thumb_begin`/`_portrait_end` — capture
sprite submissions into per-scope ranges rendered fullbright into persistent targets the RmlUi
HUD samples via the reserved texture names "bs:portrait" and "bs:thumbs"; a thumb scope renders
into one 256x256 slot of the fleet-thumbnail strip),
lighting and post-process setters, the debug layer (`_draw_line`/`_quad`/`_rect_outline`/
`_circle`/`_grid`), and frame instrumentation.
`engine/source/renderer/renderer_types.h` — `bs_texture`, `bs_color`, `bs_rect`, `bs_sprite`,
`bs_mapped_sprite`, `bs_glow_params`, `bs_light2d`, `Camera2D`, `bs_frame_stats`, `EBlendMode`,
the seven effect-parameter structs, `BS_INVALID_HANDLE`, `BS_LAYER_BLOOM_THRESHOLD`.
`engine/source/renderer/camera2d.h` — `camera2d_default`, `_view_proj`, `_screen_to_world`,
`_world_to_screen`.
`engine/source/renderer/renderer_backend.h` — `struct renderer_backend` (34 function pointers),
`renderer_backend_create`/`_destroy`; internal, consumed only by RenderBackend.

**Depends on:** Foundation, MathCore, Diagnostics, Memory, UiFacade.
**Depended on by:** AppLifecycle, RenderBackend, Widgets, DeadStarfield; and it is the widest
game-facing surface — 27 sandbox files include `renderer.h`, 17 `camera2d.h`, 12
`renderer_types.h`.

**Key invariants:**
- **`renderer_initialize` is single-shot** — guarded by `state.initialized`
  (`renderer.cpp:45-49`).
- **`frame_active` gates every draw.** Set only on a successful `begin_frame`
  (`renderer.cpp:137`), cleared in `end_frame` (`:154`); each draw function tests it
  (18 references in the file). This is what keeps the backend's ImGui `NewFrame`/`Render` pair
  balanced across the minimised and failed-acquire early-outs.
- **UI facades come up after the backend and go down before it.** `renderer_initialize` calls
  `bs_imgui_initialize` / `bs_rml_initialize` after `backend.initialize`
  (`renderer.cpp:61,75,82`); `renderer_shutdown` tears both down *before* `backend.shutdown`
  (`:102,106,108`), because they own device-bound GPU resources. Enforced by statement order.
  Both failures are deliberately non-fatal (`:77,84`).
- **`renderer_set_present_mode` only mirrors the flag when the backend confirms it applied**
  (`renderer.cpp:593-594`), because a wrongly-cached IMMEDIATE would also disable the
  application loop's software frame cap.
- **The camera copy exists so debug-line thickness can cancel zoom.** `renderer_set_camera`
  stores a local copy (`renderer.cpp:288`) that `renderer_draw_line` divides by (`:470-471`).
  A game that mutates its own camera without re-submitting gets stale thickness scaling.
- Almost every forwarder null-checks the backend function pointer before calling
  (e.g. `renderer.cpp:333,387,406`), so the vtable is treated as sparsely populated even though
  the only backend fills it completely. Failures are silent.

**Extension points:** **Adding a draw call is a three-place edit** — a `bs__api__` declaration
plus forwarder in `renderer.{h,cpp}`, a function pointer in `struct renderer_backend`
(`renderer_backend.h`), and an implementation plus a factory assignment in RenderBackend. The
existing effect calls (`_draw_nebula`, `_draw_heat_map`, …) all follow that shape. Adding a
value type means a struct in `renderer_types.h`, which is the shared vocabulary for the
frontend, the vtable, the backend and the game. Adding a backend is described under
RenderBackend.

**Known limitations / tech debt:**
- **`draw_alpha_mul` silently rewrites blend mode.** When a fade is active, an otherwise
  `BLEND_NONE` sprite is switched to `BLEND_ALPHA` (`renderer.cpp:314`) — a semantic mutation of
  a caller-owned value, unannounced at the call site.
- **The debug primitives bypass the frontend's own `renderer_draw_sprite`**, calling
  `state.backend.draw_sprite` directly after applying the alpha multiplier by hand
  (`renderer.cpp:488,509`). The two paths must be kept in agreement manually.
- Every debug primitive consumes the shared 16384-sprite batch: a circle costs one sprite per
  segment (`renderer.cpp:540-546`). `renderer_draw_grid` silently refuses to draw above 4096
  lines per axis (`:558`) with no feedback.
- **Half the texture-load path is invisible to memory accounting.** The file bytes go through
  `bs_memory_allocator` under `MEMORY_TAG_TEXTURE` (`renderer.cpp:198`), but the decoded pixel
  buffer comes from stb's own allocator and is released with `stbi_image_free` (`:211,221`).
- `renderer_load_texture` resolves paths relative to the working directory
  (`renderer.cpp:182`), tying it to the asset staging `build-all.bat` performs into `bin/`.
- `renderer_types.h` declares `bs_shader`, `bs_pipeline`, `bs_buffer`, `bs_sampler` (`:18-21`)
  that are **used nowhere** — placeholders for a resource system that does not exist.
- **`BS_LAYER_BLOOM_THRESHOLD` (`renderer_types.h:119`) is rendering policy living in a types
  header**: a sprite's `layer` number simultaneously means draw order, whether it goes through
  bloom, and (via the unlit threshold) whether it is lit.
- `bs_sprite::glow_override` (`renderer_types.h:113`) is a raw pointer the backend retains
  until `end_frame` — the only lifetime contract on the boundary, and it is undocumented in the
  header (see `engine-api-boundary.md` §5).
- `bs_sprite::custom` is an untyped `bs_color` whose four floats are interpreted only by HLSL.
- Four exported functions have no callers anywhere: `renderer_update_texture`,
  `renderer_get_draw_alpha`, `renderer_draw_grid`, `renderer_get_frame_stats`.
- The doc comment above `bs_mapped_sprite` (`renderer_types.h:62-64`) actually describes
  `bs_sprite` — it sits on the wrong struct.
- `Camera2D` is defined in `renderer_types.h` while all its operations live in `camera2d.h`,
  splitting type from behaviour. *Provisional:* whether `camera2d` belongs in this subsystem at
  all is still open (see `engine-subsystems.md` §Open questions).

**Source paths:** `engine/source/renderer/renderer.{cpp,h}`,
`engine/source/renderer/renderer_types.h`, `engine/source/renderer/renderer_backend.h`,
`engine/source/renderer/camera2d.{cpp,h}`

**Last verified:** 2026-08-12, working tree on `game` (adds the ship-portrait offscreen scopes:
`renderer_portrait_begin`/`_thumb_begin`/`_end`, vtable 31 → 34)

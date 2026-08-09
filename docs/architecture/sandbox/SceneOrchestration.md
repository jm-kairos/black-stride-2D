# SceneOrchestration

**Responsibility:** Owns the order of the frame's world-drawing passes, and the assembly and
submission of the frame's lighting. That is deliberately its whole scope: `render_scene` is 32
lines of sequencing, and `submit_frame_lighting` is the single place the engine's lighting state
is set. It explicitly does not draw anything itself, does not own any of the passes it calls,
and does not own UI panels or frame timing — `scene_renderer.h` states those stay in
`game_render`.

**Public interface:** `sandbox/source/render/scene_renderer.h` — `render_scene`.
`sandbox/source/render/frame_lighting.h` — `submit_frame_lighting`.
Both are called only by FrameOrchestrator; `frame_lighting.h` is additionally invoked from
within `render_scene`.

**Depends on:** GalaxyMapRendering, Backdrop, SystemContentRendering, ShipRendering,
InWorldOverlays, CelestialParallax, CelestialFx, RenderLayerTable, GameStateModel; engine
`renderer/renderer.h`, `defines.h`.
**Depended on by:** FrameOrchestrator.

**Key invariants:**
- **The pass order is a hard dependency, not a preference.** `scene_renderer.h` states the
  reason it exists: `draw_parallax_background` writes `s->star_pos`, and both
  `submit_frame_lighting` and `draw_ship_scene` read it. Reordering silently produces
  one-frame-stale lighting rather than an error. Expressing the order in one place instead of
  leaving it implicit in `game_render`'s call sequence is the module's entire justification.
- **`submit_frame_lighting` must run after the star position is established and before the lit
  sprite passes** — stated in `frame_lighting.h` and satisfied by its position in
  `render_scene`.
- **Lighting is submitted exactly once per frame, from one place.** The four engine calls
  (`renderer_set_lights`, `_set_glow_params`, `_set_bloom_enabled`, `_set_bloom_params`) appear
  nowhere else in the sandbox.
- **`bloom_threshold` is a real threshold again.** The engine's offscreen targets are `RGBA16F`,
  so scene luminance can exceed 1.0 and the value this pass forwards actually gates something.
  While the targets were 8-bit the shipped default of 1.2 was unreachable and bloom contributed
  nothing on every frame — a threshold above 1.0 now means "only genuinely hot pixels bloom"
  rather than "no pixels bloom". No code here changed; the number simply started working.
- **Everything cross-fades on `s->view_arena_w`, not on discrete modes.** Star light and ambient
  fade in by map weight (`1 - arena_w`) while dynamic bloom fades in by arena weight — two
  effects moving in opposite directions across one band. `frame_lighting.cpp` states the
  endpoints: at `map_w == 0` the arena is fullbright; at `map_w == 1` the map reproduces the
  original volumetric-lit look exactly.
- **The star light is reparallaxed to match the drawn star** via `celestial_center_render` with
  `depth_star`, so lighting and visuals agree. Omitting it would show as a subtle misalignment
  rather than an obvious failure.
- `submit_frame_lighting` reads `s->celestial_anchor`, which `game_render` computes earlier in
  the same frame — a precondition it cannot enforce.

**Extension points:** **A new world pass** is a function in its own `render/` module plus one
call placed correctly in `render_scene` — and, if it produces state a later pass consumes, a
note in `scene_renderer.h`'s ordering comment. Follow the existing convention of including the
peer header explicitly rather than relying on the `game.h` cascade; `scene_renderer.cpp` does
this and says so. **A new light source** is an entry appended to the `frame_lights[16]` array in
`submit_frame_lighting` before the `renderer_set_lights` call.

**Known limitations / tech debt:**
- **`scene_renderer.h`'s documented order is already out of date with its own `.cpp`** — the
  comment lists five passes (galaxy-map look → parallax background → frame lighting → ships →
  gameplay overlays) and omits the four `system_content_render` passes the implementation also
  calls.
- **The camera is not stable across the sequence.** `render_scene` sets it once at the top, but
  `draw_parallax_background` and, transitively, `GlobalBackground::draw` both change and restore
  it — two layers of camera save/restore, each correct only in combination.
- **`16` is hardcoded as the light array bound** in `frame_lighting.cpp`, matching the engine
  backend's `BS_BACKEND_MAX_LIGHTS` by coincidence of literal rather than a shared constant —
  the engine does not export its cap.
- **`LAYER_UI` is passed as the engine's `unlit_layer` threshold**, so the sandbox's layer
  numbering decides which sprites render fullbright. Changing `LAYER_UI` silently changes
  lighting.
- The star light always occupies slot 0 when present, silently displacing one editor light if
  the player has 16.
- `PROF_STARS` wraps only the galaxy-map pass; the other passes manage their own zones or none,
  so the profiler's render breakdown is incomplete.
- Both files are tagged "extracted verbatim from `game_render`", so the grouping is historical
  rather than designed — `frame_lighting` and `scene_renderer` are together here because both
  are frame-level sequencing, not because they share code.

**Source paths:** `sandbox/source/render/scene_renderer.{cpp,h}`,
`sandbox/source/render/frame_lighting.{cpp,h}`

**Last verified:** 2026-08-09, working tree on `game` (no code change here; `bloom_threshold`
became a live control once the engine's offscreen targets moved to `RGBA16F`)

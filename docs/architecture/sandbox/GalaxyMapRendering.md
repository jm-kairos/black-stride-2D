# GalaxyMapRendering

**Responsibility:** Owns the galaxy-map look — Voronoi territory and travel lanes, far-system
dots, star sunbursts, planets and orbit rings, map entities, jump and sensor range rings — and,
unusually for a render module, the two cursor hit-tests that answer "what is under the pointer"
on the map. It explicitly does not own the arena-side star draw (Backdrop's mapped-system layer
draws the same star from the other side of the blend), does not own the Voronoi diagram itself
(Territory), and does not own the tooltip's *display* — it computes the string; the RmlUi HUD
renders it.

**Public interface:** `sandbox/source/render/galaxy_map_render.h` — `draw_galaxy_map_look`;
`galaxy_pick_planet`; `galaxy_map_hover_tooltip`. It also **defines** the four star-scaling
constants `STAR_MIN_SCREEN_RADIUS`, `STAR_DIST_SCALE_FACTOR`, `STAR_MAX_DIST_SCALE` and
`STAR_HERO_MAP_MIN_RADIUS`, which are declared `extern` in `state/game_state.h` and read by
Backdrop. Used from outside by 3 subsystems.

**Depends on:** GalaxyGeneration, GalaxyRuntime, GalaxyHistory, MacroMissions, Territory,
CelestialParallax, CelestialFx, CoordinateFrames, RenderLayerTable, GameStateModel; engine
`renderer/renderer.h`, `renderer/camera2d.h`, `renderer/bs_ui.h`, `renderer/bs_imgui.h`,
`renderer/bs_rml.h`, `core/input.h`, `core/memory/bs_memory.h`, `defines.h`.
**Depended on by:** SceneOrchestration, RtsControl, FrameOrchestrator.

**Key invariants:**
- **`galaxy_pick_planet` must mirror the Pass-2 planet draw math exactly** — same parallax
  anchor, same per-type render size, same ≥6 px orbit LOD gate — so a hit corresponds to what
  the player actually sees. The header states this as a contract; any change to the draw path
  must be mirrored here.
- **The returned cache slot is valid only for the current frame.** The header says so and tells
  callers tracking a planet across frames to stash the system's `galaxy_center` instead — the
  clearest statement anywhere of how GalaxyRuntime's hot cache invalidates indices.
- **`galaxy_map_hover_tooltip` computes but does not draw**, and is deliberately called from the
  *update* path (`game_push_hud`) so the RmlUi HUD consumes it the same frame with no cursor
  lag. A render-module function invoked outside the render pass, by design.
- **The whole pass is bracketed in `renderer_set_draw_alpha(map_w)` and early-outs when
  `view_arena_w >= 1`**, making it the mirror of Backdrop's arena-weighted draw. Together the
  two produce one continuous star across the blend band.
- **Candidate stars are ranked by computed "prominence" and only the top few kept**, because the
  engine backend silently drops queue overflow — the game implements the selection policy the
  engine's fixed queue would otherwise apply arbitrarily.
- Map picking is suppressed while a UI panel owns the cursor, via `bs_imgui_wants_mouse` and
  `bs_rml_wants_mouse`.

**Extension points:** A new map overlay is a pass inside `draw_galaxy_map_look`, drawn on a
`LAYER_*` constant and faded by `map_w` so it participates in the cross-fade. A new hit-test
follows `galaxy_pick_planet`: mirror the corresponding draw math, return a frame-scoped slot
plus an index, and document the lifetime. A new tooltip section is a branch in
`galaxy_map_hover_tooltip`, which already runs a map-entity hit-test first and falls back to the
star-system test.

**Known limitations / tech debt:**
- **1734 lines — the largest render file in the sandbox**, with `draw_galaxy_overview` alone
  around 550 lines.
- **The second-highest fan-out in the sandbox (20 project includes)**, reaching into galaxy
  generation, history, missions, Voronoi, star-system generation, celestial parallax, cursor,
  view transforms, input, memory and three engine UI facades.
- **`MAX_SUNBURST_STARS` is declared locally as 4 with a comment to "keep in sync with
  `BS_MAX_SUNBURST_STARS` in the backend"** — a duplicated capacity constant across the
  engine/game boundary, because the engine does not export its cap.
- **It writes into another module's persistent state**, setting `star_fx.streak_length_mul` and
  `_intensity_mul` from the current star's radius and luminance.
- **It defines four constants declared in `state/game_state.h`** and read by Backdrop — so a
  render module supplies symbols the god struct promises, and the arena/map visual agreement
  depends on both files.
- It is **the only render module the simulation side calls into for answers** rather than
  drawing: `sim/rts_controls.cpp` includes this header for `galaxy_pick_planet`, an inverted
  dependency.
- A duplicated comment line at the top of `galaxy_pick_planet` shows a copy-paste artifact in
  the documentation.
- The four star-scaling rules (min screen radius, hero-star floor, edge-aberration `dist_scale`,
  per-planet clamps) must match `mapped_system_layer.cpp`'s independently-implemented copies at
  the blend boundary.

**Source paths:** `sandbox/source/render/galaxy_map_render.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`

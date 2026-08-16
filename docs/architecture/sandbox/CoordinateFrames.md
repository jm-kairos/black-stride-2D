# CoordinateFrames

**Responsibility:** Owns every conversion between the coordinate frames the game works in —
screen pixels, render space, true (simulation) world space, the galaxy-scale hierarchical frame,
and per-system local frames — plus the zoom-driven arena/map blend weight that render sites read
instead of branching on view mode. It explicitly does not own the camera itself (that is
`game_state::camera_state`, driven by CameraControl), does not own backdrop depth parallax (that
is CelestialParallax, split out because four render clusters consume it), and does not own the
engine's `camera2d_*` primitives it builds on.

**Public interface:** `sandbox/source/core/view_transform.h` — `view_arena_weight`;
`game_screen_to_true_world`, `game_true_world_to_render`, `game_camera_center`; the
precision-safe forms `game_screen_to_true_hierpos`, `render_from_hierpos`,
`game_camera_center_hierpos`; and the mutable global `g_zoom_out_speed_gain`.
`sandbox/source/core/cursor_world.h` — `mouse_world`, `mouse_true_world`, `mouse_true_hierpos`.
`sandbox/source/core/galaxy_coords.h` — `find_nearest_system`, `galaxy_to_system_local`,
`world_to_galaxy_pos`, `get_system_zone`.
`view_transform.h` is included by **11 other subsystems**, the most-shared sandbox header;
`cursor_world.h` by 7; `galaxy_coords.h` by 1 (FrameOrchestrator only).

**Depends on:** GameStateModel; engine `math/bs_hierpos.h`, `renderer/camera2d.h`,
`core/input.h`, `defines.h`.
**Depended on by:** Backdrop, CameraControl, CoordinateDiagnostics, DevPanels,
FrameOrchestrator, GalaxyMapRendering, InWorldOverlays, RtsControl, ShipRendering,
SystemContentRendering, Territory, FleetControl, WorldEditor.

**Key invariants:**
- **The renderer draws entities at `world − camera_hierpos`.** Every transform here applies or
  inverts exactly that, and `view_transform.h` states it as the module's premise. All render
  sites route through these functions so stars, ships, Voronoi cells and hover FX cannot drift
  apart — a discipline enforced by convention, not by the type system.
- **`game_camera_center` reconstructs the true centre as `camera_hierpos + camera.position`**,
  treating `camera.position` as a small render-space residual. That relationship between the two
  camera representations is documented only in `view_transform.cpp` and is what keeps
  `free_camera_pos` valid when stored.
- **The `Vec2` and `HierPos2` transform pairs are not equivalent.**
  `game_true_world_to_render` collapses the camera hierpos to `Vec2` and subtracts in `f32`;
  `render_from_hierpos` subtracts in integer cell space. Both are offered and picking the wrong
  one silently reintroduces the precision loss the coordinate refactor removed. **No
  compile-time distinction.**
- **`view_arena_weight` smoothsteps across `[VIEW_MAP_ZOOM 0.009, VIEW_ARENA_ZOOM 0.026]`, and
  the band MUST straddle `ZOOM_MIN` (0.015) in `sim/camera_controller.cpp`** so the two looks
  cross-fade around the label flip. That is a different constant in a different file with nothing
  but the comments at both sites tying them: move one without the other and the label flips at a
  zoom where the looks have already finished cross-fading, or vice versa. Widened from
  `[0.05, 0.14]` so the compressed ballistic engagement envelope frames inside the arena look.
- **`get_system_zone` hardcodes the player as `s->galaxy.map_entities[0]`** — index 0 is assumed
  to be the player fleet. Stated nowhere and invisible at the call site.
- Zone numbering is outside-in (0 = beyond the outermost orbit), the inverse of the intuitive
  convention; documented in `galaxy_coords.h` and nowhere else.

**Extension points:** A new frame conversion is a declaration in `view_transform.h` plus an
implementation that goes through `hierpos_diff` / `hierpos_add_vec2` rather than absolute `f32`
subtraction — every existing precision-safe function follows that shape. A new cursor-derived
query belongs in `cursor_world.h` and should be built on `game_screen_to_true_hierpos` rather
than `camera2d_screen_to_world` directly (that is the distinction between the legacy and unified
paths the header describes). `view_transform.h`'s "pure functions of zoom only" tier is the
place for anything that needs no `game_state`.

**Known limitations / tech debt:**
- **`g_zoom_out_speed_gain` is a mutable global exposed in the header**, written by an editor
  slider in DevPanels (`ui/editor_ui.cpp`) and read by CameraControl
  (`sim/camera_controller.cpp`). Three subsystems share a variable with no accessor.
- Every `game_*` transform dereferences `s` with **no null check**.
- `mouse_world`, `mouse_true_world` and `mouse_true_hierpos` each call the engine's input
  singleton (`input_get_mouse_position`) rather than taking a cursor position, so they are
  implicit reads of live global state; calling one twice in a frame can yield different answers
  if the pump ran between.
- `mouse_world` returns render space while its two siblings return true world. The comments name
  the intended callers of each ("edit picking, piloting/aiming"), but choosing wrong compiles
  and is subtly incorrect.
- `find_nearest_system` recomputes the ship's world coordinates **inside** its loop, converting
  once per candidate system rather than once overall.
- `get_system_zone` sorts orbit radii with a bubble sort into a fixed `MAX_SYSTEM_PLANETS` stack
  array justified in-comment by "N <= 5"; the loop is bounded by `planet_count`, so a system
  exceeding that constant would overflow.
- `galaxy_coords.h`'s three pure conversions take explicit coordinates while `get_system_zone`
  takes `game_state*` — an asymmetry marking it as the only one bound to live state.
- `celestial_parallax` is arguably a member of this subsystem (it is a coordinate transform) but
  was split out because its consumers are all render-side. *Inferred:* that the split is the
  right cut; the code offers no statement either way.

**Source paths:** `sandbox/source/core/view_transform.{cpp,h}`,
`sandbox/source/core/cursor_world.{cpp,h}`, `sandbox/source/core/galaxy_coords.{cpp,h}`

**Last verified:** 2026-08-09, commit `b1baf31` (cross-fade band widened with
`ZOOM_MIN`)

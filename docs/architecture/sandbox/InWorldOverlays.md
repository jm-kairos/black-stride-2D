# InWorldOverlays

**Responsibility:** Owns the in-world gameplay feedback layer — projectile rendering, RTS
selection, the enemy reticle, NPC role markers, unidentified-contact glyphs, sensor rings and
radar contacts, point-defense beams, the out-of-sensor-range contact effect, edit-mode
highlights and gizmos, travel debug, range rings, and the radiation heat map. It explicitly does
not own the *decisions* any of these visualise — sensor detection is CombatArena's, PD beams are
recorded by `point_defense.cpp`, gizmo geometry is WorldEditor's — and it does not own ship
hulls or their annotations (ShipRendering).

**Public interface:** `sandbox/source/render/gameplay_overlays.h` — `draw_gameplay_overlays`
(the dispatcher). `sandbox/source/render/projectile_marker.h` — `projectile_markers_draw`:
screen-constant visibility markers for in-flight shots. `sandbox/source/render/weapon_hub.h` — `weapon_hub_update`, `_close`,
`_draw`: the middle-mouse weapon micro-selection hub. It is the one member of this subsystem
that also *reads input* — `game_update` owns the gating and calls `_update`, the dispatcher
calls `_draw` — because the flick hit-test and the tile layout must share one geometry.
`sandbox/source/render/sensor_overlay.h` — `sensor_overlay_draw`,
`g_sensor_fade_distance`. `sandbox/source/render/defense_laser_overlay.h` —
`defense_laser_overlay_draw`. `sandbox/source/render/out_sensor_detection_fx.h` —
`struct OutSensorDetectionFX` (a `game_state` member). `sandbox/source/sim/heat_map.h` —
`draw_ship_metaballs`.
Used from outside: `gameplay_overlays.h` by 2, the others by 1 each.

**Depends on:** CoordinateFrames, CombatArena, RenderLayerTable, WorldEditor, ShipRendering,
GalaxyHistory, LocalAgentAi, BitmapText, ShipCombatModel, FleetControl, ActionLog,
GameStateModel; engine `renderer/renderer.h`, `renderer/camera2d.h`, `core/input.h`,
`math/math_utils.h`, `renderer/renderer_types.h`, `defines.h`.
**Out-degree 23 — the highest in the render tier.** FleetControl, ActionLog and `core/input.h`
all arrive through `weapon_hub.cpp`, which polls a mouse button and writes an action-log line.
**Depended on by:** SceneOrchestration, DevPanels, FrameOrchestrator, GameStateModel.

**Key invariants:**
- **It must run after ShipRendering.** Several sub-passes read `Ship::render_pos` and
  `CombatEntity::render_pos`, which `draw_ship_scene` writes; `out_sensor_detection_fx.cpp`
  notes its dependency in a trailing comment. Enforced only by the order in `render_scene`.
- **`draw_gameplay_overlays` is drawn in ALL looks.** The header states the reason: under the
  unified coordinate space gameplay is continuous across the arena↔map blend, so these overlays
  must not pop at a mode boundary. That is why the pass is unconditional rather than
  mode-gated, and why `rts_controls.draw()` runs exactly once here (the comment records it was
  previously duplicated per mode).
- **Projectile markers are cosmetic by construction, and fade on APPARENT STREAK LENGTH.**
  `projectile_markers_draw` takes a `const game_state*` so the compiler enforces that it only
  reads the pool and submits draws — it cannot touch spawn/retire, point defense, collision,
  damage or the flight model, and removing its one dispatcher call is a zero-behaviour edit.
  The fade keys off `max(speed * 0.04, radius * 4) * zoom`, which mirrors
  `ProjectileSystem::render`'s own `trail_length`, so retuning the streak moves the marker with
  it. Above 24 px of streak nothing is submitted at all, which is why the pass costs nothing at
  arena zoom. Every `ProjectileKind` gets the identical dot-and-tracer at the identical screen
  size; only the tint follows the shot's own colour, so friend/foe stays readable.
- **The hub offers the ACTIVE FIRE GROUP, never the whole hull.** Its seven directional slots
  are `ship_nth_group_weapon(ship, active_group, n)`, so switching groups with the number row
  changes what the hub shows, and a weapon outside the active group has no tile and cannot be
  committed — `weapon_hub_update` rejects the release on the same `hp < 0` that draws the tile
  crossed out. Slots fill `N, E, W, NE, NW, SE, SW`, which keeps a group of three or fewer on
  the cardinals exactly as it was before the diagonals existed; a group larger than seven
  reaches the remainder through "All", which is what the hint under South says.
- **The hit-test picks the angularly nearest slot, not a fixed sector.** Uniform 45° sectors
  cannot work with 124×46 tiles: a corner sits ~22° off horizontal, so a flick straight at the
  NE tile would land in East's sector. Comparing against each slot's own `hub_slot_px`
  direction puts every boundary on the drawn bisector, at the cost of deliberately uneven
  sectors (N wide, NE narrow). The 3×3 grid has the same bounding box the old four-tile diamond
  had, so `hub_center_px`'s edge clamp did not move.
- **The weapon hub is anchored on the press point, not a fixed screen position**, and clamped
  so a press near an edge slides the whole thing inward rather than cropping a slot. The frozen
  aim target is that same point, which is what makes the per-tile readout meaningful: the hub
  sits on what is being shot at and each tile answers "can this weapon hit *here*". Tile
  opacity is per-state (`hub_look_opacity`), so an out-of-range slot recedes as a whole instead
  of just losing its border — but a *selected* slot keeps a high-opacity border and label,
  because out-of-range weapons stay selectable and the player must see which one is armed.
- **The weapon hub is NOT mode-gated either**, for the same reason and one more: zooming past
  `ZOOM_MIN` both flips `view.mode` and force-detaches the camera
  (`sim/camera_controller.cpp`), so gating the hub's *input* on `view.mode` or
  `free_camera_active` would kill it exactly when the player zooms out to engage something far
  away — and the override drives the autopilot attack order, which is how that fight is
  actually run. `game_update` calls `weapon_hub_update` above the piloting branch, gated only
  on the editor (which owns middle-mouse for camera pan), the flagship inspector, and the two
  UI cursor-capture checks. It survives an arena↔map crossing mid-hold.
- **Radar blips are back-projected to the last sweep tick.** `snapshot_contacts_to_last_sweep`
  rewrites each unidentified contact's position along its velocity so blips step rather than
  slide; the comment explains this works without history because ballistic contacts move at
  constant velocity. Contacts at Layer 1 or closer are exempted and track live.
- **`draw_glow_line` sets `custom.w`, not `custom.x`.** Deliberate and commented: `custom.x`
  drives heat distortion in `sprite.frag.hlsl`, which would dirty the overlay. Channel meanings
  exist only in HLSL.
- **The heat map works in a camera-relative true-world frame** — every source is translated by
  `-camera_hierpos` — which keeps it precision-safe far from the origin while leaving the
  on-screen result identical, because the shader only uses relative offsets.
- **Gizmo hover colouring calls `edit_pick_gizmo`** rather than duplicating the hit-test, so
  visual feedback and pick logic share one source of truth.
- **The weapon reach ring uses the *shortest* reach in the current fire selection**, so the
  ring marks the distance inside which everything that fires on the trigger can land a shot.
  The selection is `ship_hardpoint_in_selection`, not the group mask — under a micro-selection
  override the ring collapses to that one weapon's reach and turns green to say so.

**Extension points:** A new overlay is a `draw_*` function in its own `render/` file plus one
call in `draw_gameplay_overlays` — the dispatcher already aggregates six such modules. Draw on
`LAYER_UI` (or `LAYER_GIZMO` for editor affordances), route positions through
`render_from_hierpos`, and remember the engine's two thickness conventions: radii must be
divided by zoom manually, while `renderer_draw_line`/`_circle` thickness is already in screen
pixels. A new heat source kind is an entry appended to the `MBSource` array in
`draw_ship_metaballs` with `is_detector` set appropriately.

**Known limitations / tech debt:**
- **`gameplay_overlays.cpp` is a dispatcher as much as a renderer**, with 17 project includes
  reaching into three simulation subsystems.
- **A render pass mutates simulation state**: it assigns
  `s->projectiles.glow_override = &s->render.bullet_glow` before rendering, writing a pointer
  into the projectile pool that the engine later compares by *identity* for batch breaking.
- **`heat_map.cpp` lives under `sim/` but is purely a render submission**, and its function is
  still named `draw_ship_metaballs` while everything around it says "heat map".
- **`HEAT_FADE_ZERO_ZOOM` is annotated "== ZOOM_GLOBAL_MIN" but the two do not match** — the
  camera controller's floor is 7.5e-10, this is 4e-6. A cross-module constant that drifted from
  the value its comment claims to track.
- **The PD gate fractions `{0.6, 0.8, 1.0}` appear here as a third copy**, alongside
  `sim/point_defense.cpp` and the engine's `renderer/bs_rml.h`.
- `sensor_overlay.cpp` uses a function-local `static SensorContact contacts[MAX_PROJECTILES]` —
  a hidden global sized by the projectile pool, resident for the process lifetime and
  non-reentrant.
- The blip snapshot's constant-velocity assumption is stated for ballistic contacts but the game
  also fires **guided missiles**, whose accelerating paths would be reconstructed incorrectly.
- `out_sensor_detection_fx.cpp` defines a private `LAYER_FX = 15` and uses five consecutive
  layers, none named in `core/render_layers.h`; it also duplicates
  `sensor_visibility_from_dist`, which exists as a shared function in `sim/galaxy_map.cpp`.
- `out_sensor_detection_fx.h`'s default member initialisers disagree with its `init()`
  (`color` `{1,0.25,0.25,1}` vs `{1,0.45,0.45,1}`), so the value depends on whether `init` ran.
- `sensor_overlay.h` exposes a mutable tuning global (`g_sensor_fade_distance`) written by
  DevPanels.
- `draw_glow_circle` emits one sprite per segment, so the out-of-range contact effect alone
  costs roughly 110 sprites per frame against the shared 16384 budget.
- `sensor_overlay.cpp` inverts the camera to draw screen-space chevrons through a world-space
  API (`draw_screen_line`).

**Source paths:** `sandbox/source/render/gameplay_overlays.{cpp,h}`,
`sandbox/source/render/weapon_hub.{cpp,h}`,
`sandbox/source/render/projectile_marker.{cpp,h}`,
`sandbox/source/render/sensor_overlay.{cpp,h}`,
`sandbox/source/render/defense_laser_overlay.{cpp,h}`,
`sandbox/source/render/out_sensor_detection_fx.{cpp,h}`, `sandbox/source/sim/heat_map.{cpp,h}`

**Last verified:** 2026-08-07, commit `e83a88d` + the weapon-hub group-filter change (hub slots
now follow the active fire group; 4 slots → 8, nearest-direction hit-test)

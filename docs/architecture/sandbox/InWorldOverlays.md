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
`sandbox/source/render/projectile_fx.h` — `projectile_fx_render_init`, `projectile_fx_draw`,
`projectile_fx_draw_charges`: the launch and termination halves of the weapon-fire VFX (muzzle
flashes, impacts, flak airbursts, point-defense intercepts), plus the pre-fire charge-up glow on
heavy weapons. The *travel* half is not here — an in-flight streak is a
function of the live projectile's velocity, so `ProjectileSystem::render` regenerates it from
the pool each frame (ShipCombatModel). The events these two ends produce outlive the projectile
they belong to, which is why they need storage of their own.
`sandbox/source/render/sensor_overlay.h` — `sensor_overlay_draw`,
`g_sensor_fade_distance`. `sandbox/source/render/defense_laser_overlay.h` —
`defense_laser_overlay_draw`. `sandbox/source/render/out_sensor_detection_fx.h` —
`struct OutSensorDetectionFX` (a `game_state` member). `sandbox/source/sim/heat_map.h` —
`draw_ship_metaballs`.
Used from outside: `gameplay_overlays.h` by 2, the others by 1 each.

**Depends on:** CoordinateFrames, CombatArena, RenderLayerTable, WorldEditor, ShipRendering,
GalaxyHistory, LocalAgentAi, BitmapText, ShipCombatModel, FleetControl, ActionLog,
GameStateModel, `core/projectile_fx.h`; engine `renderer/renderer.h`, `renderer/camera2d.h`,
`core/input.h`, `math/math_utils.h`, `renderer/renderer_types.h`, `defines.h`.
*(`core/projectile_fx.h` is a Tier 0 POD ring with no sandbox dependencies of its own, shared
with ShipCombatModel — the sim writes it, this subsystem reads it. It is deliberately not a
subsystem page: it is one fixed-capacity event buffer and a lifetime table.)*
**Out-degree 37 — still the highest in the render tier by a wide margin** (next is ShipRendering
at 15). FleetControl, ActionLog and `core/input.h`
all arrive through `weapon_hub.cpp`, which polls a mouse button and writes an action-log line.
**Depended on by:** SceneOrchestration, DevPanels, FrameOrchestrator, GameStateModel. *(The
RtsControl edge — `fleet_roster_wants_mouse`, an inverted `sim/` → `render/` dependency — died
with the roster's migration to the RML HUD.)*

**Key invariants:**
- **Self-drawn panels must be asked whether they own the cursor.** `weapon_hub` draws in screen
  space over the world, so neither ImGui's nor RmlUi's `wants_mouse` arbitration covers it —
  `game_update` owns its gating instead. *(The fleet roster was the second such panel; it
  migrated to the RML HUD, whose `pointer-events: auto` panel makes `bs_rml_wants_mouse`
  answer for it, and its per-panel query is gone.)* Any new self-drawn panel needs explicit
  arbitration again.
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
- **Weapon fire draws BELOW the bloom threshold, and pays for it with `custom.z`.** Shots and
  their effects sit on `LAYER_PROJECTILE` (12) and `LAYER_PROJECTILE_FX` (13), not `LAYER_UI`.
  `LAYER_UI` *is* `BS_LAYER_BLOOM_THRESHOLD`, so for as long as projectiles drew there every
  tracer and impact in the game was composited after the bloom pass and could not bloom at all
  — they read as flat coloured decals rather than as anything hot. The catch is that the same
  number is also the `unlit_layer` cutoff `frame_lighting` hands to `renderer_set_lights`, so
  dropping below it hands these sprites to the galaxy-map look's star light and bright ambient.
  Every sprite on both layers therefore sets `custom.z = 1`, the shader's self-emissive flag,
  which skips the scene-lighting branch entirely. Miss that on a new effect and it will look
  correct in the arena (fullbright, no lights submitted) and wash out on the map side only.
  `s->render.bloom_enabled` now defaults to **TRUE** (`game.cpp`), which is what makes the layer
  choice pay off — it was FALSE, and the two facts cancelled: shots sat below the threshold and
  the pass they were placed there for never ran. Turning it on is narrower in practice than it
  sounds, because `bloom_threshold` is 1.2: the static scene is pixel-identical with it on or
  off at combat framing, and only weapon fire is bright enough to cross the cut. The effects are
  still tuned to carry themselves unaided, so the editor toggle remains a look preference rather
  than a switch that turns combat legibility off.
- **Three visual families, not six looks.** `render/projectile_fx.cpp` branches muzzle and
  impact geometry on `ProjectileFxEvent::family` (a `VfxFamily` from the firing weapon's def);
  `ProjectileSystem::render` branches the travel component on the same value. The split is
  `shell` / `slug` / `ordnance` because those are distinctions the *simulation* already makes —
  guided vs unguided, powered vs inert — so it costs two extra branches per pass rather than a
  parameter set per weapon. What varies is the physics being depicted: a shell launches on
  expanding propellant gas, a slug on an electromagnetic snap (so it gets a needle-thin cone and
  almost no blast ring), and ordnance on rocket ignition (the broadest ring of the three, plus a
  pulsing engine plume drawn for its whole flight). Flak bursts and PD intercepts are
  deliberately *not* family-varied: a burst is a fused round's own behaviour, and an intercept is
  tinted by the defender that killed it, not by the weapon that fired it.
  Each family also carries **its own `bs_glow_params`** (`game_state::render::projectile_glow`,
  indexed by `VfxFamily`), which is where the colour lives: the shader ramps
  `temp_cool -> temp_warm -> temp_hot` along a sprite whenever `custom.x > 0`, so a slug reads
  cold blue and a missile plume reads as a burning motor with roughly double the heat-distortion
  amplitude. Charge-up, muzzle flash, in-flight round and screen-marker all resolve to the *same*
  pointer for a given family, which keeps one weapon's whole life-cycle in one colour language
  and in one draw run. This only became worth doing after the scene went HDR — on 8-bit targets
  a saturated tint clipped to white in the composite and all three families converged there.
- **The curved trail is exclusive to guided rounds, and that exclusivity is what pays for it.**
  `ProjectileSystem::render` threads a chain of quads through a missile's recorded positions
  instead of drawing the velocity-aligned streak (the streak is suppressed outright while the
  chain is live, or the straight quad would poke out of the arc on every turn). At 7 quads per
  round this is affordable only because missiles are rare: the same technique applied across the
  512-slot pool would be ~3,500 sprites of a 16,384 frame budget.
  *Chain width is its own constant, not the streak's.* At combat framing a 6-unit missile drawn
  at the streak's width is ~1.5 px across, and a Gaussian cross-section that narrow rasterises to
  a dashed line — the arc was present and unreadable until the chain got its own wider stroke.
- **The charge-up is gated on CYCLE TIME, not on a weapon list, and it never draws while idle.**
  `projectile_fx_draw_charges` completes the reference document's `charge -> flash -> launch`
  sequence from the front, but only for weapons whose whole cycle exceeds `CHARGE_MIN_CYCLE`
  (1 s): a build-up shorter than the gap between shots reads as a permanently lit barrel, which
  is what a 12-shot-per-second autocannon would produce. That gate falls out of authored stats,
  so a new weapon sorts itself. The glow is drawn *only while still cooling* — never on a loaded,
  idle gun. Under a held trigger the weapon fires the instant it is ready, so the build hands
  straight to the muzzle flash; at rest it shows nothing, because "this gun is loaded" is a
  readiness readout the weapon hub already owns from the same `cooldown_progress()` call.
  Which barrel lights follows the fire pattern: SALVO charges every muzzle, SEQUENTIAL only the
  one next in rotation.
  *The charge must stay SMALLER than the flash it sets up.* The first pass sized it off the
  hardpoint half-extent at 5.6x, putting a 336-unit halo in front of a 139-unit muzzle flash —
  the gun's brightest moment was its wind-up. Brightness hierarchy applies across time, not only
  within a single effect.
- **The FX pass cannot touch the simulation, and the compiler enforces it.** `projectile_fx_draw`
  takes a `const game_state*` — the same guard `projectile_markers_draw` uses — so it can only
  read the event ring and submit sprites. Deleting its one dispatcher call is a zero-behaviour
  edit, and so is nulling `ProjectileSystem::fx` on the producing side.
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
- **The weapon hub is NOT mode-gated either**, for the same reason and one more: the override it
  commits is read by *both* fire paths — the player's left-button ballistic trigger, which now
  runs in both control modes, and the autopilot's guided-ordnance engagement — so gating the
  hub's *input* on `view.mode` or `free_camera_active` would kill it in a mode where it is still
  load-bearing. `game_update` calls `weapon_hub_update` above the piloting branch, gated only on
  the editor (which owns middle-mouse for camera pan), the flagship inspector, and the two UI
  cursor-capture checks. It survives an arena↔map crossing mid-hold.
  *(The older form of this argument leaned on zoom force-detaching the camera past `ZOOM_MIN`.
  It no longer does — zoom and the control mode are decoupled — but the conclusion is unchanged
  and now rests on the override mattering in both modes rather than on a coupling.)*
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
call in `draw_gameplay_overlays` — the dispatcher already aggregates eight such modules. Draw on
`LAYER_UI` (or `LAYER_GIZMO` for editor affordances), route positions through
`render_from_hierpos`, and remember the engine's two thickness conventions: radii must be
divided by zoom manually, while `renderer_draw_line`/`_circle` thickness is already in screen
pixels. A new heat source kind is an entry appended to the `MBSource` array in
`draw_ship_metaballs` with `is_detector` set appropriately.

**Known limitations / tech debt:**
- **`gameplay_overlays.cpp` is a dispatcher as much as a renderer**, with 17 project includes
  reaching into three simulation subsystems.
- ~~**A render pass mutates simulation state**: it assigns `s->projectiles.glow_override`
  before rendering~~ — **fixed**: the per-family glow table is built here and passed to
  `ProjectileSystem::render` as an argument, so the pool no longer carries render state. The
  pointers still have to be long-lived `game_state` storage, because the backend retains them
  until `end_frame` and compares them by *identity* for batch breaking.
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
*(`render/fleet_roster.{cpp,h}` deleted — the roster is RML HUD markup in `assets/ui/hud.rml`
now, filled by `game_push_hud`)*,
`sandbox/source/render/projectile_marker.{cpp,h}`,
`sandbox/source/render/projectile_fx.{cpp,h}`,
`sandbox/source/render/sensor_overlay.{cpp,h}`,
`sandbox/source/render/defense_laser_overlay.{cpp,h}`,
`sandbox/source/render/out_sensor_detection_fx.{cpp,h}`, `sandbox/source/sim/heat_map.{cpp,h}`

**Last verified:** 2026-08-11, working tree on `game` (`render/fleet_roster` removed — the
roster is an RML HUD panel now, leaving `weapon_hub` the only input-reading member;
`render/projectile_fx` unchanged — three visual families with per-family glow params, the
heavy-weapon charge-up and the guided-round curved trail; weapon fire stays on the two
bloom-eligible layers)

# Sandbox subsystems — proposed clustering

A proposed grouping of the 128 files under `sandbox/source` into subsystems, each with a
single responsibility statable in one sentence.

**Status:** proposal. Four grouping decisions were put to the author and are settled (see
"Settled calls"); the rest follow from the measured edge structure.

**Inputs:** `_raw/dependency-graph.json` (side == "sandbox", plus `boundary_edges`) and
`_raw/file-summaries-sandbox.md`.

**Method note — why directories don't work here.** `game.h` → `state/game_state.h` is included
by 48 files, so it dominates the include graph: **27 of the 32 subsystems depend on it**.
Clustering on raw edges therefore produces one blob. All grouping below is done on the 231
*residual peer edges* after excluding that hub. Directory layout was also rejected as a
signal — `sim/` alone spans galaxy generation, deep-time history, economy, two AI tiers,
ships, and combat, and three files sit in a directory that contradicts their role.

Regenerate every number with:

```
python tools/dependency_graph/cluster_report.py --side sandbox
```

Measured basis: 324 sandbox-internal include edges (93 intra-cluster, 231 cross),
204 sandbox→engine boundary edges reaching 15 distinct engine headers.

**Cross-check target:** the boundary table near the end should be reconciled with
`docs/architecture/engine-api-boundary.md` when that exists.

---

## Settled calls

Four groupings were ambiguous from the data alone and were decided explicitly:

1. **ShipCombatModel** merges the ship/module model with weapons and projectiles. They share
   one object graph (`Ship` owns `Weapon*` in `mounts[]`/`weapon_stash[]`, weapons are sized
   against `HardpointSize`, projectiles carry `VesselFaction` from `ship.h`), and splitting
   them produced a 4/1 cycle. **The merge removed that cycle.**
2. **GalaxyGeneration** keeps node placement and per-system generation together. Splitting
   them produced a cycle via `SSGenEnv`, which is declared in `galaxy_gen.h` and consumed by
   both `ss_generation.h` and `system_evolution.h`. **The merge removed that cycle.**
3. **LocalAgentAi** stays whole and is flagged as a god object rather than speculatively split.
4. **DevPanels** groups the three `bs_ui` panel builders despite zero shared edges between them.

---

## Layering

```
                         FrameOrchestrator          (out 43 / in 0)
                                 |
        +------------------------+------------------------+
        |            |           |            |           |
   DevPanels    rendering    galaxy sim   ship/combat   player input
        |            |           |            |           |
        +------------+-----------+------------+-----------+
                                 |
                          GameStateModel               (in 48 — the hub)
                                 |
      CoordinateFrames  RenderLayerTable  DeterministicRng  Geometry2D
      BitmapText        CelestialParallax                   Profiling
```

`FrameOrchestrator` is the only cluster nothing depends on. `GameStateModel` is depended on by
27 of 32 subsystems and is a shared data substrate rather than a subsystem.

Four cycles remain, all in the simulation tier — see "Coupling flags".

| cluster | files | intra | out | in |
|---|--:|--:|--:|--:|
| CoordinateFrames | 6 | 4 | 3 | 27 |
| DeterministicRng | 1 | 0 | 0 | 5 |
| Geometry2D | 2 | 1 | 0 | 3 |
| RenderLayerTable | 1 | 0 | 0 | 10 |
| Profiling | 2 | 1 | 0 | 1 |
| BitmapText | 3 | 2 | 0 | 5 |
| ShipCombatModel | 12 | 18 | 0 | 23 |
| FleetControl | 6 | 4 | 6 | 6 |
| CombatArena | 6 | 3 | 10 | 5 |
| LocalAgentAi | 2 | 1 | 9 | 6 |
| GalaxyGeneration | 9 | 11 | 7 | 11 |
| GalaxyRuntime | 2 | 1 | 5 | 8 |
| GalaxyHistory | 2 | 1 | 5 | 7 |
| MacroMissions | 2 | 1 | 5 | 3 |
| Economy | 2 | 1 | 2 | 3 |
| Territory | 5 | 4 | 4 | 4 |
| RtsControl | 2 | 1 | 7 | 1 |
| CameraControl | 2 | 1 | 3 | 1 |
| WorldEditor | 2 | 1 | 3 | 2 |
| Discovery | 2 | 1 | 5 | 2 |
| ActionLog | 2 | 1 | 1 | 6 |
| TravelDebug | 2 | 1 | 0 | 2 |
| SceneOrchestration | 4 | 3 | 10 | 2 |
| ShipRendering | 4 | 3 | 14 | 5 |
| Backdrop | 10 | 8 | 14 | 5 |
| CelestialParallax | 2 | 1 | 2 | 7 |
| CelestialFx | 2 | 1 | 1 | 4 |
| GalaxyMapRendering | 2 | 1 | 12 | 4 |
| SystemContentRendering | 2 | 1 | 3 | 1 |
| InWorldOverlays | 10 | 8 | 21 | 5 |
| CoordinateDiagnostics | 4 | 2 | 7 | 5 |
| DevPanels | 6 | 3 | 14 | 3 |
| *GameStateModel* | 2 | 1 | 14 | 48 |
| *FrameOrchestrator* | 2 | 1 | 43 | 0 |
| *Bootstrap* | 1 | 0 | 1 | 0 |
| *DeadStarfieldGen* | 2 | 1 | 0 | 1 |

*Italic rows are not subsystems — see "Files that don't fit".*

---

## Tier 0 — shared primitives

### CoordinateFrames — high
**Responsibility:** Converts between screen, render-space, true-world, galaxy and per-system
coordinate frames.
**Files:** `core/view_transform.{cpp,h}`, `core/cursor_world.{cpp,h}`, `core/galaxy_coords.{cpp,h}`
**Interface actually used:** `view_transform.h` by **11 clusters** — the most-shared sandbox
header; `cursor_world.h` by 7; `galaxy_coords.h` by 1 (FrameOrchestrator only).
**Depends on:** GameStateModel. **Depended on by:** 11 clusters across rendering, sim and UI.
**Engine:** `bs_hierpos.h`(3), `camera2d.h`(2), `defines.h`(2), `input.h`(1).

### DeterministicRng — high
**Responsibility:** Supplies the shared splitmix64 seed hierarchy that makes worldgen reproducible.
**Files:** `sim/galaxy_rng.h`
**Interface actually used:** the whole header, by GalaxyGeneration, GalaxyHistory, DevPanels.
**Depends on:** nothing. **Engine:** none.
Split out of GalaxyGeneration because four clusters seed from it.

### Geometry2D — high
**Responsibility:** Provides point-in-polygon and point-to-segment tests.
**Files:** `core/geom2d.{cpp,h}` · **Used by:** CombatArena, WorldEditor, FrameOrchestrator
· **Engine:** `defines.h`, `math_utils.h`.

### RenderLayerTable — high
**Responsibility:** Defines the draw-order layer constants shared by every draw site.
**Files:** `core/render_layers.h` · **Used by:** 7 clusters · **Engine:** `renderer_types.h`
(for `BS_LAYER_BLOOM_THRESHOLD`, which two of its constants derive from).

### Profiling — high
**Responsibility:** Accumulates per-frame CPU timings per subsystem zone and renders the readout.
**Files:** `core/profiler.{cpp,h}` · **Used by:** nothing via include — it is a `game_state`
member reached through the hub · **Engine:** `bs_ui.h`, `renderer.h`, `defines.h`.

### BitmapText — high
**Responsibility:** Bakes an embedded 8×8 font into a GPU atlas and draws screen-pinned strings.
**Files:** `render/text.{cpp,h}`, `font8x8.h`
**Used by:** CoordinateDiagnostics, InWorldOverlays, ShipRendering, FrameOrchestrator
**Engine:** `defines.h`(2), `logger.h`, `renderer.h`, `camera2d.h`, `math_utils.h`, `renderer_types.h`.

---

## Tier 1 — ship and combat model

### ShipCombatModel — high *(merged; see Settled calls)*
**Responsibility:** Defines ships, their mountable modules and weapons, and the projectiles
they fire.
**Files:** `sim/ship.{cpp,h}`, `sim/module.{cpp,h}`, `sim/weapon.{cpp,h}`,
`sim/weapon_def.{cpp,h}`, `sim/projectile.{cpp,h}`, `render/ship_visual.{cpp,h}`
**Interface actually used:** `ship.h` by 7 clusters; `weapon.h` by 5; `projectile.h` by 3;
`weapon_def.h` by 3; `module.h` by 2; `ship_visual.h` by 1 (LocalAgentAi).
**Depends on:** GameStateModel only — **out-degree 0** after the merge, the most self-contained
large cluster in the sandbox. **Depended on by:** 8 clusters.
**Engine:** `defines.h`(6), `math_utils.h`(5), `logger.h`(4), `renderer.h`(3),
`renderer_types.h`(3), `bs_hierpos.h`(2) — the heaviest engine consumer at 23 edges.

### FleetControl — high
**Responsibility:** Owns the player fleet's membership, selection, orders, and the locomotion
that moves ships.
**Files:** `sim/fleet.{cpp,h}`, `sim/ship_control.{cpp,h}`, `sim/steering.{cpp,h}`
**Interface actually used:** `fleet.h` by 3; `ship_control.h` by 2 (CameraControl,
FrameOrchestrator); `steering.h` by 1 — **LocalAgentAi only**, despite the header describing
itself as the shared locomotion layer.
**Depends on:** ShipCombatModel, GameStateModel. **Engine:** `math_utils.h`(3), `defines.h`(3),
`bs_hierpos.h`(2), `input.h`(1).

### CombatArena — high
**Responsibility:** Mirrors ships into combat entities and resolves detection, point-defense,
and projectile impacts.
**Files:** `sim/combat_arena.{cpp,h}`, `sim/point_defense.{cpp,h}`, `sim/sensor_system.{cpp,h}`
**Interface actually used:** `combat_arena.h` by 3; `point_defense.h` by 1 (FrameOrchestrator);
`sensor_system.h` by 1 (InWorldOverlays).
**Depends on:** ShipCombatModel, GalaxyHistory, LocalAgentAi, ActionLog, Geometry2D.
**Engine:** `defines.h`(3), `math_utils.h`(2), `bs_hierpos.h`(1).
*Carries a four-step call-order contract with point defense and projectiles, documented only
in comments.*

### LocalAgentAi — high as a boundary, flagged as a god object
**Responsibility:** Materialises and drives the transient NPC agents present in the player's
current system.
**Files:** `sim/ai_ship.{cpp,h}`
**Interface actually used:** `ai_ship.h` by 6 clusters.
**Depends on:** GalaxyHistory, MacroMissions, Discovery, Economy, GalaxyRuntime, FleetControl,
ShipCombatModel. **In three cycles.** **Engine:** `logger.h`, `defines.h`.
*2524 lines covering population management, a behaviour FSM, combat wings, trading, mining,
and the macro→local mission handoff. Kept whole by decision; see "Coupling flags".*

---

## Tier 2 — galaxy simulation

### GalaxyGeneration — high *(merged; see Settled calls)*
**Responsibility:** Deterministically generates the galaxy and its star systems from a master seed.
**Files:** `sim/galaxy_gen.{cpp,h}`, `sim/galaxy_params.h`, `sim/galaxy_spatial.{cpp,h}`,
`sim/ss_generation.{cpp,h}`, `sim/system_evolution.{cpp,h}`
**Interface actually used:** `ss_generation.h` by 5 clusters; `galaxy_gen.h` by 3;
`galaxy_spatial.h` by 2; `system_evolution.h` by 2 (DevPanels, FrameOrchestrator);
`galaxy_params.h` by 1 (GameStateModel).
**Depends on:** DeterministicRng, GameStateModel. **Depended on by:** GalaxyRuntime,
GalaxyMapRendering, DevPanels.
**Engine:** `defines.h`(5), `bs_memory.h`(2), `bs_hierpos.h`(2), `logger.h`(1).

### GalaxyRuntime — high
**Responsibility:** Maintains the hot cache of materialised systems near the camera, plus lane
routing and deterministic station layout.
**Files:** `sim/galaxy_map.{cpp,h}` · **Interface actually used:** `galaxy_map.h` by
**8 clusters** · **Engine:** `logger.h`, `bs_memory.h`, `defines.h`, `bs_hierpos.h`.
*Also defines two functions declared in `game_state.h` (`sensor_visibility_from_dist`,
`get_sensor_visibility`), which is why three render clusters call them without including it.*

### GalaxyHistory — high
**Responsibility:** Simulates civilisations across deep time and answers diplomacy, ownership
and garrison queries.
**Files:** `sim/galaxy_history.{cpp,h}` · **Used by:** 7 clusters · **Engine:** `logger.h`,
`bs_memory.h`, `bs_ui.h`, `defines.h`.
*Builds five ImGui browser windows itself — a simulation module owning substantial presentation.*

### MacroMissions — high
**Responsibility:** Moves persistent cross-system travellers along the lane graph and settles
the civ economy.
**Files:** `sim/ship_mission.{cpp,h}` · **Used by:** FrameOrchestrator, GalaxyMapRendering,
LocalAgentAi · **Engine:** `logger.h`, `defines.h`, `bs_hierpos.h`.
*2735 lines — the second-largest sandbox file after `game_state.h`.*

### Economy — high
**Responsibility:** Derives deterministic station markets and applies the decaying stock deltas
trade produces.
**Files:** `sim/station_market.{cpp,h}` · **Used by:** LocalAgentAi, MacroMissions,
FrameOrchestrator · **Engine:** `defines.h`.

### Territory — high
**Responsibility:** Partitions the galaxy into Voronoi cells and renders and hit-tests them.
**Files:** `sim/voronoi_galaxy.{cpp,h}`, `jc_voronoi.h`, `render/voronoi_cell_hover_effect.{cpp,h}`
**Interface actually used:** `voronoi_galaxy.h` by 2; `voronoi_cell_hover_effect.h` by 2.
**Engine:** `bs_hierpos.h`(3), `math_utils.h`(2), `renderer.h`(2), `defines.h`(2),
`renderer_types.h`(2).
*Mixes generation, query and rendering in one cluster, unlike the surrounding sim/render split.*

---

## Tier 3 — player interaction

| Subsystem | Responsibility | Files | Used by | Engine headers | Conf |
|---|---|---|---|---|---|
| **RtsControl** | Handles fleet selection, orders, jump mode and the free camera, and draws their overlays. | `sim/rts_controls.{cpp,h}` | hub member | `math_utils.h`(2), `input.h`, `renderer.h`, `bs_imgui.h`, `bs_rml.h`, `camera2d.h`, `defines.h`, `renderer_types.h` | high |
| **CameraControl** | Drives zoom from the wheel and flips the arena/map view mode with a control hand-off. | `sim/camera_controller.{cpp,h}` | FrameOrchestrator | `input.h`, `defines.h` | high |
| **WorldEditor** | Picks and drags in-world entities via transform gizmos. | `sim/editor_tools.{cpp,h}` | InWorldOverlays, FrameOrchestrator | `input.h`, `bs_imgui.h`, `bs_rml.h`, `defines.h`, `bs_hierpos.h` | high |
| **Discovery** | Detects and records first identification of NPC ships and stations. | `sim/discovery.{cpp,h}` | LocalAgentAi, FrameOrchestrator | `bs_hierpos.h`, `defines.h` | high |
| **ActionLog** | Appends formatted messages to the rolling HUD log. | `sim/action_log.{cpp,h}` | 5 clusters | `defines.h` | high |
| **TravelDebug** | Interpolates a point-to-point journey for the editor-gated travel overlay. | `sim/travel.{cpp,h}` | GameStateModel only | `defines.h`, `bs_hierpos.h` | high (near-dead) |

---

## Tier 4 — rendering

### SceneOrchestration — high
**Responsibility:** Owns the ordered world-drawing passes and submits the frame's lighting.
**Files:** `render/scene_renderer.{cpp,h}`, `render/frame_lighting.{cpp,h}`
**Interface actually used:** both headers, by FrameOrchestrator only.
**Depends on:** every render cluster it sequences. **Engine:** `renderer.h`(2), `defines.h`.
*The pass order is a hard dependency: `parallax_background` writes `s->star_pos`, which
`frame_lighting` and `ship_scene` read.*

### ShipRendering — high
**Responsibility:** Draws ship hulls, mounts, exhaust, emblems and hardpoint annotations.
**Files:** `render/ship_scene.{cpp,h}`, `render/ship_render.{cpp,h}`
**Interface actually used:** `ship_render.h` by 3 clusters; `ship_scene.h` by 2.
**Engine:** `renderer.h`(2), `camera2d.h`(2), `defines.h`.
*Writes `Ship::render_pos`, which five other clusters read — a produced-state dependency
invisible in the include graph.*

### Backdrop — high
**Responsibility:** Draws the parallax backdrop layer stack: procedural starfield, nebula, and
the camera's current star system.
**Files:** `render/global_background.{cpp,h}`, `starfield_layer.{cpp,h}`, `nebula_layer.{cpp,h}`,
`mapped_system_layer.{cpp,h}`, `parallax_background.{cpp,h}`
**Interface actually used:** `global_background.h` by 2; `parallax_background.h` by 2;
`mapped_system_layer.h` by 1.
**Engine (second-heaviest, 22 edges):** `renderer.h`(5), `camera2d.h`(5), `defines.h`(5),
`math_utils.h`(4), `logger.h`, `renderer_types.h`, `bs_hierpos.h`.
*The only sandbox cluster using raw `new`/`delete`, bypassing the engine's tagged allocator.*

### CelestialParallax — high
**Responsibility:** Computes the shared depth-parallax offset applied to celestial backdrop bodies.
**Files:** `sim/celestial_parallax.{cpp,h}` · **Used by 5 clusters** (Backdrop,
SceneOrchestration, ShipRendering, GalaxyMapRendering, FrameOrchestrator)
· **Engine:** `defines.h`, `bs_hierpos.h`.
Split out of Backdrop precisely because four render clusters consume it.

### CelestialFx — high
**Responsibility:** Renders stars and planets as procedural sprites and impostor spheres, and
owns their editor tuning.
**Files:** `render/star_fx.{cpp,h}` · **Used by:** Backdrop, SceneOrchestration, GameStateModel
· **Engine:** `logger.h`, `renderer.h`, `bs_ui.h`, `defines.h`, `math_utils.h`, `renderer_types.h`.
*The only sandbox module with editor state persisted to disk (`bin/planet_editor.cfg`).*

### GalaxyMapRendering — high
**Responsibility:** Draws the galaxy-map look and answers cursor hit-tests against it.
**Files:** `render/galaxy_map_render.{cpp,h}` · **Used by:** FrameOrchestrator,
SceneOrchestration, RtsControl
**Engine:** `input.h`, `bs_memory.h`, `renderer.h`, `camera2d.h`, `bs_ui.h`, `bs_imgui.h`,
`bs_rml.h`, `defines.h`.
*The only render cluster the sim side calls into for answers rather than drawing
(`galaxy_pick_planet`, `galaxy_map_hover_tooltip`). Also defines four `STAR_*` constants
declared in `game_state.h` and read by Backdrop.*

### SystemContentRendering — high
**Responsibility:** Draws per-system ambient content — asteroids, resource nodes, dust and
stations — under a sprite budget.
**Files:** `render/system_content_render.{cpp,h}` · **Used by:** SceneOrchestration
· **Engine:** `bs_hierpos.h`, `renderer.h`, `camera2d.h`, `defines.h`.

### InWorldOverlays — high
**Responsibility:** Draws in-world gameplay feedback: projectiles, markers, sensor and
point-defense visuals, and the radiation heat map.
**Files:** `render/gameplay_overlays.{cpp,h}`, `sensor_overlay.{cpp,h}`,
`defense_laser_overlay.{cpp,h}`, `out_sensor_detection_fx.{cpp,h}`, `sim/heat_map.{cpp,h}`
**Interface actually used:** `gameplay_overlays.h` by 2; `heat_map.h` by 1;
`out_sensor_detection_fx.h` by 1 (GameStateModel); `sensor_overlay.h` by 1 (DevPanels).
**Depends on (out-degree 21, the highest of any render cluster):** CoordinateFrames,
CombatArena, RenderLayerTable, WorldEditor, ShipRendering, GalaxyHistory, LocalAgentAi,
BitmapText, ShipCombatModel. **Engine:** `renderer.h`(5), `defines.h`(3), `camera2d.h`(2),
`math_utils.h`, `renderer_types.h`.

### CoordinateDiagnostics — high
**Responsibility:** Visualises and validates the hierarchical coordinate lattice in debug builds.
**Files:** `core/coord_diag.{cpp,h}`, `render/debug_overlay.{cpp,h}`
**Used by:** DevPanels, FrameOrchestrator, ShipRendering · **Engine:** `defines.h`(2),
`bs_hierpos.h`, `renderer.h`, `camera2d.h`.

---

## Tier 5 — UI and shell

### DevPanels — high *(grouped by decision; see Settled calls)*
**Responsibility:** Builds the game's ImGui developer, setup and inspector panels.
**Files:** `ui/editor_ui.{cpp,h}`, `ui/new_game_setup.{cpp,h}`, `ui/system_inspector.{cpp,h}`
**Interface actually used:** all three headers, by FrameOrchestrator only.
**Depends on:** GalaxyGeneration, CoordinateDiagnostics, CoordinateFrames, ActionLog,
CombatArena, ShipRendering, InWorldOverlays, DeterministicRng.
**Engine:** `bs_ui.h`(3), `bs_rml.h`, `logger.h`.
*Zero include edges between the three members — they cohere only by call site and engine
dependency. `editor_ui.cpp` writes three globals owned by other clusters
(`g_debug_cell_grid`, `g_zoom_out_speed_gain`, `g_sensor_fade_distance`).*

### Bootstrap — high
**Responsibility:** Supplies `game_create`, the single symbol the engine links against.
**Files:** `entry.cpp` · **Engine:** `entry.h`, `bs_memory.h`.

---

## Files that don't fit cleanly

**`state/game_state.h` + `game.h` — a god object, not a subsystem.** 3652 lines defining every
enum, record and capacity constant, reached via a 7-line shim by 48 files. **27 of 32
subsystems depend on it**, which is why include-based clustering yields nothing without
stripping it. Four headers (`galaxy_history.h`, `ai_ship.h`, `ship_mission.h`,
`ss_generation.h`) deliberately delegate their types here, so the data/behaviour split is a
real convention — but the consequence is that every subsystem's data lives outside the
subsystem. Listed as `GameStateModel` for completeness only.

**`game.cpp` + `game_modules.h` — the god orchestrator.** Out-degree 43, in-degree 0.
`game.cpp` is 3403 lines doing three unrelated jobs: the four engine callbacks, ~25 inline
keybindings, and a ~960-line `game_push_hud` marshalling the entire RmlUi snapshot.
`game_modules.h` is a build-hygiene device (an aggregate include for exactly one TU), not a module.

**`jc_voronoi.h`** — vendored third-party (MIT) inside `sandbox/source`, compiled under the
game's `-Wall -Werror` unlike the engine's vendored libraries, and allocating via `malloc`
outside the tagged allocator. Assigned to Territory for want of a better home.

**`font8x8.h`** — vendored public-domain data with a single consumer. Sits in BitmapText.

**`render/starfield_generator.{cpp,h}` — dead.** Never called; `global_background.cpp` includes
the header and uses nothing. Its GPU-side counterpart
(`engine/source/renderer/starfield_gpu_resources`) is equally dead. `starfield_layer.cpp` sets
`layer_data = nullptr` ("procedural path"), which kills both halves of the old VBO pipeline.

**`sim/travel.{cpp,h}` — near-dead.** Editor-gated debug only; two of three ease modes are
unreachable (`travel_init` always selects linear and nothing else writes `ease_mode`), and its
`active`/`paused` fields duplicate `game_state`'s own flags.

**Three files whose directory contradicts their role.** `sim/celestial_parallax` and
`sim/heat_map` are render-space code under `sim/`; `render/ship_visual` is ship data-model code
under `render/`. All three are clustered by responsibility, not directory.

---

## Coupling flags

### LocalAgentAi sits in three cycles — a god object, not a boundary problem
`ai_ship.cpp` ⇄ `galaxy_history.h`, ⇄ `ship_mission.h`, ⇄ `discovery.h` (1 edge each way in
all three). At 2524 lines it does population management, a behaviour FSM, combat wings,
trading, mining, and the macro→local mission handoff. The three cycles are all legitimate
collaborations; the problem is the file's size, not the cluster's edges. Kept whole by
decision. Worth revisiting alongside `ship_mission.cpp` (2735 lines) as one AI-tier
decomposition — together they are ~5,200 lines across two subsystems.

### GalaxyHistory ⇄ GalaxyRuntime (1 / 1)
`galaxy_map.cpp` seeds and reads history ownership; `galaxy_history.cpp` calls
`galaxy_nearest_node`. Mild and expected for a hot cache over a simulated galaxy. Flagging,
not merging.

### Resolved by the settled calls
`ShipCombatModel` and `GalaxyGeneration` each absorbed a cycle that the finer-grained split
had produced. Both merges verified: neither pair appears in the cycle report any more.

### Not a cycle, but worth noting
`InWorldOverlays` has out-degree 21 — the highest in the rendering tier — reaching into nine
other clusters including three simulation ones. It is a dispatcher as much as a renderer.

---

## Boundary: engine headers per sandbox subsystem

204 boundary edges reach **15 distinct engine headers**. To be reconciled with
`docs/architecture/engine-api-boundary.md`.

| sandbox subsystem | edges | engine headers |
|---|--:|---|
| ShipCombatModel | 23 | `defines.h`(6), `math_utils.h`(5), `logger.h`(4), `renderer.h`(3), `renderer_types.h`(3), `bs_hierpos.h`(2) |
| Backdrop | 22 | `renderer.h`(5), `camera2d.h`(5), `defines.h`(5), `math_utils.h`(4), `logger.h`, `renderer_types.h`, `bs_hierpos.h` |
| InWorldOverlays | 12 | `renderer.h`(5), `defines.h`(3), `camera2d.h`(2), `math_utils.h`, `renderer_types.h` |
| Territory | 11 | `bs_hierpos.h`(3), `math_utils.h`(2), `renderer.h`(2), `defines.h`(2), `renderer_types.h`(2) |
| GalaxyGeneration | 10 | `defines.h`(5), `bs_memory.h`(2), `bs_hierpos.h`(2), `logger.h` |
| FrameOrchestrator | 9 | one each of `logger.h`, `input.h`, `math_utils.h`, `bs_hierpos.h`, `renderer.h`, `camera2d.h`, `bs_imgui.h`, `bs_rml.h`, `bs_ui.h` |
| FleetControl | 9 | `math_utils.h`(3), `defines.h`(3), `bs_hierpos.h`(2), `input.h` |
| RtsControl | 9 | `math_utils.h`(2), + one each of `input.h`, `renderer.h`, `bs_imgui.h`, `bs_rml.h`, `camera2d.h`, `defines.h`, `renderer_types.h` |
| CoordinateFrames | 8 | `bs_hierpos.h`(3), `camera2d.h`(2), `defines.h`(2), `input.h` |
| GalaxyMapRendering | 8 | one each of `input.h`, `bs_memory.h`, `renderer.h`, `camera2d.h`, `bs_ui.h`, `bs_imgui.h`, `bs_rml.h`, `defines.h` |
| BitmapText | 7 | `defines.h`(2), `logger.h`, `renderer.h`, `camera2d.h`, `math_utils.h`, `renderer_types.h` |
| CelestialFx | 6 | `logger.h`, `renderer.h`, `bs_ui.h`, `defines.h`, `math_utils.h`, `renderer_types.h` |
| CombatArena | 6 | `defines.h`(3), `math_utils.h`(2), `bs_hierpos.h` |
| CoordinateDiagnostics | 5 | `defines.h`(2), `bs_hierpos.h`, `renderer.h`, `camera2d.h` |
| ShipRendering | 5 | `renderer.h`(2), `camera2d.h`(2), `defines.h` |
| WorldEditor | 5 | `input.h`, `bs_imgui.h`, `bs_rml.h`, `defines.h`, `bs_hierpos.h` |
| DevPanels | 5 | `bs_ui.h`(3), `bs_rml.h`, `logger.h` |
| SystemContentRendering | 4 | `bs_hierpos.h`, `renderer.h`, `camera2d.h`, `defines.h` |
| GalaxyHistory | 4 | `logger.h`, `bs_memory.h`, `bs_ui.h`, `defines.h` |
| GalaxyRuntime | 4 | `logger.h`, `bs_memory.h`, `defines.h`, `bs_hierpos.h` |
| GameStateModel | 4 | `defines.h`, `game_types.h`, `renderer_types.h`, `containers/vector.h` |
| Profiling | 3 | `bs_ui.h`, `renderer.h`, `defines.h` |
| SceneOrchestration | 3 | `renderer.h`(2), `defines.h` |
| MacroMissions | 3 | `logger.h`, `defines.h`, `bs_hierpos.h` |
| Geometry2D, Bootstrap, LocalAgentAi, CameraControl, Discovery, TravelDebug | 2 each | see the tool output |
| RenderLayerTable, DeadStarfieldGen, ActionLog, Economy | 1 each | see the tool output |

### Patterns worth validating against the engine-side boundary doc

- **`core/input.h` is reached by 7 clusters** (CoordinateFrames, FleetControl, RtsControl,
  CameraControl, WorldEditor, GalaxyMapRendering, FrameOrchestrator) — input is not funnelled
  through a single place on the game side.
- **The three UI facades (`bs_imgui.h`, `bs_rml.h`, `bs_ui.h`) are reached by 8 clusters**,
  including three simulation ones (RtsControl, WorldEditor, GalaxyHistory) — UI concerns are
  not confined to the UI tier.
- **`core/memory/bs_memory.h` is used by only 5 clusters** (GalaxyGeneration, GalaxyRuntime,
  GalaxyHistory, GalaxyMapRendering, Bootstrap) — everything else is fixed-size arrays.
  Two allocating paths bypass it entirely: Backdrop's `new`/`delete` and `jc_voronoi`'s `malloc`.
- **No sandbox cluster touches `platform.h`, `event.h`, `application.h`, or `asserts.h`** —
  consistent with the engine-side finding that those are internal. The engine's event bus has
  zero game-side consumers despite `event_register`/`event_fire` being exported.

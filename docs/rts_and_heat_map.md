# RTS and Heat-Map System

This document describes the real-time-strategy (RTS) fleet controls and the radiation-detector heat map that are active in the Black Stride sandbox. The merged cyan "radar union" outline that previously surrounded fleet ships was removed because its fullscreen pass could not be reliably aligned with the sprite batch; only the heat map remains as the sensor visualization.

## RTS Controls (Global Mode)

When the game is in `MODE_GLOBAL` the player can switch between **piloted ship mode** and **free-camera RTS mode**. The `RtsControls` class (`sandbox/source/rts_controls.cpp`) owns the input state and transitions.

### Camera Modes

- **Piloted ship** — the camera is locked to the ship the player is currently piloting (default is the flagship, index 0). The player flies with WASD, Q/E strafe, and A/D turn.
- **Free camera** — the camera detaches from the piloted ship. The ship continues to coast under physics while the player:
  - Pans with WASD or by moving the mouse to the screen edges.
  - Drags with middle mouse to reposition the view.
  - Issues selection and orders as described below.

Pressing a number key (1-4) while detached sets a new piloted ship and starts a 0.6s smooth camera transition back to that ship. Once the transition finishes, free-camera mode is disabled.

### Selection

Selection is only available while in free camera:

- **Left-click** on a fleet ship to select it.
- **Drag left mouse** to draw a box; all fleet ships whose origins fall inside the world-space box are selected.
- **Hover** over a fleet ship draws a dashed rotating ring around it.

The visual feedback (selection rectangles, hover rings, move/attack markers) is drawn on `HOVER_CIRCLE_LAYER` (layer 50) using `renderer_draw_rect_outline`, `renderer_draw_line`, and `renderer_draw_*` primitives.

### Orders

- **Right-click on empty space** — selected ships receive a `FleetOrder::Move` order.
- **Right-click on an enemy** — selected ships receive a `FleetOrder::Attack` order targeting that ship.
- Piloting a ship clears its current order because the player is now in direct control.

## Fleet System

The fleet is managed by the `Fleet` class in `sandbox/source/fleet.h` / `fleet.cpp`.

### Data Model

- `FLEET_MAX_SHIPS = 8` — hard upper bound on fleet size.
- `FleetShip` contains the `Ship` pose, `ShipFlight` inertial state, `ShipType`, selection flag, and current order.
- `Fleet` stores a fixed array of `FleetShip` so `Ship*` pointers handed to combat/render systems remain stable.
- Member 0 is the **flagship** (the historical single player ship). Additional ships are spawned alongside it.

### Orders and Autopilot

`FleetOrder` has three states:

- `None` — no RTS order; the ship coasts or is manually piloted.
- `Move` — autopilot to `move_target`.
- `Attack` — autopilot to engage `attack_target`.

`Fleet::update_autopilot` runs every frame (except for the piloted ship):

- `FleetShip::update_move` rotates the ship toward the target, then thrusts or brakes based on braking distance. When the ship is within `RTS_MOVE_ARRIVE_DIST` (60 units) and nearly stopped, the order completes.
- `FleetShip::update_attack` rotates toward the target, approaches to `RTS_ATTACK_RANGE` (600 units), maintains a standoff distance, and fires when the target is within the firing cone and range. It uses projectile-speed leading for moving targets.

### Formation Layout

For a multi-ship move order, `Fleet::order_move` builds a centered grid formation:

- `forward` = centroid → target.
- `right` = perpendicular.
- Spacing = `max(2.5 * max_bounding_radius, 120)` world units.
- Grid dimensions: `cols = ceil(sqrt(n))`, `rows = ceil(n / cols)`.
- Each selected ship is assigned a slot centered on the target.

### Simulation

`Fleet::simulate_all` integrates every ship's pose from its linear and angular velocity. The piloted ship receives manual turn commands; all other ships auto-stabilize their spin when no turn command is active.

## Heat Map / Radiation Detector

The heat map is a fullscreen GPU overlay that visualizes the radiation field around active combat entities. It is drawn only in `MODE_GLOBAL` when `show_metaball_ui` is enabled.

### CPU Gathering (`draw_ship_metaballs`)

The function `draw_ship_metaballs` in `sandbox/source/game.cpp` runs once per frame:

1. Computes the world-space viewport from `fb_width / camera.zoom` and `fb_height / camera.zoom`.
2. Culls sources that are outside the viewport plus a margin of `base_radius * 3`.
3. Iterates over `combat_entities` and collects any active entity with `radiation_emission > 0`.
4. For non-drone (non-fleet) sources, attenuates the emission by distance to the nearest fleet ship using `(base_radius / dist)^4` beyond `base_detection_radius`. This makes distant non-fleet sources very faint.
5. Appends velocity-extrapolated trail points if the source is moving. Trail count is clamped by `heat_tail_length`, and emission fades with `heat_tail_fade`.
6. Fills a `bs_heat_map_params` struct and calls `renderer_draw_heat_map`.

Tunable parameters live in `game_state` (`sandbox/source/game.h`):

- `show_metaball_ui` — toggle.
- `base_detection_radius` — detector field radius.
- `metaball_threshold` — alpha threshold.
- `heat_map_intensity` — global opacity.
- `heat_tail_length` / `heat_tail_fade` — trail geometry.
- `heat_warp_strength` — domain warp displacement.

### GPU Submission

`renderer_draw_heat_map` stores the parameters in the backend. The SDL GPU backend (`engine/source/renderer/backend/renderer_backend_sdlgpu.cpp`) queues the parameters in `g_sdl.heat_map_params` and draws a fullscreen triangle in both the bloom and non-bloom paths using `pipeline_heat_map` or `pipeline_heat_map_swapchain`.

The fragment uniform buffer layout is:

```
ubo[0..3]  = camera_pos.x, camera_pos.y, viewport_w, viewport_h
ubo[4..7]  = base_radius, heat_warp_strength, threshold, intensity
ubo[8..11] = source_count, 0, 0, 0
ubo[12..]  = sources[0..count-1] as (x, y, 0, emission)
```

### Shader Algorithm (`heat_map.frag.hlsl`)

The fragment shader (`assets/shaders/src/heat_map.frag.hlsl`) executes per pixel:

1. Converts screen UV to world position using `camera.xy` and `camera.zw`, inverting Y.
2. Adds a low-frequency smooth-noise domain warp controlled by `heat_warp_strength`.
3. For each source, accumulates `base_radius^2 / distance^2` to a scalar field and `emission^2 * base_radius^2 / distance^2` to a weighted visual field.
4. Maps the weighted value to a rainbow gradient: blue → cyan → green → yellow → red.
5. Computes alpha as `saturate(value_weighted / threshold) * intensity`.

The result is an alpha-blended colored overlay behind the sprite batch.

## Removed Radar Union

A separate fullscreen pass (`radar_union.frag.hlsl`) used to draw a merged cyan outline around the detection disks of all fleet ships. It was submitted by `draw_ship_metaballs` via `renderer_draw_radar_union`. This feature was removed because the outline position could not be made to match the sprite batch positions reliably (the union shader's camera/viewport transform diverged from the live swapchain camera used by sprites). The heat map continues to provide the sensor visualization.

## Key Files

| System | Files |
|--------|-------|
| RTS input & visuals | `sandbox/source/rts_controls.h`, `sandbox/source/rts_controls.cpp` |
| Fleet data & autopilot | `sandbox/source/fleet.h`, `sandbox/source/fleet.cpp` |
| Heat map CPU submission | `sandbox/source/game.cpp` (`draw_ship_metaballs`) |
| Heat map params | `engine/source/renderer/renderer_types.h` (`bs_heat_map_params`) |
| Heat map renderer API | `engine/source/renderer/renderer.h`, `engine/source/renderer/renderer.cpp` |
| Heat map GPU backend | `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` |
| Heat map shader | `assets/shaders/src/heat_map.frag.hlsl` |

# Heat-Signature Detection Gameplay Design

This document proposes concrete ways to integrate the existing heat-map / radiation-detector system into gameplay. The renderer already draws a procedural colored heat map from `CombatEntity.radiation_emission`; the goal here is to turn that visualization into a meaningful sensor layer that drives long-range combat, strategic weapon deployment, and contamination avoidance.

## Current Sensor Model (as-is)

The sensor model is implemented in `sandbox/source/game.cpp` (`draw_ship_metaballs`) and the `heat_map.frag.hlsl` shader.

- Every `CombatEntity` has a `radiation_emission` field (0..1). Fleet drones currently emit `0.005` when stationary and scale up to `0.05` at full speed. The enemy raider emits `1.0`.
- The CPU gathers active entities within the world viewport, culls them with a `base_detection_radius * 3` margin, and attenuates non-fleet sources by `(base_detection_radius / distance)^4` beyond the fleet's detection radius.
- The shader accumulates `base_radius^2 / distance^2` into a scalar field and maps the weighted result to a rainbow gradient plus alpha.
- Tunables already exist in `game_state`: `show_metaball_ui`, `base_detection_radius`, `metaball_threshold`, `heat_map_intensity`, `heat_tail_length`, `heat_tail_fade`, `heat_warp_strength`.

This is purely a visualization today. The player sees every radiating contact, but the display does not distinguish "detected," "identified," or "threatening" states, and emissions are static per ship type.

## Design Goals

1. **Enforce long-range warfare** — engagements are fought at standoff distances; closing with hostiles is dangerous and usually a mistake.
2. **Make exploration risky** — unknown contacts should appear before they can be identified, creating uncertainty.
3. **Reward scouting** — fast, low-emission scouts should be able to find enemies without being detected first.
4. **Punish close-range kills** — a destroyed ship's damaged core releases intense radiation, contaminating all ships within a large radius and forcing the player to keep their distance.
5. **Keep the heat map central** — the existing renderer is the single source of truth; gameplay rules should feed into it, not replace it.

## Proposed Mechanics

### 1. Detection Range vs. Identification Range

- **Detection range** (`base_detection_radius`) — a heat blob appears on the map as soon as an entity is within range. The blob shows only position and approximate intensity.
- **Identification range** (e.g., `0.6 * base_detection_radius`) — when the contact is closer, the blob resolves into a specific icon (enemy raider, drone, derelict, asteroid, etc.) and the player can target it.
- **Implementation:** split the CPU gather step into two passes. Emit both `sources` and `identifiable_sources` arrays (or mark each source with a `f32 identity_level`). The shader can use the same field, but the UI can render an icon overlay only when the source is within identification range.
- **Tunable:** `identification_range_ratio` (0..1) in `game_state`.

### 2. Contact Identification Delay

- Even within identification range, a contact should require a short "scan" time before its type and faction are revealed. The scan progresses only while the source is continuously within range and not occluded.
- **Implementation:** add a `f32 scan_progress` and `f32 scan_rate` to `CombatEntity`. In `game_update`, increment `scan_progress` for any entity within identification range; clamp it to 1.0. The heat map and UI only show full identification when `scan_progress >= 1.0`. Reset progress when the contact leaves range for too long.
- **Gameplay effect:** a fast-moving ship may pass through detection range before it is fully identified, preserving the "unidentified blip" tension.

### 3. Sensor Roles and Ship Modules

- Not every ship should be a sensor platform. Add a per-ship **sensor strength** value:
  - **Flagship** — moderate range, slow scan.
  - **Scout drone** — long range, fast scan, but fragile.
  - **Combat drone** — short range, slow scan, high emissions.
- **Implementation:** add `f32 sensor_range` and `f32 scan_rate` to `Ship`. In `draw_ship_metaballs`, compute the effective fleet detection range as the max sensor range among fleet ships, and apply per-ship scan rates during identification. If the fleet is dispersed, only the ship with the best sensor contributes to the detection range for the whole fleet (or compute per-ship coverage for a more advanced version).
- **Tunable:** per-ship sensor values in ship data files or hardcoded defaults.

### 4. Environmental Masking and Background Noise

- Nebulae, stars, or asteroid fields could add noise to the heat map, making contacts harder to detect or identify.
- **Implementation:** the heat map shader already supports `heat_warp_strength`. A per-region "background emission" field can be passed to the shader as an additional uniform or noise texture. Alternatively, the CPU can modulate `base_detection_radius` and `metaball_threshold` based on the player's current zone.
- **Tunable:** `region_heat_noise`, `region_detection_range_modifier`.

### 5. Core Breach Contamination (Close-Range Death Penalty)

- When a ship is destroyed at close range, its damaged core releases a massive radiation burst. Any ship inside the contamination radius receives escalating damage, reduced sensor performance, and a persistent heat bloom that makes it visible across the map.
- **Range categories:**
  - **Safe standoff** — beyond the contamination radius; the normal, preferred combat range.
  - **Contamination zone** — inside the radius; ships accumulate radiation exposure and heat bloom.
  - **Core proximity** — very close to the detonation; severe, possibly fatal, damage over time.
- **Implementation:** add `f32 core_breach_radius` and `f32 core_breach_emission` to `CombatEntity`. On death, spawn a short-lived "radiation hazard" entity with an extremely high emission and a damage field. The heat map shader will naturally render it as a blinding bloom. The `game_update` step applies exposure to ships in range and pushes log warnings via `action_log_push`.
- **Tunable:** `core_breach_radius`, `core_breach_lifetime`, `core_breach_dps`, `core_breach_sensor_blind_duration`.

### 6. Long-Range Probability-Cone Weapons

- Primary weapons are not point-and-click lasers. They are long-range projectiles or missiles fired into a **cone of probability**. The player picks a target and a firing solution; the actual projectile path is randomized within the cone.
- **Cone size** depends on distance, target scan quality, weapon type, and firing ship's sensor/tracking capability. At very long range the cone is wide, so saturation fire or multiple firing ships are needed to score hits.
- **Strategic firing:**
  - **Single precision shot** — narrow cone, high per-projectile damage, long cooldown; used when the target is well-scanned and within a moderate range.
  - **Saturation salvo** — wide cone, many low-damage projectiles, high ammo/heat cost; used at extreme range to suppress or hit evasive targets.
  - **Coordinated fire** — multiple fleet ships fire into overlapping cones to increase the chance at least one projectile hits.
- **Implementation:** add a `fire_cone_angle` and `fire_cone_range` to `Weapon` or the firing function. The projectile spawn function (`weapon_fire_*`) computes a random offset vector within the cone based on the target direction and distance. Add a UI rendering of the expected cone when the player is about to fire (e.g., `RtsControls::draw()` draws the cone from the firing ship toward the target).
- **Tunable:** `weapon_base_cone_angle`, `cone_angle_per_distance`, `max_cone_angle`, `salvo_projectile_count`, `salvo_spread_pattern`.

## Implementation Notes

Most of the proposed mechanics can be implemented without changing the shader:

| Change | Files | Scope |
|--------|-------|-------|
| Per-ship sensor range and scan rate | `sandbox/source/ship.h`, `sandbox/source/fleet.h` | Add fields to `Ship` and `FleetShip`. |
| Identification delay | `sandbox/source/game.h`, `sandbox/source/game.cpp` | Add `scan_progress` to `CombatEntity`; update in `game_update`. |
| UI icon overlay | `sandbox/source/rts_controls.cpp` or a new HUD module | Draw icons over identified contacts on the heat map layer. |
| Environmental masking | `sandbox/source/game.cpp`, `heat_map.frag.hlsl` | Pass region noise/threshold modifier to the shader. |
| Core breach hazard | `sandbox/source/game.h`, `sandbox/source/game.cpp` | Spawn a high-emission radiation entity on ship death; apply exposure damage in `game_update`. |
| Probability-cone weapons | `sandbox/source/weapon.h`, `sandbox/source/weapon.cpp`, `sandbox/source/rts_controls.cpp` | Add cone parameters to weapons; randomize projectile heading within the cone; draw cone UI. |

The heat map shader (`assets/shaders/src/heat_map.frag.hlsl`) and the backend submission (`renderer_draw_heat_map`) remain unchanged except for possibly adding an optional background-noise uniform. The probability-cone weapons can reuse the existing projectile system; only the initial velocity direction changes.

## Open Questions

1. Should identification progress reset instantly when a contact leaves range, or decay slowly?
2. Should the enemy AI use the same sensor rules to detect the player, or should the AI "cheat" for simpler behavior?
3. How should environmental masking be authored? Per-star-system modifier, or a 2D procedural field?
4. Should there be a dedicated sensor/scout ship type, or should sensor modules be attached to existing drones?
5. How large should the core-breach contamination radius be relative to normal weapon range? Should it scale with ship class or reactor size?
6. Should probability-cone spread be driven purely by distance, or also by target velocity and scan quality?
7. Should the AI actively avoid closing to contamination range, or only the player be forced to do so?
8. What visual/audio feedback should accompany a core breach so the player understands why close-range kills are dangerous?

## Recommended First Steps

A minimal viable implementation would be:

1. Add `sensor_range` and `scan_rate` to `Ship` and `CombatEntity`.
2. Implement detection range and identification range in `draw_ship_metaballs` / `game_update`.
3. Add a simple scan-progress bar and log message when a new enemy is identified.
4. Make emissions speed-dependent for all ships, not just drones, so the player can see fast raiders before slow ones.
5. Add a core-breach hazard on ship death: a high-emission entity with a damage-over-time field. This immediately establishes the "don't fight close" rule.
6. Convert the default weapon to a probability-cone weapon by adding a small random spread to the projectile heading based on distance, and draw the expected cone in the RTS UI.

This gives the player the core long-range combat loop: detect at range, identify, fire into a cone, and avoid the resulting radiation bloom.

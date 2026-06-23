# Weapon System Skill

Architecture, integration points, and conventions for the Black Stride weapon / projectile system.

## Ownership Model

- Weapons belong to ships. Each `Ship` has `weapons[SHIP_MAX_WEAPONS]` (4 slots), `weapon_count`, and `active_weapon_idx`.
- Weapons are allocated on the heap during game init (`weapon_create_ballistic_cannon`) and referenced by raw pointer.
- `Ship` does **not** own `ProjectileSystem` — that is a single global manager inside `game_state`.
- `CombatEntity` pool lives in `game_state`, synced each frame to ship positions for collision.

## Firing Flow

1. **Input** (`game.cpp`) — left click in `MODE_GLOBAL`, gated on `!bs_imgui_wants_mouse()`. `1-4` keys switch `active_weapon_idx`.
2. **Direction** — `mouse_world(s) - ship.origin` gives the world-space aim vector.
3. **Velocity inheritance** — the ship's current `flight.velocity` is passed to `Weapon::fire(..., ship_velocity, projectiles)`.
4. **Projectile spawn** — `BallisticWeapon::fire` normalises the aim direction, computes `projectile_vel = ship_velocity + dir * projectile_speed`, and calls `ProjectileSystem::spawn()`.
5. **Faction colour** — all factions now use warm incandescent `bs_color{1.0f, 0.75f, 0.35f, 1.0f}` for a unified bullet look.

## ProjectileSystem

- Fixed-size pool (`MAX_PROJECTILES = 256`). No heap allocation during gameplay.
- `init()` generates two procedural GPU textures:
  - `streak_texture`: 128x512 tapered bullet streak (Gaussian width falloff + hot white core line)
  - `flash_texture`: 128x128 radial gradient for muzzle bursts
- `update(dt)` advances positions, retires expired projectiles, and increments `age`.
- `render(layer)` draws each active projectile as **two additive sprites**:
  1. **Streak** — velocity-aligned, anchored at the head, uses the streak texture
  2. **Muzzle flash** — large radial burst (only first 50 ms, fades with age)
- Projectiles track `age` (seconds since spawn) for the muzzle flash timing and shader shimmer.

## Collision System

- Projectiles collide with `CombatEntity` instances of **opposing factions** only.
- Broad-phase: circle-circle distance test using `CombatEntity::radius`.
- Narrow-phase: for ship-backed entities (`ce->ship != nullptr`), a `point_in_polygon` test using the ship's collider corners.
- On hit: projectile is deactivated (`p->active = FALSE`) and removed from the pool.

## Shader Glow System

- The sprite fragment shader (`sprite.frag.hlsl`) supports procedural glow, heat distortion, colour temperature, and head bloom — all driven by per-sprite `custom` parameters.
- `bs_sprite.custom` is a `bs_color` (float4) passed through the vertex format as `TEXCOORD3`:
  - `custom.x` = glow intensity multiplier (set to `1.0` for bullets)
  - `custom.y` = age / time (drives heat shimmer phase)
- The shader reads tunable parameters from a uniform block pushed per draw-run:
  - `glow[0]` = intensity, falloff, head_mult, head_falloff
  - `glow[1]` = head_range, distort_amp, wave_speed, wave_freq
  - `glow[2]` = jitter_speed, jitter_freq
  - `glow[3..6]` = glow_tint, temp_cool, temp_warm, temp_hot RGB
- These parameters are editable in real-time via the **BULLET GLOW** section of the EDITOR PANEL.

## Tunable Parameters (Editor Panel)

| Parameter | Default | Range | Description |
|---|---|---|---|
| `intensity` | `1.0` | `0..3` | Global glow multiplier |
| `falloff` | `6.0` | `1..20` | Radial Gaussian exponent |
| `head_mult` | `4.0` | `0..8` | Head bloom intensity |
| `head_falloff` | `2.5` | `0.5..10` | Head bloom Gaussian width |
| `head_range` | `0.80` | `0..1` | UV threshold for head bloom |
| `distort_amp` | `0.08` | `0..0.3` | Heat distortion UV warp amplitude |
| `wave_speed` | `15.0` | `0..50` | Primary wave speed |
| `wave_freq` | `8.0` | `0..30` | Primary wave frequency |
| `jitter_speed` | `45.0` | `0..100` | Secondary jitter speed |
| `jitter_freq` | `24.0` | `0..60` | Secondary jitter frequency |
| `glow_tint` | `(1, 0.85, 0.5)` | RGB | Base glow colour |
| `temp_cool` | `(0.9, 0.15, 0.02)` | RGB | Tail temperature colour |
| `temp_warm` | `(1, 0.45, 0.05)` | RGB | Mid temperature colour |
| `temp_hot` | `(1, 0.98, 0.90)` | RGB | Head temperature colour |

## Extending the System

### Add a new weapon type

1. Derive from `Weapon` (override `fire`, `update`, `ready`, `cooldown_progress`).
2. Create a factory function (e.g. `weapon_create_missile_launcher`).
3. Equip it to a ship slot in `game_init`.
4. The existing WEAPONS HUD reads the generic `Weapon` interface — no UI changes needed.

### Add new projectile behaviour

- Extend `Projectile` struct with new fields (e.g. `homing_target`, `damage`, `explosion_radius`).
- Modify `ProjectileSystem::update` to handle the new logic.
- Keep rendering in `ProjectileSystem::render` unless the projectile needs a completely different draw path.

### Add glow to new sprite types

- Set `bs_sprite.custom.x` to the desired glow intensity.
- Set `bs_sprite.custom.y` to a time/age value for animated shimmer.
- The shader will automatically apply radial glow, temperature gradient, and head bloom.

## Key Conventions

- `Weapon::fire()` receives **ship_velocity** so projectiles inherit momentum. Always add it to the projectile velocity.
- Cooldowns are tracked per-weapon (`cooldown_remaining`), updated in `Weapon::update(dt)`.
- The UI shows `ready()` / `cooldown_progress()` state; no separate event system is needed.
- Rendering layer: `LAYER_UI` for projectiles so they draw on top of ships but below gizmos.
- Projectile colours are warm incandescent white-yellow-orange for all factions (set in `weapon.cpp`).
- The `bs_sprite.custom` field is the official channel for passing per-sprite shader parameters.
